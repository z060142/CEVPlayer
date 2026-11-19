// Media Foundation hardware decode backend (M9, docs/tasks/M9.md rulings R1 / R2; docs/design/M6-ui-source.md §6, §10).
//
// One dedicated thread per backend ("CEV MF Decode") owns everything COM / D3D: CoInitializeEx(MTA), the process-wide
// counted MFStartup / MFShutdown, a private D3D11 device on the first DXGI adapter (VIDEO_SUPPORT, multithread
// protected), the DXGI device manager, the VP9 decoder MFT(s) (colour and, for alpha, a second instance), the staging
// textures and the read-back. Pool threads and the consumer thread only talk to it through the queues below.
//
// Behaviour of this MFT as observed by the stage-0 probe (tools/probes/mf_decode_probe, 2026-11-19, RTX 4070 Ti):
//   * VP9VideoExtensionDecoder, synchronous, MF_SA_D3D11_AWARE, no attribute that says "hardware": hardware decode =
//     SetInputType succeeds after MFT_MESSAGE_SET_D3D_MANAGER and the output sample is an IMFDXGIBuffer.
//   * One MF_E_TRANSFORM_STREAM_CHANGE before the first output; NV12 texture of an aligned height (1080 -> 1088), the
//     UV plane starts at RowPitch x texture height of the mapped staging texture; picture = aperture at (0, 0).
//   * Read-back: copy to staging, Flush, Map two frames later (synchronous Map costs 2.7 / 7.9 ms with 25 ms p99).
//   * VP8 has no hardware decode (MF_E_UNSUPPORTED_D3D_TYPE); this backend only accepts VP9.
#include "CEVCore/VideoDecoderBackend.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <emmintrin.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include "CEVCore/VideoDecoder.h"

using Microsoft::WRL::ComPtr;

namespace cev
{
	namespace
	{
		using Steady = std::chrono::steady_clock;

		inline float Ms(Steady::time_point a, Steady::time_point b)
		{
			return std::chrono::duration<float, std::milli>(b - a).count();
		}

		// ---------------------------------------------------------------------------------------------------- globals
		std::mutex g_startupMutex;
		uint32_t g_startups = 0;                         // MFStartup calls not yet paired by MFShutdown (under the mutex)
		std::atomic<uint32_t> g_shutdownTimeouts{0};
		std::atomic<uint8_t> g_fault{0};
		std::atomic<uint32_t> g_faultParam{0};

		bool CountedMfStartup()
		{
			std::lock_guard<std::mutex> lock(g_startupMutex);
			if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
				return false; // MFStartup itself is reference counted by Media Foundation; we pair every success
			++g_startups;
			return true;
		}

		void CountedMfShutdown()
		{
			std::lock_guard<std::mutex> lock(g_startupMutex);
			MFShutdown();
			--g_startups;
		}

		constexpr uint32_t kInCap = 4;        // packets queued for the backend thread
		constexpr uint32_t kOutCap = 8;       // completed tickets not yet received
		constexpr uint32_t kMaxInFlight = 3;  // read-back delay (2) + 1
		constexpr uint32_t kWorkCap = 16;     // packets fed to the MFT whose output did not come yet
		constexpr uint32_t kStaging = 5;      // read-back slots (staging textures per stream)
		constexpr uint32_t kInSamples = 8;    // reused input samples per MFT
		constexpr uint32_t kReadbackDelay = 2;
		constexpr int64_t kSeqTime = 10000;   // sample time units (100 ns) per submitted packet: output -> ticket
		constexpr int kOpenTimeoutMs = 5000;
		constexpr int kFlushTimeoutMs = 2000;
		constexpr int kJoinTimeoutMs = 2000;
		constexpr int kIdleMapMs = 4;         // no new input for this long: map what is pending

		void Copy(char* dst, size_t size, const char* src)
		{
			if (dst == nullptr || size == 0)
				return;
			std::snprintf(dst, size, "%s", src != nullptr ? src : "");
		}

		void Narrow(const wchar_t* w, char* out, size_t size)
		{
			if (size == 0)
				return;
			out[0] = 0;
			if (w != nullptr)
				WideCharToMultiByte(CP_UTF8, 0, w, -1, out, static_cast<int>(size), nullptr, nullptr);
			out[size - 1] = 0;
		}

		// Splits `n` interleaved UV pairs into U and V.
		void Deinterleave(const uint8_t* uv, uint8_t* u, uint8_t* v, int n)
		{
			int i = 0;
			const __m128i mask = _mm_set1_epi16(0x00FF);
			for (; i + 16 <= n; i += 16)
			{
				const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i*>(uv + 2 * i));
				const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i*>(uv + 2 * i + 16));
				const __m128i ue = _mm_packus_epi16(_mm_and_si128(a, mask), _mm_and_si128(b, mask));
				const __m128i ve = _mm_packus_epi16(_mm_srli_epi16(a, 8), _mm_srli_epi16(b, 8));
				_mm_storeu_si128(reinterpret_cast<__m128i*>(u + i), ue);
				_mm_storeu_si128(reinterpret_cast<__m128i*>(v + i), ve);
			}
			for (; i < n; ++i)
			{
				u[i] = uv[2 * i];
				v[i] = uv[2 * i + 1];
			}
		}

		struct InEntry
		{
			std::unique_ptr<uint8_t[]> color;
			std::unique_ptr<uint8_t[]> alpha;
			uint32_t colorSize = 0;
			uint32_t alphaSize = 0;
			bool keyframe = false;
			DecodeTicket ticket;
		};

		enum class OutState : uint8_t { Waiting, Copied, None };

		// A packet fed to the MFT(s), waiting for its output / read-back (backend thread only).
		struct Work
		{
			DecodeTicket ticket;
			int64_t seq = 0;
			bool alphaFed = false;      // alpha packet went to the alpha MFT
			bool alphaWritten = false;  // A plane already written (libvpx alpha, or opaque)
			OutState color = OutState::Waiting;
			OutState alpha = OutState::Waiting;
			int rb = -1;                // read-back slot
			float decodeMs = 0.0f;
			float stagingMs = 0.0f;
		};

		// One decoder MFT (synchronous; the observed VP9VideoExtensionDecoder is).
		struct Mft
		{
			ComPtr<IMFActivate> activate;
			ComPtr<IMFTransform> t;
			ComPtr<IMFSample> inSample[kInSamples];
			ComPtr<IMFMediaBuffer> inBuffer[kInSamples];
			uint32_t inNext = 0;
			UINT32 outW = 0;
			UINT32 outH = 0;
			MFVideoArea aperture = {};
			int streamChanges = 0;

			HRESULT SetNv12()
			{
				for (DWORD i = 0;; ++i)
				{
					ComPtr<IMFMediaType> mt;
					HRESULT hr = t->GetOutputAvailableType(0, i, &mt);
					if (FAILED(hr))
						return hr;
					GUID st = GUID_NULL;
					mt->GetGUID(MF_MT_SUBTYPE, &st);
					if (st != MFVideoFormat_NV12)
						continue;
					hr = t->SetOutputType(0, mt.Get(), 0);
					if (FAILED(hr))
						return hr;
					ComPtr<IMFMediaType> cur;
					if (SUCCEEDED(t->GetOutputCurrentType(0, &cur)))
					{
						MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &outW, &outH);
						UINT32 apSize = 0;
						aperture = MFVideoArea();
						if (FAILED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&aperture), sizeof(aperture), &apSize)))
						{
							aperture.Area.cx = static_cast<LONG>(outW);
							aperture.Area.cy = static_cast<LONG>(outH);
						}
					}
					return S_OK;
				}
			}

			void Release()
			{
				if (t)
				{
					t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
					t->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
				}
				for (uint32_t i = 0; i < kInSamples; ++i)
				{
					inSample[i].Reset();
					inBuffer[i].Reset();
				}
				t.Reset();
				if (activate)
					activate->ShutdownObject();
				activate.Reset();
			}
		};

		// ------------------------------------------------------------------------------------------- shared state
		// Owned jointly by the backend object and its thread (the thread keeps it alive when a bounded join timed out).
		struct Shared
		{
			MediaFoundationConfig cfg;
			FrameFormat format;

			std::mutex m;
			std::condition_variable cvThread; // backend thread waits
			std::condition_variable cvCaller; // creator / Flush / destructor wait

			// ---- under m
			InEntry in[kInCap];
			uint32_t inHead = 0;
			uint32_t inCount = 0;
			bool inBusy = false;              // the head entry is being processed (not popped yet)
			DecodeTicket out[kOutCap];
			Status outResult[kOutCap] = {};
			uint32_t outHead = 0;
			uint32_t outCount = 0;
			int inFlight[kMaxInFlight + kInCap];
			uint32_t inFlightCount = 0;       // slot tickets submitted, not received
			uint64_t flushGen = 0;
			uint64_t flushAck = 0;
			bool drainReq = false;
			bool quit = false;
			bool exited = false;
			bool opened = false;
			bool openOk = false;
			bool failed = false;              // unusable: Submit / tickets -> BackendFailed
			bool lastValid = false;
			DecodedImageInfo last;
			DecodeBackendInfo info;
			std::atomic<bool> noWrites{false}; // the owner gave up on the thread: never touch slot memory again

			// ---- backend thread only
			bool comInit = false;
			bool mfStarted = false;
			ComPtr<ID3D11Device> device;
			ComPtr<ID3D11DeviceContext> ctx;
			ComPtr<IMFDXGIDeviceManager> manager;
			Mft color;
			Mft alpha;
			bool alphaMft = false;
			std::unique_ptr<VideoDecoder> alphaVpx;
			ComPtr<ID3D11Texture2D> stgColor[kStaging];
			ComPtr<ID3D11Texture2D> stgAlpha[kStaging];
			UINT stgW = 0, stgH = 0, stgAW = 0, stgAH = 0;
			bool rbUsed[kStaging] = {};
			Work work[kWorkCap];
			uint32_t workHead = 0;
			uint32_t workCount = 0;
			int64_t seq = 1;
			uint64_t completedPictures = 0;
			bool firstOutputChecked = false;

			std::atomic<bool> wakeOn{true};

			void Wake()
			{
				if (wakeOn.load(std::memory_order_acquire) && cfg.wake != nullptr)
					cfg.wake(cfg.wakeContext);
			}

			// ------------------------------------------------------------------------- failure handling (thread)
			void FailLocked(const char* reason)
			{
				if (!failed)
				{
					failed = true;
					Copy(info.reason, sizeof(info.reason), reason);
				}
			}

			void Fail(const char* what, HRESULT hr)
			{
				char b[192];
				const bool removed = hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
					(device && FAILED(device->GetDeviceRemovedReason()));
				if (removed)
				{
					const HRESULT why = device ? device->GetDeviceRemovedReason() : hr;
					std::snprintf(b, sizeof(b), "Media Foundation: device removed during %s (hr 0x%08lX)", what, static_cast<unsigned long>(why));
				}
				else
					std::snprintf(b, sizeof(b), "Media Foundation: %s failed (hr 0x%08lX)", what, static_cast<unsigned long>(hr));
				std::lock_guard<std::mutex> lock(m);
				FailLocked(b);
			}

			bool IsFailed()
			{
				std::lock_guard<std::mutex> lock(m);
				return failed;
			}

			// Completes a ticket (thread -> caller).
			void Complete(const DecodeTicket& t, Status result)
			{
				if (t.slotIndex < 0)
					return;
				{
					std::lock_guard<std::mutex> lock(m);
					if (outCount < kOutCap)
					{
						const uint32_t i = (outHead + outCount) % kOutCap;
						out[i] = t;
						outResult[i] = result;
						++outCount;
					}
				}
				Wake();
			}

			// After a failure: every pending ticket comes back as BackendFailed, internal state is dropped.
			void FailAllWork()
			{
				for (uint32_t k = 0; k < workCount; ++k)
				{
					Work& w = work[(workHead + k) % kWorkCap];
					Complete(w.ticket, Status::BackendFailed);
				}
				workCount = 0;
				for (bool& u : rbUsed)
					u = false;
			}

			// --------------------------------------------------------------------------------------- open (thread)
			bool ActivateMft(Mft& mft, bool isAlpha, char* reason, size_t reasonSize)
			{
				const MediaFoundationTestFault fault = static_cast<MediaFoundationTestFault>(g_fault.load());
				MFT_REGISTER_TYPE_INFO inType = {MFMediaType_Video, MFVideoFormat_VP90};
				IMFActivate** acts = nullptr;
				UINT32 n = 0;
				const UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
				HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &inType, nullptr, &acts, &n);
				if (FAILED(hr))
					n = 0;
				if (fault == MediaFoundationTestFault::NoDecoderMft)
				{
					for (UINT32 i = 0; i < n; ++i)
						acts[i]->Release();
					n = 0;
				}
				if (n == 0)
				{
					if (acts != nullptr)
						CoTaskMemFree(acts);
					std::snprintf(reason, reasonSize, "no VP9 decoder MFT (VP9 Video Extensions not installed)%s",
						fault == MediaFoundationTestFault::NoDecoderMft ? " [test switch]" : "");
					return false;
				}
				mft.activate = acts[0];
				for (UINT32 i = 0; i < n; ++i)
					acts[i]->Release();
				CoTaskMemFree(acts);

				hr = mft.activate->ActivateObject(IID_PPV_ARGS(&mft.t));
				if (FAILED(hr))
				{
					std::snprintf(reason, reasonSize, "ActivateObject failed (hr 0x%08lX)", static_cast<unsigned long>(hr));
					return false;
				}
				ComPtr<IMFAttributes> at;
				UINT32 isAsync = 0, aware = 0;
				if (SUCCEEDED(mft.t->GetAttributes(&at)) && at)
				{
					at->GetUINT32(MF_TRANSFORM_ASYNC, &isAsync);
					at->GetUINT32(MF_SA_D3D11_AWARE, &aware);
				}
				if (isAsync != 0)
				{
					Copy(reason, reasonSize, "the VP9 decoder MFT is asynchronous (not supported by this backend)");
					return false;
				}
				if (aware == 0)
				{
					Copy(reason, reasonSize, "the VP9 decoder MFT is not D3D11 aware");
					return false;
				}
				hr = mft.t->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(manager.Get()));
				if (FAILED(hr))
				{
					std::snprintf(reason, reasonSize, "MFT_MESSAGE_SET_D3D_MANAGER failed (hr 0x%08lX)", static_cast<unsigned long>(hr));
					return false;
				}
				ComPtr<IMFMediaType> type;
				hr = MFCreateMediaType(&type);
				if (SUCCEEDED(hr))
				{
					const VideoStreamInfo& v = cfg.stream;
					type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
					type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_VP90);
					MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(v.width), static_cast<UINT32>(v.height));
					const double fps = v.frameRate > 0.0 ? v.frameRate : 30.0;
					MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps * 1000.0 + 0.5), 1000);
					type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
					hr = mft.t->SetInputType(0, type.Get(), 0);
				}
				if (fault == MediaFoundationTestFault::SetInputTypeFails)
					hr = MF_E_UNSUPPORTED_D3D_TYPE;
				if (FAILED(hr))
				{
					std::snprintf(reason, reasonSize, "%sSetInputType with the D3D11 manager failed (hr 0x%08lX): no hardware VP9 decode on this GPU%s",
						isAlpha ? "alpha MFT: " : "", static_cast<unsigned long>(hr),
						fault == MediaFoundationTestFault::SetInputTypeFails ? " [test switch]" : "");
					return false;
				}
				hr = mft.SetNv12();
				if (FAILED(hr))
				{
					std::snprintf(reason, reasonSize, "no NV12 output type (hr 0x%08lX)", static_cast<unsigned long>(hr));
					return false;
				}
				MFT_OUTPUT_STREAM_INFO si = {};
				mft.t->GetOutputStreamInfo(0, &si);
				if ((si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) == 0)
				{
					Copy(reason, reasonSize, "the MFT does not provide its output samples (no D3D11 output)");
					return false;
				}
				// Reusable input samples (no allocation per packet).
				const uint32_t cap = std::max<uint32_t>(isAlpha ? cfg.maxAdditionalSize : cfg.maxPacketSize, 1u);
				for (uint32_t i = 0; i < kInSamples; ++i)
				{
					hr = MFCreateMemoryBuffer(cap, &mft.inBuffer[i]);
					if (SUCCEEDED(hr))
						hr = MFCreateSample(&mft.inSample[i]);
					if (SUCCEEDED(hr))
						hr = mft.inSample[i]->AddBuffer(mft.inBuffer[i].Get());
					if (FAILED(hr))
					{
						std::snprintf(reason, reasonSize, "input sample allocation failed (hr 0x%08lX)", static_cast<unsigned long>(hr));
						return false;
					}
				}
				mft.t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
				mft.t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
				return true;
			}

			bool Open(char* reason, size_t reasonSize)
			{
				const VideoStreamInfo& v = cfg.stream;
				if (v.codec != VideoCodec::VP9)
				{
					Copy(reason, reasonSize, "Media Foundation: only VP9 is decoded in hardware (VP8 uses libvpx)");
					return false;
				}
				if (!format.Packable())
				{
					Copy(reason, reasonSize, "Media Foundation: only 4:2:0 with an even width (packed layout) is supported");
					return false;
				}
				if (cfg.maxPacketSize == 0)
				{
					Copy(reason, reasonSize, "Media Foundation: no packet size");
					return false;
				}
				HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				if (FAILED(hr))
				{
					std::snprintf(reason, reasonSize, "CoInitializeEx failed (hr 0x%08lX)", static_cast<unsigned long>(hr));
					return false;
				}
				comInit = true;
				if (!CountedMfStartup())
				{
					Copy(reason, reasonSize, "MFStartup failed");
					return false;
				}
				mfStarted = true;

				// Private D3D11 device on the first DXGI adapter (= the default adapter the engine uses).
				ComPtr<IDXGIFactory1> factory;
				ComPtr<IDXGIAdapter1> adapter;
				hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
				if (SUCCEEDED(hr))
					hr = factory->EnumAdapters1(0, &adapter);
				if (FAILED(hr))
				{
					std::snprintf(reason, reasonSize, "no DXGI adapter (hr 0x%08lX)", static_cast<unsigned long>(hr));
					return false;
				}
				DXGI_ADAPTER_DESC1 desc = {};
				adapter->GetDesc1(&desc);
				{
					std::lock_guard<std::mutex> lock(m);
					Narrow(desc.Description, info.adapter, sizeof(info.adapter));
				}
				D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
				hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
					D3D11_SDK_VERSION, &device, &fl, &ctx);
				if (FAILED(hr))
				{
					std::snprintf(reason, reasonSize, "D3D11CreateDevice with VIDEO_SUPPORT failed (hr 0x%08lX)", static_cast<unsigned long>(hr));
					return false;
				}
				ComPtr<ID3D11Multithread> mt;
				if (SUCCEEDED(ctx.As(&mt)))
					mt->SetMultithreadProtected(TRUE);
				UINT token = 0;
				hr = MFCreateDXGIDeviceManager(&token, &manager);
				if (SUCCEEDED(hr))
					hr = manager->ResetDevice(device.Get(), token);
				if (FAILED(hr))
				{
					std::snprintf(reason, reasonSize, "DXGI device manager failed (hr 0x%08lX)", static_cast<unsigned long>(hr));
					return false;
				}

				if (!ActivateMft(color, false, reason, reasonSize))
					return false;

				bool alphaHw = false;
				if (v.hasAlpha)
				{
					char alphaWhy[192] = {};
					alphaMft = ActivateMft(alpha, true, alphaWhy, sizeof(alphaWhy));
					if (!alphaMft)
					{
						alpha.Release();
						// The alpha stream alone falls back to libvpx (the colour stream stays on Media Foundation).
						VideoStreamInfo av = v;
						av.hasAlpha = false;
						if (Failed(VideoDecoder::Create(av, std::max(1u, cfg.alphaThreads), alphaVpx)))
						{
							Copy(reason, reasonSize, "alpha: neither a second MFT nor libvpx could be created");
							return false;
						}
					}
					alphaHw = alphaMft;
				}
				std::lock_guard<std::mutex> lock(m);
				info.kind = DecodeBackendKind::MediaFoundation;
				info.hardware = true;
				info.alphaHardware = alphaHw;
				return true;
			}

			void Close()
			{
				for (uint32_t i = 0; i < kStaging; ++i)
				{
					stgColor[i].Reset();
					stgAlpha[i].Reset();
				}
				color.Release();
				alpha.Release();
				alphaVpx.reset();
				if (ctx)
				{
					ctx->ClearState();
					ctx->Flush();
				}
				manager.Reset();
				ctx.Reset();
				device.Reset();
				if (mfStarted)
					CountedMfShutdown();
				mfStarted = false;
				if (comInit)
					CoUninitialize();
				comInit = false;
			}

			// ------------------------------------------------------------------------------------- decode (thread)
			Work* FindWork(int64_t s)
			{
				for (uint32_t k = 0; k < workCount; ++k)
				{
					Work& w = work[(workHead + k) % kWorkCap];
					if (w.seq == s)
						return &w;
				}
				return nullptr;
			}

			int AcquireReadbackSlot()
			{
				for (uint32_t i = 0; i < kStaging; ++i)
					if (!rbUsed[i])
					{
						rbUsed[i] = true;
						return static_cast<int>(i);
					}
				return -1;
			}

			bool EnsureStaging(ID3D11Texture2D* src, bool isAlpha)
			{
				D3D11_TEXTURE2D_DESC d;
				src->GetDesc(&d);
				UINT& w = isAlpha ? stgAW : stgW;
				UINT& h = isAlpha ? stgAH : stgH;
				ComPtr<ID3D11Texture2D>* arr = isAlpha ? stgAlpha : stgColor;
				if (w == d.Width && h == d.Height && arr[0])
					return true;
				d.ArraySize = 1;
				d.MipLevels = 1;
				d.BindFlags = 0;
				d.MiscFlags = 0;
				d.Usage = D3D11_USAGE_STAGING;
				d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				for (uint32_t i = 0; i < kStaging; ++i)
				{
					arr[i].Reset();
					const HRESULT hr = device->CreateTexture2D(&d, nullptr, &arr[i]);
					if (FAILED(hr))
					{
						Fail("CreateTexture2D (staging)", hr);
						return false;
					}
				}
				w = d.Width;
				h = d.Height;
				return true;
			}

			// Copies the decoded texture of `sample` into the work's read-back slot.
			bool CopyOutput(IMFSample* sample, Work& w, bool isAlpha)
			{
				ComPtr<IMFMediaBuffer> buf;
				ComPtr<IMFDXGIBuffer> dx;
				if (FAILED(sample->GetBufferByIndex(0, &buf)) || FAILED(buf.As(&dx)))
				{
					std::lock_guard<std::mutex> lock(m);
					FailLocked("Media Foundation: output is not a D3D11 texture (software decode)");
					return false;
				}
				ComPtr<ID3D11Texture2D> tex;
				UINT sub = 0;
				HRESULT hr = dx->GetResource(IID_PPV_ARGS(&tex));
				if (SUCCEEDED(hr))
					hr = dx->GetSubresourceIndex(&sub);
				if (FAILED(hr))
				{
					Fail("IMFDXGIBuffer::GetResource", hr);
					return false;
				}
				if (!EnsureStaging(tex.Get(), isAlpha))
					return false;
				if (w.rb < 0)
				{
					w.rb = AcquireReadbackSlot();
					if (w.rb < 0)
					{
						std::lock_guard<std::mutex> lock(m);
						FailLocked("Media Foundation: read-back slots exhausted (internal)");
						return false;
					}
				}
				const Steady::time_point t0 = Steady::now();
				ctx->CopySubresourceRegion((isAlpha ? stgAlpha : stgColor)[w.rb].Get(), 0, 0, 0, 0, tex.Get(), sub, nullptr);
				ctx->Flush(); // submit the copy now so the delayed Map finds it finished
				w.stagingMs += Ms(t0, Steady::now());
				return true;
			}

			void OnOutput(IMFSample* sample, bool isAlpha)
			{
				LONGLONG time = 0;
				sample->GetSampleTime(&time);
				const int64_t s = time / kSeqTime;
				// Earlier packets without an output of this stream: no picture from them.
				for (uint32_t k = 0; k < workCount; ++k)
				{
					Work& w = work[(workHead + k) % kWorkCap];
					if (w.seq >= s)
						break;
					OutState& st = isAlpha ? w.alpha : w.color;
					if (st == OutState::Waiting)
						st = OutState::None;
				}
				Work* w = FindWork(s);
				if (w == nullptr)
					return; // flushed meanwhile
				OutState& st = isAlpha ? w->alpha : w->color;
				if (st != OutState::Waiting)
					return;
				if (!firstOutputChecked && !isAlpha)
				{
					// Hardware confirmed only by a D3D11 texture output (§10).
					firstOutputChecked = true;
					ComPtr<IMFMediaBuffer> buf;
					ComPtr<IMFDXGIBuffer> dx;
					if (FAILED(sample->GetBufferByIndex(0, &buf)) || FAILED(buf.As(&dx)))
					{
						std::lock_guard<std::mutex> lock(m);
						FailLocked("Media Foundation: output is not a D3D11 texture (software decode)");
						return;
					}
				}
				if (w->ticket.slotIndex < 0 || noWrites.load(std::memory_order_acquire))
				{
					st = OutState::None; // skip decode: no read-back
					return;
				}
				if (!isAlpha)
				{
					// The picture must be the ring's picture (aperture at 0, 0 of the format's size).
					const MFVideoArea& ap = color.aperture;
					std::lock_guard<std::mutex> lock(m);
					last.width = static_cast<int>(ap.Area.cx);
					last.height = static_cast<int>(ap.Area.cy);
					last.chromaShiftX = 1;
					last.chromaShiftY = 1;
					last.hasAlpha = format.hasAlpha;
					lastValid = true;
				}
				if (CopyOutput(sample, *w, isAlpha))
					st = OutState::Copied;
			}

			// Returns false on a failure (state already set).
			bool PullAll(Mft& mft, bool isAlpha)
			{
				for (int i = 0; i < 64; ++i)
				{
					MFT_OUTPUT_DATA_BUFFER db = {};
					DWORD status = 0;
					const HRESULT hr = mft.t->ProcessOutput(0, 1, &db, &status);
					if (db.pEvents != nullptr)
						db.pEvents->Release();
					if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
					{
						if (db.pSample != nullptr)
							db.pSample->Release();
						return true;
					}
					if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
					{
						if (db.pSample != nullptr)
							db.pSample->Release();
						++mft.streamChanges;
						const HRESULT h2 = mft.SetNv12();
						if (FAILED(h2))
						{
							Fail("SetOutputType after STREAM_CHANGE", h2);
							return false;
						}
						continue;
					}
					if (FAILED(hr))
					{
						if (db.pSample != nullptr)
							db.pSample->Release();
						Fail("ProcessOutput", hr);
						return false;
					}
					if (db.pSample != nullptr)
					{
						OnOutput(db.pSample, isAlpha);
						db.pSample->Release();
						if (IsFailed())
							return false;
					}
				}
				return true;
			}

			bool Feed(Mft& mft, const uint8_t* data, uint32_t size, bool keyframe, int64_t s, bool isAlpha)
			{
				const uint32_t k = mft.inNext;
				mft.inNext = (mft.inNext + 1) % kInSamples;
				IMFSample* sample = mft.inSample[k].Get();
				IMFMediaBuffer* buf = mft.inBuffer[k].Get();
				BYTE* p = nullptr;
				DWORD maxLen = 0;
				HRESULT hr = buf->Lock(&p, &maxLen, nullptr);
				if (FAILED(hr) || maxLen < size)
				{
					if (SUCCEEDED(hr))
						buf->Unlock();
					Fail("input buffer Lock", FAILED(hr) ? hr : E_OUTOFMEMORY);
					return false;
				}
				std::memcpy(p, data, size);
				buf->Unlock();
				buf->SetCurrentLength(size);
				sample->SetSampleTime(s * kSeqTime);
				sample->SetSampleDuration(kSeqTime);
				sample->SetUINT32(MFSampleExtension_CleanPoint, keyframe ? TRUE : FALSE);
				for (int tries = 0; tries < 64; ++tries)
				{
					hr = mft.t->ProcessInput(0, sample, 0);
					if (hr == MF_E_NOTACCEPTING)
					{
						if (!PullAll(mft, isAlpha))
							return false;
						continue;
					}
					if (FAILED(hr))
					{
						Fail("ProcessInput", hr);
						return false;
					}
					return PullAll(mft, isAlpha);
				}
				Fail("ProcessInput (keeps returning MF_E_NOTACCEPTING)", MF_E_NOTACCEPTING);
				return false;
			}

			void WriteOpaque(FrameRing::Slot& slot)
			{
				for (int r = 0; r < format.height; ++r)
					std::memset(slot.plane[3] + static_cast<ptrdiff_t>(r) * slot.stride[3], 255, static_cast<size_t>(format.width));
			}

			// Maps the read-back slot of the head work and writes the slot planes, completes the ticket.
			void MapHead()
			{
				Work& w = work[workHead];
				DecodeTicket t = w.ticket;
				t.decodeMs = w.decodeMs;
				t.readbackMs = w.stagingMs;
				Status result = Status::Ok;
				const Steady::time_point t0 = Steady::now();
				D3D11_MAPPED_SUBRESOURCE ms = {};
				HRESULT hr = ctx->Map(stgColor[w.rb].Get(), 0, D3D11_MAP_READ, 0, &ms);
				const Steady::time_point t1 = Steady::now();
				float copyMs = 0.0f;
				if (FAILED(hr))
				{
					Fail("Map (staging)", hr);
					result = Status::BackendFailed;
				}
				else
				{
					FrameRing::Slot& slot = *t.slot;
					const int width = format.width;
					const int height = format.height;
					const int cw = format.ChromaWidth();
					const int ch = format.ChromaHeight();
					const uint8_t* src = static_cast<const uint8_t*>(ms.pData);
					const MFVideoArea ap = color.aperture;
					if (ap.Area.cx != width || ap.Area.cy != height || ap.OffsetX.value != 0 || ap.OffsetY.value != 0 ||
						static_cast<int>(stgH) < height || static_cast<int>(stgW) < width)
						result = Status::InvalidData; // stream change (LastFrameInfo says the size)
					else if (!noWrites.load(std::memory_order_acquire))
					{
						for (int r = 0; r < height; ++r)
							std::memcpy(slot.plane[0] + static_cast<ptrdiff_t>(r) * slot.stride[0], src + static_cast<size_t>(r) * ms.RowPitch,
								static_cast<size_t>(width));
						const uint8_t* uv = src + static_cast<size_t>(ms.RowPitch) * stgH; // UV start = RowPitch x texture height
						for (int r = 0; r < ch; ++r)
							Deinterleave(uv + static_cast<size_t>(r) * ms.RowPitch, slot.plane[1] + static_cast<ptrdiff_t>(r) * slot.stride[1],
								slot.plane[2] + static_cast<ptrdiff_t>(r) * slot.stride[2], cw);
					}
					ctx->Unmap(stgColor[w.rb].Get(), 0);
					copyMs = Ms(t1, Steady::now());
				}
				if (result == Status::Ok && format.hasAlpha && slot_has_alpha(t) && !w.alphaWritten)
				{
					if (w.alpha == OutState::Copied)
					{
						const Steady::time_point a0 = Steady::now();
						hr = ctx->Map(stgAlpha[w.rb].Get(), 0, D3D11_MAP_READ, 0, &ms);
						const Steady::time_point a1 = Steady::now();
						t.readbackMs += Ms(a0, a1);
						if (FAILED(hr))
						{
							Fail("Map (alpha staging)", hr);
							result = Status::BackendFailed;
						}
						else
						{
							if (!noWrites.load(std::memory_order_acquire))
							{
								const uint8_t* src = static_cast<const uint8_t*>(ms.pData);
								for (int r = 0; r < format.height; ++r)
									std::memcpy(t.slot->plane[3] + static_cast<ptrdiff_t>(r) * t.slot->stride[3], src + static_cast<size_t>(r) * ms.RowPitch,
										static_cast<size_t>(format.width));
							}
							ctx->Unmap(stgAlpha[w.rb].Get(), 0);
							copyMs += Ms(a1, Steady::now());
						}
					}
					else if (!noWrites.load(std::memory_order_acquire))
						WriteOpaque(*t.slot); // no alpha picture for this frame: opaque, as libvpx
				}
				t.readbackMs += Ms(t0, t1);
				t.copyMs = copyMs;
				rbUsed[w.rb] = false;
				workHead = (workHead + 1) % kWorkCap;
				--workCount;
				if (result == Status::Ok)
				{
					++completedPictures;
					const uint32_t lostAfter = g_faultParam.load();
					if (static_cast<MediaFoundationTestFault>(g_fault.load()) == MediaFoundationTestFault::DeviceLostAfter && completedPictures >= lostAfter)
					{
						std::lock_guard<std::mutex> lock(m);
						FailLocked("Media Foundation: device removed (simulated by the test switch)");
					}
				}
				Complete(t, result);
			}

			static bool slot_has_alpha(const DecodeTicket& t) { return t.slot != nullptr && t.slot->plane[3] != nullptr; }

			bool HeadResolved() const
			{
				if (workCount == 0)
					return false;
				const Work& w = work[workHead];
				const bool alphaPending = alphaMft && w.alphaFed && w.alpha == OutState::Waiting;
				return w.color != OutState::Waiting && !alphaPending;
			}

			// Completes resolved works at the head: no picture -> Cancelled; read-back when `force` or more than the
			// read-back delay are pending.
			void Advance(bool force)
			{
				for (;;)
				{
					if (!HeadResolved())
						return;
					Work& w = work[workHead];
					if (w.color != OutState::Copied || w.ticket.slotIndex < 0)
					{
						if (w.rb >= 0)
							rbUsed[w.rb] = false;
						const DecodeTicket t = w.ticket;
						workHead = (workHead + 1) % kWorkCap;
						--workCount;
						Complete(t, Status::Cancelled);
						continue;
					}
					uint32_t pending = 0;
					for (uint32_t k = 0; k < workCount; ++k)
					{
						const Work& x = work[(workHead + k) % kWorkCap];
						if (x.color == OutState::Copied && x.ticket.slotIndex >= 0)
							++pending;
					}
					if (!force && pending <= kReadbackDelay)
						return;
					MapHead();
					if (IsFailed())
						return;
				}
			}

			uint32_t PendingReadbacks() const
			{
				uint32_t n = 0;
				for (uint32_t k = 0; k < workCount; ++k)
				{
					const Work& x = work[(workHead + k) % kWorkCap];
					n += x.color == OutState::Copied ? 1u : 0u;
				}
				return n;
			}

			void Process(InEntry& e)
			{
				if (workCount == kWorkCap)
					Advance(true);
				if (workCount == kWorkCap)
				{
					// The MFT keeps that many packets without output: treat as unusable.
					std::lock_guard<std::mutex> lock(m);
					FailLocked("Media Foundation: decoder holds too many packets (internal)");
					return;
				}
				Work& w = work[(workHead + workCount) % kWorkCap];
				w = Work();
				w.ticket = e.ticket;
				w.seq = seq++;
				++workCount;
				const bool hasAlphaPacket = format.hasAlpha && e.alphaSize > 0;
				const Steady::time_point t0 = Steady::now();
				if (alphaMft && hasAlphaPacket)
				{
					w.alphaFed = true;
					if (!Feed(alpha, e.alpha.get(), e.alphaSize, e.keyframe, w.seq, true))
						return;
				}
				else if (format.hasAlpha)
				{
					// libvpx alpha (or no alpha packet for this frame): the A plane is written now.
					bool wrote = false;
					if (alphaVpx && hasAlphaPacket && Succeeded(alphaVpx->Decode(e.alpha.get(), e.alphaSize, nullptr, 0)) && alphaVpx->HasFrame())
					{
						YuvaImage img;
						if (w.ticket.slot != nullptr && Succeeded(alphaVpx->GetFrameView(img)) && img.width == format.width && img.height == format.height &&
							!noWrites.load(std::memory_order_acquire))
						{
							for (int r = 0; r < format.height; ++r)
								std::memcpy(w.ticket.slot->plane[3] + static_cast<ptrdiff_t>(r) * w.ticket.slot->stride[3],
									img.plane[0] + static_cast<ptrdiff_t>(r) * img.stride[0], static_cast<size_t>(format.width));
							wrote = true;
						}
					}
					if (!wrote && w.ticket.slot != nullptr && !noWrites.load(std::memory_order_acquire))
						WriteOpaque(*w.ticket.slot);
					w.alphaWritten = true;
					w.alpha = OutState::None;
				}
				Work* cur = FindWork(w.seq); // `w` may have moved? (no: fixed array) - keep the reference simple
				if (cur == nullptr)
					return;
				if (!Feed(color, e.color.get(), e.colorSize, e.keyframe, cur->seq, false))
					return;
				cur = FindWork(w.seq);
				if (cur != nullptr)
					cur->decodeMs = Ms(t0, Steady::now());
				Advance(false);
			}

			void DoFlush()
			{
				if (color.t)
				{
					color.t->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
					color.t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
				}
				if (alpha.t)
				{
					alpha.t->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
					alpha.t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
				}
				workCount = 0;
				workHead = 0;
				for (bool& u : rbUsed)
					u = false;
			}

			void DoDrain()
			{
				Mft* list[2] = {&color, alphaMft ? &alpha : nullptr};
				for (Mft* mft : list)
				{
					if (mft == nullptr || !mft->t)
						continue;
					mft->t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
					mft->t->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
					if (!PullAll(*mft, mft == &alpha))
						return;
				}
				// Whatever did not come out now never will.
				for (uint32_t k = 0; k < workCount; ++k)
				{
					Work& w = work[(workHead + k) % kWorkCap];
					if (w.color == OutState::Waiting)
						w.color = OutState::None;
					if (w.alpha == OutState::Waiting)
						w.alpha = OutState::None;
				}
				Advance(true);
				for (Mft* mft : list)
					if (mft != nullptr && mft->t)
						mft->t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
			}

			// ----------------------------------------------------------------------------------- thread main loop
			void Run()
			{
				char reason[192] = {};
				const bool ok = Open(reason, sizeof(reason));
				{
					std::lock_guard<std::mutex> lock(m);
					opened = true;
					openOk = ok;
					if (!ok)
					{
						failed = true;
						Copy(info.reason, sizeof(info.reason), reason);
					}
				}
				cvCaller.notify_all();
				std::unique_lock<std::mutex> lock(m);
				while (ok)
				{
					const bool pending = PendingReadbacks() > 0;
					const auto ready = [&] { return quit || flushGen != flushAck || inCount > 0 || drainReq; };
					if (pending)
					{
						if (!cvThread.wait_for(lock, std::chrono::milliseconds(kIdleMapMs), ready))
						{
							// Idle: nothing new to decode, finish the pending read-backs.
							lock.unlock();
							if (!IsFailed())
							{
								Advance(true);
								if (IsFailed())
									FailAllWork();
							}
							lock.lock();
							continue;
						}
					}
					else
						cvThread.wait(lock, ready);
					if (quit)
						break;
					if (flushGen != flushAck)
					{
						const uint64_t gen = flushGen;
						lock.unlock();
						DoFlush();
						lock.lock();
						flushAck = gen;
						cvCaller.notify_all();
						continue;
					}
					if (inCount > 0)
					{
						InEntry& e = in[inHead];
						inBusy = true;
						const bool isFailed = failed;
						lock.unlock();
						if (isFailed)
							Complete(e.ticket, Status::BackendFailed);
						else
						{
							Process(e);
							if (IsFailed())
								FailAllWork();
						}
						lock.lock();
						inBusy = false;
						inHead = (inHead + 1) % kInCap;
						--inCount;
						lock.unlock();
						Wake(); // queue space
						lock.lock();
						continue;
					}
					if (drainReq)
					{
						drainReq = false;
						lock.unlock();
						if (!IsFailed())
						{
							DoDrain();
							if (IsFailed())
								FailAllWork();
						}
						lock.lock();
					}
				}
				lock.unlock();
				Close();
				lock.lock();
				exited = true;
				cvCaller.notify_all();
			}
		};

		// --------------------------------------------------------------------------------------------- backend
		class MfBackend final : public IVideoDecoderBackend
		{
		public:
			explicit MfBackend(std::shared_ptr<Shared> s) : m_s(std::move(s)) {}

			~MfBackend() override { Stop(); }

			// Asks the thread to finish and joins with a bounded wait (R1).
			void Stop()
			{
				if (!m_thread.joinable())
					return;
				Shared& s = *m_s;
				std::unique_lock<std::mutex> lock(s.m);
				s.quit = true;
				s.cvThread.notify_all();
				const bool done = s.cvCaller.wait_for(lock, std::chrono::milliseconds(kJoinTimeoutMs), [&] { return s.exited; });
				lock.unlock();
				if (done)
					m_thread.join();
				else
				{
					// A driver call does not return: leave the thread running with its own reference to the shared
					// state, never let it write slot memory again, and count the defect (the module logs it).
					s.noWrites.store(true, std::memory_order_release);
					s.wakeOn.store(false, std::memory_order_release);
					g_shutdownTimeouts.fetch_add(1);
					m_thread.detach();
				}
			}

			std::thread& Thread() { return m_thread; }

			void SetOutputFormat(const FrameFormat& format) override
			{
				// Fixed at creation (container size, 4:2:0); the ring is built from the same values.
				(void)format;
			}

			Status Submit(const DecodePacket& color, const DecodePacket* alpha, const DecodeTicket& ticket) override
			{
				Shared& s = *m_s;
				const bool withAlpha = alpha != nullptr && alpha->data != nullptr && alpha->size > 0;
				if (color.data == nullptr || color.size == 0 || color.size > s.cfg.maxPacketSize ||
					(withAlpha && alpha->size > s.cfg.maxAdditionalSize))
					return Status::InvalidArgument;
				{
					std::lock_guard<std::mutex> lock(s.m);
					if (s.failed)
						return Status::BackendFailed;
					if (s.inCount == kInCap || (ticket.slotIndex >= 0 && s.inFlightCount >= kMaxInFlight))
						return Status::Busy;
					InEntry& e = s.in[(s.inHead + s.inCount) % kInCap];
					std::memcpy(e.color.get(), color.data, color.size);
					e.colorSize = color.size;
					e.alphaSize = 0;
					if (withAlpha && e.alpha)
					{
						std::memcpy(e.alpha.get(), alpha->data, alpha->size);
						e.alphaSize = alpha->size;
					}
					e.keyframe = color.keyframe;
					e.ticket = ticket;
					if (ticket.slotIndex >= 0)
						s.inFlight[s.inFlightCount++] = ticket.slotIndex;
					++s.inCount;
				}
				s.cvThread.notify_one();
				return Status::Ok;
			}

			bool Receive(DecodeTicket& out, Status& result) override
			{
				Shared& s = *m_s;
				std::lock_guard<std::mutex> lock(s.m);
				if (s.outCount == 0)
					return false;
				out = s.out[s.outHead];
				result = s.outResult[s.outHead];
				s.outHead = (s.outHead + 1) % kOutCap;
				--s.outCount;
				RemoveInFlightLocked(out.slotIndex);
				return true;
			}

			uint32_t Flush(int* abandoned, uint32_t capacity) override
			{
				Shared& s = *m_s;
				std::unique_lock<std::mutex> lock(s.m);
				// Packets not started yet are dropped here; the one being processed finishes first.
				if (s.inCount > 0)
				{
					const uint32_t keep = s.inBusy ? 1u : 0u;
					s.inCount = keep;
				}
				if (!s.exited && s.openOk)
				{
					const uint64_t gen = ++s.flushGen;
					s.cvThread.notify_all();
					s.cvCaller.wait_for(lock, std::chrono::milliseconds(kFlushTimeoutMs), [&] { return s.flushAck >= gen || s.exited; });
					if (s.flushAck < gen && !s.exited)
					{
						// The thread did not confirm: it must not write the slots we hand back.
						s.noWrites.store(true, std::memory_order_release);
						s.FailLocked("Media Foundation: flush not confirmed within 2 s");
					}
				}
				s.outCount = 0;
				s.outHead = 0;
				uint32_t n = 0;
				for (uint32_t i = 0; i < s.inFlightCount; ++i)
					if (abandoned != nullptr && n < capacity)
						abandoned[n++] = s.inFlight[i];
				s.inFlightCount = 0;
				return n;
			}

			void Drain() override
			{
				Shared& s = *m_s;
				{
					std::lock_guard<std::mutex> lock(s.m);
					s.drainReq = true;
				}
				s.cvThread.notify_one();
			}

			uint32_t MaxInFlight() const override { return kMaxInFlight; }

			DecodeBackendInfo Info() const override
			{
				std::lock_guard<std::mutex> lock(m_s->m);
				return m_s->info;
			}

			bool LastFrameInfo(DecodedImageInfo& out) const override
			{
				std::lock_guard<std::mutex> lock(m_s->m);
				if (!m_s->lastValid)
					return false;
				out = m_s->last;
				return true;
			}

		private:
			void RemoveInFlightLocked(int slot)
			{
				Shared& s = *m_s;
				for (uint32_t i = 0; i < s.inFlightCount; ++i)
					if (s.inFlight[i] == slot)
					{
						s.inFlight[i] = s.inFlight[--s.inFlightCount];
						return;
					}
			}

			std::shared_ptr<Shared> m_s;
			std::thread m_thread;
		};

		void NameThisThread()
		{
			using SetDesc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
			HMODULE k = GetModuleHandleW(L"kernel32.dll");
			if (k == nullptr)
				return;
			const SetDesc fn = reinterpret_cast<SetDesc>(reinterpret_cast<void*>(GetProcAddress(k, "SetThreadDescription")));
			if (fn != nullptr)
				fn(GetCurrentThread(), L"CEV MF Decode");
		}
	}

	bool MediaFoundationCompiled() { return true; }

	Status CreateMediaFoundationBackend(const MediaFoundationConfig& config, std::unique_ptr<IVideoDecoderBackend>& out, char* reason,
		size_t reasonSize)
	{
		out.reset();
		Copy(reason, reasonSize, "");
		if (config.stream.codec != VideoCodec::VP9)
		{
			Copy(reason, reasonSize, "Media Foundation: only VP9 is decoded in hardware (VP8 uses libvpx)");
			return Status::Unsupported;
		}
		std::shared_ptr<Shared> s;
		std::unique_ptr<MfBackend> b;
		try
		{
			s = std::make_shared<Shared>();
			s->cfg = config;
			s->format.width = config.stream.width;
			s->format.height = config.stream.height;
			s->format.hasAlpha = config.stream.hasAlpha;
			s->format.withRgba = false;
			for (InEntry& e : s->in)
			{
				e.color.reset(new uint8_t[std::max<uint32_t>(config.maxPacketSize, 1u)]);
				if (config.stream.hasAlpha)
					e.alpha.reset(new uint8_t[std::max<uint32_t>(config.maxAdditionalSize, 1u)]);
			}
			b.reset(new MfBackend(s));
			std::shared_ptr<Shared> keep = s; // the thread's own reference
			b->Thread() = std::thread([keep] {
				NameThisThread();
				keep->Run();
			});
		}
		catch (...)
		{
			Copy(reason, reasonSize, "Media Foundation: out of memory / threads");
			return Status::OutOfMemory;
		}
		bool ok = false;
		{
			std::unique_lock<std::mutex> lock(s->m);
			const bool opened = s->cvCaller.wait_for(lock, std::chrono::milliseconds(kOpenTimeoutMs), [&] { return s->opened; });
			ok = opened && s->openOk;
			if (!opened)
				Copy(reason, reasonSize, "Media Foundation: opening the decoder timed out (5 s)");
			else if (!ok)
				Copy(reason, reasonSize, s->info.reason);
		}
		if (!ok)
		{
			b.reset(); // bounded join
			return Status::Unsupported;
		}
		out = std::move(b);
		return Status::Ok;
	}

	void SetMediaFoundationTestFault(MediaFoundationTestFault fault, uint32_t param)
	{
		g_faultParam.store(param);
		g_fault.store(static_cast<uint8_t>(fault));
	}

	uint32_t MediaFoundationStartupCount()
	{
		std::lock_guard<std::mutex> lock(g_startupMutex);
		return g_startups;
	}

	uint32_t MediaFoundationShutdownTimeouts() { return g_shutdownTimeouts.load(); }
}

#else // !_WIN32

namespace cev
{
	bool MediaFoundationCompiled() { return false; }

	Status CreateMediaFoundationBackend(const MediaFoundationConfig&, std::unique_ptr<IVideoDecoderBackend>& out, char* reason, size_t reasonSize)
	{
		out.reset();
		if (reason != nullptr && reasonSize > 0)
			std::snprintf(reason, reasonSize, "%s", "Media Foundation is not available on this platform");
		return Status::Unsupported;
	}

	void SetMediaFoundationTestFault(MediaFoundationTestFault, uint32_t) {}
	uint32_t MediaFoundationStartupCount() { return 0; }
	uint32_t MediaFoundationShutdownTimeouts() { return 0; }
}

#endif
