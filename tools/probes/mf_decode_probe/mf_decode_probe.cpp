// S0 measurement probe: Media Foundation VP9 / VP8 decoder MFT on this machine (not part of any product target).
//
//   mf_decode_probe --enum
//   mf_decode_probe <clip.webm> [--frames N] [--compare N] [--mode nodx|dx|both]
//
// Demuxing uses CEVPlayerCore's WebmDemuxer; the reference picture comes from CEVPlayerCore's VideoDecoder (libvpx).
// For each mode (no D3D manager / own D3D11 device + DXGI device manager) it runs:
//   P1 timing     decode as fast as possible, no readback (+ alpha stream in a second MFT instance)
//   P2 readback   sync readback (dx: copy to staging + Map; nodx: Lock) + Y plane comparison against libvpx for the
//                 first --compare frames (main and alpha), then a seek test (flush, refeed from a keyframe)
//   P3/P4 (dx)    async readback, Map delayed by 1 and 2 frames
// Everything printed is what the MFT actually did on this run.
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>
#include <timeapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "CEVCore/FrameRing.h"
#include "CEVCore/IByteSource.h"
#include "CEVCore/VideoDecoder.h"
#include "CEVCore/WebmDemuxer.h"

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;

namespace
{
	double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

	std::string Narrow(const wchar_t* w)
	{
		if (!w) return "";
		int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
		std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
		if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
		return s;
	}

	std::string GuidStr(const GUID& g)
	{
		wchar_t buf[64];
		StringFromGUID2(g, buf, 64);
		return Narrow(buf);
	}

	std::string SubtypeName(const GUID& g)
	{
		if (g == MFVideoFormat_NV12) return "NV12";
		if (g == MFVideoFormat_P010) return "P010";
		if (g == MFVideoFormat_YV12) return "YV12";
		if (g == MFVideoFormat_IYUV) return "IYUV";
		if (g == MFVideoFormat_I420) return "I420";
		if (g == MFVideoFormat_YUY2) return "YUY2";
		if (g == MFVideoFormat_ARGB32) return "ARGB32";
		if (g == MFVideoFormat_RGB32) return "RGB32";
		return GuidStr(g);
	}

	struct Series
	{
		std::vector<double> v;
		std::string Str() const
		{
			if (v.empty()) return "n=0";
			std::vector<double> s = v;
			std::sort(s.begin(), s.end());
			double sum = 0; for (double x : s) sum += x;
			const size_t i99 = std::min(s.size() - 1, static_cast<size_t>(s.size() * 0.99));
			char b[160];
			std::snprintf(b, sizeof b, "mean %.3f ms p99 %.3f ms max %.3f ms (n=%zu)", sum / s.size(), s[i99], s.back(), s.size());
			return b;
		}
	};

	double ProcessCpuSeconds()
	{
		FILETIME c, e, k, u;
		GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
		auto f = [](FILETIME t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32 | t.dwLowDateTime) * 1e-7; };
		return f(k) + f(u);
	}

	std::vector<ComPtr<IMFActivate>> Enumerate(const GUID& subtype, UINT32 flags)
	{
		MFT_REGISTER_TYPE_INFO in = {MFMediaType_Video, subtype};
		IMFActivate** acts = nullptr;
		UINT32 n = 0;
		std::vector<ComPtr<IMFActivate>> out;
		if (SUCCEEDED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &in, nullptr, &acts, &n)))
		{
			for (UINT32 i = 0; i < n; ++i) { out.emplace_back(acts[i]); acts[i]->Release(); }
			CoTaskMemFree(acts);
		}
		return out;
	}

	void PrintActivate(IMFActivate* a)
	{
		wchar_t* name = nullptr; UINT32 len = 0;
		std::string nm = SUCCEEDED(a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len)) ? Narrow(name) : "?";
		if (name) CoTaskMemFree(name);
		GUID clsid = GUID_NULL; a->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid);
		UINT32 flags = 0; a->GetUINT32(MF_TRANSFORM_FLAGS_Attribute, &flags);
		UINT32 merit = 0; a->GetUINT32(MFT_CODEC_MERIT_Attribute, &merit);
		wchar_t* url = nullptr;
		std::string hw = SUCCEEDED(a->GetAllocatedString(MFT_ENUM_HARDWARE_URL_Attribute, &url, &len)) ? Narrow(url) : "(none)";
		if (url) CoTaskMemFree(url);
		std::printf("    name=\"%s\" clsid=%s MF_TRANSFORM_FLAGS=0x%X merit=%u hwURL=%s\n", nm.c_str(), GuidStr(clsid).c_str(), flags, merit, hw.c_str());
	}

	// ------------------------------------------------------------------------------------------------------
	// Reference decode (libvpx through CEVPlayerCore)
	// ------------------------------------------------------------------------------------------------------
	struct RefFrames { std::vector<std::vector<uint8_t>> y, a; int w = 0, h = 0; };

	bool RefDecode(cev::WebmDemuxer& dmx, uint32_t start, int count, RefFrames& out)
	{
		const cev::VideoStreamInfo& v = dmx.Video();
		std::unique_ptr<cev::VideoDecoder> dec;
		if (cev::VideoDecoder::Create(v, 4, dec) != cev::Status::Ok) return false;
		std::vector<uint8_t> pkt(v.maxPacketSize + 64), add(v.maxAdditionalSize + 64);
		cev::FrameRing ring; cev::FrameFormat fmt; bool init = false;
		for (uint32_t i = start; i < dmx.VideoPacketCount() && static_cast<int>(out.y.size()) < count; ++i)
		{
			const cev::PacketRef& p = dmx.VideoPacket(i);
			dmx.ReadVideoPacket(i, pkt.data(), static_cast<uint32_t>(pkt.size()));
			uint32_t as = 0;
			if (p.additionalSize && dmx.ReadVideoAdditional(i, add.data(), static_cast<uint32_t>(add.size())) == cev::Status::Ok) as = p.additionalSize;
			if (dec->Decode(pkt.data(), p.size, as ? add.data() : nullptr, as) != cev::Status::Ok) return false;
			if (!dec->HasFrame()) continue;
			if (!init)
			{
				cev::DecodedImageInfo info; dec->GetFrameInfo(info);
				fmt.width = info.width; fmt.height = info.height; fmt.hasAlpha = v.hasAlpha; fmt.withRgba = false;
				if (ring.Init(fmt, 2) != cev::Status::Ok) return false;
				out.w = fmt.width; out.h = fmt.height; init = true;
			}
			cev::FrameRing::Slot& s = ring.GetSlot(0);
			if (dec->WriteFrame(fmt, s) != cev::Status::Ok) return false;
			std::vector<uint8_t> y(static_cast<size_t>(fmt.width) * fmt.height);
			for (int r = 0; r < fmt.height; ++r) std::memcpy(&y[static_cast<size_t>(r) * fmt.width], s.plane[0] + static_cast<ptrdiff_t>(r) * s.stride[0], fmt.width);
			out.y.push_back(std::move(y));
			if (fmt.hasAlpha && s.plane[3])
			{
				std::vector<uint8_t> a(static_cast<size_t>(fmt.width) * fmt.height);
				for (int r = 0; r < fmt.height; ++r) std::memcpy(&a[static_cast<size_t>(r) * fmt.width], s.plane[3] + static_cast<ptrdiff_t>(r) * s.stride[3], fmt.width);
				out.a.push_back(std::move(a));
			}
		}
		return true;
	}

	struct Diff { int maxd = 0; double sum = 0; size_t n = 0; void Add(const uint8_t* p, int pitch, const std::vector<uint8_t>& ref, int w, int h)
	{
		for (int r = 0; r < h; ++r) for (int x = 0; x < w; ++x)
		{ int d = std::abs(int(p[static_cast<ptrdiff_t>(r) * pitch + x]) - int(ref[static_cast<size_t>(r) * w + x])); maxd = std::max(maxd, d); sum += d; ++n; }
	} std::string Str() const { char b[96]; std::snprintf(b, sizeof b, "max %d mean %.4f over %zu px", maxd, n ? sum / n : 0.0, n); return b; } };

	// ------------------------------------------------------------------------------------------------------
	// MFT driver (sync and async MFTs)
	// ------------------------------------------------------------------------------------------------------
	using OnOut = std::function<void(IMFSample*)>;

	struct Mft
	{
		ComPtr<IMFTransform> t;
		ComPtr<IMFMediaEventGenerator> ev;
		bool async = false;
		bool d3d11Aware = false;
		DWORD outFlags = 0; DWORD outCb = 0;
		UINT32 outW = 0, outH = 0; INT32 stride = 0; GUID outSubtype = GUID_NULL;
		std::string outDesc;
		int needInput = 0;
		UINT bindFlags = 0; // 0 = leave MF_SA_D3D11_BINDFLAGS alone
		int streamChanges = 0;
		std::string err;

		bool Fail(const char* what, HRESULT hr) { char b[160]; std::snprintf(b, sizeof b, "%s failed hr=0x%08lX", what, static_cast<unsigned long>(hr)); err = b; return false; }

		bool SetNv12()
		{
			for (DWORD i = 0;; ++i)
			{
				ComPtr<IMFMediaType> mt;
				HRESULT hr = t->GetOutputAvailableType(0, i, &mt);
				if (FAILED(hr)) return Fail("no NV12 output type; GetOutputAvailableType", hr);
				GUID st; mt->GetGUID(MF_MT_SUBTYPE, &st);
				if (st != MFVideoFormat_NV12) continue;
				hr = t->SetOutputType(0, mt.Get(), 0);
				if (FAILED(hr)) return Fail("SetOutputType", hr);
				ComPtr<IMFMediaType> cur; t->GetOutputCurrentType(0, &cur);
				cur->GetGUID(MF_MT_SUBTYPE, &outSubtype);
				MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &outW, &outH);
				UINT32 s = 0; stride = SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) ? static_cast<INT32>(s) : 0;
				MFVideoArea ap = {}; UINT32 apSize = 0;
				char apb[64] = "none";
				if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&ap), sizeof ap, &apSize)))
					std::snprintf(apb, sizeof apb, "%dx%d@%d,%d", ap.Area.cx, ap.Area.cy, ap.OffsetX.value, ap.OffsetY.value);
				UINT32 sit = 0; bool hasSit = SUCCEEDED(cur->GetUINT32(MF_MT_VIDEO_CHROMA_SITING, &sit));
				MFT_OUTPUT_STREAM_INFO si = {}; t->GetOutputStreamInfo(0, &si);
				outFlags = si.dwFlags; outCb = si.cbSize;
				char b[320];
				std::snprintf(b, sizeof b, "subtype=%s frame=%ux%u defaultStride=%d aperture=%s chromaSiting=%s0x%X streamInfo.flags=0x%lX cbSize=%lu",
					SubtypeName(outSubtype).c_str(), outW, outH, stride, apb, hasSit ? "" : "(absent)", sit, static_cast<unsigned long>(outFlags), static_cast<unsigned long>(outCb));
				outDesc = b;
				return true;
			}
		}

		bool Init(IMFActivate* act, const GUID& subtype, int w, int h, double fps, IMFDXGIDeviceManager* mgr)
		{
			HRESULT hr = act->ActivateObject(IID_PPV_ARGS(&t));
			if (FAILED(hr)) return Fail("ActivateObject", hr);
			ComPtr<IMFAttributes> at;
			if (SUCCEEDED(t->GetAttributes(&at)) && at)
			{
				UINT32 v = 0;
				async = SUCCEEDED(at->GetUINT32(MF_TRANSFORM_ASYNC, &v)) && v != 0;
				v = 0; d3d11Aware = SUCCEEDED(at->GetUINT32(MF_SA_D3D11_AWARE, &v)) && v != 0;
				if (async) at->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
			}
			if (async)
			{
				hr = t.As(&ev);
				if (FAILED(hr)) return Fail("async MFT without IMFMediaEventGenerator", hr);
			}
			if (mgr)
			{
				hr = t->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(mgr));
				if (FAILED(hr)) return Fail("MFT_MESSAGE_SET_D3D_MANAGER", hr);
			}
			ComPtr<IMFMediaType> in;
			MFCreateMediaType(&in);
			in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
			in->SetGUID(MF_MT_SUBTYPE, subtype);
			MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(w), static_cast<UINT32>(h));
			MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(std::lround(fps * 1000)), 1000);
			in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
			hr = t->SetInputType(0, in.Get(), 0);
			if (FAILED(hr)) return Fail("SetInputType", hr);
			if (bindFlags)
			{
				ComPtr<IMFAttributes> oa;
				hr = t->GetOutputStreamAttributes(0, &oa);
				if (FAILED(hr) || !oa) return Fail("GetOutputStreamAttributes", hr);
				oa->SetUINT32(MF_SA_D3D11_BINDFLAGS, bindFlags);
			}
			if (!SetNv12()) return false;
			t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
			t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
			return true;
		}

		// One ProcessOutput call. Returns 1 = got a sample, 0 = needs more input, -1 = error.
		int PullOne(const OnOut& onOut)
		{
			MFT_OUTPUT_DATA_BUFFER db = {};
			ComPtr<IMFSample> own;
			if (!(outFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)))
			{
				ComPtr<IMFMediaBuffer> buf;
				MFCreateMemoryBuffer(outCb ? outCb : outW * outH * 3 / 2, &buf);
				MFCreateSample(&own); own->AddBuffer(buf.Get());
				db.pSample = own.Get();
			}
			DWORD status = 0;
			HRESULT hr = t->ProcessOutput(0, 1, &db, &status);
			if (db.pEvents) db.pEvents->Release();
			if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return 0;
			if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
			{
				++streamChanges;
				if (db.pSample && !own) db.pSample->Release();
				return SetNv12() ? 2 : -1;
			}
			if (FAILED(hr)) { Fail("ProcessOutput", hr); return -1; }
			if (db.pSample)
			{
				onOut(db.pSample);
				if (!own) db.pSample->Release();
				return 1;
			}
			return 2;
		}

		// Async: wait for one event (<= timeoutMs). Returns event type or 0 on timeout.
		MediaEventType WaitEvent(int timeoutMs)
		{
			const auto t0 = Clock::now();
			for (;;)
			{
				ComPtr<IMFMediaEvent> e;
				HRESULT hr = ev->GetEvent(MF_EVENT_FLAG_NO_WAIT, &e);
				if (SUCCEEDED(hr) && e) { MediaEventType type = 0; e->GetType(&type); return type; }
				if (Ms(t0, Clock::now()) > timeoutMs) return 0;
				std::this_thread::yield();
			}
		}

		bool AsyncPump(const OnOut& onOut, bool untilNeedInput, bool untilDrain)
		{
			for (;;)
			{
				if (untilNeedInput && needInput > 0) return true;
				MediaEventType e = WaitEvent(5000);
				if (e == 0) return Fail("async MFT event timeout (5 s)", E_FAIL);
				if (e == METransformNeedInput) ++needInput;
				else if (e == METransformHaveOutput) { int r; do { r = PullOne(onOut); } while (r == 2); if (r < 0) return false; }
				else if (e == METransformDrainComplete) { if (untilDrain) return true; }
			}
		}

		bool Feed(IMFSample* s, const OnOut& onOut)
		{
			if (async)
			{
				if (!AsyncPump(onOut, true, false)) return false;
				--needInput;
				HRESULT hr = t->ProcessInput(0, s, 0);
				return SUCCEEDED(hr) ? true : Fail("ProcessInput", hr);
			}
			for (int tries = 0; tries < 64; ++tries)
			{
				HRESULT hr = t->ProcessInput(0, s, 0);
				if (hr == MF_E_NOTACCEPTING) { if (!PullAll(onOut)) return false; continue; }
				if (FAILED(hr)) return Fail("ProcessInput", hr);
				return PullAll(onOut);
			}
			return Fail("ProcessInput keeps returning MF_E_NOTACCEPTING", MF_E_NOTACCEPTING);
		}

		bool PullAll(const OnOut& onOut)
		{
			for (int i = 0; i < 64; ++i) { int r = PullOne(onOut); if (r == 0) return true; if (r < 0) return false; }
			return true;
		}

		bool Drain(const OnOut& onOut)
		{
			t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
			t->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
			if (async) return AsyncPump(onOut, false, true);
			return PullAll(onOut);
		}

		void Flush()
		{
			t->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
			needInput = 0;
			t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
		}

		void Stop()
		{
			if (t) { t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0); }
		}
	};

	ComPtr<IMFSample> MakeSample(const uint8_t* data, uint32_t size, int64_t pts100, int64_t dur100, bool key)
	{
		ComPtr<IMFMediaBuffer> buf; MFCreateMemoryBuffer(size, &buf);
		BYTE* p = nullptr; buf->Lock(&p, nullptr, nullptr); std::memcpy(p, data, size); buf->Unlock(); buf->SetCurrentLength(size);
		ComPtr<IMFSample> s; MFCreateSample(&s); s->AddBuffer(buf.Get());
		s->SetSampleTime(pts100); s->SetSampleDuration(dur100);
		if (key) s->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
		return s;
	}

	// ------------------------------------------------------------------------------------------------------
	struct Ctx
	{
		cev::WebmDemuxer* dmx = nullptr;
		std::vector<std::vector<uint8_t>> pkts, adds;
		ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
		ComPtr<IMFDXGIDeviceManager> mgr;
		IMFActivate* act = nullptr;
		IMFActivate* actAlpha = nullptr; // separate IMFActivate -> separate MFT instance
		GUID subtype = GUID_NULL;
		int frames = 300, compare = 10;
		RefFrames ref, seekRef;
		uint32_t seekKey = 0;
	};

	struct OutInfo { bool printed = false; };

	void DescribeSample(IMFSample* s, Mft& m, const char* tag)
	{
		DWORD nb = 0; s->GetBufferCount(&nb);
		ComPtr<IMFMediaBuffer> b; s->GetBufferByIndex(0, &b);
		ComPtr<IMFDXGIBuffer> dx;
		ComPtr<IMF2DBuffer> b2;
		std::printf("    [%s] output: %s\n", tag, m.outDesc.c_str());
		std::printf("    [%s] sample: buffers=%lu IMFDXGIBuffer=%s IMF2DBuffer=%s", tag, static_cast<unsigned long>(nb), SUCCEEDED(b.As(&dx)) ? "yes" : "no", SUCCEEDED(b.As(&b2)) ? "yes" : "no");
		if (dx)
		{
			ComPtr<ID3D11Texture2D> tex; UINT sub = 0;
			if (SUCCEEDED(dx->GetResource(IID_PPV_ARGS(&tex))) && SUCCEEDED(dx->GetSubresourceIndex(&sub)))
			{
				D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
				std::printf(" texture %ux%u fmt=%d(NV12=%d) array=%u mips=%u bind=0x%X usage=%d misc=0x%X subresource=%u", d.Width, d.Height, d.Format, DXGI_FORMAT_NV12,
					d.ArraySize, d.MipLevels, d.BindFlags, d.Usage, d.MiscFlags, sub);
			}
		}
		DWORD maxLen = 0, curLen = 0; BYTE* p = nullptr;
		if (!dx && SUCCEEDED(b->Lock(&p, &maxLen, &curLen))) { b->Unlock(); std::printf(" length=%lu", static_cast<unsigned long>(curLen)); }
		std::printf("\n");
	}

	// Runs one pass. readback: 0 none, 1 sync, 2/3 async with delay 1/2. withAlpha: second MFT for the alpha stream.
	bool RunPass(Ctx& c, bool dxMode, int readback, bool withAlpha, bool doCompare, bool doSeek, const char* name, bool paced = false, UINT bindFlags = 0)
	{
		const cev::VideoStreamInfo& v = c.dmx->Video();
		Mft m, ma;
		IMFDXGIDeviceManager* mgr = dxMode ? c.mgr.Get() : nullptr;
		m.bindFlags = bindFlags;
		if (!m.Init(c.act, c.subtype, v.width, v.height, v.frameRate, mgr)) { std::printf("  %s: main MFT init: %s\n", name, m.err.c_str()); return false; }
		bool alphaOk = false;
		if (withAlpha)
		{
			alphaOk = ma.Init(c.actAlpha, c.subtype, v.width, v.height, v.frameRate, mgr);
			if (!alphaOk) std::printf("  %s: alpha MFT init: %s\n", name, ma.err.c_str());
		}
		if (readback == 0 && !withAlpha) {}

		Series tFeed, tFeedA, tInterval, tRead;
		Diff dY, dA, dSeek;
		int outN = 0, outA = 0, compared = 0, comparedA = 0, wouldBlock = 0;
		bool described = false, describedA = false;
		Clock::time_point lastOut = Clock::now();

		// staging ring for dx readback
		std::vector<ComPtr<ID3D11Texture2D>> stg;
		std::vector<int> stgFrame;
		const int delay = readback >= 2 ? readback - 1 : 0;
		auto mapAndCompare = [&](ID3D11Texture2D* st, int frameIdx, Diff* d, const RefFrames* ref, bool alpha, bool wait) -> bool
		{
			D3D11_MAPPED_SUBRESOURCE ms;
			HRESULT hr = c.ctx->Map(st, 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &ms);
			if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { ++wouldBlock; hr = c.ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms); }
			if (FAILED(hr)) return false;
			{
				// copy the picture out (Y rows + interleaved UV rows), as a readback backend would into its ring slot
				D3D11_TEXTURE2D_DESC sd; st->GetDesc(&sd);
				const int vw = c.dmx->Video().width, vh = c.dmx->Video().height;
				static std::vector<uint8_t> sink;
				sink.resize(static_cast<size_t>(vw) * vh * 3 / 2 + 64);
				const uint8_t* src = static_cast<const uint8_t*>(ms.pData);
				for (int r = 0; r < vh; ++r) std::memcpy(&sink[static_cast<size_t>(r) * vw], src + static_cast<size_t>(r) * ms.RowPitch, vw);
				const uint8_t* uv = src + static_cast<size_t>(ms.RowPitch) * sd.Height;
				for (int r = 0; r < vh / 2; ++r) std::memcpy(&sink[static_cast<size_t>(vw) * vh + static_cast<size_t>(r) * vw], uv + static_cast<size_t>(r) * ms.RowPitch, vw);
			}
			if (ref == nullptr) { c.ctx->Unmap(st, 0); return true; }
			const auto& planes = alpha ? ref->a : ref->y;
			if (d && ref && frameIdx >= 0 && frameIdx < static_cast<int>(planes.size()))
			{ d->Add(static_cast<const uint8_t*>(ms.pData), static_cast<int>(ms.RowPitch), planes[static_cast<size_t>(frameIdx)], ref->w, ref->h); if (alpha) ++comparedA; else ++compared; }
			c.ctx->Unmap(st, 0);
			return true;
		};
		auto ensureStaging = [&](ID3D11Texture2D* src, int count)
		{
			if (!stg.empty()) return;
			D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
			d.ArraySize = 1; d.MipLevels = 1; d.BindFlags = 0; d.MiscFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			for (int i = 0; i < count; ++i) { ComPtr<ID3D11Texture2D> t; c.dev->CreateTexture2D(&d, nullptr, &t); stg.push_back(t); stgFrame.push_back(-1); }
		};
		ComPtr<ID3D11Texture2D> stgA;

		const RefFrames* refNow = &c.ref;
		int frameBase = 0;
		bool seeking = false;

		auto onOut = [&](IMFSample* s)
		{
			const auto now = Clock::now();
			if (!seeking && outN > 0) tInterval.v.push_back(Ms(lastOut, now));
			lastOut = now;
			if (!described) { DescribeSample(s, m, "main"); described = true; }
			const int idx = outN - frameBase;
			ComPtr<IMFMediaBuffer> b; s->GetBufferByIndex(0, &b);
			ComPtr<IMFDXGIBuffer> dx;
			const bool cmp = doCompare || seeking;
			Diff* d = seeking ? &dSeek : &dY;
			if (readback && SUCCEEDED(b.As(&dx)))
			{
				ComPtr<ID3D11Texture2D> tex; UINT sub = 0; dx->GetResource(IID_PPV_ARGS(&tex)); dx->GetSubresourceIndex(&sub);
				ensureStaging(tex.Get(), delay + 1);
				const auto r0 = Clock::now();
				const int slot = outN % static_cast<int>(stg.size());
				if (delay > 0 && stgFrame[static_cast<size_t>(slot)] >= 0) mapAndCompare(stg[static_cast<size_t>(slot)].Get(), -1, nullptr, nullptr, false, true);
				c.ctx->CopySubresourceRegion(stg[static_cast<size_t>(slot)].Get(), 0, 0, 0, 0, tex.Get(), sub, nullptr);
				stgFrame[static_cast<size_t>(slot)] = outN;
				if (delay > 0) c.ctx->Flush(); // submit the copy now so the delayed Map finds it finished
				if (delay == 0) mapAndCompare(stg[static_cast<size_t>(slot)].Get(), idx, cmp ? d : nullptr, refNow, false, true);
				else
				{
					// map the frame queued `delay` outputs ago (it shares the slot we will overwrite next round)
					const int old = (outN + 1) % static_cast<int>(stg.size());
					if (stgFrame[static_cast<size_t>(old)] >= 0) { mapAndCompare(stg[static_cast<size_t>(old)].Get(), -1, nullptr, nullptr, false, false); stgFrame[static_cast<size_t>(old)] = -1; }
				}
				if (!(cmp && idx < static_cast<int>(refNow->y.size()))) tRead.v.push_back(Ms(r0, Clock::now()));
			}
			else if (readback)
			{
				const auto r0 = Clock::now();
				ComPtr<IMF2DBuffer> b2; BYTE* p = nullptr; LONG pitch = 0;
				if (SUCCEEDED(b.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch)))
				{
					if (cmp && idx < static_cast<int>(refNow->y.size())) { d->Add(p, pitch, refNow->y[static_cast<size_t>(idx)], refNow->w, refNow->h); ++compared; }
					b2->Unlock2D();
				}
				else
				{
					DWORD mx = 0, cur = 0;
					if (SUCCEEDED(b->Lock(&p, &mx, &cur)))
					{
						const int pt = m.stride > 0 ? m.stride : static_cast<int>(m.outW);
						if (cmp && idx < static_cast<int>(refNow->y.size())) { d->Add(p, pt, refNow->y[static_cast<size_t>(idx)], refNow->w, refNow->h); ++compared; }
						b->Unlock();
					}
				}
				if (!(cmp && idx < static_cast<int>(refNow->y.size()))) tRead.v.push_back(Ms(r0, Clock::now()));
			}
			++outN;
		};
		auto onOutA = [&](IMFSample* s)
		{
			if (!describedA) { DescribeSample(s, ma, "alpha"); describedA = true; }
			if (readback && doCompare && outA < static_cast<int>(c.ref.a.size()))
			{
				ComPtr<IMFMediaBuffer> b; s->GetBufferByIndex(0, &b);
				ComPtr<IMFDXGIBuffer> dx;
				if (SUCCEEDED(b.As(&dx)))
				{
					ComPtr<ID3D11Texture2D> tex; UINT sub = 0; dx->GetResource(IID_PPV_ARGS(&tex)); dx->GetSubresourceIndex(&sub);
					if (!stgA)
					{
						D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
						d.ArraySize = 1; d.MipLevels = 1; d.BindFlags = 0; d.MiscFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
						c.dev->CreateTexture2D(&d, nullptr, &stgA);
					}
					c.ctx->CopySubresourceRegion(stgA.Get(), 0, 0, 0, 0, tex.Get(), sub, nullptr);
					mapAndCompare(stgA.Get(), outA, &dA, &c.ref, true, true);
				}
				else
				{
					ComPtr<IMF2DBuffer> b2; BYTE* p = nullptr; LONG pitch = 0;
					DWORD mx = 0, cur = 0;
					if (SUCCEEDED(b.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch))) { dA.Add(p, pitch, c.ref.a[static_cast<size_t>(outA)], c.ref.w, c.ref.h); ++comparedA; b2->Unlock2D(); }
					else if (SUCCEEDED(b->Lock(&p, &mx, &cur)))
					{ dA.Add(p, ma.stride > 0 ? ma.stride : static_cast<int>(ma.outW), c.ref.a[static_cast<size_t>(outA)], c.ref.w, c.ref.h); ++comparedA; b->Unlock(); }
				}
			}
			++outA;
		};

		const int64_t dur = static_cast<int64_t>(1e7 / (v.frameRate > 0 ? v.frameRate : 30));
		const int n = std::min<int>(c.frames, static_cast<int>(c.pkts.size()));
		const double cpu0 = ProcessCpuSeconds();
		const auto w0 = Clock::now();
		bool ok = true;
		int alphaFed = 0;
		for (int i = 0; i < n && ok; ++i)
		{
			if (paced) { const auto due = w0 + std::chrono::microseconds(static_cast<int64_t>(i * dur / 10)); while (Clock::now() < due) Sleep(1); }
			const cev::PacketRef& p = c.dmx->VideoPacket(static_cast<uint32_t>(i));
			auto s = MakeSample(c.pkts[static_cast<size_t>(i)].data(), p.size, p.ptsNs / 100, dur, p.keyframe);
			const auto f0 = Clock::now();
			ok = m.Feed(s.Get(), onOut);
			tFeed.v.push_back(Ms(f0, Clock::now()));
			if (!ok) { std::printf("  %s: main feed %d: %s\n", name, i, m.err.c_str()); break; }
			if (alphaOk && !c.adds[static_cast<size_t>(i)].empty())
			{
				auto sa = MakeSample(c.adds[static_cast<size_t>(i)].data(), static_cast<uint32_t>(c.adds[static_cast<size_t>(i)].size()), p.ptsNs / 100, dur, p.keyframe);
				const auto a0 = Clock::now();
				if (!ma.Feed(sa.Get(), onOutA)) { std::printf("  %s: alpha feed %d: %s\n", name, i, ma.err.c_str()); alphaOk = false; }
				tFeedA.v.push_back(Ms(a0, Clock::now()));
				++alphaFed;
			}
		}
		if (ok) ok = m.Drain(onOut);
		if (alphaOk) ma.Drain(onOutA);
		// finish delayed maps
		for (size_t k = 0; k < stg.size(); ++k) if (stgFrame[k] >= 0) mapAndCompare(stg[k].Get(), -1, nullptr, nullptr, false, true);
		const double wall = Ms(w0, Clock::now());
		const double cpu = ProcessCpuSeconds() - cpu0;

		std::printf("  %s%s: fed %d packets, %d main outputs, %d alpha outputs (alpha packets fed %d), streamChanges main=%d; async=%d d3d11Aware=%d\n",
			name, paced ? " (paced at the clip frame rate)" : "", n, outN, outA, alphaFed, m.streamChanges, m.async ? 1 : 0, m.d3d11Aware ? 1 : 0);
		std::printf("    wall %.1f ms = %.3f ms/frame (%.1f fps); process CPU %.1f ms = %.1f%% of one core\n", wall, outN ? wall / outN : 0.0, outN ? outN * 1000.0 / wall : 0.0, cpu * 1000, wall > 0 ? cpu * 1000 * 100 / wall : 0.0);
		std::printf("    ProcessInput+ProcessOutput per packet: %s\n", tFeed.Str().c_str());
		if (!tFeedA.v.empty()) std::printf("    alpha MFT per packet: %s\n", tFeedA.Str().c_str());
		std::printf("    interval between outputs: %s\n", tInterval.Str().c_str());
		if (readback) std::printf("    readback per frame (%s): %s; maps that would have blocked: %d\n",
			dxMode ? (delay ? (delay == 1 ? "copy n, Map+copy-out n-1" : "copy n, Map+copy-out n-2") : "copy to staging + Map + copy-out, same frame") : "Lock2D/Lock", tRead.Str().c_str(), wouldBlock);
		if (doCompare) std::printf("    Y vs libvpx (%d frames): %s\n", compared, dY.Str().c_str());
		if (doCompare && withAlpha) std::printf("    alpha-stream Y vs libvpx alpha plane (%d frames): %s\n", comparedA, dA.Str().c_str());

		if (doSeek && ok)
		{
			// flush, refeed from a keyframe in the middle
			seeking = true; frameBase = outN; refNow = &c.seekRef; compared = 0;
			m.Flush();
			const int outBefore = outN;
			const int end = std::min<int>(static_cast<int>(c.pkts.size()), static_cast<int>(c.seekKey) + 8);
			for (int i = static_cast<int>(c.seekKey); i < end && ok; ++i)
			{
				const cev::PacketRef& p = c.dmx->VideoPacket(static_cast<uint32_t>(i));
				auto s = MakeSample(c.pkts[static_cast<size_t>(i)].data(), p.size, p.ptsNs / 100, dur, p.keyframe);
				ok = m.Feed(s.Get(), onOut);
			}
			if (ok) ok = m.Drain(onOut);
			for (size_t k = 0; k < stg.size(); ++k) if (stgFrame[k] >= 0) mapAndCompare(stg[k].Get(), -1, nullptr, nullptr, false, true);
			std::printf("    seek: flush + refeed from keyframe packet %u (pts %.3f s): %s, %d outputs; Y vs libvpx from the same keyframe (%d frames): %s\n",
				c.seekKey, c.dmx->VideoPts(c.seekKey), ok ? "ok" : m.err.c_str(), outN - outBefore, compared, dSeek.Str().c_str());
		}
		m.Stop(); ma.Stop();
		return ok;
	}
}

int main(int argc, char** argv)
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	timeBeginPeriod(1);
	MFStartup(MF_VERSION, MFSTARTUP_LITE);

	// adapters
	{
		ComPtr<IDXGIFactory1> f; CreateDXGIFactory1(IID_PPV_ARGS(&f));
		ComPtr<IDXGIAdapter1> a;
		for (UINT i = 0; f && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset())
		{
			DXGI_ADAPTER_DESC1 d; a->GetDesc1(&d);
			std::printf("adapter %u: \"%s\" vendor=0x%04X device=0x%04X vram=%llu MB flags=0x%X%s\n", i, Narrow(d.Description).c_str(), d.VendorId, d.DeviceId,
				static_cast<unsigned long long>(d.DedicatedVideoMemory >> 20), d.Flags, i == 0 ? " (default)" : "");
		}
	}
	const UINT32 kFlagsSyncAsync = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER; // 0x43
	const UINT32 kFlagsHw = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
	for (auto st : {std::make_pair("VP90", MFVideoFormat_VP90), std::make_pair("VP80", MFVideoFormat_VP80)})
	{
		auto sa = Enumerate(st.second, kFlagsSyncAsync);
		std::printf("MFTEnumEx(%s, flags 0x%X): %zu\n", st.first, kFlagsSyncAsync, sa.size());
		for (auto& a : sa) PrintActivate(a.Get());
		auto hw = Enumerate(st.second, kFlagsHw);
		std::printf("MFTEnumEx(%s, flags 0x%X hardware): %zu\n", st.first, kFlagsHw, hw.size());
		for (auto& a : hw) PrintActivate(a.Get());
		auto all = Enumerate(st.second, MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_UNTRUSTED_STOREMFT);
		std::printf("MFTEnumEx(%s, flags ALL|UNTRUSTED_STOREMFT 0x%X): %zu\n", st.first, MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_UNTRUSTED_STOREMFT, all.size());
		for (auto& a : all) PrintActivate(a.Get());
	}
	if (argc < 2 || std::strcmp(argv[1], "--enum") == 0) return 0;

	Ctx c;
	std::string mode = "both";
	for (int i = 2; i < argc; ++i)
	{
		if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) c.frames = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--compare") && i + 1 < argc) c.compare = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc) mode = argv[++i];
	}
	std::unique_ptr<cev::IByteSource> src;
	std::unique_ptr<cev::WebmDemuxer> dmx;
	if (cev::FileByteSource::Open(argv[1], true, src) != cev::Status::Ok || cev::WebmDemuxer::Open(std::move(src), dmx) != cev::Status::Ok || !dmx->HasVideo())
	{ std::printf("cannot open %s\n", argv[1]); return 1; }
	c.dmx = dmx.get();
	const cev::VideoStreamInfo& v = dmx->Video();
	c.subtype = v.codec == cev::VideoCodec::VP9 ? MFVideoFormat_VP90 : MFVideoFormat_VP80;
	std::printf("clip %s: %s %dx%d %.2f fps alpha=%d packets=%u\n", argv[1], v.codec == cev::VideoCodec::VP9 ? "VP9" : "VP8", v.width, v.height, v.frameRate, v.hasAlpha ? 1 : 0, dmx->VideoPacketCount());
	for (uint32_t i = 0; i < dmx->VideoPacketCount(); ++i)
	{
		const cev::PacketRef& p = dmx->VideoPacket(i);
		std::vector<uint8_t> b(p.size); dmx->ReadVideoPacket(i, b.data(), p.size); c.pkts.push_back(std::move(b));
		std::vector<uint8_t> a(p.additionalSize);
		if (p.additionalSize) dmx->ReadVideoAdditional(i, a.data(), p.additionalSize);
		c.adds.push_back(std::move(a));
	}
	c.seekKey = dmx->FindVideoKeyframe(dmx->DurationSeconds() * 0.5);
	if (!RefDecode(*dmx, 0, c.compare, c.ref) || !RefDecode(*dmx, c.seekKey, 8, c.seekRef)) { std::printf("libvpx reference decode failed\n"); return 1; }

	auto acts = Enumerate(c.subtype, kFlagsSyncAsync);
	if (acts.empty()) { std::printf("no decoder MFT for this codec\n"); return 0; }
	c.act = acts[0].Get();
	auto acts2 = Enumerate(c.subtype, kFlagsSyncAsync);
	c.actAlpha = acts2[0].Get();
	std::printf("using:\n"); PrintActivate(c.act);

	if (mode == "nodx" || mode == "both")
	{
		std::printf("== mode: no D3D manager\n");
		RunPass(c, false, 0, v.hasAlpha, false, false, "P1 timing");
		c.act->ShutdownObject(); c.actAlpha->ShutdownObject();
		RunPass(c, false, 0, v.hasAlpha, false, false, "P1b timing", true);
		c.act->ShutdownObject(); c.actAlpha->ShutdownObject();
		RunPass(c, false, 1, v.hasAlpha, true, true, "P2 readback+compare+seek");
		c.act->ShutdownObject(); c.actAlpha->ShutdownObject();
	}
	if (mode == "dx" || mode == "both")
	{
		std::printf("== mode: own D3D11 device (default adapter, VIDEO_SUPPORT, multithread protected) + DXGI device manager\n");
		D3D_FEATURE_LEVEL fl;
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
			D3D11_SDK_VERSION, &c.dev, &fl, &c.ctx);
		if (FAILED(hr)) { std::printf("  D3D11CreateDevice failed 0x%08lX\n", static_cast<unsigned long>(hr)); return 1; }
		ComPtr<ID3D11Multithread> mt; if (SUCCEEDED(c.ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
		UINT token = 0; MFCreateDXGIDeviceManager(&token, &c.mgr); c.mgr->ResetDevice(c.dev.Get(), token);
		std::printf("  feature level 0x%X\n", fl);
		RunPass(c, true, 0, v.hasAlpha, false, false, "P1 timing");
		c.act->ShutdownObject(); c.actAlpha->ShutdownObject();
		RunPass(c, true, 0, v.hasAlpha, false, false, "P1b timing", true);
		c.act->ShutdownObject(); c.actAlpha->ShutdownObject();
		RunPass(c, true, 1, v.hasAlpha, true, true, "P2 sync readback+compare+seek");
		c.act->ShutdownObject(); c.actAlpha->ShutdownObject();
		RunPass(c, true, 1, false, false, false, "P2b sync readback", true);
		c.act->ShutdownObject(); c.actAlpha->ShutdownObject();
		RunPass(c, true, 2, false, false, false, "P3 async readback delay 1", true);
		c.act->ShutdownObject(); c.actAlpha->ShutdownObject();
		RunPass(c, true, 3, false, false, false, "P4 async readback delay 2", true);
		c.act->ShutdownObject(); c.actAlpha->ShutdownObject();
		std::printf("  P5: request MF_SA_D3D11_BINDFLAGS = DECODER|SHADER_RESOURCE on the output stream\n");
		c.frames = std::min(c.frames, 30);
		RunPass(c, true, 0, false, false, false, "P5 bind flags", false, D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE);
		c.act->ShutdownObject(); c.actAlpha->ShutdownObject();
	}
	MFShutdown();
	CoUninitialize();
	return 0;
}
