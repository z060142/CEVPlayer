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
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "CEVCore/AudioClock.h"
#include "CEVCore/AudioDecoder.h"
#include "CEVCore/ColorConvert.h"
#include "CEVCore/DecodePool.h"
#include "CEVCore/FrameRing.h"
#include "CEVCore/IByteSource.h"
#include "CEVCore/IClock.h"
#include "CEVCore/PlayerCore.h"
#include "CEVCore/Resampler.h"
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
	std::atomic<size_t> g_maxAlloc{0}; // largest single operator new request since ResetMaxAlloc()

	void NoteAlloc(size_t size)
	{
		size_t cur = g_maxAlloc.load(std::memory_order_relaxed);
		while (size > cur && !g_maxAlloc.compare_exchange_weak(cur, size, std::memory_order_relaxed)) {}
	}
	void ResetMaxAlloc() { g_maxAlloc.store(0); }
	size_t MaxAlloc() { return g_maxAlloc.load(); }

	uint64_t AllocCounter() { return g_newCount.load(std::memory_order_relaxed); }

	void* CountedAlloc(size_t size)
	{
		g_newCount.fetch_add(1, std::memory_order_relaxed);
		NoteAlloc(size);
		void* p = std::malloc(size != 0 ? size : 1);
		if (p == nullptr)
			std::abort();
		return p;
	}

	void* CountedAllocAligned(size_t size, size_t alignment)
	{
		g_newCount.fetch_add(1, std::memory_order_relaxed);
		NoteAlloc(size);
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
	// ---------------------------------------------------------------------------------------------------------------
	// Hardening regression tests: malformed files built in memory from a test clip.
	// ---------------------------------------------------------------------------------------------------------------
	constexpr size_t kAllocCap = 256u << 20; // largest single allocation any hardened load may make

	bool ReadClipBytes(const char* name, std::vector<uint8_t>& out)
	{
		std::unique_ptr<IByteSource> s = OpenSource(g_mediaDir + "/" + name, true);
		if (!s)
			return false;
		out.resize(static_cast<size_t>(s->Length()));
		return Succeeded(s->Read(0, out.data(), out.size()));
	}

	unsigned RawVintLen(uint8_t first)
	{
		if (first == 0)
			return 0;
		unsigned len = 1;
		for (uint8_t m = 0x80; (first & m) == 0; m >>= 1)
			++len;
		return len;
	}

	bool RawVint(const std::vector<uint8_t>& b, size_t pos, uint64_t& value, unsigned& len)
	{
		if (pos >= b.size())
			return false;
		len = RawVintLen(b[pos]);
		if (len == 0 || pos + len > b.size())
			return false;
		value = b[pos] & (0xFFu >> len);
		for (unsigned i = 1; i < len; ++i)
			value = (value << 8) | b[pos + i];
		return true;
	}

	struct RawBlock
	{
		size_t data = 0;      // first byte after the block's size field (track number vint)
		uint64_t size = 0;
		uint32_t track = 0;
		bool key = false;
	};

	struct RawLayout
	{
		size_t segmentSizePos = 0;
		unsigned segmentSizeLen = 0;
		size_t firstCluster = 0;
		std::vector<RawBlock> blocks; // every SimpleBlock of every cluster, in file order
	};

	// Minimal walk of a clip with known-size elements (as written by the clip generator).
	bool ParseLayout(const std::vector<uint8_t>& b, RawLayout& out)
	{
		size_t pos = 0;
		uint64_t size = 0;
		unsigned len = 0;
		// EBML header: 4 byte id + size
		if (b.size() < 8 || !RawVint(b, 4, size, len))
			return false;
		pos = 4 + len + static_cast<size_t>(size);
		if (pos + 4 > b.size() || b[pos] != 0x18 || b[pos + 1] != 0x53 || b[pos + 2] != 0x80 || b[pos + 3] != 0x67)
			return false;
		out.segmentSizePos = pos + 4;
		if (!RawVint(b, out.segmentSizePos, size, len))
			return false;
		out.segmentSizeLen = len;
		pos = out.segmentSizePos + len;
		while (pos + 5 <= b.size())
		{
			const unsigned idLen = RawVintLen(b[pos]);
			uint64_t esize = 0;
			unsigned slen = 0;
			if (idLen == 0 || idLen > 4 || !RawVint(b, pos + idLen, esize, slen))
				break;
			const size_t dataStart = pos + idLen + slen;
			const bool isCluster = idLen == 4 && b[pos] == 0x1F && b[pos + 1] == 0x43 && b[pos + 2] == 0xB6 && b[pos + 3] == 0x75;
			if (isCluster)
			{
				if (out.firstCluster == 0)
					out.firstCluster = pos;
				size_t cp = dataStart;
				const size_t cend = std::min<size_t>(b.size(), dataStart + static_cast<size_t>(esize));
				while (cp + 2 < cend)
				{
					const unsigned cidLen = RawVintLen(b[cp]);
					uint64_t csize = 0;
					unsigned cslen = 0;
					if (cidLen == 0 || !RawVint(b, cp + cidLen, csize, cslen))
						break;
					const size_t cdata = cp + cidLen + cslen;
					if (cidLen == 1 && b[cp] == 0xA3 && cdata + 4 <= cend)
					{
						uint64_t track = 0;
						unsigned tlen = 0;
						if (RawVint(b, cdata, track, tlen) && cdata + tlen + 3 <= b.size())
						{
							RawBlock rb;
							rb.data = cdata;
							rb.size = csize;
							rb.track = static_cast<uint32_t>(track);
							rb.key = (b[cdata + tlen + 2] & 0x80) != 0;
							out.blocks.push_back(rb);
						}
					}
					cp = cdata + static_cast<size_t>(csize);
				}
			}
			pos = dataStart + static_cast<size_t>(esize);
		}
		return out.firstCluster != 0 && !out.blocks.empty();
	}

	void SetUnknownSize(std::vector<uint8_t>& b, size_t pos, unsigned len)
	{
		b[pos] = static_cast<uint8_t>((0x80u >> (len - 1)) | ((1u << (8 - len)) - 1u));
		for (unsigned i = 1; i < len; ++i)
			b[pos + i] = 0xFF;
	}

	void AppendBytes(std::vector<uint8_t>& b, std::initializer_list<uint8_t> bytes) { b.insert(b.end(), bytes.begin(), bytes.end()); }

	// Appends a cluster that claims 0xF8000000 bytes with one SimpleBlock that claims `declaredBlockSize` bytes.
	void AppendBadCluster(std::vector<uint8_t>& b, uint8_t track, uint8_t flags, uint64_t declaredBlockSize,
		const std::vector<uint8_t>& payload)
	{
		AppendBytes(b, {0x1F, 0x43, 0xB6, 0x75, 0x01, 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00}); // Cluster, 8 byte size
		AppendBytes(b, {0xE7, 0x81, 0x00});                                                        // Timecode 0
		b.push_back(0xA3);                                                                        // SimpleBlock
		b.push_back(0x01);                                                                        // 8 byte size vint
		for (int i = 6; i >= 0; --i)
			b.push_back(static_cast<uint8_t>(declaredBlockSize >> (8 * i)));
		b.push_back(static_cast<uint8_t>(0x80 | track));
		AppendBytes(b, {0x00, 0x00, flags});
		b.insert(b.end(), payload.begin(), payload.end());
	}

	// Every indexed packet must lie inside the file and the size caps must hold.
	void CheckIndexBounded(const WebmDemuxer& d, uint64_t len)
	{
		bool ok = true;
		for (uint32_t i = 0; i < d.VideoPacketCount(); ++i)
		{
			const PacketRef& p = d.VideoPacket(i);
			ok = ok && p.offset <= len && p.size <= len - p.offset && p.size <= (64u << 20) && p.size > 0;
			ok = ok && (p.additionalSize == 0 || (p.additionalOffset <= len && p.additionalSize <= len - p.additionalOffset));
			ok = ok && p.ptsNs >= 0 && p.ptsNs <= 1000000000000000ll;
			ok = ok && (i == 0 || p.ptsNs >= d.VideoPacket(i - 1).ptsNs);
		}
		for (uint32_t i = 0; i < d.AudioPacketCount(); ++i)
		{
			const PacketRef& p = d.AudioPacket(i);
			ok = ok && p.offset <= len && p.size <= len - p.offset && p.size <= (1u << 20) && p.size > 0;
			ok = ok && p.ptsNs >= 0 && p.ptsNs <= 1000000000000000ll;
			ok = ok && (i == 0 || p.ptsNs >= d.AudioPacket(i - 1).ptsNs);
		}
		CHECK(ok);
		CHECKF(d.Video().maxPacketSize <= len && d.Audio().maxPacketSize <= len && d.Video().maxAdditionalSize <= len,
			"max packet sizes %u / %u / %u for a %llu byte file", d.Video().maxPacketSize, d.Audio().maxPacketSize,
			d.Video().maxAdditionalSize, static_cast<unsigned long long>(len));
	}

	// Opens the bytes with the demuxer (index must be bounded whether it opens or not), then loads them in a
	// player and checks that no allocation was out of proportion. Returns the load state the player reached.
	LoadState HardenedLoad(const std::vector<uint8_t>& bytes, bool* demuxOpened = nullptr)
	{
		const uint64_t len = bytes.size();
		{
			std::vector<uint8_t> copy = bytes;
			std::unique_ptr<WebmDemuxer> d;
			const Status st = WebmDemuxer::Open(std::make_unique<MemoryByteSource>(std::move(copy)), d, nullptr, nullptr);
			if (demuxOpened != nullptr)
				*demuxOpened = Succeeded(st);
			CHECK(Succeeded(st) == (d != nullptr));
			if (d)
				CheckIndexBounded(*d, len);
		}
		std::vector<uint8_t> copy = bytes;
		PlayerCore p;
		CHECK(Succeeded(p.Load(std::make_unique<MemoryByteSource>(std::move(copy)))));
		CHECK(WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 20000));
		const LoadState ls = p.GetLoadState();
		CHECK(ls == LoadState::Ready || ls == LoadState::Failed);
		p.Unload();
		return ls;
	}

	void TestMalformedIndex()
	{
		g_context = "malformed-index";
		std::vector<uint8_t> clip;
		CHECK(ReadClipBytes("vp8_vorbis.webm", clip));
		RawLayout lay;
		if (clip.empty() || !ParseLayout(clip, lay))
		{
			CHECK(false);
			return;
		}
		uint32_t videoTrack = 1, audioTrack = 2;
		{
			std::unique_ptr<WebmDemuxer> d;
			CHECK(Succeeded(WebmDemuxer::Open(std::make_unique<MemoryByteSource>(std::vector<uint8_t>(clip)), d, nullptr, nullptr)));
			if (d)
			{
				videoTrack = d->Video().trackNumber;
				audioTrack = d->Audio().trackNumber;
			}
		}

		// Header of the clip up to its first cluster, with the Segment size set to "unknown" so appended data is read.
		std::vector<uint8_t> head(clip.begin(), clip.begin() + static_cast<std::ptrdiff_t>(lay.firstCluster));
		SetUnknownSize(head, lay.segmentSizePos, lay.segmentSizeLen);
		std::vector<uint8_t> whole = clip;
		SetUnknownSize(whole, lay.segmentSizePos, lay.segmentSizeLen);

		const std::vector<uint8_t> junk(8, 0x55);
		struct Case { const char* name; uint8_t track; uint8_t flags; uint64_t declared; std::vector<uint8_t> payload; };
		const Case cases[] = {
			{"huge video block (4 GB claim)", static_cast<uint8_t>(videoTrack), 0x80, 0xF0000000ull, junk},
			{"huge audio block (4 GB claim)", static_cast<uint8_t>(audioTrack), 0x80, 0xF0000000ull, junk},
			{"fixed lacing, no lace byte (size == header)", static_cast<uint8_t>(audioTrack), 0x04, 4, {}},
			{"fixed lacing, 256 frames in 8 bytes", static_cast<uint8_t>(audioTrack), 0x04, 4 + 1 + 8, {0xFF, 1, 2, 3, 4, 5, 6, 7, 8}},
			{"xiph lacing runs off the block", static_cast<uint8_t>(videoTrack), 0x02, 4 + 1 + 4, {0x05, 0xFF, 0xFF, 0xFF, 0xFF}},
			{"ebml lacing with zero sized frames", static_cast<uint8_t>(audioTrack), 0x06, 4 + 1 + 4, {0x03, 0x80, 0x80, 0x80, 0x80}},
			{"xiph lacing with zero sized frames", static_cast<uint8_t>(audioTrack), 0x02, 4 + 1 + 4, {0x02, 0x00, 0x00, 0x01, 0x02}},
			{"video block larger than the file", static_cast<uint8_t>(videoTrack), 0x80, 0x01000000ull, junk},
		};
		for (const Case& c : cases)
		{
			g_context = std::string("malformed-index: ") + c.name;
			// (a) bad cluster only, (b) appended to the whole good clip
			std::vector<uint8_t> a = head;
			AppendBadCluster(a, c.track, c.flags, c.declared, c.payload);
			std::vector<uint8_t> w = whole;
			AppendBadCluster(w, c.track, c.flags, c.declared, c.payload);
			ResetMaxAlloc();
			bool opened = false;
			HardenedLoad(a, &opened);
			CHECK(!opened); // nothing valid in the file
			HardenedLoad(w, &opened);
			CHECK(opened);  // the good clusters before the damaged one stay playable
			CHECKF(MaxAlloc() < kAllocCap, "largest allocation %zu bytes", MaxAlloc());
		}

		// Truncation at many points: no packet may reach past the end of the file.
		g_context = "malformed-index: truncation";
		ResetMaxAlloc();
		for (int i = 1; i < 40; ++i)
		{
			std::vector<uint8_t> cut(clip.begin(), clip.begin() + static_cast<std::ptrdiff_t>(clip.size() * i / 40));
			HardenedLoad(cut);
		}
		// Deterministic byte mutations (header region and blocks).
		g_context = "malformed-index: mutations";
		uint32_t rng = 12345;
		const auto next = [&rng] { rng = rng * 1664525u + 1013904223u; return rng >> 8; };
		for (int i = 0; i < 120; ++i)
		{
			std::vector<uint8_t> m = clip;
			const int edits = 1 + static_cast<int>(next() % 4);
			for (int e = 0; e < edits; ++e)
			{
				if (next() % 2 == 0 && !lay.blocks.empty())
				{
					const RawBlock& blk = lay.blocks[next() % lay.blocks.size()];
					const size_t at = blk.data + next() % 6; // track, timecode, flags, first payload bytes
					if (at < m.size())
						m[at] = static_cast<uint8_t>(next());
				}
				else
				{
					m[next() % std::min<size_t>(m.size(), 1500)] = static_cast<uint8_t>(next());
				}
			}
			HardenedLoad(m);
		}
		CHECKF(MaxAlloc() < kAllocCap, "largest allocation %zu bytes", MaxAlloc());
	}

	// Vorbis identification header patched to absurd / supported sample rates; PCM ring stays bounded.
	void TestAudioRateAndRing()
	{
		g_context = "audio-rate";
		std::vector<uint8_t> clip;
		CHECK(ReadClipBytes("vp8_vorbis.webm", clip));
		size_t rateAt = 0;
		const uint8_t sig[7] = {1, 'v', 'o', 'r', 'b', 'i', 's'};
		for (size_t i = 0; i + 16 < clip.size(); ++i)
			if (std::memcmp(&clip[i], sig, 7) == 0)
			{
				rateAt = i + 7 + 4 + 1; // packet type + "vorbis", version, channels -> sample rate
				break;
			}
		CHECK(rateAt != 0);
		if (rateAt == 0)
			return;
		const auto patchRate = [&](uint32_t rate)
		{
			std::vector<uint8_t> m = clip;
			for (int i = 0; i < 4; ++i)
				m[rateAt + i] = static_cast<uint8_t>(rate >> (8 * i));
			return m;
		};
		for (uint32_t rate : {0x10000000u, 0xFFFFFFFFu, 384001u, 400000u})
		{
			g_context = "audio-rate " + std::to_string(rate);
			ResetMaxAlloc();
			std::vector<uint8_t> m = patchRate(rate);
			std::unique_ptr<WebmDemuxer> d;
			CHECK(Succeeded(WebmDemuxer::Open(std::make_unique<MemoryByteSource>(std::vector<uint8_t>(m)), d, nullptr, nullptr)));
			if (d && d->HasAudio())
			{
				std::unique_ptr<AudioDecoder> ad;
				CHECK(Failed(AudioDecoder::Create(d->Audio(), ad)));
			}
			PlayerCore p;
			CHECK(Succeeded(p.Load(std::make_unique<MemoryByteSource>(std::move(m)))));
			CHECK(WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 20000));
			CHECK(p.GetLoadState() == LoadState::Ready); // video plays, the unsupported audio is dropped
			CHECK(!p.GetInfo().hasAudio);
			p.Unload();
			CHECKF(MaxAlloc() < kAllocCap / 4, "largest allocation %zu bytes", MaxAlloc());
		}
		{
			g_context = "audio-rate 384000 + huge audioBufferMs";
			ResetMaxAlloc();
			PlayerConfig cfg;
			cfg.audioBufferMs = 0xFFFFFFF0u;
			PlayerCore p(cfg);
			CHECK(Succeeded(p.Load(std::make_unique<MemoryByteSource>(patchRate(384000)))));
			CHECK(WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 20000));
			CHECK(p.GetLoadState() == LoadState::Ready);
			CHECK(p.GetInfo().hasAudio && p.GetInfo().audioSampleRate == 384000);
			// the ring fills up and stops: at most 4 seconds of PCM
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			p.Update();
			const uint32_t buffered = p.AudioBufferedFrames();
			CHECKF(buffered > 0 && buffered <= 4u * 384000u, "buffered %u", buffered);
			p.Unload();
			CHECKF(MaxAlloc() < kAllocCap / 4, "largest allocation %zu bytes", MaxAlloc());
		}
		{
			g_context = "audio-ring cap at the clip's rate";
			PlayerConfig cfg;
			cfg.audioBufferMs = 0xFFFFFFF0u;
			PlayerCore p(cfg);
			CHECK(Succeeded(p.Load(std::make_unique<MemoryByteSource>(std::vector<uint8_t>(clip)))));
			CHECK(WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 20000));
			CHECK(p.GetLoadState() == LoadState::Ready);
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			p.Update();
			const uint32_t buffered = p.AudioBufferedFrames();
			CHECKF(buffered > 0 && buffered <= 4u * 44100u, "buffered %u", buffered);
		}
	}

	// Bogus timestamps: playback must still reach Ended (and loop).
	void TestBogusTimestamps()
	{
		g_context = "bogus-pts";
		std::vector<uint8_t> clip;
		CHECK(ReadClipBytes("vp8_vorbis.webm", clip));
		RawLayout lay;
		if (clip.empty() || !ParseLayout(clip, lay))
		{
			CHECK(false);
			return;
		}
		const uint32_t videoTrack = 1;
		std::vector<size_t> videoBlocks;
		for (size_t i = 0; i < lay.blocks.size(); ++i)
			if (lay.blocks[i].track == videoTrack)
				videoBlocks.push_back(i);
		CHECK(videoBlocks.size() > 30);
		if (videoBlocks.size() <= 30)
			return;

		struct Variant { const char* name; int block; uint8_t hi, lo; };
		const Variant variants[] = {
			{"video block 10 jumps +32.8 s", 10, 0x7F, 0xFF},
			{"video block 20 jumps -32.8 s", 20, 0x80, 0x00},
		};
		for (const Variant& v : variants)
		{
			g_context = std::string("bogus-pts: ") + v.name;
			std::vector<uint8_t> m = clip;
			const RawBlock& blk = lay.blocks[videoBlocks[static_cast<size_t>(v.block)]];
			m[blk.data + 1] = v.hi; // the track number is one byte (0x81) in this clip
			m[blk.data + 2] = v.lo;
			for (int mode = 0; mode < 2; ++mode) // 0: play to Ended, 1: loop
			{
				ManualClock clk;
				PlayerCore p(MakeConfig(clk));
				CHECK(Succeeded(p.Load(std::make_unique<MemoryByteSource>(std::vector<uint8_t>(m)))));
				CHECK(WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 20000));
				CHECK(p.GetLoadState() == LoadState::Ready);
				if (p.GetLoadState() != LoadState::Ready)
					continue;
				Sim sim;
				sim.Init(p, clk);
				p.SetLoop(mode == 1);
				CHECK(Succeeded(p.Play()));
				if (mode == 0)
				{
					const bool ended = sim.RunToEnd(p.GetInfo().durationSeconds + 5.0);
					CHECKF(ended && p.GetState() == PlayerState::Ended, "not ended: t=%.3f presented %llu", clk.NowSeconds(),
						static_cast<unsigned long long>(sim.presented));
					CHECK(sim.presented > 60);
				}
				else
				{
					const uint64_t limit = static_cast<uint64_t>((p.GetInfo().durationSeconds + 5.0) * 3 / sim.step) + 600;
					while (sim.steps < limit && p.GetLoopCount() < 2)
						if (!sim.Step())
							break;
					CHECKF(p.GetLoopCount() >= 2, "loop count %u", p.GetLoopCount());
				}
			}
		}

		// Timestamp overflow: the first cluster's Timecode rewritten to a huge 8 byte value; pts must stay
		// clamped, non-negative and non-decreasing (the signed multiplication used to overflow).
		{
			g_context = "bogus-pts: cluster timecode overflow";
			std::vector<uint8_t> m;
			const size_t cp = lay.firstCluster + 4;
			uint64_t csize = 0;
			unsigned clen = 0;
			CHECK(RawVint(clip, cp, csize, clen));
			const size_t dataStart = cp + clen;
			CHECK(clip[dataStart] == 0xE7);
			const size_t tcLen = 2 + (clip[dataStart + 1] & 0x7F);
			m.assign(clip.begin(), clip.begin() + static_cast<std::ptrdiff_t>(dataStart));
			AppendBytes(m, {0xE7, 0x88, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00});
			m.insert(m.end(), clip.begin() + static_cast<std::ptrdiff_t>(dataStart + tcLen), clip.end());
			SetUnknownSize(m, cp, clen);
			SetUnknownSize(m, lay.segmentSizePos, lay.segmentSizeLen);
			std::unique_ptr<WebmDemuxer> d;
			const Status st = WebmDemuxer::Open(std::make_unique<MemoryByteSource>(std::vector<uint8_t>(m)), d, nullptr, nullptr);
			if (Succeeded(st) && d)
			{
				CheckIndexBounded(*d, m.size());
				CHECK(d->HasVideo() && d->VideoPts(d->VideoPacketCount() - 1) <= d->DurationSeconds() + 1.0);
			}
			HardenedLoad(m);
		}
	}

	// Resolution switch mid-stream: the player must fail cleanly instead of freezing silently.
	void TestMidStreamResize()
	{
		g_context = "mid-stream-resize";
		std::vector<uint8_t> clip;
		CHECK(ReadClipBytes("vp8_vorbis.webm", clip));
		RawLayout lay;
		if (clip.empty() || !ParseLayout(clip, lay))
		{
			CHECK(false);
			return;
		}
		// VP8 keyframe payload: 3 byte frame tag, start code 9D 01 2A, 16 bit width, 16 bit height (14 bit + scale).
		int patched = 0;
		std::vector<uint8_t> m = clip;
		int seen = 0;
		for (const RawBlock& blk : lay.blocks)
		{
			if (blk.track != 1 || !blk.key)
				continue;
			const size_t p = blk.data + 4; // one byte track number + timecode + flags
			if (++seen >= 2 && p + 10 <= m.size() && m[p + 3] == 0x9D && m[p + 4] == 0x01 && m[p + 5] == 0x2A)
			{
				m[p + 6] = 0xA0; m[p + 7] = 0x00; // 160 x 90
				m[p + 8] = 0x5A; m[p + 9] = 0x00;
				++patched;
				break;
			}
		}
		if (patched == 0)
		{
			Log("  mid-stream-resize: clip has a single keyframe, skipped\n");
			return;
		}
		ManualClock clk;
		PlayerCore p(MakeConfig(clk));
		CHECK(Succeeded(p.Load(std::make_unique<MemoryByteSource>(std::move(m)))));
		CHECK(WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 20000));
		CHECK(p.GetLoadState() == LoadState::Ready);
		if (p.GetLoadState() != LoadState::Ready)
			return;
		CHECK(Succeeded(p.Play()));
		const auto start = std::chrono::steady_clock::now();
		while (p.GetLoadState() == LoadState::Ready && p.GetState() != PlayerState::Ended &&
			std::chrono::steady_clock::now() - start < std::chrono::seconds(10))
		{
			clk.Advance(1.0 / 60.0);
			p.Update();
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		CHECK(p.GetLoadState() == LoadState::Failed);
		CHECK(p.GetLoadError().status == Status::Unsupported);
		CHECK(p.GetLoadError().detail == StatusDetail::StreamChanged);
		p.Unload();
		CHECK(p.GetLoadState() == LoadState::Unloaded);
	}

	// ReadAudio() from its own thread while the consumer thread loads / unloads (the engine's audio callback).
	void TestAudioThreadVersusUnload()
	{
		g_context = "audio-thread";
		std::vector<uint8_t> clip;
		CHECK(ReadClipBytes("vp8_vorbis.webm", clip));
		PlayerConfig cfg;
		cfg.audioBufferMs = 100;
		PlayerCore p(cfg);
		std::atomic<bool> stop{false};
		std::atomic<uint64_t> calls{0};
		std::thread audio([&]
		{
			std::vector<float> buf(2048 * 8);
			while (!stop.load())
			{
				p.ReadAudio(buf.data(), 512);
				calls.fetch_add(1);
			}
		});
		for (int i = 0; i < 25; ++i)
		{
			CHECK(Succeeded(p.Load(std::make_unique<MemoryByteSource>(std::vector<uint8_t>(clip)))));
			if (i % 3 != 0)
				WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 20000);
			if (p.GetLoadState() == LoadState::Ready)
			{
				p.Play();
				std::this_thread::sleep_for(std::chrono::milliseconds(2 + i % 5));
				p.Update();
			}
			if (i % 4 != 3)
				p.Unload();
		}
		p.Unload();
		stop = true;
		audio.join();
		CHECK(calls.load() > 0);
	}

	// Byte source that throws (bad_alloc / runtime_error) after a number of reads: exceptions on worker threads
	// must end up as a failed / ended player, never std::terminate.
	class ThrowingSource final : public IByteSource
	{
	public:
		ThrowingSource(std::vector<uint8_t> data, uint32_t limit, bool badAlloc, std::atomic<uint32_t>* reads)
			: m_inner(std::move(data)), m_limit(limit), m_badAlloc(badAlloc), m_reads(reads) {}
		Status Read(uint64_t offset, void* dst, size_t size) override
		{
			if (m_reads->fetch_add(1) >= m_limit)
			{
				if (m_badAlloc)
					throw std::bad_alloc();
				throw std::runtime_error("source failure");
			}
			return m_inner.Read(offset, dst, size);
		}
		uint64_t Length() const override { return m_inner.Length(); }

	private:
		MemoryByteSource m_inner;
		uint32_t m_limit;
		bool m_badAlloc;
		std::atomic<uint32_t>* m_reads;
	};

	void TestWorkerExceptions()
	{
		g_context = "worker-exceptions";
		std::vector<uint8_t> clip;
		CHECK(ReadClipBytes("vp8_vorbis.webm", clip));
		// reads needed to reach Ready
		std::atomic<uint32_t> readsAtReady{0};
		{
			ManualClock clk;
			PlayerCore p(MakeConfig(clk));
			CHECK(Succeeded(p.Load(std::make_unique<ThrowingSource>(clip, 0xFFFFFFFFu, true, &readsAtReady))));
			CHECK(WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 20000));
			CHECK(p.GetLoadState() == LoadState::Ready);
		}
		const uint32_t loadReads = readsAtReady.load();
		for (int kind = 0; kind < 2; ++kind)
		{
			const bool badAlloc = kind == 0;
			g_context = badAlloc ? "worker-exceptions bad_alloc" : "worker-exceptions runtime_error";
			for (uint32_t limit : {0u, 1u, 7u, loadReads / 2})
			{
				std::atomic<uint32_t> reads{0};
				PlayerCore p;
				CHECK(Succeeded(p.Load(std::make_unique<ThrowingSource>(clip, limit, badAlloc, &reads))));
				CHECK(WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 20000));
				CHECK(p.GetLoadState() == LoadState::Failed);
				CHECK(p.GetLoadError().status != Status::Ok);
				p.Unload();
			}
			// throws after Ready, during playback: the stream counts as ended, the process survives
			std::atomic<uint32_t> reads{0};
			ManualClock clk;
			PlayerCore p(MakeConfig(clk));
			CHECK(Succeeded(p.Load(std::make_unique<ThrowingSource>(clip, loadReads + 5, badAlloc, &reads))));
			CHECK(WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 20000));
			CHECK(p.GetLoadState() == LoadState::Ready);
			if (p.GetLoadState() == LoadState::Ready)
			{
				CHECK(Succeeded(p.Play()));
				const auto start = std::chrono::steady_clock::now();
				while (p.GetState() != PlayerState::Ended && std::chrono::steady_clock::now() - start < std::chrono::seconds(10))
				{
					clk.Advance(1.0 / 60.0);
					p.Update();
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				CHECK(p.GetState() == PlayerState::Ended);
				CHECK(p.DecodeErrors() >= 1);
			}
		}
	}

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

	// =================================================================================================================
	// M2a: Resampler, AudioClock, ReadAudioResampled
	// =================================================================================================================
	constexpr double kTwoPi = 6.283185307179586;

	struct SineStats
	{
		bool finite = true;
		double freq = 0.0;
		double amp = 0.0;       // sqrt(2 * mean(x^2)) of the left channel
		double maxDelta = 0.0;  // max |x[i] - x[i-1]| of the left channel
	};

	// Left channel of interleaved stereo `st`, frames [head, count - tail).
	SineStats AnalyzeSine(const float* st, size_t count, size_t head, size_t tail, double sampleRate)
	{
		SineStats r;
		if (count <= head + tail + 8)
			return r;
		const size_t end = count - tail;
		double sumSq = 0.0;
		int crossings = 0;
		double firstCross = 0.0, lastCross = 0.0;
		for (size_t i = head; i < end; ++i)
		{
			const double x = st[2 * i];
			if (!std::isfinite(x) || !std::isfinite(st[2 * i + 1]))
				r.finite = false;
			sumSq += x * x;
			if (i > head)
			{
				const double p = st[2 * (i - 1)];
				r.maxDelta = std::max(r.maxDelta, std::fabs(x - p));
				if (p < 0.0 && x >= 0.0)
				{
					const double pos = static_cast<double>(i - 1) + (-p) / (x - p);
					if (crossings == 0)
						firstCross = pos;
					lastCross = pos;
					++crossings;
				}
			}
		}
		r.amp = std::sqrt(2.0 * sumSq / static_cast<double>(end - head));
		if (crossings >= 2)
			r.freq = (crossings - 1) * sampleRate / (lastCross - firstCross);
		return r;
	}

	uint32_t Lcg(uint32_t& s)
	{
		s = s * 1664525u + 1013904223u;
		return s >> 8;
	}

	// Runs `in` (srcCh interleaved) through a fresh resampler in chunks of random size. Returns stereo output.
	std::vector<float> RunResampler(Resampler& r, const std::vector<float>& in, uint32_t srcCh, bool randomChunks, bool flush)
	{
		const size_t frames = in.size() / srcCh;
		std::vector<float> out;
		out.reserve(static_cast<size_t>(frames / std::max(0.05, r.Step()) + 4096) * 2);
		std::vector<float> tmp(2 * 1024);
		uint32_t seed = 12345;
		size_t ci = 0;
		int guard = 0;
		while (ci < frames && guard++ < 10000000)
		{
			const uint32_t chunk = randomChunks ? 1 + Lcg(seed) % 700 : 1024;
			const uint32_t outCap = randomChunks ? 1 + Lcg(seed) % 900 : 1024;
			const uint32_t n = static_cast<uint32_t>(std::min<size_t>(chunk, frames - ci));
			uint32_t consumed = 0, produced = 0;
			CHECK(Succeeded(r.Process(in.data() + ci * srcCh, n, tmp.data(), outCap, &consumed, &produced)));
			out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<ptrdiff_t>(produced) * 2);
			ci += consumed;
		}
		if (flush)
		{
			uint32_t got = 0;
			do
			{
				got = r.Pull(tmp.data(), 1024); // input may still be buffered (small output capacity above)
				out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<ptrdiff_t>(got) * 2);
			} while (got > 0);
			r.Flush();
			do
			{
				got = r.Pull(tmp.data(), 1024);
				out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<ptrdiff_t>(got) * 2);
			} while (got > 0);
		}
		return out;
	}

	void CheckSine(uint32_t srcRate, double rate, double freq, uint32_t dstRate = 48000)
	{
		char ctx[96];
		std::snprintf(ctx, sizeof(ctx), "resampler %u->%u rate %.2f", srcRate, dstRate, rate);
		g_context = ctx;
		Resampler r;
		CHECK(Succeeded(r.Init(srcRate, 1, dstRate, 2, 1024)));
		r.SetRate(rate);
		const size_t n = srcRate; // one second
		std::vector<float> in(n);
		for (size_t i = 0; i < n; ++i)
			in[i] = static_cast<float>(0.5 * std::sin(kTwoPi * freq * static_cast<double>(i) / srcRate));

		const std::vector<float> out = RunResampler(r, in, 1, true, true);
		const size_t frames = out.size() / 2;
		const double expectFrames = static_cast<double>(n) / r.Step();
		CHECKF(std::fabs(static_cast<double>(frames) - expectFrames) <= 3.0, "output %zu frames, expected %.1f", frames, expectFrames);
		const SineStats s = AnalyzeSine(out.data(), frames, 128, 128, dstRate);
		const double ideal = freq * rate; // Hz heard at the output rate: stream plays `rate` times faster
		CHECK(s.finite);
		CHECKF(std::fabs(s.freq - ideal) < ideal * 0.005, "frequency %.2f Hz, expected %.2f", s.freq, ideal);
		const double db = 20.0 * std::log10(std::max(1e-9, s.amp / 0.5));
		CHECKF(std::fabs(db) < 1.0, "amplitude %.4f (%.2f dB)", s.amp, db);
		const double bound = 1.05 * 0.5 * kTwoPi * ideal / dstRate + 1e-4;
		CHECKF(s.maxDelta <= bound, "max sample delta %.5f > %.5f (click)", s.maxDelta, bound);
		// right channel is a copy of the left one
		bool same = true;
		for (size_t i = 0; i < frames && same; ++i)
			same = out[2 * i] == out[2 * i + 1];
		CHECK(same);

		// Chunking must not change the signal: compare with 1024 sized blocks.
		Resampler ref;
		CHECK(Succeeded(ref.Init(srcRate, 1, dstRate, 2, 1024)));
		ref.SetRate(rate);
		const std::vector<float> out2 = RunResampler(ref, in, 1, false, true);
		CHECK(out2.size() / 2 + 1 >= frames && frames + 1 >= out2.size() / 2);
		double maxDiff = 0.0;
		for (size_t i = 0; i < std::min(out.size(), out2.size()); ++i)
			maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(out[i] - out2[i])));
		CHECKF(maxDiff < 1e-4, "block boundary difference %.2e", maxDiff);
		Log("  %-34s ok: %zu frames, %.2f Hz, %.3f dB, max delta %.4f (bound %.4f)\n", ctx, frames, s.freq, db, s.maxDelta, bound);
	}

	void TestResampler()
	{
		g_context = "resampler";
		// Rates required by the spec: 44.1k and 22.05k to 48k, identity, plus the varispeed range.
		CheckSine(44100, 1.0, 1000.0);
		CheckSine(48000, 1.0, 1000.0);
		CheckSine(22050, 1.0, 1000.0);
		CheckSine(44100, 0.5, 1000.0);
		CheckSine(44100, 1.5, 1000.0);
		CheckSine(48000, 2.0, 1000.0);
		CheckSine(48000, 0.5, 1000.0);
		CheckSine(96000, 1.0, 1000.0);   // downsampling (anti-alias kernel)
		CheckSine(16000, 1.25, 700.0);

		// 48k -> 48k at rate 1 is a bit exact pass through.
		{
			g_context = "resampler passthrough";
			Resampler r;
			CHECK(Succeeded(r.Init(48000, 2, 48000, 2, 512)));
			std::vector<float> in(2 * 5000);
			for (size_t i = 0; i < in.size(); ++i)
				in[i] = static_cast<float>(std::sin(0.01 * static_cast<double>(i)));
			const std::vector<float> out = RunResampler(r, in, 2, true, true);
			CHECK(out.size() == in.size());
			CHECK(out == in);
		}

		// Rate change while streaming (varispeed sweep): no discontinuity, nothing non finite.
		{
			g_context = "resampler rate sweep";
			Resampler r;
			CHECK(Succeeded(r.Init(44100, 1, 48000, 2, 1024)));
			std::vector<float> in(44100);
			for (size_t i = 0; i < in.size(); ++i)
				in[i] = static_cast<float>(0.5 * std::sin(kTwoPi * 800.0 * static_cast<double>(i) / 44100.0));
			std::vector<float> out;
			std::vector<float> tmp(2 * 2048);
			size_t ci = 0;
			double rate = 1.0;
			while (ci < in.size())
			{
				rate = 0.6 + 1.2 * std::min(1.0, static_cast<double>(ci) / in.size()); // 0.6 .. 1.8
				r.SetRate(rate);
				const uint32_t n = static_cast<uint32_t>(std::min<size_t>(480, in.size() - ci));
				uint32_t consumed = 0, produced = 0;
				CHECK(Succeeded(r.Process(in.data() + ci, n, tmp.data(), 1024, &consumed, &produced)));
				out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<ptrdiff_t>(produced) * 2);
				ci += consumed;
			}
			const SineStats s = AnalyzeSine(out.data(), out.size() / 2, 128, 2048, 48000.0);
			const double bound = 1.05 * 0.5 * kTwoPi * 800.0 * 1.8 / 48000.0 + 1e-4;
			CHECK(s.finite);
			CHECKF(s.maxDelta <= bound, "max delta %.5f > %.5f", s.maxDelta, bound);
			CHECKF(s.amp > 0.45 && s.amp < 0.55, "amp %.3f", s.amp);
		}

		// Channel layouts.
		{
			g_context = "resampler channels";
			const double f = 1000.0;
			const size_t n = 44100;
			// mono: both channels equal
			{
				Resampler r;
				CHECK(Succeeded(r.Init(44100, 1, 48000, 2, 1024)));
				std::vector<float> in(n);
				for (size_t i = 0; i < n; ++i)
					in[i] = static_cast<float>(0.5 * std::sin(kTwoPi * f * static_cast<double>(i) / 44100.0));
				const std::vector<float> out = RunResampler(r, in, 1, false, true);
				const SineStats s = AnalyzeSine(out.data(), out.size() / 2, 128, 128, 48000.0);
				CHECKF(std::fabs(s.amp - 0.5) < 0.01, "mono amp %.3f", s.amp);
				bool same = true;
				for (size_t i = 0; i < out.size() / 2; ++i)
					same = same && out[2 * i] == out[2 * i + 1];
				CHECK(same);
			}
			// 6 channels (Vorbis order FL FC FR BL BR LFE): drive one channel at a time
			float cl[8], cr[8];
			Resampler::DownmixCoefficients(6, cl, cr, 8);
			CHECKF(std::fabs(cl[0] - cr[2]) < 1e-6f && cl[2] == 0.0f && cr[0] == 0.0f && cl[5] == 0.0f && cr[5] == 0.0f, "downmix matrix");
			float sumL = 0.0f;
			for (int c = 0; c < 6; ++c)
				sumL += cl[c];
			CHECKF(sumL <= 1.0001f, "downmix would clip: %.4f", sumL);
			const struct { uint32_t ch; const char* name; } probes[] = {{0, "FL"}, {1, "FC"}, {2, "FR"}, {3, "BL"}, {4, "BR"}, {5, "LFE"}};
			for (const auto& pr : probes)
			{
				Resampler r;
				CHECK(Succeeded(r.Init(44100, 6, 48000, 2, 1024)));
				std::vector<float> in(n * 6, 0.0f);
				for (size_t i = 0; i < n; ++i)
					in[i * 6 + pr.ch] = static_cast<float>(0.5 * std::sin(kTwoPi * f * static_cast<double>(i) / 44100.0));
				const std::vector<float> out = RunResampler(r, in, 6, true, true);
				double sl = 0.0, sr = 0.0;
				const size_t frames = out.size() / 2;
				for (size_t i = 200; i + 200 < frames; ++i)
				{
					sl += static_cast<double>(out[2 * i]) * out[2 * i];
					sr += static_cast<double>(out[2 * i + 1]) * out[2 * i + 1];
				}
				const double cnt = static_cast<double>(frames - 400);
				const double ampL = std::sqrt(2.0 * sl / cnt), ampR = std::sqrt(2.0 * sr / cnt);
				const double wantL = 0.5 * cl[pr.ch], wantR = 0.5 * cr[pr.ch];
				CHECKF(std::fabs(ampL - wantL) < 0.01 && std::fabs(ampR - wantR) < 0.01, "6ch %s: L %.4f (want %.4f) R %.4f (want %.4f)",
					pr.name, ampL, wantL, ampR, wantR);
			}
			// 6 channels, all driven in phase: no clipping beyond full scale
			{
				Resampler r;
				CHECK(Succeeded(r.Init(48000, 6, 48000, 2, 256)));
				std::vector<float> in(6 * 1000, 1.0f);
				const std::vector<float> out = RunResampler(r, in, 6, false, false);
				float mx = 0.0f;
				for (float v : out)
					mx = std::max(mx, std::fabs(v));
				CHECKF(mx <= 1.0001f && mx > 0.5f, "full scale 6ch sum %.4f", mx);
			}
		}

		// API edges
		{
			g_context = "resampler api";
			Resampler r;
			uint32_t c = 0, p = 0;
			float buf[8] = {};
			CHECK(r.Process(buf, 1, buf, 1, &c, &p) == Status::InvalidState);
			CHECK(r.Init(500, 2) == Status::InvalidArgument);
			CHECK(r.Init(44100, 0) == Status::InvalidArgument);
			CHECK(r.Init(44100, 2, 48000, 3) == Status::InvalidArgument);
			CHECK(r.Init(44100, 2, 48000, 2, 0) == Status::InvalidArgument);
			CHECK(!r.IsInitialized());
			CHECK(Succeeded(r.Init(44100, 2, 48000, 1, 64)));
			CHECK(r.Process(nullptr, 4, buf, 1, &c, &p) == Status::InvalidArgument);
			r.SetRate(100.0);
			CHECK(r.Rate() == Resampler::kMaxRate);
			r.SetRate(0.0001);
			CHECK(r.Rate() == Resampler::kMinRate);
			r.SetRate(-1.0);
			CHECK(r.Rate() == Resampler::kMinRate);
			r.Shutdown();
			CHECK(!r.IsInitialized());
			CHECK(r.Pull(buf, 4) == 0);
		}

		// Reset drops the history: the first output after Reset() is the new first input.
		{
			g_context = "resampler reset";
			Resampler r;
			CHECK(Succeeded(r.Init(44100, 2, 48000, 2, 512)));
			std::vector<float> loud(2 * 512, 1.0f), quiet(2 * 512, 0.0f);
			float tmp[2 * 2048];
			uint32_t c = 0, p = 0;
			CHECK(Succeeded(r.Process(loud.data(), 512, tmp, 2048, &c, &p)));
			r.Reset();
			CHECK(Succeeded(r.Process(quiet.data(), 512, tmp, 2048, &c, &p)));
			float mx = 0.0f;
			for (uint32_t i = 0; i < p * 2; ++i)
				mx = std::max(mx, std::fabs(tmp[i]));
			CHECKF(p > 400 && mx == 0.0f, "stale output after Reset: %u frames, peak %f", p, mx);
		}

		// Steady state: no allocation in Push / Pull / Process / SetRate / Reset.
		{
			g_context = "resampler allocs";
			Resampler r;
			CHECK(Succeeded(r.Init(44100, 6, 48000, 2, 1024)));
			std::vector<float> in(6 * 1024, 0.25f);
			std::vector<float> out(2 * 2048);
			uint32_t c = 0, p = 0;
			const uint64_t n0 = g_newCount.load();
			for (int i = 0; i < 200; ++i)
			{
				r.SetRate(0.5 + 0.0075 * i);
				if (i == 100)
					r.Reset();
				CHECK(Succeeded(r.Process(in.data(), 1024, out.data(), 2048, &c, &p)));
				r.Flush();
			}
			const uint64_t n1 = g_newCount.load();
			CHECKF(n1 == n0, "%llu allocations in the resampler", (unsigned long long)(n1 - n0));
		}
		Log("  resampler tests done\n");
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Virtual time + simulated audio device
	// ---------------------------------------------------------------------------------------------------------------
	struct VirtualTime
	{
		std::atomic<int64_t> ns{1000000000};
		static int64_t Now(void* user) { return static_cast<VirtualTime*>(user)->ns.load(std::memory_order_relaxed); }
	};

	void TestAudioClockUnit()
	{
		g_context = "audioclock unit";
		VirtualTime vt;
		AudioClock clk(48000);
		clk.SetTimeSource(&VirtualTime::Now, &vt);
		const int64_t base = vt.ns.load();
		const auto tAt = [&](double sec) { return base + static_cast<int64_t>(sec * 1e9); };
		const auto callbacks = [&](double from, double to) // 10 ms blocks of 480 frames at from, from + 10 ms, ...
		{
			for (double t = from; t < to - 1e-9; t += 0.01)
			{
				vt.ns = tAt(t);
				clk.OnFramesConsumed(480);
			}
		};

		// no latency: the clock follows the callbacks
		clk.Start();
		CHECK(clk.IsRunning());
		callbacks(0.0, 1.0);
		vt.ns = tAt(1.0);
		const double t1 = clk.NowSeconds();
		CHECKF(std::fabs(t1 - 1.0) < 0.02, "clock %.4f s, expected ~1.0", t1);

		// latency of 960 frames (20 ms): media time lags by 20 ms (minus the block delivered at the last callback)
		clk.SetOutputLatencyFrames(960);
		clk.Seek(0.0);
		callbacks(1.0, 2.0);
		vt.ns = tAt(2.0);
		const double t2 = clk.NowSeconds();
		CHECKF(std::fabs(t2 - (1.0 - 0.02)) < 0.02, "clock %.4f s with 20 ms latency, expected ~0.98", t2);

		// pause: frozen while callbacks continue; resume continues
		clk.Pause();
		CHECK(!clk.IsRunning());
		const double tp = clk.NowSeconds();
		callbacks(2.0, 3.0);
		vt.ns = tAt(3.0);
		CHECK(clk.NowSeconds() == tp);
		clk.Resume();
		CHECK(clk.IsRunning());
		callbacks(3.0, 4.0);
		vt.ns = tAt(4.0);
		const double t3 = clk.NowSeconds();
		CHECKF(std::fabs((t3 - tp) - 0.98) < 0.03, "resumed clock advanced %.4f s, expected ~0.98", t3 - tp);

		// seek keeps running, rate 2 doubles the speed, rate 0.5 halves it
		clk.Seek(10.0);
		CHECK(clk.NowSeconds() == 10.0);
		clk.SetRate(2.0);
		CHECK(clk.Rate() == 2.0);
		callbacks(4.0, 5.0);
		vt.ns = tAt(5.0);
		const double t4 = clk.NowSeconds();
		CHECKF(std::fabs((t4 - 10.0) - 1.96) < 0.06, "rate 2: advanced %.4f s, expected ~1.96", t4 - 10.0);
		clk.SetRate(0.5);
		const double t5a = clk.NowSeconds();
		callbacks(5.0, 7.0);
		vt.ns = tAt(7.0);
		const double t5 = clk.NowSeconds();
		CHECKF(std::fabs((t5 - t5a) - 1.0) < 0.05, "rate 0.5: advanced %.4f s over 2 s, expected ~1.0", t5 - t5a);
		clk.SetRate(1.0);
		CHECK(clk.Rate() == 1.0);
		clk.SetRate(-3.0); // rejected
		CHECK(clk.Rate() == 1.0);

		// callbacks stop (device lost / no data): the clock continues on the wall clock, monotonic
		clk.Seek(20.0);
		callbacks(7.0, 8.0);
		double last = clk.NowSeconds();
		double worstStep = 0.0;
		for (double t = 8.0; t < 11.0; t += 1.0 / 60.0)
		{
			vt.ns = tAt(t);
			const double now = clk.NowSeconds();
			CHECK(now >= last);
			worstStep = std::max(worstStep, now - last);
			last = now;
		}
		vt.ns = tAt(11.0);
		last = clk.NowSeconds();
		CHECKF(std::fabs((last - 20.0) - 3.98) < 0.1, "stalled: advanced %.4f s over 4 s, expected ~3.98", last - 20.0);
		CHECKF(worstStep < 0.03, "stalled clock step %.4f s", worstStep);
		CHECK(clk.IsStalled());
		// callbacks come back: no jump backwards, normal speed again
		callbacks(11.0, 12.0);
		vt.ns = tAt(12.0);
		const double back = clk.NowSeconds();
		CHECK(back >= last);
		CHECKF(std::fabs((back - last) - 1.0) < 0.1, "after the stall the clock advanced %.4f s over 1 s", back - last);
		CHECK(!clk.IsStalled());

		// no device at all: wall clock from Start()
		AudioClock noDevice;
		noDevice.SetTimeSource(&VirtualTime::Now, &vt);
		noDevice.Start();
		const double n0 = noDevice.NowSeconds();
		vt.ns = vt.ns.load() + 1000000000;
		const double n1 = noDevice.NowSeconds();
		CHECKF(std::fabs((n1 - n0) - 1.0) < 1e-3, "no device: advanced %.4f s", n1 - n0);
		noDevice.Pause();
		vt.ns = vt.ns.load() + 1000000000;
		CHECK(std::fabs(noDevice.NowSeconds() - n1) < 1e-3);
		noDevice.SetRate(2.0);
		noDevice.Resume();
		vt.ns = vt.ns.load() + 500000000;
		CHECKF(std::fabs((noDevice.NowSeconds() - n1) - 1.0) < 1e-3, "no device, rate 2 advanced %.4f s", noDevice.NowSeconds() - n1);
		Log("  audioclock unit ok\n");
	}

	// Simulated audio device and 60 Hz render loop on a virtual time line (deterministic, faster than real time).
	// Audio callbacks run every `block` frames of the output rate, the consumer polls every 1/60 s. The device
	// model: the audio of a block is heard `pipeFrames` frames after its callback ran.
	struct AvSim
	{
		PlayerCore* p = nullptr;
		AudioClock* clk = nullptr;
		VirtualTime* vt = nullptr;
		static constexpr uint32_t kBlock = 480;
		static constexpr uint32_t kPipeFrames = 480;
		static constexpr double kOutRate = 48000.0;
		std::vector<float> buf;
		int64_t t0 = 0;
		uint64_t cbIndex = 0;
		uint64_t pollIndex = 0;
		uint32_t minBuffered = 1400;

		uint64_t realFrames = 0;
		uint64_t presented = 0;
		bool timedOut = false;
		bool nonFinite = false;
		float peak = 0.0f;
		uint64_t quietRun = 1u << 30;
		uint64_t onsetFrame[64];
		int onsetCount = 0;
		double flashPts[64];
		int64_t flashPresentedNs[64];
		int flashCount = 0;
		bool detectFlash = false;

		// capture of the stereo output
		float* cap = nullptr;
		size_t capFrames = 0;
		size_t capCount = 0;

		void Init(PlayerCore& player, AudioClock& clock, VirtualTime& time)
		{
			p = &player;
			clk = &clock;
			vt = &time;
			buf.assign(kBlock * 2, 0.0f);
			clk->SetTimeSource(&VirtualTime::Now, vt);
			clk->SetOutputLatencyFrames(kPipeFrames + kBlock);
			t0 = vt->ns.load();
		}

		int64_t CallbackNs(uint64_t k) const { return t0 + static_cast<int64_t>(static_cast<double>(k) * kBlock * 1e9 / kOutRate + 0.5); }
		int64_t PollNs(uint64_t j) const { return t0 + static_cast<int64_t>(static_cast<double>(j) * 1e9 / 60.0 + 0.5); }
		// virtual time at which output frame n (counting from the first callback) is heard
		int64_t HeardNs(uint64_t n) const { return t0 + static_cast<int64_t>(static_cast<double>(n + kPipeFrames) * 1e9 / kOutRate + 0.5); }

		void AudioCallback()
		{
			const MediaInfo& info = p->GetInfo();
			if (info.hasAudio && p->GetState() == PlayerState::Playing)
			{
				if (!WaitFor([&] { return p->AudioBufferedFrames() >= minBuffered || p->AudioDecoderAtEnd(); }, 15000))
					timedOut = true;
			}
			const uint32_t got = p->ReadAudioResampled(buf.data(), kBlock);
			realFrames += got;
			const uint64_t base = cbIndex * kBlock;
			for (uint32_t i = 0; i < kBlock; ++i)
			{
				const float l = buf[2 * i], r = buf[2 * i + 1];
				if (!std::isfinite(l) || !std::isfinite(r))
					nonFinite = true;
				const float m = std::max(std::fabs(l), std::fabs(r));
				peak = std::max(peak, m);
				if (m > 0.1f)
				{
					if (quietRun >= 24000 && onsetCount < 64)
						onsetFrame[onsetCount++] = base + i;
					quietRun = 0;
				}
				else
				{
					++quietRun;
				}
				if (cap != nullptr && capCount < capFrames)
				{
					cap[2 * capCount] = l;
					cap[2 * capCount + 1] = r;
					++capCount;
				}
			}
			clk->OnFramesConsumed(kBlock);
			++cbIndex;
		}

		void VideoPoll(int64_t ns)
		{
			const MediaInfo& info = p->GetInfo();
			if (info.hasVideo && p->GetState() == PlayerState::Playing)
			{
				if (!WaitFor([&] { return p->VideoBufferedFrame() || p->VideoDecoderAtEnd(); }, 15000))
					timedOut = true;
			}
			if (p->Update())
			{
				++presented;
				if (detectFlash && flashCount < 64)
				{
					FrameView fv;
					if (p->GetFrame(fv) && fv.plane[0] != nullptr &&
						fv.plane[0][static_cast<ptrdiff_t>(fv.height / 2) * fv.stride[0] + fv.width / 2] > 200)
					{
						flashPts[flashCount] = fv.pts;
						flashPresentedNs[flashCount] = ns;
						++flashCount;
					}
				}
			}
		}

		// Runs the next event (audio callback or video poll, whichever is first in virtual time).
		bool Step()
		{
			const int64_t ta = CallbackNs(cbIndex);
			const int64_t tv = PollNs(pollIndex);
			if (ta <= tv)
			{
				vt->ns = ta;
				AudioCallback();
			}
			else
			{
				vt->ns = tv;
				VideoPoll(tv);
				++pollIndex;
			}
			return !timedOut;
		}

		bool RunToEnd(double maxVirtualSeconds)
		{
			const int64_t limit = t0 + static_cast<int64_t>(maxVirtualSeconds * 1e9);
			while (vt->ns.load() < limit)
			{
				if (!Step())
					return false;
				if (p->GetState() == PlayerState::Ended)
					return true;
			}
			return false;
		}

		bool RunUntilPresented(uint64_t target)
		{
			const int64_t limit = vt->ns.load() + 120000000000ll;
			while (presented < target && vt->ns.load() < limit)
				if (!Step())
					return false;
			return presented >= target;
		}

		bool RunUntilMediaTime(double t)
		{
			const int64_t limit = vt->ns.load() + 120000000000ll;
			while (clk->NowSeconds() < t && vt->ns.load() < limit)
				if (!Step())
					return false;
			return clk->NowSeconds() >= t;
		}

		bool RunUntilCaptured()
		{
			while (capCount < capFrames)
				if (!Step())
					return false;
			return true;
		}
	};

	PlayerConfig MakeResampledConfig(AudioClock& clk)
	{
		PlayerConfig cfg;
		cfg.externalClock = &clk;
		cfg.outputSampleRate = 48000;
		cfg.enableStats = true;
		return cfg;
	}

	// Pause / Resume / Seek / SetRate reach the AudioClock through PlayerCore.
	void TestAudioClockWiring(const Clip& clip)
	{
		g_context = std::string("audioclock wiring ") + clip.file;
		VirtualTime vt;
		AudioClock clk(48000);
		PlayerCore p(MakeResampledConfig(clk));
		if (!LoadAndWait(p, ClipPath(clip), true))
			return;
		CHECK(p.GetInfo().outputAudioSampleRate == 48000 && p.GetInfo().outputAudioChannels == 2);
		AvSim s;
		s.Init(p, clk, vt);
		CHECK(p.Update());
		CHECK(!clk.IsRunning());
		CHECK(Succeeded(p.Play()));
		CHECK(clk.IsRunning());
		CHECK(s.RunUntilPresented(30));
		const double t1 = p.GetTime();
		CHECK(t1 > 0.5);
		CHECK(Succeeded(p.Pause()));
		CHECK(!clk.IsRunning());
		const uint64_t cb0 = s.cbIndex;
		for (int i = 0; i < 100; ++i) // callbacks continue while paused, the clock must not move
			CHECK(s.Step());
		CHECK(s.cbIndex > cb0);
		CHECK(p.GetTime() == t1);
		CHECK(Succeeded(p.Resume()));
		CHECK(clk.IsRunning());
		CHECK(Succeeded(p.SetRate(1.5)));
		CHECK(clk.Rate() == 1.5);
		CHECK(Succeeded(p.SetRate(1.0)));
		CHECK(Succeeded(p.Seek(2.0)));
		CHECK(std::fabs(p.GetTime() - 2.0) < 1e-6);
		CHECK(Succeeded(p.Stop()));
		CHECK(!clk.IsRunning());
		CHECK(p.GetTime() == 0.0);
		p.Unload();
		CHECK(!clk.IsRunning() && clk.NowSeconds() == 0.0);
		Log("  %-34s ok\n", g_context.c_str());
	}

	// End to end: decoded audio through the resampler, total length and sanity.
	void TestResampledPlayback(const Clip& clip)
	{
		g_context = std::string("resampled playback ") + clip.file;
		VirtualTime vt;
		AudioClock clk(48000);
		PlayerCore p(MakeResampledConfig(clk));
		if (!LoadAndWait(p, ClipPath(clip), false))
			return;
		const MediaInfo& info = p.GetInfo();
		CHECK(info.hasAudio && info.audioSampleRate == clip.audioRate);
		CHECK(info.outputAudioSampleRate == 48000 && info.outputAudioChannels == 2);
		AvSim s;
		s.Init(p, clk, vt);
		CHECK(p.Update());
		CHECK(Succeeded(p.Play()));
		const bool ended = s.RunToEnd(clip.duration + 2.0);
		CHECK(ended);
		CHECK(!s.timedOut);
		CHECK(!s.nonFinite);
		CHECKF(s.peak > 0.05f && s.peak < 1.5f, "peak %f", s.peak);
		const double expected = clip.duration * 48000.0;
		const double ratio = static_cast<double>(s.realFrames) / expected;
		CHECKF(std::fabs(ratio - 1.0) <= 0.005, "resampled output %llu frames, expected %.0f (%.3f%%)",
			(unsigned long long)s.realFrames, expected, (ratio - 1.0) * 100.0);
		CHECK(p.DecodeErrors() == 0);
		Log("  %-34s ok: %llu frames at 48 kHz for %.3f s (%.3f%% of duration x 48000), peak %.2f\n", g_context.c_str(),
			(unsigned long long)s.realFrames, clip.duration, ratio * 100.0, s.peak);
	}

	// A/V sync with the AudioClock: flash frame presented vs. the moment the beep is heard.
	void TestAvSyncAudioClock()
	{
		const Clip& clip = *FindClip("av_sync_flash_beep.webm");
		g_context = "av sync audioclock";
		VirtualTime vt;
		AudioClock clk(48000);
		PlayerCore p(MakeResampledConfig(clk));
		if (!LoadAndWait(p, ClipPath(clip), true))
			return;
		AvSim s;
		s.Init(p, clk, vt);
		s.detectFlash = true;
		CHECK(p.Update()); // frame 0 (a flash) is shown while stopped: not part of the measurement
		CHECK(Succeeded(p.Play()));
		CHECK(s.RunToEnd(clip.duration + 2.0));
		CHECK(!s.timedOut);
		CHECK(!s.nonFinite);
		CHECK(p.GetState() == PlayerState::Ended);
		CHECKF(s.onsetCount >= 9, "beep onsets %d", s.onsetCount);
		CHECKF(s.flashCount >= 8, "flash frames presented %d", s.flashCount);

		double worst = 0.0;
		int compared = 0;
		for (int f = 0; f < s.flashCount; ++f)
		{
			const int sec = static_cast<int>(std::lround(s.flashPts[f]));
			if (sec < 1)
				continue;
			int onset = -1;
			for (int o = 0; o < s.onsetCount; ++o)
				if (std::llround(static_cast<double>(s.onsetFrame[o]) / 48000.0) == sec)
					onset = o;
			CHECKF(onset >= 0, "no beep onset near %d s", sec);
			if (onset < 0)
				continue;
			const double offsetMs = static_cast<double>(s.flashPresentedNs[f] - s.HeardNs(s.onsetFrame[onset])) * 1e-6;
			worst = std::max(worst, std::fabs(offsetMs));
			CHECKF(std::fabs(offsetMs) < 1000.0 / 30.0, "flash at %d s presented %.2f ms from its beep", sec, offsetMs);
			++compared;
		}
		CHECKF(compared >= 8, "only %d flash/beep pairs compared", compared);
		const StatsSnapshot st = p.GetStats();
		CHECKF(st.framesDropped == 0, "dropped %llu frames", (unsigned long long)st.framesDropped);
		const double ratio = static_cast<double>(s.realFrames) / (clip.duration * 48000.0);
		CHECKF(std::fabs(ratio - 1.0) <= 0.005, "audio length ratio %.4f", ratio);
		Log("  av sync (AudioClock): %d flash/beep pairs, worst |offset| %.2f ms (limit 33.33 ms), dropped %llu\n", compared, worst,
			(unsigned long long)st.framesDropped);
	}

	void TestResampledAllocations(const Clip& clip)
	{
		g_context = std::string("allocs resampled ") + clip.file;
		VirtualTime vt;
		AudioClock clk(48000);
		PlayerCore p(MakeResampledConfig(clk));
		if (!LoadAndWait(p, ClipPath(clip), true))
			return;
		AvSim s;
		s.Init(p, clk, vt);
		CHECK(p.Update());
		s.presented = 1;
		CHECK(Succeeded(p.Play()));
		CHECK(s.RunUntilPresented(40));
		const uint64_t n0 = g_newCount.load();
		const uint64_t d0 = g_deleteCount.load();
		const uint64_t presented0 = s.presented;
		const uint64_t audio0 = s.realFrames;
		CHECK(s.RunUntilPresented(presented0 + 60));
		const uint64_t n1 = g_newCount.load();
		const uint64_t d1 = g_deleteCount.load();
		CHECK(!s.timedOut);
		CHECKF(n1 == n0, "%llu allocations in the steady state window", (unsigned long long)(n1 - n0));
		CHECKF(d1 == d0, "%llu frees in the steady state window", (unsigned long long)(d1 - d0));
		CHECK(s.realFrames > audio0 + 48000);
		Log("  %-34s ok: %llu allocations / %llu frees over %llu frames, %llu audio frames at 48 kHz\n", g_context.c_str(),
			(unsigned long long)(n1 - n0), (unsigned long long)(d1 - d0), (unsigned long long)(s.presented - presented0),
			(unsigned long long)(s.realFrames - audio0));
	}

	// After a seek (or loop) the first output samples belong to the new position; nothing stale comes out.
	void TestResampledSeek()
	{
		const Clip& clip = *FindClip("av_sync_flash_beep.webm");
		g_context = "resampled seek";
		VirtualTime vt;
		AudioClock clk(48000);
		PlayerCore p(MakeResampledConfig(clk));
		if (!LoadAndWait(p, ClipPath(clip), true))
			return;
		AvSim s;
		s.Init(p, clk, vt);
		CHECK(p.Update());
		CHECK(Succeeded(p.Play()));
		std::vector<float> cap(2 * 48000);
		s.cap = cap.data();
		s.capFrames = 48000;

		// 1. just before the beep at 1.0 s (its samples are buffered in the ring / resampler), seek into silence.
		CHECK(s.RunUntilMediaTime(0.985));
		CHECK(Succeeded(p.Seek(3.5)));
		CHECK(std::fabs(p.GetTime() - 3.5) < 1e-6);
		s.capCount = 0;
		CHECK(s.RunUntilCaptured());
		{
			int64_t first = -1;
			float peakEarly = 0.0f;
			for (size_t i = 0; i < s.capFrames; ++i)
			{
				const float m = std::max(std::fabs(cap[2 * i]), std::fabs(cap[2 * i + 1]));
				if (i < 23000)
					peakEarly = std::max(peakEarly, m);
				if (first < 0 && m > 0.1f)
					first = static_cast<int64_t>(i);
			}
			// beep at 4.0 s = 0.5 s after the seek target
			const double firstMs = first < 0 ? -1.0 : first * 1000.0 / 48000.0;
			CHECKF(peakEarly < 0.01f, "stale audio after seek: peak %.4f before the beep", peakEarly);
			CHECKF(first >= 0 && std::fabs(firstMs - 500.0) < 8.0, "beep %.2f ms after the seek, expected 500 ms", firstMs);
			Log("  seek to 3.5 s: beep %.2f ms after the seek (expected 500), silence peak %.5f\n", firstMs, peakEarly);
		}

		// 2. seek into the middle of the beep at 4.0 .. 4.05 s: sound at once, ends 30 ms later.
		CHECK(Succeeded(p.Seek(4.02)));
		s.capCount = 0;
		CHECK(s.RunUntilCaptured());
		{
			int64_t first = -1, last = -1;
			for (size_t i = 0; i < 9600; ++i)
			{
				const float m = std::max(std::fabs(cap[2 * i]), std::fabs(cap[2 * i + 1]));
				if (m > 0.1f)
				{
					if (first < 0)
						first = static_cast<int64_t>(i);
					last = static_cast<int64_t>(i);
				}
			}
			const double firstMs = first < 0 ? -1.0 : first * 1000.0 / 48000.0;
			const double lastMs = last < 0 ? -1.0 : last * 1000.0 / 48000.0;
			CHECKF(first >= 0 && firstMs < 2.0, "first sound %.2f ms after the seek, expected immediately", firstMs);
			CHECKF(last >= 0 && std::fabs(lastMs - 30.0) < 8.0, "beep ends %.2f ms after the seek, expected 30 ms", lastMs);
			Log("  seek to 4.02 s: sound %.2f .. %.2f ms after the seek (expected 0 .. 30)\n", firstMs, lastMs);
		}

		// 3. loop: the stream restarts at 0 (beep at the start) without stale audio from the end.
		CHECK(Succeeded(p.SetLoop(true)));
		CHECK(Succeeded(p.Seek(clip.duration - 0.6)));
		CHECK(s.RunUntilMediaTime(clip.duration - 0.1));
		const uint32_t loops0 = p.GetLoopCount();
		int guard = 0;
		while (p.GetLoopCount() == loops0 && guard++ < 2000)
			CHECK(s.Step());
		CHECK(p.GetLoopCount() == loops0 + 1);
		s.capCount = 0;
		CHECK(s.RunUntilCaptured());
		{
			int64_t first = -1;
			for (size_t i = 0; i < s.capFrames && first < 0; ++i)
				if (std::max(std::fabs(cap[2 * i]), std::fabs(cap[2 * i + 1])) > 0.1f)
					first = static_cast<int64_t>(i);
			const double firstMs = first < 0 ? -1.0 : first * 1000.0 / 48000.0;
			CHECKF(first >= 0 && firstMs < 12.0, "after the loop the first sound is %.2f ms in, expected the start of the beep", firstMs);
			Log("  loop: first sound %.2f ms after the wrap\n", firstMs);
		}
		CHECK(!s.timedOut);
		CHECK(!s.nonFinite);
	}

	// Real time: a real thread plays the role of the audio device (10 ms blocks), the clock must follow wall time,
	// survive the device going away for a while and never go backwards.
	void TestAudioClockRealtime()
	{
		g_context = "audioclock realtime";
		const Clip& clip = *FindClip("vp8_vorbis.webm");
		AudioClock clk(48000);
		PlayerCore p(MakeResampledConfig(clk));
		if (!LoadAndWait(p, ClipPath(clip), true))
			return;
		using Steady = std::chrono::steady_clock;
		clk.SetOutputLatencyFrames(960);
		std::atomic<bool> stop{false};
		std::atomic<bool> deviceOn{true};
		std::atomic<uint64_t> callbacks{0};
		std::thread audio([&]
		{
			std::vector<float> block(480 * 2);
			auto next = Steady::now();
			while (!stop.load())
			{
				next += std::chrono::milliseconds(10);
				std::this_thread::sleep_until(next);
				if (!deviceOn.load())
				{
					next = Steady::now();
					continue;
				}
				p.ReadAudioResampled(block.data(), 480);
				clk.OnFramesConsumed(480);
				callbacks.fetch_add(1);
			}
		});

		CHECK(p.Update());
		CHECK(Succeeded(p.Play()));
		const auto start = Steady::now();
		const auto wall = [&] { return std::chrono::duration<double>(Steady::now() - start).count(); };
		double last = 0.0;
		bool monotonic = true;
		double maxJump = 0.0;
		const auto poll = [&](double untilWall)
		{
			while (wall() < untilWall)
			{
				p.Update();
				const double t = p.GetTime();
				if (t < last)
					monotonic = false;
				maxJump = std::max(maxJump, t - last);
				last = t;
				std::this_thread::sleep_for(std::chrono::milliseconds(8));
			}
		};
		poll(1.2);
		const double tNormal = p.GetTime();
		const double w1 = wall();
		CHECKF(std::fabs(tNormal - (w1 - 0.02)) < 0.10, "real time: clock %.3f s at wall %.3f s", tNormal, w1);
		// device goes away for 0.6 s: the clock keeps running
		deviceOn = false;
		const double tOff = p.GetTime();
		const double wOff = wall();
		poll(wOff + 0.6);
		const double advanced = p.GetTime() - tOff;
		const double wallAdvanced = wall() - wOff;
		CHECKF(std::fabs(advanced - wallAdvanced) < 0.08, "device off: clock advanced %.3f s in %.3f s of wall time", advanced, wallAdvanced);
		deviceOn = true;
		poll(wall() + 1.0);
		stop = true;
		audio.join();
		CHECK(monotonic);
		CHECKF(maxJump < 0.12, "largest clock step between two polls %.3f s", maxJump);
		CHECK(callbacks.load() > 100);
		Log("  audioclock realtime ok: clock %.3f s at wall %.3f s, device off for %.2f s -> advanced %.3f s, max step %.1f ms\n",
			tNormal, w1, wallAdvanced, advanced, maxJump * 1000.0);
		p.Unload();
	}

	void TestAudio2()
	{
		TestResampler();
		TestAudioClockUnit();
		TestAudioClockWiring(*FindClip("vp8_vorbis.webm"));
		TestResampledPlayback(*FindClip("vp8_vorbis.webm"));
		TestResampledPlayback(*FindClip("av_sync_flash_beep.webm"));
		TestAvSyncAudioClock();
		TestResampledSeek();
		TestResampledAllocations(*FindClip("vp8_vorbis.webm"));
		TestResampledAllocations(*FindClip("av_sync_flash_beep.webm"));
		TestAudioClockRealtime();
	}

	// ---------------------------------------------------------------------------------------------------------------
	// M2 fixes: PlayerConfig::decodeVideo and the A/V offset
	// ---------------------------------------------------------------------------------------------------------------
	struct DecodeVideoRun
	{
		bool loaded = false;
		bool ended = false;
		uint64_t presented = 0;
		uint64_t realFrames = 0;
		size_t maxAlloc = 0;
		double duration = 0.0;
		double endTime = 0.0;
		bool hasVideo = false;
		bool videoSkipped = false;
		bool gotFrame = false;
		bool videoBuffered = false;
		uint32_t decodeErrors = 0;
	};

	// Plays a clip to the end on the simulated device and reports what the player did. MaxAlloc covers Load() + playback.
	DecodeVideoRun RunDecodeVideo(const Clip& clip, bool decodeVideo)
	{
		DecodeVideoRun r;
		VirtualTime vt;
		AudioClock clk(48000);
		PlayerConfig cfg = MakeResampledConfig(clk);
		cfg.decodeVideo = decodeVideo;
		PlayerCore p(cfg);
		ResetMaxAlloc();
		if (!LoadAndWait(p, ClipPath(clip), false))
			return r;
		r.loaded = true;
		const MediaInfo& info = p.GetInfo();
		r.hasVideo = info.hasVideo;
		r.videoSkipped = info.videoSkipped;
		r.duration = info.durationSeconds;
		AvSim s;
		s.Init(p, clk, vt);
		p.Update();
		CHECK(Succeeded(p.Play()));
		r.ended = s.RunToEnd(clip.duration + 2.0);
		r.presented = s.presented;
		r.realFrames = s.realFrames;
		r.endTime = p.GetTime();
		FrameView fv;
		r.gotFrame = p.GetFrame(fv);
		r.videoBuffered = p.VideoBufferedFrame();
		r.decodeErrors = p.DecodeErrors();
		CHECK(!s.timedOut);
		CHECK(!s.nonFinite);
		r.maxAlloc = MaxAlloc();
		return r;
	}

	void TestDecodeVideoOff()
	{
		// 1. video + audio clip, video ignored
		{
			const Clip& clip = *FindClip("vp9_1080p30_opus.webm");
			g_context = "decodeVideo=false vp9_1080p30_opus";
			const DecodeVideoRun on = RunDecodeVideo(clip, true);
			const DecodeVideoRun off = RunDecodeVideo(clip, false);
			CHECK(on.loaded && off.loaded);
			CHECK(on.hasVideo && !on.videoSkipped && on.presented > 250);
			CHECK(!off.hasVideo);
			CHECK(off.videoSkipped);
			CHECKF(off.presented == 0, "%llu frames presented", (unsigned long long)off.presented);
			CHECK(!off.gotFrame);
			CHECK(!off.videoBuffered);
			CHECK(off.ended);
			CHECK(off.decodeErrors == 0);
			CHECKF(std::fabs(off.endTime - off.duration) < 1e-6, "ended at %.4f s, duration %.4f s", off.endTime, off.duration);
			const double ratio = static_cast<double>(off.realFrames) / (clip.duration * 48000.0);
			CHECKF(std::fabs(ratio - 1.0) <= 0.005, "audio length ratio %.4f", ratio);
			CHECKF(off.realFrames == on.realFrames, "audio frames %llu with video, %llu without", (unsigned long long)on.realFrames,
				(unsigned long long)off.realFrames);
			// A 1080p FrameRing slot is several MB: with the video ignored nothing that big may be allocated.
			CHECKF(on.maxAlloc > (3u << 20), "control run: largest allocation only %zu bytes", on.maxAlloc);
			CHECKF(off.maxAlloc < (1u << 20), "largest allocation %zu bytes: a frame buffer was allocated", off.maxAlloc);
			Log("  %-34s ok: 0 frames presented, audio %.4f of the duration, largest allocation %zu bytes (with video: %zu)\n",
				g_context.c_str(), ratio, off.maxAlloc, on.maxAlloc);
		}

		// 2. audio-only clip: the flag changes nothing
		{
			const Clip& clip = *FindClip("audio_en.webm");
			g_context = "decodeVideo=false audio_en";
			const DecodeVideoRun on = RunDecodeVideo(clip, true);
			const DecodeVideoRun off = RunDecodeVideo(clip, false);
			CHECK(on.loaded && off.loaded);
			CHECK(!on.hasVideo && !off.hasVideo);
			CHECK(!on.videoSkipped && !off.videoSkipped);
			CHECK(on.ended && off.ended);
			CHECK(on.presented == 0 && off.presented == 0);
			CHECK(on.realFrames == off.realFrames);
			CHECK(on.duration == off.duration);
			const double ratio = static_cast<double>(off.realFrames) / (clip.duration * 48000.0);
			CHECKF(std::fabs(ratio - 1.0) <= 0.005, "audio length ratio %.4f", ratio);
			Log("  %-34s ok: identical to the default (%llu audio frames, %.3f s)\n", g_context.c_str(),
				(unsigned long long)off.realFrames, off.duration);
		}

		// 3. nothing to play once the video is ignored: the load fails cleanly
		{
			g_context = "decodeVideo=false video only";
			PlayerConfig cfg;
			cfg.decodeVideo = false;
			PlayerCore p(cfg);
			std::unique_ptr<IByteSource> src = OpenSource(ClipPath(*FindClip("vp9_alpha.webm")), false);
			CHECK(src != nullptr);
			if (src)
			{
				CHECK(Succeeded(p.Load(std::move(src))));
				CHECK(WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 20000));
				CHECK(p.GetLoadState() == LoadState::Failed);
				CHECK(p.GetLoadError().status == Status::Unsupported);
				CHECK(p.GetLoadError().detail == StatusDetail::NoSupportedTrack);
				CHECK(!p.Update());
			}
		}

		// 4. steady state: no allocation / free while an audio-only player plays
		{
			const Clip& clip = *FindClip("vp9_1080p30_opus.webm");
			g_context = "allocs decodeVideo=false";
			VirtualTime vt;
			AudioClock clk(48000);
			PlayerConfig cfg = MakeResampledConfig(clk);
			cfg.decodeVideo = false;
			PlayerCore p(cfg);
			if (!LoadAndWait(p, ClipPath(clip), true))
				return;
			AvSim s;
			s.Init(p, clk, vt);
			p.Update();
			CHECK(Succeeded(p.Play()));
			CHECK(s.RunUntilMediaTime(2.0));
			const uint64_t n0 = g_newCount.load();
			const uint64_t d0 = g_deleteCount.load();
			const uint64_t audio0 = s.realFrames;
			CHECK(s.RunUntilMediaTime(6.0));
			const uint64_t n1 = g_newCount.load();
			const uint64_t d1 = g_deleteCount.load();
			CHECK(!s.timedOut);
			CHECKF(n1 == n0, "%llu allocations in the steady state window", (unsigned long long)(n1 - n0));
			CHECKF(d1 == d0, "%llu frees in the steady state window", (unsigned long long)(d1 - d0));
			CHECK(s.realFrames > audio0 + 3 * 48000);
			CHECK(s.presented == 0);
			Log("  %-34s ok: %llu allocations / %llu frees over %llu audio frames\n", g_context.c_str(),
				(unsigned long long)(n1 - n0), (unsigned long long)(d1 - d0), (unsigned long long)(s.realFrames - audio0));
		}
	}

	// Mean (flash presented - beep heard) over the flash / beep pairs of av_sync_flash_beep.
	struct AvOffsetResult
	{
		bool ok = false;
		int compared = 0;
		double meanMs = 0.0;
		double minMs = 0.0;
		double maxMs = 0.0;
		bool ended = false;
		double endTime = 0.0;
	};

	// cfgOffset goes through PlayerConfig, setter (if not NaN) through SetAvOffset() after the load.
	AvOffsetResult MeasureAvOffset(double cfgOffset, double setter)
	{
		AvOffsetResult r;
		const Clip& clip = *FindClip("av_sync_flash_beep.webm");
		VirtualTime vt;
		AudioClock clk(48000);
		PlayerConfig cfg = MakeResampledConfig(clk);
		cfg.avOffsetSeconds = cfgOffset;
		PlayerCore p(cfg);
		if (!LoadAndWait(p, ClipPath(clip), true))
			return r;
		if (setter == setter)
			CHECK(Succeeded(p.SetAvOffset(setter)));
		AvSim s;
		s.Init(p, clk, vt);
		s.detectFlash = true;
		CHECK(p.Update());
		CHECK(Succeeded(p.Play()));
		r.ended = s.RunToEnd(clip.duration + 3.0);
		r.endTime = p.GetTime();
		CHECK(r.ended);
		CHECK(!s.timedOut);
		CHECK(p.GetState() == PlayerState::Ended);
		CHECKF(s.onsetCount >= 9, "beep onsets %d", s.onsetCount);
		double sum = 0.0;
		r.minMs = 1e9;
		r.maxMs = -1e9;
		for (int f = 0; f < s.flashCount; ++f)
		{
			const int sec = static_cast<int>(std::lround(s.flashPts[f]));
			if (sec < 1)
				continue;
			int onset = -1;
			for (int o = 0; o < s.onsetCount; ++o)
				if (std::llround(static_cast<double>(s.onsetFrame[o]) / 48000.0) == sec)
					onset = o;
			if (onset < 0)
				continue;
			const double ms = static_cast<double>(s.flashPresentedNs[f] - s.HeardNs(s.onsetFrame[onset])) * 1e-6;
			sum += ms;
			r.minMs = std::min(r.minMs, ms);
			r.maxMs = std::max(r.maxMs, ms);
			++r.compared;
		}
		CHECKF(r.compared >= 8, "only %d flash/beep pairs compared", r.compared);
		if (r.compared > 0)
			r.meanMs = sum / r.compared;
		const double ratio = static_cast<double>(s.realFrames) / (clip.duration * 48000.0);
		CHECKF(std::fabs(ratio - 1.0) <= 0.005, "audio length ratio %.4f (the offset must not touch the audio)", ratio);
		r.ok = r.compared >= 8;
		return r;
	}

	void TestAvOffset()
	{
		g_context = "avoffset api";
		{
			PlayerCore p;
			CHECK(p.GetAvOffset() == 0.0);
			CHECK(Succeeded(p.SetAvOffset(0.25)));
			CHECK(p.GetAvOffset() == 0.25);
			CHECK(Succeeded(p.SetAvOffset(-9.0)));
			CHECK(p.GetAvOffset() == -PlayerCore::kMaxAvOffsetSeconds);
			CHECK(Succeeded(p.SetAvOffset(9.0)));
			CHECK(p.GetAvOffset() == PlayerCore::kMaxAvOffsetSeconds);
			CHECK(p.SetAvOffset(std::nan("")) == Status::InvalidArgument);
			CHECK(p.SetAvOffset(HUGE_VAL) == Status::InvalidArgument);
			CHECK(p.GetAvOffset() == PlayerCore::kMaxAvOffsetSeconds);
			PlayerConfig cfg;
			cfg.avOffsetSeconds = 50.0;
			PlayerCore q(cfg);
			CHECK(q.GetAvOffset() == PlayerCore::kMaxAvOffsetSeconds);
			cfg.avOffsetSeconds = std::nan("");
			PlayerCore n(cfg);
			CHECK(n.GetAvOffset() == 0.0);
		}

		g_context = "avoffset flash/beep";
		const AvOffsetResult base = MeasureAvOffset(0.0, std::nan(""));
		const AvOffsetResult plus = MeasureAvOffset(0.1, std::nan(""));     // through the config
		const AvOffsetResult minus = MeasureAvOffset(0.0, -0.1);            // through SetAvOffset()
		CHECK(base.ok && plus.ok && minus.ok);
		const double frameMs = 1000.0 / 30.0;
		const double dPlus = plus.meanMs - base.meanMs;
		const double dMinus = minus.meanMs - base.meanMs;
		CHECKF(std::fabs(base.meanMs) < frameMs, "baseline offset %.2f ms", base.meanMs);
		CHECKF(std::fabs(dPlus - 100.0) < frameMs, "+100 ms moved the picture by %+.2f ms", dPlus);
		CHECKF(std::fabs(dMinus + 100.0) < frameMs, "-100 ms moved the picture by %+.2f ms", dMinus);
		// the audio / clock end is not shifted: playback still ends at the duration
		CHECKF(std::fabs(plus.endTime - 10.008) < 1e-6 && std::fabs(minus.endTime - 10.008) < 1e-6, "end times %.4f / %.4f", plus.endTime,
			minus.endTime);
		Log("  avoffset: baseline %+.2f ms (min %+.2f, max %+.2f), +100 ms -> %+.2f ms (shift %+.2f), -100 ms -> %+.2f ms (shift %+.2f)\n",
			base.meanMs, base.minMs, base.maxMs, plus.meanMs, dPlus, minus.meanMs, dMinus);
	}

	void TestM2Fix()
	{
		TestDecodeVideoOff();
		TestAvOffset();
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
	if (wanted("audio2"))
		TestAudio2();
	if (wanted("m2fix"))
		TestM2Fix();
	if (wanted("harden"))
	{
		TestMalformedIndex();
		TestAudioRateAndRing();
		TestBogusTimestamps();
		TestMidStreamResize();
		TestAudioThreadVersusUnload();
		TestWorkerExceptions();
	}
	if (wanted("stress"))
		TestStress(stressIterations);

	DecodePool::ShutdownShared(); // the shared pool outlives the players now; release it before the leak check
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
