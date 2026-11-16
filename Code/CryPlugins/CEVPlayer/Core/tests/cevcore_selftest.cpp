// CEVPlayerCore selftest. Plain executable (no engine, no test framework).
//
//   cevcore_selftest [--media <dir>] [--only <substring>] [--stress <iterations>] [--verbose]
//
// The media directory holds the clips produced by tools/make_test_media.sh. Playback is simulated with a manual
// clock that advances 1/60 s per step, as fast as the decoder delivers frames (faster than real time).
// Global operator new / delete are replaced to count allocations: after warm-up, steady state playback must
// perform zero allocations on the consumer thread and on the decode workers.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "CEVCore/AudioDecoder.h"
#include "CEVCore/ColorConvert.h"
#include "CEVCore/DecodePool.h"
#include "CEVCore/FrameRing.h"
#include "CEVCore/IByteSource.h"
#include "CEVCore/IClock.h"
#include "CEVCore/PlayerCore.h"
#include "CEVCore/Stats.h"
#include "CEVCore/WebmDemuxer.h"

#if defined(_WIN32)
	#include <malloc.h>
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
	#include <crtdbg.h> // Debug CRT heap: guard bytes, periodic heap validation, leak diff (also covers the C libraries)
	#define CEV_CRT_DEBUG 1
#endif

// =====================================================================================================================
// Global allocation counting
// =====================================================================================================================
namespace
{
	std::atomic<uint64_t> g_newCount{0};
	std::atomic<uint64_t> g_deleteCount{0};

	uint64_t AllocCounter() { return g_newCount.load(std::memory_order_relaxed); }

	void* CountedAlloc(size_t size)
	{
		g_newCount.fetch_add(1, std::memory_order_relaxed);
		void* p = std::malloc(size != 0 ? size : 1);
		if (p == nullptr)
			std::abort();
		return p;
	}

	void* CountedAllocAligned(size_t size, size_t alignment)
	{
		g_newCount.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
		void* p = _aligned_malloc(size != 0 ? size : 1, alignment);
#else
		void* p = std::aligned_alloc(alignment, ((size != 0 ? size : 1) + alignment - 1) / alignment * alignment);
#endif
		if (p == nullptr)
			std::abort();
		return p;
	}

	void CountedFree(void* p)
	{
		if (p == nullptr)
			return;
		g_deleteCount.fetch_add(1, std::memory_order_relaxed);
		std::free(p);
	}

	void CountedFreeAligned(void* p)
	{
		if (p == nullptr)
			return;
		g_deleteCount.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
		_aligned_free(p);
#else
		std::free(p);
#endif
	}
}

void* operator new(size_t size) { return CountedAlloc(size); }
void* operator new[](size_t size) { return CountedAlloc(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { g_newCount.fetch_add(1, std::memory_order_relaxed); return std::malloc(size != 0 ? size : 1); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { g_newCount.fetch_add(1, std::memory_order_relaxed); return std::malloc(size != 0 ? size : 1); }
void* operator new(size_t size, std::align_val_t al) { return CountedAllocAligned(size, static_cast<size_t>(al)); }
void* operator new[](size_t size, std::align_val_t al) { return CountedAllocAligned(size, static_cast<size_t>(al)); }
void operator delete(void* p) noexcept { CountedFree(p); }
void operator delete[](void* p) noexcept { CountedFree(p); }
void operator delete(void* p, size_t) noexcept { CountedFree(p); }
void operator delete[](void* p, size_t) noexcept { CountedFree(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { CountedFree(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { CountedFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { CountedFreeAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { CountedFreeAligned(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { CountedFreeAligned(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { CountedFreeAligned(p); }

// =====================================================================================================================
// Tiny test framework
// =====================================================================================================================
using namespace cev;

namespace
{
	int g_failures = 0;
	int g_checks = 0;
	bool g_verbose = false;
	std::string g_context;

	void Log(const char* fmt, ...)
	{
		va_list ap;
		va_start(ap, fmt);
		std::vprintf(fmt, ap);
		va_end(ap);
		std::fflush(stdout);
	}

	void ReportFailure(int line, const char* expr, const char* extra)
	{
		++g_failures;
		Log("  FAIL [%s] line %d: %s %s\n", g_context.c_str(), line, expr, extra != nullptr ? extra : "");
	}

#define CHECK(cond) do { ++g_checks; if (!(cond)) ReportFailure(__LINE__, #cond, nullptr); } while (0)
#define CHECKF(cond, ...) do { ++g_checks; if (!(cond)) { char _b[256]; std::snprintf(_b, sizeof(_b), __VA_ARGS__); ReportFailure(__LINE__, #cond, _b); } } while (0)

	template <typename Pred>
	bool WaitFor(Pred pred, int timeoutMs)
	{
		const auto start = std::chrono::steady_clock::now();
		uint32_t spins = 0;
		while (!pred())
		{
			if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > timeoutMs)
				return false;
			if (++spins < 2000)
				std::this_thread::yield();
			else
				std::this_thread::sleep_for(std::chrono::microseconds(500));
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Manual clock: only moves when the test advances it.
	// ---------------------------------------------------------------------------------------------------------------
	class ManualClock final : public IClock
	{
	public:
		double NowSeconds() override { return m_time; }
		void Start() override { m_time = 0.0; m_running = true; }
		void Pause() override { m_running = false; }
		void Resume() override { m_running = true; }
		void Seek(double s) override { m_time = s; }
		void SetRate(double r) override { m_rate = r; }
		double Rate() const override { return m_rate; }
		bool IsRunning() const override { return m_running; }
		void Advance(double dt) { if (m_running) m_time += dt * m_rate; }

	private:
		double m_time = 0.0;
		double m_rate = 1.0;
		bool m_running = false;
	};

	// ---------------------------------------------------------------------------------------------------------------
	// Clip table (frame counts verified with ffprobe -count_packets)
	// ---------------------------------------------------------------------------------------------------------------
	struct Clip
	{
		const char* file;
		uint32_t videoFrames;   // 0 = audio only
		int width, height;
		bool alpha;
		AudioCodec audio;
		uint32_t audioRate;
		double duration;        // ffprobe container duration
		double audioStart;      // seconds of leading silence expected (block timestamp of the first audio packet)
	};

	const Clip kClips[] = {
		{"vp9_1080p30_opus.webm", 300, 1920, 1080, false, AudioCodec::Opus, 48000, 10.008, 0.0},
		{"vp9_alpha.webm", 150, 640, 360, true, AudioCodec::None, 0, 5.000, 0.0},
		{"vp8_vorbis.webm", 125, 640, 360, false, AudioCodec::Vorbis, 44100, 5.003, 0.0},
		{"vp9_odd_321x181.webm", 150, 321, 181, false, AudioCodec::None, 0, 5.000, 0.0},
		{"audio_start_offset.webm", 150, 640, 360, false, AudioCodec::Opus, 48000, 6.508, 1.5},
		{"opus_preskip.webm", 150, 320, 180, false, AudioCodec::Opus, 48000, 5.008, 0.0},
		{"av_sync_flash_beep.webm", 300, 1280, 720, false, AudioCodec::Opus, 48000, 10.008, 0.0},
		{"audio_en.webm", 0, 0, 0, false, AudioCodec::Opus, 48000, 5.008, 0.0},
		{"audio_ja.webm", 0, 0, 0, false, AudioCodec::Opus, 48000, 5.008, 0.0},
	};

	const Clip* FindClip(const char* file)
	{
		for (const Clip& c : kClips)
			if (std::strcmp(c.file, file) == 0)
				return &c;
		return nullptr;
	}

	std::string g_mediaDir;

	std::string ClipPath(const Clip& c) { return g_mediaDir + "/" + c.file; }

	std::unique_ptr<IByteSource> OpenSource(const std::string& path, bool preload)
	{
		std::unique_ptr<IByteSource> src;
		if (Failed(FileByteSource::Open(path.c_str(), preload, src)))
			return nullptr;
		return src;
	}

	bool LoadAndWait(PlayerCore& p, const std::string& path, bool preload, int timeoutMs = 20000)
	{
		std::unique_ptr<IByteSource> src = OpenSource(path, preload);
		CHECKF(src != nullptr, "cannot open %s", path.c_str());
		if (!src)
			return false;
		CHECK(Succeeded(p.Load(std::move(src))));
		const bool ok = WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, timeoutMs);
		CHECK(ok);
		if (p.GetLoadState() != LoadState::Ready)
		{
			const LoadError e = p.GetLoadError();
			Log("  load failed: %s / %s\n", StatusToString(e.status), StatusDetailToString(e.detail));
			CHECK(false);
			return false;
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Playback simulation. Allocation free (used inside the zero-allocation window).
	// ---------------------------------------------------------------------------------------------------------------
	struct Sim
	{
		PlayerCore* player = nullptr;
		ManualClock* clock = nullptr;
		std::vector<float> audioBuf;          // preallocated by Init
		double step = 1.0 / 60.0;
		double audioDebt = 0.0;
		uint32_t sampleRateForOnsets = 48000;

		uint64_t presented = 0;
		uint64_t steps = 0;
		bool timedOut = false;

		// audio statistics since ResetAudio()
		uint64_t audioFrames = 0;             // frames delivered by ReadAudio (decoded data incl. leading silence)
		int64_t firstNonZero = -1;            // frame index of the first sample with |x| > 1e-3
		double energyHead = 0.0;              // sum of squares over the first headFrames frames
		uint64_t headFrames = 0;
		uint64_t headLimit = 4800;
		bool nonFinite = false;
		float peak = 0.0f;
		double onsets[64];                    // beep onsets: first |x| > 0.1 after >= 0.5 s of quiet
		int onsetCount = 0;
		uint64_t quietRun = 1u << 30;

		void (*onFrame)(const FrameView&, uint64_t index, void*) = nullptr;
		void* onFrameUser = nullptr;

		void Init(PlayerCore& p, ManualClock& c)
		{
			player = &p;
			clock = &c;
			audioBuf.assign(8192 * 8, 0.0f);
		}

		void ResetAudio()
		{
			audioFrames = 0;
			firstNonZero = -1;
			energyHead = 0.0;
			headFrames = 0;
			nonFinite = false;
			peak = 0.0f;
			audioDebt = 0.0;
			onsetCount = 0;
			quietRun = 1u << 30;
		}

		// One 1/60 s step. Returns false when the decoder did not deliver in time.
		bool Step()
		{
			PlayerCore& p = *player;
			const MediaInfo& info = p.GetInfo();
			++steps;

			if (info.hasVideo)
			{
				if (!WaitFor([&] { return p.VideoBufferedFrame() || p.VideoDecoderAtEnd(); }, 15000))
				{
					timedOut = true;
					return false;
				}
			}
			if (p.Update())
			{
				if (onFrame != nullptr)
				{
					FrameView fv;
					if (p.GetFrame(fv))
						onFrame(fv, presented, onFrameUser);
				}
				++presented;
			}

			if (info.hasAudio && p.GetState() == PlayerState::Playing)
			{
				audioDebt += info.audioSampleRate * step;
				const uint32_t need = static_cast<uint32_t>(audioDebt);
				audioDebt -= need;
				if (!WaitFor([&] { return p.AudioBufferedFrames() >= need || p.AudioDecoderAtEnd(); }, 15000))
				{
					timedOut = true;
					return false;
				}
				const uint32_t got = p.ReadAudio(audioBuf.data(), need);
				sampleRateForOnsets = info.audioSampleRate;
				Account(got, info.audioChannels);
			}
			clock->Advance(step);
			return true;
		}

		void Account(uint32_t frames, uint32_t channels)
		{
			for (uint32_t i = 0; i < frames; ++i)
			{
				float m = 0.0f;
				for (uint32_t c = 0; c < channels; ++c)
				{
					const float v = audioBuf[static_cast<size_t>(i) * channels + c];
					if (!std::isfinite(v))
						nonFinite = true;
					m = std::max(m, std::fabs(v));
					if (headFrames < headLimit)
						energyHead += static_cast<double>(v) * v;
				}
				peak = std::max(peak, m);
				if (m > 0.1f)
				{
					if (quietRun >= sampleRateForOnsets / 2 && onsetCount < 64)
						onsets[onsetCount++] = static_cast<double>(audioFrames + i) / sampleRateForOnsets;
					quietRun = 0;
				}
				else
				{
					++quietRun;
				}
				if (firstNonZero < 0 && m > 1e-3f)
					firstNonZero = static_cast<int64_t>(audioFrames + i);
				if (headFrames < headLimit)
					++headFrames;
			}
			audioFrames += frames;
		}

		// Runs until Ended (or timeout / safety limit). Returns true if Ended was reached.
		bool RunToEnd(double maxMediaSeconds)
		{
			const uint64_t limit = steps + static_cast<uint64_t>(maxMediaSeconds / step) + 600;
			while (steps < limit)
			{
				if (!Step())
					return false;
				if (player->GetState() == PlayerState::Ended)
					return true;
			}
			return false;
		}

		bool RunUntilPresented(uint64_t target)
		{
			const uint64_t limit = steps + 60000;
			while (presented < target && steps < limit)
				if (!Step())
					return false;
			return presented >= target;
		}
	};

	PlayerConfig MakeConfig(ManualClock& clk)
	{
		PlayerConfig cfg;
		cfg.externalClock = &clk;
		cfg.enableStats = true;
		return cfg;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Tests
	// ---------------------------------------------------------------------------------------------------------------
	void TestColor()
	{
		g_context = "color";
		CHECK(ResolveColorStandard(ColorStandard::Auto, -1, 640, 360) == ColorStandard::BT601);
		CHECK(ResolveColorStandard(ColorStandard::Auto, -1, 1280, 720) == ColorStandard::BT709);
		CHECK(ResolveColorStandard(ColorStandard::Auto, -1, 1920, 1080) == ColorStandard::BT709);
		CHECK(ResolveColorStandard(ColorStandard::Auto, 1, 320, 180) == ColorStandard::BT709);
		CHECK(ResolveColorStandard(ColorStandard::Auto, 6, 1920, 1080) == ColorStandard::BT601);
		CHECK(ResolveColorStandard(ColorStandard::BT709, 6, 320, 180) == ColorStandard::BT709);

		// 2x2 image: black, white, mid grey, saturated red-ish; limited range.
		uint8_t y[4] = {16, 235, 126, 81};
		uint8_t u[1] = {128};
		uint8_t v[1] = {128};
		uint8_t a[4] = {255, 255, 128, 0};
		YuvaImage img;
		img.plane[0] = y; img.plane[1] = u; img.plane[2] = v; img.plane[3] = a;
		img.stride[0] = 2; img.stride[1] = 1; img.stride[2] = 1; img.stride[3] = 2;
		img.width = 2; img.height = 2;
		uint8_t out[16] = {};
		ConvertYuvaToRgbaPremultiplied(img, ColorStandard::BT601, ColorRange::Limited, out, 8);
		CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 255);
		CHECK(out[4] == 255 && out[5] == 255 && out[6] == 255 && out[7] == 255);
		// pixel (0,1): Y=126 -> ~128 grey, alpha 128 -> premultiplied ~64
		CHECKF(std::abs(static_cast<int>(out[8]) - 64) <= 1 && out[11] == 128, "got %u/%u", out[8], out[11]);
		// pixel (1,1): alpha 0 -> everything 0
		CHECK(out[12] == 0 && out[13] == 0 && out[14] == 0 && out[15] == 0);

		// Chroma: V high -> red channel higher for 601 and 709 (and the two matrices differ).
		uint8_t u2[1] = {90}, v2[1] = {240};
		uint8_t y2[4] = {120, 120, 120, 120};
		img.plane[0] = y2; img.plane[1] = u2; img.plane[2] = v2; img.plane[3] = nullptr;
		uint8_t o601[16], o709[16];
		ConvertYuvaToRgbaPremultiplied(img, ColorStandard::BT601, ColorRange::Limited, o601, 8);
		ConvertYuvaToRgbaPremultiplied(img, ColorStandard::BT709, ColorRange::Limited, o709, 8);
		CHECK(o601[0] > o601[1] && o601[0] > o601[2]);
		CHECK(o601[1] != o709[1]);
		// 601: R = 1.164*(120-16) + 1.596*(240-128) = 121.06+178.75 = 299.8 -> 255
		CHECK(o601[0] == 255);
	}

	void TestStatusAndSources()
	{
		g_context = "status";
		CHECK(std::strcmp(StatusToString(Status::Ok), "Ok") == 0);
		CHECK(std::strcmp(StatusToString(Status::DecodeError), "DecodeError") == 0);

		std::unique_ptr<IByteSource> src;
		CHECK(FileByteSource::Open("does/not/exist.webm", false, src) == Status::IoError);
		CHECK(!src);

		uint8_t data[16];
		for (int i = 0; i < 16; ++i) data[i] = static_cast<uint8_t>(i);
		MemoryByteSource mem(data, sizeof(data));
		uint8_t out[8] = {};
		CHECK(mem.Read(4, out, 8) == Status::Ok && out[0] == 4 && out[7] == 11);
		CHECK(mem.Read(12, out, 8) == Status::OutOfRange);
		CHECK(mem.Length() == 16);

		PlayerCore p;
		CHECK(p.Load(nullptr) == Status::InvalidArgument);
		CHECK(p.Play() == Status::NotReady);
		CHECK(p.Seek(1.0) == Status::NotReady);
		CHECK(!p.Update());
		CHECK(p.GetLoadState() == LoadState::Unloaded);
	}

	void TestBadInput(const std::string& anyClip)
	{
		g_context = "bad-input";
		// 1. garbage
		{
			std::vector<uint8_t> junk(4096);
			for (size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<uint8_t>(i * 31 + 7);
			PlayerCore p;
			CHECK(Succeeded(p.Load(std::make_unique<MemoryByteSource>(std::move(junk)))));
			CHECK(WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 5000));
			CHECK(p.GetLoadState() == LoadState::Failed);
			CHECK(p.GetLoadError().status != Status::Ok);
			CHECK(p.Play() == Status::NotReady);
			p.Unload();
			CHECK(p.GetLoadState() == LoadState::Unloaded);
		}
		// 2. truncated clip: must not crash; either loads (partial index) or fails cleanly
		{
			std::unique_ptr<IByteSource> full = OpenSource(anyClip, true);
			CHECK(full != nullptr);
			if (full)
			{
				std::vector<uint8_t> part(static_cast<size_t>(full->Length() / 3));
				CHECK(Succeeded(full->Read(0, part.data(), part.size())));
				PlayerCore p;
				CHECK(Succeeded(p.Load(std::make_unique<MemoryByteSource>(std::move(part)))));
				CHECK(WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 10000));
				if (p.GetLoadState() == LoadState::Ready)
				{
					CHECK(Succeeded(p.Play()));
					for (int i = 0; i < 20; ++i)
					{
						p.Update();
						std::this_thread::sleep_for(std::chrono::milliseconds(2));
					}
				}
			}
		}
	}

	void TestDemux(const Clip& clip)
	{
		g_context = std::string("demux ") + clip.file;
		std::unique_ptr<IByteSource> src = OpenSource(ClipPath(clip), false);
		CHECK(src != nullptr);
		if (!src)
			return;
		std::unique_ptr<WebmDemuxer> d;
		StatusDetail det = StatusDetail::None;
		const Status st = WebmDemuxer::Open(std::move(src), d, &det, nullptr);
		CHECKF(Succeeded(st), "open failed %s/%s", StatusToString(st), StatusDetailToString(det));
		if (!d)
			return;
		CHECK(d->HasVideo() == (clip.videoFrames != 0));
		if (d->HasVideo())
		{
			CHECK(d->VideoPacketCount() == clip.videoFrames);
			CHECK(d->Video().width == clip.width && d->Video().height == clip.height);
			CHECK(d->Video().hasAlpha == clip.alpha);
			if (clip.alpha)
			{
				uint32_t withAlpha = 0;
				for (uint32_t i = 0; i < d->VideoPacketCount(); ++i)
					withAlpha += d->VideoPacket(i).additionalSize > 0 ? 1u : 0u;
				CHECKF(withAlpha > clip.videoFrames * 9 / 10, "alpha packets %u", withAlpha);
			}
			CHECK(d->VideoPacket(0).keyframe);
			double prev = -1.0;
			bool mono = true;
			for (uint32_t i = 0; i < d->VideoPacketCount(); ++i)
			{
				mono = mono && d->VideoPts(i) >= prev;
				prev = d->VideoPts(i);
			}
			CHECK(mono);
		}
		CHECK(d->HasAudio() == (clip.audio != AudioCodec::None));
		if (d->HasAudio())
		{
			CHECK(d->Audio().codec == clip.audio);
			CHECK(d->Audio().channels == 2);
			if (clip.audio == AudioCodec::Opus)
			{
				CHECK(d->Audio().codecDelayNs > 0);
				CHECKF(std::fabs(d->Audio().firstPts - clip.audioStart) < 0.02, "audio first pts %f", d->Audio().firstPts);
			}
			std::unique_ptr<AudioDecoder> ad;
			CHECK(Succeeded(AudioDecoder::Create(d->Audio(), ad)));
			if (ad)
			{
				CHECK(ad->SampleRate() == clip.audioRate);
				CHECK(ad->Channels() == 2);
			}
		}
		CHECKF(std::fabs(d->DurationSeconds() - clip.duration) < 0.05, "duration %f vs %f", d->DurationSeconds(), clip.duration);
	}

	// ---- per frame checks used by TestPlayback ----
	struct FrameProbe
	{
		const Clip* clip = nullptr;
		uint8_t alphaMin = 255, alphaMax = 0;
		uint64_t alphaFramesSampled = 0;
		bool premulOk = true;
		bool dimsOk = true;
		bool oddOk = true;
		int oddChecked = 0;
		double lastPts = -1.0;
		bool ptsMonotonic = true;
		bool flashOk = true;      // av_sync clip: white frame exactly on every 30th frame
		int flashesSeen = 0;
	};

	void ProbeFrame(const FrameView& f, uint64_t index, void* user)
	{
		FrameProbe& pr = *static_cast<FrameProbe*>(user);
		const Clip& c = *pr.clip;
		if (f.width != c.width || f.height != c.height || f.rgba == nullptr || f.rgbaStride != c.width * 4)
			pr.dimsOk = false;
		if (f.pts < pr.lastPts)
			pr.ptsMonotonic = false;
		pr.lastPts = f.pts;

		if (std::strcmp(c.file, "av_sync_flash_beep.webm") == 0 && f.plane[0] != nullptr)
		{
			const uint8_t luma = f.plane[0][static_cast<ptrdiff_t>(f.height / 2) * f.stride[0] + f.width / 2];
			const bool bright = luma > 200;
			const bool shouldFlash = std::fabs(f.pts - std::round(f.pts)) < 1e-3;
			if (bright != shouldFlash)
				pr.flashOk = false;
			pr.flashesSeen += bright ? 1 : 0;
		}

		if (c.alpha && index % 10 == 0)
		{
			++pr.alphaFramesSampled;
			if (f.plane[3] == nullptr)
			{
				pr.premulOk = false;
				return;
			}
			for (int y = 0; y < f.height; ++y)
			{
				const uint8_t* arow = f.plane[3] + static_cast<ptrdiff_t>(y) * f.stride[3];
				const uint8_t* rgba = f.rgba + static_cast<ptrdiff_t>(y) * f.rgbaStride;
				for (int x = 0; x < f.width; ++x)
				{
					const uint8_t a = arow[x];
					pr.alphaMin = std::min(pr.alphaMin, a);
					pr.alphaMax = std::max(pr.alphaMax, a);
					if (rgba[x * 4 + 3] != a || rgba[x * 4] > a || rgba[x * 4 + 1] > a || rgba[x * 4 + 2] > a)
						pr.premulOk = false;
				}
			}
		}

		// Independent float reference for a few pixels incl. the odd right / bottom edge (BT.601 limited).
		if (!c.alpha && c.width % 2 == 1 && (index == 20 || index == 21) && f.plane[0] != nullptr)
		{
			const int xs[] = {0, 1, f.width / 2, f.width - 2, f.width - 1};
			const int ys[] = {0, 1, f.height / 2, f.height - 2, f.height - 1};
			for (int yi = 0; yi < 5; ++yi)
				for (int xi = 0; xi < 5; ++xi)
				{
					const int x = xs[xi], y = ys[yi];
					const double Y = f.plane[0][static_cast<ptrdiff_t>(y) * f.stride[0] + x];
					const double U = f.plane[1][static_cast<ptrdiff_t>(y >> 1) * f.stride[1] + (x >> 1)];
					const double V = f.plane[2][static_cast<ptrdiff_t>(y >> 1) * f.stride[2] + (x >> 1)];
					const double r = 1.164383 * (Y - 16) + 1.596027 * (V - 128);
					const double g = 1.164383 * (Y - 16) - 0.391762 * (U - 128) - 0.812968 * (V - 128);
					const double b = 1.164383 * (Y - 16) + 2.017232 * (U - 128);
					const uint8_t* px = f.rgba + static_cast<ptrdiff_t>(y) * f.rgbaStride + x * 4;
					auto clamp = [](double v) { return v < 0 ? 0.0 : (v > 255 ? 255.0 : v); };
					if (std::fabs(clamp(r) - px[0]) > 1.6 || std::fabs(clamp(g) - px[1]) > 1.6 ||
						std::fabs(clamp(b) - px[2]) > 1.6 || px[3] != 255)
						pr.oddOk = false;
					++pr.oddChecked;
				}
		}
	}

	void TestPlayback(const Clip& clip)
	{
		g_context = std::string("playback ") + clip.file;
		const int failuresBefore = g_failures;
		ManualClock clk;
		PlayerCore p(MakeConfig(clk));
		if (!LoadAndWait(p, ClipPath(clip), (clip.width % 2) == 1))
			return;
		const MediaInfo& info = p.GetInfo();
		CHECK(info.hasVideo == (clip.videoFrames != 0));
		CHECK(info.hasAudio == (clip.audio != AudioCodec::None));
		CHECK(info.width == clip.width && info.height == clip.height);
		CHECK(info.hasAlpha == clip.alpha);
		CHECK(info.premultiplied);
		CHECK(info.videoPacketCount == clip.videoFrames);
		CHECKF(std::fabs(info.durationSeconds - clip.duration) < 0.05, "duration %f", info.durationSeconds);
		CHECK(p.GetState() == PlayerState::Stopped);
		CHECK(p.GetLoadError().status == Status::Ok);

		Sim sim;
		sim.Init(p, clk);
		FrameProbe probe;
		probe.clip = &clip;
		sim.onFrame = &ProbeFrame;
		sim.onFrameUser = &probe;

		// The first frame is decoded during Load: it is presented while still stopped.
		if (info.hasVideo)
		{
			CHECK(p.VideoBufferedFrame());
			CHECK(p.Update());
			FrameView fv;
			CHECK(p.GetFrame(fv));
			CHECK(fv.width == clip.width && fv.height == clip.height);
			CHECK(fv.rgba != nullptr && fv.plane[0] != nullptr && fv.plane[1] != nullptr && fv.plane[2] != nullptr);
			CHECK((fv.plane[3] != nullptr) == clip.alpha);
			ProbeFrame(fv, 0, &probe);
			sim.presented = 1;
			CHECK(!p.Update()); // nothing new while stopped
		}

		CHECK(Succeeded(p.Play()));
		CHECK(p.GetState() == PlayerState::Playing);
		const auto t0 = std::chrono::steady_clock::now();
		const bool ended = sim.RunToEnd(clip.duration + 1.0);
		const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		CHECK(ended);
		CHECK(!sim.timedOut);
		CHECK(p.GetState() == PlayerState::Ended);
		CHECK(!p.Update());
		CHECK(p.DecodeErrors() == 0);

		const StatsSnapshot st = p.GetStats();
		if (info.hasVideo)
		{
			CHECKF(sim.presented + st.framesDropped == clip.videoFrames, "presented %llu + dropped %llu != %u",
				(unsigned long long)sim.presented, (unsigned long long)st.framesDropped, clip.videoFrames);
			CHECKF(st.framesDropped == 0, "dropped %llu", (unsigned long long)st.framesDropped);
			CHECK(st.framesPresented == sim.presented);
			CHECK(probe.dimsOk);
			CHECK(probe.ptsMonotonic);
			CHECK(st.samples > 0);
		}
		if (std::strcmp(clip.file, "av_sync_flash_beep.webm") == 0)
		{
			CHECK(probe.flashOk);
			CHECKF(probe.flashesSeen >= 9, "flashes %d", probe.flashesSeen);
			CHECKF(sim.onsetCount >= 9, "beep onsets %d", sim.onsetCount);
			double worst = 0.0;
			for (int i = 0; i < sim.onsetCount; ++i)
				worst = std::max(worst, std::fabs(sim.onsets[i] - std::round(sim.onsets[i])));
			CHECKF(worst < 0.010, "beep onset vs flash offset %.4f s", worst);
			Log("  av sync: %d flashes, %d beeps, worst audio onset offset %.2f ms\n", probe.flashesSeen, sim.onsetCount, worst * 1000.0);
		}
		if (clip.alpha)
		{
			CHECK(probe.alphaFramesSampled >= 10);
			CHECKF(probe.alphaMin == 0 && probe.alphaMax == 255, "alpha range %u..%u", probe.alphaMin, probe.alphaMax);
			CHECK(probe.premulOk);
		}
		if (clip.width % 2 == 1)
		{
			CHECK(probe.oddChecked >= 25);
			CHECK(probe.oddOk);
		}
		if (info.hasAudio)
		{
			const double decodedSeconds = static_cast<double>(sim.audioFrames) / info.audioSampleRate;
			CHECKF(std::fabs(decodedSeconds - clip.duration) < 0.1, "audio %f s vs %f", decodedSeconds, clip.duration);
			CHECK(!sim.nonFinite);
			CHECKF(sim.peak > 0.05f && sim.peak < 1.5f, "peak %f", sim.peak);
			CHECK(info.audioSampleRate == clip.audioRate && info.audioChannels == 2);
			if (clip.audioStart > 0.0)
			{
				const double lead = sim.firstNonZero >= 0 ? static_cast<double>(sim.firstNonZero) / info.audioSampleRate : -1.0;
				CHECKF(std::fabs(lead - clip.audioStart) < 0.03, "leading silence %f s", lead);
			}
			else
			{
				// Opus pre-skip handling: sound must begin right away (tone starts at sample 0 +- codec ramp).
				const double lead = sim.firstNonZero >= 0 ? static_cast<double>(sim.firstNonZero) / info.audioSampleRate : -1.0;
				CHECKF(lead >= 0.0 && lead < 0.05, "audio starts at %f s", lead);
			}
		}
		Log("  %-26s %s: %3llu frames presented, %llu dropped, audio %.3f s, decode avg %.2f ms, convert avg %.2f ms, wall %.2f s\n",
			clip.file, g_failures == failuresBefore ? "ok" : "FAILED",
			(unsigned long long)sim.presented, (unsigned long long)st.framesDropped,
			info.hasAudio ? static_cast<double>(sim.audioFrames) / info.audioSampleRate : 0.0,
			st.decodeMs.avg, st.convertMs.avg, wall);

		// Stop / restart / Unload
		CHECK(Succeeded(p.Stop()));
		CHECK(p.GetState() == PlayerState::Stopped);
		CHECK(p.GetTime() == 0.0);
		if (info.hasVideo)
		{
			CHECK(WaitFor([&] { return p.VideoBufferedFrame(); }, 10000));
			CHECK(p.Update());
			FrameView fv;
			CHECK(p.GetFrame(fv));
			CHECK(fv.pts < 0.1);
		}
		p.Unload();
		CHECK(p.GetLoadState() == LoadState::Unloaded);
		CHECK(p.Play() == Status::NotReady);
		FrameView none;
		CHECK(!p.GetFrame(none));
	}

	// seek to `target`, continue to the end; verify first frame, remaining frame count and audio.
	void TestSeek(const Clip& clip, double target)
	{
		char ctx[128];
		std::snprintf(ctx, sizeof(ctx), "seek %s -> %.2f", clip.file, target);
		g_context = ctx;
		ManualClock clk;
		PlayerCore p(MakeConfig(clk));
		if (!LoadAndWait(p, ClipPath(clip), false))
			return;
		const MediaInfo& info = p.GetInfo();

		// expected covering frame from an independent demuxer instance
		uint32_t covering = 0;
		if (info.hasVideo)
		{
			std::unique_ptr<WebmDemuxer> d;
			CHECK(Succeeded(WebmDemuxer::Open(OpenSource(ClipPath(clip), true), d)));
			if (d)
				for (uint32_t i = 0; i < d->VideoPacketCount(); ++i)
					if (d->VideoPts(i) <= target + 1e-4)
						covering = i;
		}

		Sim sim;
		sim.Init(p, clk);
		sim.ResetAudio();
		sim.headLimit = 2400;
		CHECK(Succeeded(p.Play()));
		CHECK(Succeeded(p.Seek(target)));
		CHECK(std::fabs(p.GetTime() - target) < 1e-6);

		if (info.hasVideo)
		{
			CHECK(sim.RunUntilPresented(1));
			FrameView fv;
			CHECK(p.GetFrame(fv));
			CHECKF(fv.pts <= target + 1e-3 && fv.pts > target - 1.5 / info.frameRate, "first frame after seek pts %f", fv.pts);
		}
		const uint64_t presentedBefore = sim.presented;
		const bool ended = sim.RunToEnd(clip.duration - target + 1.0);
		CHECK(ended);
		CHECK(!sim.timedOut);
		const StatsSnapshot st = p.GetStats();
		if (info.hasVideo)
		{
			// the stats counters also saw the frames before the seek? none (play->seek immediately), but the first
			// frame decoded at load may have been flushed unpresented: only count what was presented after Play.
			const uint64_t expected = clip.videoFrames - covering;
			CHECKF(sim.presented == expected, "presented %llu expected %llu (covering frame %u)",
				(unsigned long long)sim.presented, (unsigned long long)expected, covering);
			CHECKF(st.framesDropped == 0, "dropped %llu", (unsigned long long)st.framesDropped);
			(void)presentedBefore;
		}
		if (info.hasAudio)
		{
			const double secs = static_cast<double>(sim.audioFrames) / info.audioSampleRate;
			CHECKF(std::fabs(secs - (clip.duration - target)) < 0.1, "audio after seek %f s vs %f", secs, clip.duration - target);
			CHECK(!sim.nonFinite);
			const double lead = sim.firstNonZero >= 0 ? static_cast<double>(sim.firstNonZero) / info.audioSampleRate : -1.0;
			if (clip.audioStart > target)
				CHECKF(std::fabs(lead - (clip.audioStart - target)) < 0.03, "silence after seek %f s (expected %f)", lead, clip.audioStart - target);
			else
				CHECKF(lead >= 0.0 && lead < 0.05, "audio after seek starts at %f s", lead);
		}
		Log("  %-34s ok: %llu frames after seek, audio %.3f s\n", ctx, (unsigned long long)sim.presented,
			info.hasAudio ? static_cast<double>(sim.audioFrames) / info.audioSampleRate : 0.0);

		// Seek back while Ended: becomes Paused, shows the earlier frame.
		CHECK(p.GetState() == PlayerState::Ended);
		CHECK(Succeeded(p.Seek(0.0)));
		CHECK(p.GetState() == PlayerState::Paused);
		if (info.hasVideo)
		{
			CHECK(WaitFor([&] { return p.VideoBufferedFrame(); }, 10000));
			CHECK(p.Update());
			FrameView fv;
			CHECK(p.GetFrame(fv));
			CHECK(fv.pts < 0.1);
		}
	}

	void TestLoop(const Clip& clip)
	{
		g_context = std::string("loop ") + clip.file;
		ManualClock clk;
		PlayerCore p(MakeConfig(clk));
		if (!LoadAndWait(p, ClipPath(clip), true))
			return;
		Sim sim;
		sim.Init(p, clk);
		CHECK(Succeeded(p.SetLoop(true)));
		CHECK(p.IsLooping());
		CHECK(p.Update());
		sim.presented = 1;
		CHECK(Succeeded(p.Play()));
		const uint64_t limit = 12 * (clip.videoFrames + 120);
		uint64_t guard = 0;
		while (p.GetLoopCount() < 2 && guard++ < limit)
		{
			if (!sim.Step())
				break;
		}
		CHECK(!sim.timedOut);
		CHECK(p.GetLoopCount() == 2);
		CHECK(p.GetState() == PlayerState::Playing);
		CHECKF(sim.presented == 2ull * clip.videoFrames, "presented %llu after two loops", (unsigned long long)sim.presented);
		CHECK(Succeeded(p.SetLoop(false)));
		CHECK(sim.RunToEnd(clip.duration + 2.0));
		CHECK(p.GetState() == PlayerState::Ended);
		CHECKF(sim.presented == 3ull * clip.videoFrames, "presented %llu after three runs", (unsigned long long)sim.presented);
		CHECK(p.GetStats().framesDropped == 0);
		CHECK(p.GetLoopCount() == 2);

		// Play after Ended restarts from the beginning.
		CHECK(Succeeded(p.Play()));
		CHECK(p.GetState() == PlayerState::Playing);
		CHECK(sim.RunUntilPresented(sim.presented + 10));
		CHECK(Succeeded(p.Pause()));
		CHECK(p.GetState() == PlayerState::Paused);
		const double tPaused = p.GetTime();
		for (int i = 0; i < 10; ++i)
		{
			CHECK(!p.Update());
			clk.Advance(1.0 / 60.0); // paused clock does not move
		}
		CHECK(p.GetTime() == tPaused);
		CHECK(Succeeded(p.Resume()));
		CHECK(p.GetState() == PlayerState::Playing);
		Log("  %-34s ok: 2 loops, %llu frames, no drops\n", g_context.c_str(), (unsigned long long)sim.presented);
	}

	void TestSteadyStateAllocations(const Clip& clip)
	{
		g_context = std::string("allocs ") + clip.file;
		ManualClock clk;
		PlayerConfig cfg = MakeConfig(clk);
		PlayerCore p(cfg);
		if (!LoadAndWait(p, ClipPath(clip), true))
			return;
		Sim sim;
		sim.Init(p, clk);
		CHECK(p.Update());
		sim.presented = 1;
		CHECK(Succeeded(p.Play()));

		// warm-up: first 40 frames
		CHECK(sim.RunUntilPresented(40));
		const uint64_t n0 = g_newCount.load();
		const uint64_t d0 = g_deleteCount.load();
		const uint64_t presented0 = sim.presented;
		const uint64_t audio0 = sim.audioFrames;
		// window: next 60 frames (2 s of media), consumer + workers
		CHECK(sim.RunUntilPresented(presented0 + 60));
		const uint64_t n1 = g_newCount.load();
		const uint64_t d1 = g_deleteCount.load();
		CHECK(!sim.timedOut);
		CHECKF(n1 == n0, "%llu allocations in the steady state window", (unsigned long long)(n1 - n0));
		CHECKF(d1 == d0, "%llu frees in the steady state window", (unsigned long long)(d1 - d0));
		CHECK(sim.presented - presented0 >= 60);
		if (p.GetInfo().hasAudio)
			CHECK(sim.audioFrames > audio0);
		Log("  %-34s ok: %llu allocations / %llu frees over %llu frames (%s)\n", g_context.c_str(),
			(unsigned long long)(n1 - n0), (unsigned long long)(d1 - d0), (unsigned long long)(sim.presented - presented0),
			p.GetInfo().hasAudio ? "A+V" : "video");
	}

	// ---- stress: 4 players concurrently on the pool, repeated create / destroy ----
	void TestStress(int iterations)
	{
		g_context = "stress";
		const char* files[4] = {"vp8_vorbis.webm", "vp9_alpha.webm", "opus_preskip.webm", "vp9_odd_321x181.webm"};
		std::string paths[4];
		for (int i = 0; i < 4; ++i)
			paths[i] = g_mediaDir + "/" + files[i];

		int64_t baselineLive = -1;
		std::vector<float> audio(8192 * 2);
		for (int iter = 0; iter < iterations + 1; ++iter)
		{
			{
				std::unique_ptr<PlayerCore> players[4];
				for (int i = 0; i < 4; ++i)
				{
					PlayerConfig cfg;
					cfg.enableStats = (iter % 2) == 0;
					cfg.audioBufferMs = 200;
					players[i] = std::make_unique<PlayerCore>(cfg);
					CHECK(Succeeded(players[i]->Load(OpenSource(paths[i], (iter + i) % 2 == 0))));
				}
				const int mode = iter % 4; // 0: play, 1: destroy while loading, 2: seek around, 3: loop + unload early
				if (mode != 1)
				{
					for (int i = 0; i < 4; ++i)
					{
						CHECK(WaitFor([&] { return players[i]->GetLoadState() != LoadState::Loading; }, 20000));
						CHECK(players[i]->GetLoadState() == LoadState::Ready);
						if (mode == 3)
							players[i]->SetLoop(true);
						if (players[i]->GetLoadState() == LoadState::Ready)
							CHECK(Succeeded(players[i]->Play()));
					}
					const auto start = std::chrono::steady_clock::now();
					int tick = 0;
					while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(250))
					{
						for (int i = 0; i < 4; ++i)
						{
							if (players[i]->GetLoadState() != LoadState::Ready)
								continue;
							players[i]->Update();
							if (players[i]->GetInfo().hasAudio)
								players[i]->ReadAudio(audio.data(), 800);
							if (mode == 2 && tick % 15 == 7)
								players[i]->Seek(0.3 * (i + 1) + (tick % 4) * 0.2);
						}
						++tick;
						std::this_thread::sleep_for(std::chrono::milliseconds(3));
					}
					if (mode == 3)
					{
						players[0]->Unload();
						players[1]->Stop();
					}
				}
				// players destroyed here in reverse creation order for odd iterations
				if (iter % 2 == 1)
					for (int i = 3; i >= 0; --i)
						players[i].reset();
			}
			const int64_t live = static_cast<int64_t>(g_newCount.load()) - static_cast<int64_t>(g_deleteCount.load());
			if (iter == 0)
				baselineLive = live;
			else if (iter == iterations)
				CHECKF(live == baselineLive, "live allocations grew: %lld -> %lld", (long long)baselineLive, (long long)live);
		}
		Log("  stress ok: %d iterations x 4 players, live allocations %s\n", iterations, "stable");
	}
}

// =====================================================================================================================
int main(int argc, char** argv)
{
#if defined(CEV_DEFAULT_MEDIA_DIR)
	g_mediaDir = CEV_DEFAULT_MEDIA_DIR;
#endif
	const char* only = nullptr;
	int stressIterations = 20;
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--media") == 0 && i + 1 < argc)
			g_mediaDir = argv[++i];
		else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc)
			only = argv[++i];
		else if (std::strcmp(argv[i], "--stress") == 0 && i + 1 < argc)
			stressIterations = std::atoi(argv[++i]);
		else if (std::strcmp(argv[i], "--verbose") == 0)
			g_verbose = true;
		else
		{
			std::printf("usage: cevcore_selftest [--media <dir>] [--only <substring>] [--stress <n>] [--verbose]\n");
			return 2;
		}
	}
	while (!g_mediaDir.empty() && (g_mediaDir.back() == '/' || g_mediaDir.back() == '\\'))
		g_mediaDir.pop_back();

	Stats::SetAllocCounter(&AllocCounter);
#if defined(CEV_CRT_DEBUG)
	_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
	_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
	_CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
	_CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_ALLOC_MEM_DF | _CRTDBG_CHECK_EVERY_1024_DF);
#endif
	const auto wanted = [&](const char* name) { return only == nullptr || std::strstr(name, only) != nullptr; };

	std::printf("CEVPlayerCore selftest, media dir: %s\n", g_mediaDir.c_str());
#if defined(CEV_CRT_DEBUG)
	_CrtMemState heapStart;
	_CrtMemCheckpoint(&heapStart);
#endif
	const auto begin = std::chrono::steady_clock::now();

	if (wanted("color")) TestColor();
	if (wanted("status")) TestStatusAndSources();

	for (const Clip& c : kClips)
	{
		FILE* f = std::fopen(ClipPath(c).c_str(), "rb");
		if (f == nullptr)
		{
			g_context = c.file;
			CHECKF(false, "missing clip %s", ClipPath(c).c_str());
			continue;
		}
		std::fclose(f);
	}
	if (g_failures != 0)
	{
		std::printf("clips missing, aborting\n");
		return 1;
	}

	if (wanted("demux"))
		for (const Clip& c : kClips)
			TestDemux(c);
	if (wanted("bad"))
		TestBadInput(ClipPath(*FindClip("vp9_odd_321x181.webm")));
	if (wanted("playback"))
		for (const Clip& c : kClips)
			TestPlayback(c);
	if (wanted("seek"))
	{
		TestSeek(*FindClip("vp8_vorbis.webm"), 2.5);
		TestSeek(*FindClip("av_sync_flash_beep.webm"), 5.0);
		TestSeek(*FindClip("audio_start_offset.webm"), 1.0);
		TestSeek(*FindClip("audio_start_offset.webm"), 4.2);
		TestSeek(*FindClip("vp9_alpha.webm"), 3.3);
		TestSeek(*FindClip("audio_en.webm"), 2.0);
	}
	if (wanted("loop"))
		TestLoop(*FindClip("vp9_odd_321x181.webm"));
	if (wanted("allocs"))
	{
		TestSteadyStateAllocations(*FindClip("vp9_alpha.webm"));
		TestSteadyStateAllocations(*FindClip("av_sync_flash_beep.webm"));
		TestSteadyStateAllocations(*FindClip("vp8_vorbis.webm"));
	}
	if (wanted("stress"))
		TestStress(stressIterations);

#if defined(CEV_CRT_DEBUG)
	{
		g_context = "crt-heap";
		std::string().swap(g_mediaDir);
		std::string().swap(g_context);
		CHECK(_CrtCheckMemory() != 0); // whole debug heap validated (guard bytes)
		_CrtMemState heapEnd, diff;
		_CrtMemCheckpoint(&heapEnd);
		const bool grew = _CrtMemDifference(&diff, &heapStart, &heapEnd) != 0 && static_cast<long long>(diff.lSizes[_NORMAL_BLOCK]) > 0;
		if (grew)
			_CrtMemDumpStatistics(&diff);
		g_context = "crt-heap";
		CHECKF(!grew, "debug heap grew by %lld bytes (leak)", static_cast<long long>(diff.lSizes[_NORMAL_BLOCK]));
		std::printf("  debug CRT heap: validated, leak diff %lld bytes\n", static_cast<long long>(diff.lSizes[_NORMAL_BLOCK]));
	}
#endif
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
	std::printf("\nSummary: %d checks, %d failures, %.1f s, total allocations %llu / frees %llu\n", g_checks, g_failures, secs,
		(unsigned long long)g_newCount.load(), (unsigned long long)g_deleteCount.load());
	std::printf(g_failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
	return g_failures == 0 ? 0 : 1;
}
