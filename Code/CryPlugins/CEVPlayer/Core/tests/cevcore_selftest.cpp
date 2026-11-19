// CEVPlayerCore selftest. Plain executable (no engine, no test framework).
//
//   cevcore_selftest [--media <dir>] [--only <substring>] [--stress <iterations>] [--bench <dir>] [--verbose]
//
// --bench <dir>: the six measurement clips of tools/probes/make_bench_media.sh (vp9_{1080p30,1080p60,2160p30}[_alpha]),
// used by the M8 reference sampler check on real content.
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
#include "CEVCore/DescriptorPlan.h"
#include "CEVCore/FrameRing.h"
#include "CEVCore/IByteSource.h"
#include "CEVCore/IClock.h"
#include "CEVCore/PlayerCore.h"
#include "CEVCore/Resampler.h"
#include "CEVCore/Stats.h"
#include "CEVCore/VideoDecoder.h"
#include "CEVCore/VideoDecoderBackend.h"
#include "CEVCore/WebmDemuxer.h"

#include "ReferenceSampler.h"

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

	DecodeMode g_decodeMode = DecodeMode::Cpu; // M9: backend of the players MakeConfig() configures

	PlayerConfig MakeConfig(ManualClock& clk)
	{
		PlayerConfig cfg;
		cfg.externalClock = &clk;
		cfg.enableStats = true;
		cfg.decodeMode = g_decodeMode;
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

	// ---- DescriptorPlan: variant / audio source decisions shared by the runtime module and the editor preview
	bool PlanExists(const char* path, void* user)
	{
		const std::vector<std::string>& files = *static_cast<const std::vector<std::string>*>(user);
		return std::find(files.begin(), files.end(), std::string(path)) != files.end();
	}

	void TestDescriptorPlan()
	{
		g_context = "descriptor";
		std::vector<std::string> files = { "v/main.webm", "v/main_720.webm", "v/main.japanese.webm" };
		VideoDescriptor d;
		d.video = "v/main.webm";
		d.route = VideoRoute::Mixed;
		d.pauseTrigger = "Pause_Root";
		VideoDescriptor::SLanguage en; en.name = "english"; en.embedded = true;
		VideoDescriptor::SLanguage ja; ja.name = "Japanese"; ja.file = "v/main.japanese.webm";
		VideoDescriptor::SLanguage de; de.name = "german"; de.file = "v/main.german.webm";
		VideoDescriptor::SLanguage fr; fr.name = "french"; fr.trigger = "Play_FR";
		d.languages = { en, ja, de, fr };
		d.defaultLanguage = "english";
		VideoDescriptor::SVariant v720; v720.file = "v/main_720.webm"; v720.width = 1280; v720.height = 720;
		VideoDescriptor::SVariant v540; v540.file = "v/main_540.webm"; v540.width = 960; v540.height = 540;
		d.variants = { v720, v540 };
		VideoDescriptor::SMarker m1; m1.name = "b"; m1.time = 2.0;
		VideoDescriptor::SMarker m2; m2.name = "a"; m2.time = 1.0;
		VideoDescriptor::SMarker m3; m3.name = "c"; m3.time = 2.0;
		d.markers = { m1, m2, m3 };
		SortDescriptorMarkers(d);
		CHECK(d.markers[0].name == "a" && d.markers[1].name == "b" && d.markers[2].name == "c"); // stable

		CHECK(FindDescriptorLanguage(d, "JAPANESE") == 1);
		CHECK(FindDescriptorLanguage(d, "") == -1);
		CHECK(FindDescriptorLanguage(d, nullptr) == -1);
		CHECK(std::strcmp(VideoRouteName(VideoRoute::Cue), "cue") == 0);

		DescriptorAudioPlan a;
		const char* const reqJa[3] = { "", "japanese", "english" };
		ResolveDescriptorAudio(d, reqJa, 3, &PlanExists, &files, a);
		CHECK(a.language == "Japanese" && a.externalPath == "v/main.japanese.webm" && !a.cue && a.missingExternal.empty());
		const char* const reqDe[2] = { "unknown", "german" };
		ResolveDescriptorAudio(d, reqDe, 2, &PlanExists, &files, a);
		CHECK(a.language == "german" && a.externalPath.empty() && a.missingExternal == "v/main.german.webm");
		const char* const reqFr[1] = { "french" };
		ResolveDescriptorAudio(d, reqFr, 1, &PlanExists, &files, a);
		CHECK(a.cue && a.trigger == "Play_FR" && a.pauseTrigger == "Pause_Root");
		const char* const reqNone[1] = { "klingon" };
		ResolveDescriptorAudio(d, reqNone, 1, &PlanExists, &files, a);
		CHECK(a.language.empty() && a.externalPath.empty() && !a.cue);

		VideoDescriptor e = d;
		e.route = VideoRoute::Embedded;
		ResolveDescriptorAudio(e, reqJa, 3, &PlanExists, &files, a);
		CHECK(a.language == "Japanese" && a.externalPath.empty() && !a.cue);
		e.route = VideoRoute::Cue;
		e.trigger = "Play_Root";
		ResolveDescriptorAudio(e, reqJa, 3, &PlanExists, &files, a);
		CHECK(a.cue && a.trigger == "Play_Root" && a.language == "Japanese");
		ResolveDescriptorAudio(e, reqFr, 1, &PlanExists, &files, a);
		CHECK(a.cue && a.trigger == "Play_FR");

		DescriptorVariantChoice c = PickDescriptorVariant(d, -1, false, 0, &PlanExists, &files);
		CHECK(c.variant == -1 && c.videoPath == "v/main.webm");
		c = PickDescriptorVariant(d, 1, false, 0, &PlanExists, &files);
		CHECK(c.variant == 1 && c.videoPath == "v/main_720.webm" && c.height == 720);
		c = PickDescriptorVariant(d, 2, false, 0, &PlanExists, &files);
		CHECK(c.variant == -1 && c.missingFile == "v/main_540.webm" && c.videoPath == "v/main.webm");
		c = PickDescriptorVariant(d, 5, false, 0, &PlanExists, &files);
		CHECK(c.variant == -1 && c.forcedOutOfRange);
		c = PickDescriptorVariant(d, 0, true, 100, &PlanExists, &files);
		CHECK(c.variant == -1 && !c.forcedOutOfRange); // 0 = main video, wins over the automatic choice
		c = PickDescriptorVariant(d, -1, true, 700, &PlanExists, &files);
		CHECK(c.variant == 1); // smallest not below 700
		c = PickDescriptorVariant(d, -1, true, 1080, &PlanExists, &files);
		CHECK(c.variant == -1 && c.missingFile.empty()); // none big enough: the main video
		c = PickDescriptorVariant(d, -1, true, 500, &PlanExists, &files);
		CHECK(c.variant == -1 && c.missingFile == "v/main_540.webm"); // 540 chosen but missing
		CHECK(DescriptorAvOffsetSeconds(d) == 0.0);

		// M7: start-of-playback choice by the aggregated on-screen pixel height (360 / 720 / 1080 variants)
		VideoDescriptor h;
		h.video = "h/main.webm";
		std::vector<std::string> hfiles = { "h/main.webm", "h/v360.webm", "h/v720.webm", "h/v1080.webm" };
		VideoDescriptor::SVariant h1080; h1080.file = "h/v1080.webm"; h1080.width = 1920; h1080.height = 1080;
		VideoDescriptor::SVariant h360; h360.file = "h/v360.webm"; h360.width = 640; h360.height = 360;
		VideoDescriptor::SVariant h720; h720.file = "h/v720.webm"; h720.width = 1280; h720.height = 720;
		h.variants = { h1080, h360, h720 };
		c = PickDescriptorVariant(h, -1, true, 300, &PlanExists, &hfiles);
		CHECK(c.variant == 2 && c.videoPath == "h/v360.webm");
		c = PickDescriptorVariant(h, -1, true, 700, &PlanExists, &hfiles);
		CHECK(c.variant == 3 && c.videoPath == "h/v720.webm");
		c = PickDescriptorVariant(h, -1, true, 1000, &PlanExists, &hfiles);
		CHECK(c.variant == 1 && c.videoPath == "h/v1080.webm");
		c = PickDescriptorVariant(h, -1, true, 0, &PlanExists, &hfiles);
		CHECK(c.variant == -1 && c.videoPath == "h/main.webm"); // unknown height: the main video, not the smallest
		c = PickDescriptorVariant(h, -1, true, -5, &PlanExists, &hfiles);
		CHECK(c.variant == -1 && c.videoPath == "h/main.webm");
		c = PickDescriptorVariant(h, 3, true, 300, &PlanExists, &hfiles);
		CHECK(c.variant == 3); // a forced variant wins over the height
		c = PickDescriptorVariant(h, 0, true, 300, &PlanExists, &hfiles);
		CHECK(c.variant == -1); // forced main video
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
		bool legacyClock = false;       // report every block as audible (OnFramesConsumed(frames))
		uint32_t silentBlocks = 0;      // next callbacks deliver silence without touching the player (worker not ready)
		uint64_t quietNeeded = 24000;   // silent frames before a sound counts as a new onset

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
		// clock samples (one per video poll), for tests that cannot rely on every flash frame being presented
		bool recordClock = false;
		int64_t clockNs[4096];
		double clockVal[4096];
		int clockCount = 0;

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
			uint32_t got = 0;
			if (silentBlocks > 0)
			{
				--silentBlocks;
				std::fill(buf.begin(), buf.end(), 0.0f);
			}
			else
			{
				if (info.hasAudio && p->GetState() == PlayerState::Playing)
				{
					if (!WaitFor([&] { return p->AudioBufferedFrames() >= minBuffered || p->AudioDecoderAtEnd(); }, 15000))
						timedOut = true;
				}
				got = p->ReadAudioResampled(buf.data(), kBlock);
			}
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
					if (quietRun >= quietNeeded && onsetCount < 64)
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
			if (legacyClock)
				clk->OnFramesConsumed(kBlock);
			else
				clk->OnFramesConsumed(kBlock, got);
			++cbIndex;
		}

		// media time at virtual time `ns`, interpolated between the recorded polls
		double ClockAt(int64_t ns) const
		{
			for (int i = 1; i < clockCount; ++i)
				if (clockNs[i] >= ns)
				{
					const double f = static_cast<double>(ns - clockNs[i - 1]) / static_cast<double>(std::max<int64_t>(1, clockNs[i] - clockNs[i - 1]));
					return clockVal[i - 1] + f * (clockVal[i] - clockVal[i - 1]);
				}
			return -1.0;
		}

		void VideoPoll(int64_t ns)
		{
			if (recordClock && clockCount < 4096)
			{
				clockNs[clockCount] = ns;
				clockVal[clockCount] = clk->NowSeconds();
				++clockCount;
			}
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
				const uint32_t got = p.ReadAudioResampled(block.data(), 480);
				clk.OnFramesConsumed(480, got);
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

	// ---------------------------------------------------------------------------------------------------------------
	// m2fix2: rate range, A/V sync after seek / loop / underrun with the AudioClock
	// ---------------------------------------------------------------------------------------------------------------
	void TestAudioClockWaiting()
	{
		g_context = "audioclock waiting";
		VirtualTime vt;
		AudioClock clk(48000);
		clk.SetTimeSource(&VirtualTime::Now, &vt);
		const int64_t base = vt.ns.load();
		const auto tAt = [&](double sec) { return base + static_cast<int64_t>(sec * 1e9); };
		const auto callbacks = [&](double from, double to, bool audible) // 10 ms blocks of 480 frames
		{
			for (double t = from; t < to - 1e-9; t += 0.01)
			{
				vt.ns = tAt(t);
				clk.OnFramesConsumed(480, audible ? 480u : 0u);
			}
		};

		// 1. seek: the silence before the audio of the new position arrives does not move the clock
		clk.Start();
		callbacks(0.0, 1.0, true);
		clk.Seek(5.0);
		callbacks(1.0, 1.1, false);
		vt.ns = tAt(1.1);
		CHECK(clk.NowSeconds() == 5.0);
		callbacks(1.1, 2.1, true);
		vt.ns = tAt(2.1);
		const double t1 = clk.NowSeconds();
		CHECKF(std::fabs(t1 - 6.0) < 0.02, "seek + 100 ms of silence: clock %.4f s, expected ~6.0 (no lag)", t1);

		// 2. underrun (0.2 s, shorter than the timeout): the clock waits, audible time only
		clk.Seek(10.0);
		callbacks(2.1, 3.1, true);
		callbacks(3.1, 3.3, false);
		vt.ns = tAt(3.3);
		const double tHold = clk.NowSeconds();
		CHECKF(std::fabs(tHold - 11.0) < 0.03, "held at %.4f s during the underrun, expected ~11.0", tHold);
		callbacks(3.3, 4.3, true);
		vt.ns = tAt(4.3);
		const double t2 = clk.NowSeconds();
		CHECKF(std::fabs(t2 - 12.0) < 0.03, "after the underrun the clock is at %.4f s, expected ~12.0", t2);
		CHECK(!clk.CountsSilence());

		// 3. no audible audio at all: after kStallSeconds the wall clock takes over, never backwards, no freeze
		clk.Seek(20.0);
		double last = clk.NowSeconds();
		double worstStep = 0.0;
		bool monotonic = true;
		for (int i = 0; i < 100; ++i)
		{
			vt.ns = tAt(4.3 + i * 0.01);
			clk.OnFramesConsumed(480, 0);
			if (i % 2 == 0) // poll every 20 ms
			{
				const double now = clk.NowSeconds();
				monotonic = monotonic && now >= last;
				worstStep = std::max(worstStep, now - last);
				last = now;
			}
		}
		vt.ns = tAt(5.3);
		last = clk.NowSeconds();
		CHECK(monotonic);
		CHECKF(std::fabs((last - 20.0) - 0.75) < 0.06, "1 s without audio: clock advanced %.4f s, expected ~0.75 (0.25 s hold)", last - 20.0);
		CHECKF(worstStep < 0.04, "fallback step %.4f s", worstStep);
		CHECK(clk.CountsSilence());
		// audio comes back: no jump back, normal speed, underruns hold again
		callbacks(5.3, 6.3, true);
		vt.ns = tAt(6.3);
		const double t3 = clk.NowSeconds();
		CHECK(t3 >= last);
		CHECKF(std::fabs((t3 - last) - 1.0) < 0.05, "after the fallback the clock advanced %.4f s over 1 s", t3 - last);
		CHECK(!clk.CountsSilence());
		callbacks(6.3, 6.5, false);
		vt.ns = tAt(6.5);
		CHECK(std::fabs(clk.NowSeconds() - t3) < 0.03);

		// 4. NotifyAudioEnded(): silence counts at once (audio shorter than the video / no audio)
		clk.Seek(30.0);
		clk.NotifyAudioEnded();
		CHECK(clk.CountsSilence());
		callbacks(6.5, 7.5, false);
		vt.ns = tAt(7.5);
		const double t4 = clk.NowSeconds();
		CHECKF(std::fabs((t4 - 30.0) - 1.0) < 0.03, "audio ended: clock advanced %.4f s over 1 s of silence", t4 - 30.0);
		// the next seek is a new audio epoch again
		clk.Seek(40.0);
		CHECK(!clk.CountsSilence());
		callbacks(7.5, 7.6, false);
		vt.ns = tAt(7.6);
		CHECK(clk.NowSeconds() == 40.0);

		// 5. latency: the first audible block is heard `latency` later, the clock starts then
		clk.SetOutputLatencyFrames(960);
		clk.Seek(50.0);
		callbacks(7.6, 7.7, false);
		callbacks(7.7, 8.7, true);
		vt.ns = tAt(8.7);
		const double t5 = clk.NowSeconds();
		CHECKF(std::fabs(t5 - (50.0 + 1.0 - 0.02)) < 0.02, "latency 20 ms: clock %.4f s, expected ~50.98", t5);

		// 6. an audible count above the block size is clamped
		const uint64_t c0 = clk.FramesCounted();
		clk.OnFramesConsumed(100, 5000);
		CHECK(clk.FramesCounted() - c0 == 100);
		Log("  audioclock waiting ok: seek lag %+.1f ms, underrun hold at %+.3f s, no-audio fallback advanced %.3f s in 1 s\n",
			(t1 - 6.0) * 1000.0, tHold - 11.0, last - 20.0);
	}

	struct SeekProbe
	{
		bool ok = false;
		int pairs = 0;
		double meanMs = 0.0;
		double worstMs = 0.0;
	};

	// Seek (or loop wrap) with `gapBlocks` callbacks that deliver silence because the worker has not applied the
	// seek yet (the gap that used to shift the clock for good), then flash / beep offsets of the next seconds.
	// legacy = the mixer reports every block as audible (the old OnFramesConsumed(frames) contract).
	SeekProbe MeasureSeekOffset(bool loopWrap, double seekTo, uint32_t gapBlocks, bool legacy)
	{
		SeekProbe r;
		const Clip& clip = *FindClip("av_sync_flash_beep.webm");
		VirtualTime vt;
		AudioClock clk(48000);
		PlayerCore p(MakeResampledConfig(clk));
		if (!LoadAndWait(p, ClipPath(clip), true))
			return r;
		AvSim s;
		s.Init(p, clk, vt);
		s.legacyClock = legacy;
		s.detectFlash = true;
		CHECK(p.Update());
		CHECK(Succeeded(p.Play()));
		int firstBeepSec = 0;
		double untilMedia = 0.0;
		if (loopWrap)
		{
			CHECK(Succeeded(p.SetLoop(true)));
			CHECK(Succeeded(p.Seek(clip.duration - 0.6)));
			CHECK(s.RunUntilMediaTime(clip.duration - 0.1));
			const uint32_t loops0 = p.GetLoopCount();
			int guard = 0;
			while (p.GetLoopCount() == loops0 && guard++ < 4000)
				CHECK(s.Step());
			CHECK(p.GetLoopCount() == loops0 + 1);
			untilMedia = 3.4;
		}
		else
		{
			CHECK(s.RunUntilMediaTime(1.5));
			CHECK(Succeeded(p.Seek(seekTo)));
			firstBeepSec = static_cast<int>(std::ceil(seekTo));
			untilMedia = seekTo + 3.0;
		}
		s.silentBlocks = gapBlocks;
		s.flashCount = 0;
		s.onsetCount = 0;
		s.quietRun = 1u << 30;
		CHECK(s.RunUntilMediaTime(untilMedia));
		CHECK(!s.timedOut);
		CHECK(!s.nonFinite);
		double sum = 0.0;
		for (int f = 0; f < s.flashCount; ++f)
		{
			const int sec = static_cast<int>(std::lround(s.flashPts[f]));
			const int o = sec - firstBeepSec;
			if (sec < 1 || o < 0 || o >= s.onsetCount)
				continue;
			const double ms = static_cast<double>(s.flashPresentedNs[f] - s.HeardNs(s.onsetFrame[o])) * 1e-6;
			sum += ms;
			if (std::fabs(ms) > std::fabs(r.worstMs))
				r.worstMs = ms;
			++r.pairs;
		}
		if (r.pairs > 0)
			r.meanMs = sum / r.pairs;
		r.ok = r.pairs >= 2;
		return r;
	}

	void TestSeekSync()
	{
		g_context = "m2fix2 seek sync";
		const double frameMs = 1000.0 / 30.0;
		const SeekProbe fixedSeek = MeasureSeekOffset(false, 3.5, 8, false);
		const SeekProbe oldSeek = MeasureSeekOffset(false, 3.5, 8, true);
		CHECK(fixedSeek.ok && oldSeek.ok);
		CHECKF(std::fabs(fixedSeek.worstMs) < frameMs, "seek: worst flash/beep offset %+.2f ms (limit 1 frame = %.1f ms)", fixedSeek.worstMs, frameMs);
		CHECKF(oldSeek.meanMs < -50.0, "control (legacy contract): offset %+.2f ms, the 80 ms gap should show up", oldSeek.meanMs);
		Log("  seek 3.5 s with an 80 ms silent gap: %d pairs, offset %+.2f ms mean / %+.2f ms worst (legacy report: %+.2f ms)\n",
			fixedSeek.pairs, fixedSeek.meanMs, fixedSeek.worstMs, oldSeek.meanMs);

		g_context = "m2fix2 loop sync";
		const SeekProbe fixedLoop = MeasureSeekOffset(true, 0.0, 8, false);
		const SeekProbe oldLoop = MeasureSeekOffset(true, 0.0, 8, true);
		CHECK(fixedLoop.ok && oldLoop.ok);
		CHECKF(std::fabs(fixedLoop.worstMs) < frameMs, "loop: worst flash/beep offset %+.2f ms", fixedLoop.worstMs);
		CHECKF(oldLoop.meanMs < -50.0, "control (legacy contract): loop offset %+.2f ms", oldLoop.meanMs);
		Log("  loop wrap with an 80 ms silent gap: %d pairs, offset %+.2f ms mean / %+.2f ms worst (legacy report: %+.2f ms)\n",
			fixedLoop.pairs, fixedLoop.meanMs, fixedLoop.worstMs, oldLoop.meanMs);

		g_context = "m2fix2 underrun sync";
		{
			const Clip& clip = *FindClip("av_sync_flash_beep.webm");
			VirtualTime vt;
			AudioClock clk(48000);
			PlayerCore p(MakeResampledConfig(clk));
			if (LoadAndWait(p, ClipPath(clip), true))
			{
				AvSim s;
				s.Init(p, clk, vt);
				s.detectFlash = true;
				CHECK(p.Update());
				CHECK(Succeeded(p.Play()));
				CHECK(s.RunUntilMediaTime(1.5));
				s.silentBlocks = 15; // decoding stalled for 150 ms: silent callbacks, the ring is not read
				s.flashCount = 0;
				s.onsetCount = 0;
				s.quietRun = 1u << 30;
				CHECK(s.RunUntilMediaTime(5.5));
				double worst = 0.0;
				int pairs = 0;
				for (int f = 0; f < s.flashCount; ++f)
				{
					const int sec = static_cast<int>(std::lround(s.flashPts[f]));
					const int o = sec - 2;
					if (sec < 2 || o < 0 || o >= s.onsetCount)
						continue;
					const double ms = static_cast<double>(s.flashPresentedNs[f] - s.HeardNs(s.onsetFrame[o])) * 1e-6;
					if (std::fabs(ms) > std::fabs(worst))
						worst = ms;
					++pairs;
				}
				CHECK(pairs >= 2);
				CHECKF(std::fabs(worst) < frameMs, "underrun: worst flash/beep offset %+.2f ms", worst);
				Log("  150 ms underrun at 1.5 s: %d pairs, worst offset %+.2f ms\n", pairs, worst);
			}
		}
	}

	void TestRateRange()
	{
		g_context = "m2fix2 rate api";
		static_assert(PlayerCore::kMinRate == Resampler::kMinRate && PlayerCore::kMaxRate == Resampler::kMaxRate, "rate range");
		{
			PlayerCore p;
			CHECK(p.GetRate() == 1.0);
			CHECK(Succeeded(p.SetRate(8.0)));
			CHECK(p.GetRate() == PlayerCore::kMaxRate);
			CHECK(Succeeded(p.SetRate(0.1)));
			CHECK(p.GetRate() == PlayerCore::kMinRate);
			CHECK(Succeeded(p.SetRate(100.0)));
			CHECK(p.GetRate() == PlayerCore::kMaxRate);
			CHECK(Succeeded(p.SetRate(1.5)));
			CHECK(p.GetRate() == 1.5);
			CHECK(p.SetRate(0.0) == Status::InvalidArgument);
			CHECK(p.SetRate(-1.0) == Status::InvalidArgument);
			CHECK(p.SetRate(std::nan("")) == Status::InvalidArgument);
			CHECK(p.GetRate() == 1.5);
		}

		g_context = "m2fix2 rate 8 -> 4";
		const double frameMs = 1000.0 / 30.0;
		{
			const Clip& clip = *FindClip("av_sync_flash_beep.webm");
			VirtualTime vt;
			AudioClock clk(48000);
			PlayerCore p(MakeResampledConfig(clk));
			if (!LoadAndWait(p, ClipPath(clip), true))
				return;
			CHECK(Succeeded(p.SetRate(8.0)));
			CHECK(clk.Rate() == 4.0);
			CHECK(p.GetRate() == 4.0);
			AvSim s;
			s.Init(p, clk, vt);
			s.recordClock = true;
			s.minBuffered = 8000;
			s.quietNeeded = 6000;
			CHECK(p.Update());
			CHECK(Succeeded(p.Play()));
			CHECK(s.RunUntilMediaTime(0.6));
			const double m0 = clk.NowSeconds();
			const int64_t v0 = vt.ns.load();
			const uint64_t a0 = s.realFrames;
			CHECK(s.RunUntilMediaTime(5.4));
			const double m1 = clk.NowSeconds();
			const int64_t v1 = vt.ns.load();
			const uint64_t a1 = s.realFrames;
			const double clockSpeed = (m1 - m0) / (static_cast<double>(v1 - v0) * 1e-9);
			const double audioSpeed = (static_cast<double>(a1 - a0) * 4.0 / 48000.0) / (static_cast<double>(v1 - v0) * 1e-9);
			CHECKF(std::fabs(clockSpeed - 4.0) < 0.05, "clock speed %.3f, expected 4.0", clockSpeed);
			CHECKF(std::fabs(audioSpeed - 4.0) < 0.1, "audio consumes %.3f x real time of the source, expected 4.0", audioSpeed);
			// the beep k (media time k s) is heard when the clock must read k: compare within one video frame
			double worst = 0.0;
			int pairs = 0;
			for (int k = 1; k < s.onsetCount && k <= 5; ++k)
			{
				const double c = s.ClockAt(s.HeardNs(s.onsetFrame[k]));
				if (c < 0.0)
					continue;
				const double ms = (c - static_cast<double>(k)) * 1000.0;
				if (std::fabs(ms) > std::fabs(worst))
					worst = ms;
				++pairs;
			}
			CHECKF(pairs >= 4, "only %d beeps compared at rate 4", pairs);
			CHECKF(std::fabs(worst) < frameMs, "rate 8 (-> 4): clock %+.2f ms off at a beep (media time)", worst);
			CHECK(!s.timedOut);
			Log("  rate 8.0 requested: effective %.2f, clock speed %.3f, audio speed %.3f, %d beeps, worst clock error %+.2f ms\n",
				clk.Rate(), clockSpeed, audioSpeed, pairs, worst);
		}

		g_context = "m2fix2 rate 0.1 -> 0.25";
		{
			const Clip& clip = *FindClip("av_sync_flash_beep.webm");
			VirtualTime vt;
			AudioClock clk(48000);
			PlayerCore p(MakeResampledConfig(clk));
			if (!LoadAndWait(p, ClipPath(clip), true))
				return;
			CHECK(Succeeded(p.SetRate(0.1)));
			CHECK(clk.Rate() == 0.25);
			AvSim s;
			s.Init(p, clk, vt);
			CHECK(p.Update());
			CHECK(Succeeded(p.Play()));
			CHECK(s.RunUntilMediaTime(0.3));
			const double m0 = clk.NowSeconds();
			const int64_t v0 = vt.ns.load();
			const uint64_t a0 = s.realFrames;
			CHECK(s.RunUntilMediaTime(1.3));
			const double m1 = clk.NowSeconds();
			const int64_t v1 = vt.ns.load();
			const uint64_t a1 = s.realFrames;
			const double elapsed = static_cast<double>(v1 - v0) * 1e-9;
			const double clockSpeed = (m1 - m0) / elapsed;
			const double audioSpeed = (static_cast<double>(a1 - a0) * 0.25 / 48000.0) / elapsed;
			CHECKF(std::fabs(clockSpeed - 0.25) < 0.01, "clock speed %.4f, expected 0.25", clockSpeed);
			CHECKF(std::fabs(audioSpeed - 0.25) < 0.01, "audio source speed %.4f, expected 0.25", audioSpeed);
			CHECK(!s.timedOut);
			Log("  rate 0.1 requested: effective %.2f, clock speed %.4f, audio speed %.4f\n", clk.Rate(), clockSpeed, audioSpeed);
		}
	}

	// Audio that ends before the video (and media without audio) must not freeze the clock: the clip still ends.
	void TestAudioShorterThanVideo()
	{
		g_context = "m2fix2 audio ends early";
		std::vector<uint8_t> bytes;
		RawLayout lay;
		if (!ReadClipBytes("vp8_vorbis.webm", bytes) || !ParseLayout(bytes, lay))
		{
			CHECK(false);
			return;
		}
		uint32_t audioTrack = 2;
		{
			std::unique_ptr<WebmDemuxer> d;
			CHECK(Succeeded(WebmDemuxer::Open(std::make_unique<MemoryByteSource>(std::vector<uint8_t>(bytes)), d, nullptr, nullptr)));
			if (d)
				audioTrack = d->Audio().trackNumber;
		}
		// keep the first 40 audio blocks (~0.9 s of 5 s): the rest is re-addressed to a track nobody declared
		size_t audioBlocks = 0, kept = 0;
		for (const RawBlock& b : lay.blocks)
		{
			if (b.track != audioTrack)
				continue;
			++audioBlocks;
			if (++kept > 40)
				bytes[b.data] = static_cast<uint8_t>(0x80u | 0x3Fu);
		}
		CHECKF(audioBlocks > 80, "clip has %zu audio blocks", audioBlocks);

		VirtualTime vt;
		AudioClock clk(48000);
		PlayerCore p(MakeResampledConfig(clk));
		CHECK(Succeeded(p.Load(std::make_unique<MemoryByteSource>(std::move(bytes)))));
		CHECK(WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 20000));
		if (p.GetLoadState() != LoadState::Ready)
		{
			CHECK(false);
			return;
		}
		CHECK(p.GetInfo().hasAudio && p.GetInfo().hasVideo);
		AvSim s;
		s.Init(p, clk, vt);
		CHECK(p.Update());
		CHECK(Succeeded(p.Play()));
		const bool ended = s.RunToEnd(p.GetInfo().durationSeconds + 2.0);
		const double virt = static_cast<double>(vt.ns.load() - s.t0) * 1e-9;
		CHECK(ended);
		CHECK(!s.timedOut);
		CHECK(p.GetState() == PlayerState::Ended);
		CHECK(p.AudioEnded());
		CHECKF(s.realFrames < 48000u * 2u, "audio frames %llu: the audio was not shortened", (unsigned long long)s.realFrames);
		CHECKF(std::fabs(virt - p.GetInfo().durationSeconds) < 0.15, "ended after %.3f s of virtual time, duration %.3f s", virt,
			p.GetInfo().durationSeconds);
		Log("  audio %.2f s of %.2f s: Ended after %.3f s of virtual time\n", static_cast<double>(s.realFrames) / 48000.0,
			p.GetInfo().durationSeconds, virt);

		// the same for a clip without any audio track
		g_context = "m2fix2 no audio track";
		{
			VirtualTime vt2;
			AudioClock clk2(48000);
			PlayerCore q(MakeResampledConfig(clk2));
			if (!LoadAndWait(q, ClipPath(*FindClip("vp9_alpha.webm")), true))
				return;
			CHECK(!q.GetInfo().hasAudio);
			AvSim s2;
			s2.Init(q, clk2, vt2);
			CHECK(q.Update());
			CHECK(Succeeded(q.Play()));
			const bool ended2 = s2.RunToEnd(q.GetInfo().durationSeconds + 2.0);
			const double virt2 = static_cast<double>(vt2.ns.load() - s2.t0) * 1e-9;
			CHECK(ended2);
			CHECK(!s2.timedOut);
			CHECKF(std::fabs(virt2 - q.GetInfo().durationSeconds) < 0.15, "no audio: ended after %.3f s, duration %.3f s", virt2,
				q.GetInfo().durationSeconds);
			Log("  no audio track: Ended after %.3f s of virtual time (duration %.3f s)\n", virt2, q.GetInfo().durationSeconds);
		}
	}

	// Steady state with the audible-frames overload: no allocations, also at a non-trivial rate.
	void TestHoldAllocations()
	{
		g_context = "m2fix2 allocs";
		const Clip& clip = *FindClip("av_sync_flash_beep.webm");
		VirtualTime vt;
		AudioClock clk(48000);
		PlayerCore p(MakeResampledConfig(clk));
		if (!LoadAndWait(p, ClipPath(clip), true))
			return;
		CHECK(Succeeded(p.SetRate(2.0)));
		AvSim s;
		s.Init(p, clk, vt);
		s.minBuffered = 4000;
		CHECK(p.Update());
		s.presented = 1;
		CHECK(Succeeded(p.Play()));
		CHECK(s.RunUntilPresented(40));
		const uint64_t n0 = g_newCount.load();
		const uint64_t d0 = g_deleteCount.load();
		const uint64_t audio0 = s.realFrames;
		CHECK(s.RunUntilPresented(s.presented + 60));
		const uint64_t n1 = g_newCount.load();
		const uint64_t d1 = g_deleteCount.load();
		CHECK(!s.timedOut);
		CHECKF(n1 == n0, "%llu allocations in the steady state window", (unsigned long long)(n1 - n0));
		CHECKF(d1 == d0, "%llu frees in the steady state window", (unsigned long long)(d1 - d0));
		CHECK(s.realFrames > audio0 + 24000);
		Log("  %-34s ok: %llu allocations / %llu frees\n", g_context.c_str(), (unsigned long long)(n1 - n0), (unsigned long long)(d1 - d0));
	}

	void TestM2Fix2()
	{
		TestRateRange();
		TestAudioClockWaiting();
		TestSeekSync();
		TestAudioShorterThanVideo();
		TestHoldAllocations();
	}

	void TestM2Fix()
	{
		TestDecodeVideoOff();
		TestAvOffset();
	}
}

// =====================================================================================================================
	// ---------------------------------------------------------------------------------------------------------------
	// M7: visibility (decode only / stopped / catch-up) and the end of playback while hidden
	// ---------------------------------------------------------------------------------------------------------------
	//! One hidden frame step: no waiting for frames (nothing is presented while hidden).
	bool HiddenStep(PlayerCore& p, ManualClock& clk, double step)
	{
		p.SetVisible(false, step);
		const bool fresh = p.Update();
		clk.Advance(step);
		return fresh;
	}

	void TestHiddenCatchUp(const Clip& clip, uint32_t keyframeInterval)
	{
		g_context = std::string("hidden ") + clip.file;
		ManualClock clk;
		PlayerCore p(MakeConfig(clk));
		if (!LoadAndWait(p, ClipPath(clip), true))
			return;
		const MediaInfo& info = p.GetInfo();
		const double step = 1.0 / 60.0;
		const double period = 1.0 / info.frameRate;
		p.SetHiddenStopSeconds(1.0);
		Sim sim;
		sim.Init(p, clk);
		CHECK(p.GetVisibility() == VideoVisibility::Visible);
		CHECK(Succeeded(p.Play()));
		for (int i = 0; i < 30; ++i)
		{
			p.SetVisible(true, step);
			CHECK(sim.Step());
		}
		CHECK(sim.presented > 5);

		// --- phase A: hidden for 0.5 s (decode only)
		const uint64_t presentedA = p.GetStats().framesPresented;
		bool freshWhileHidden = HiddenStep(p, clk, step);
		CHECK(p.GetVisibility() == VideoVisibility::DecodeOnly);
		std::this_thread::sleep_for(std::chrono::milliseconds(50)); // a frame in flight at the switch settles
		const StatsSnapshot a0 = p.GetStats();
		const double tA0 = clk.NowSeconds();
		for (int i = 0; i < 29; ++i)
			freshWhileHidden = HiddenStep(p, clk, step) || freshWhileHidden;
		const double tA = clk.NowSeconds();
		const uint64_t expectA = static_cast<uint64_t>((tA - tA0) / period) - 5; // up to 3 frames were decoded into the ring before hiding
		CHECKF(WaitFor([&] { return p.GetStats().framesDecodeOnly - a0.framesDecodeOnly >= expectA; }, 5000),
			"decode only frames %llu, expected >= %llu", (unsigned long long)(p.GetStats().framesDecodeOnly - a0.framesDecodeOnly), (unsigned long long)expectA);
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		const StatsSnapshot a1 = p.GetStats();
		CHECK(!freshWhileHidden);
		CHECK(a1.framesPresented == presentedA);
		CHECKF(a1.copyBytes == a0.copyBytes && a1.colorBytes == a0.colorBytes, "copy %llu -> %llu, colour %llu -> %llu",
			(unsigned long long)a0.copyBytes, (unsigned long long)a1.copyBytes, (unsigned long long)a0.colorBytes, (unsigned long long)a1.colorBytes);
		// the decoder keeps pace with the clock but does not run ahead of it
		CHECKF(a1.framesDecodeOnly - a0.framesDecodeOnly <= static_cast<uint64_t>((tA - tA0) / period) + 3,
			"decode only ran ahead: %llu frames in %.3f s", (unsigned long long)(a1.framesDecodeOnly - a0.framesDecodeOnly), tA - tA0);

		// --- visible again after the first stage: output resumes without a catch-up
		p.SetVisible(true, step);
		CHECK(p.GetVisibility() == VideoVisibility::Visible);
		const uint64_t before = sim.presented;
		CHECK(sim.RunUntilPresented(before + 1));
		FrameView fv;
		CHECK(p.GetFrame(fv));
		CHECKF(std::fabs(fv.pts - p.GetTime()) <= 2.0 * period + step, "resumed frame pts %.3f at clock %.3f", fv.pts, p.GetTime());
		CHECK(p.GetStats().catchUps == 0);

		// --- phase B: hidden for 1.5 s (decode only, then stopped after 1.0 s)
		int steps = 0;
		while (p.GetVisibility() != VideoVisibility::Stopped && steps < 200)
		{
			HiddenStep(p, clk, step);
			++steps;
		}
		CHECKF(steps >= 59 && steps <= 61, "stopped after %d hidden steps (threshold 1.0 s = 60 steps)", steps);
		std::this_thread::sleep_for(std::chrono::milliseconds(80));
		const uint64_t stoppedAt = p.GetStats().framesDecodeOnly;
		for (int i = 0; i < 30; ++i)
			HiddenStep(p, clk, step);
		std::this_thread::sleep_for(std::chrono::milliseconds(80));
		CHECKF(p.GetStats().framesDecodeOnly == stoppedAt, "decoding went on while stopped: %llu -> %llu",
			(unsigned long long)stoppedAt, (unsigned long long)p.GetStats().framesDecodeOnly);

		// --- visible: catch up from the keyframe before the clock, then present at the clock
		p.SetVisible(true, step);
		CHECK(p.GetVisibility() == VideoVisibility::Visible);
		const uint64_t beforeB = sim.presented;
		CHECK(sim.RunUntilPresented(beforeB + 1));
		CHECK(WaitFor([&] { return p.GetStats().catchUps == 1; }, 5000));
		const StatsSnapshot b = p.GetStats();
		CHECK(p.GetFrame(fv));
		CHECKF(std::fabs(fv.pts - p.GetTime()) <= 2.0 * period + step, "frame after catch-up pts %.3f at clock %.3f", fv.pts, p.GetTime());
		CHECKF(b.lastCatchUpFrames <= keyframeInterval, "catch-up decoded %u frames (keyframe interval %u)", b.lastCatchUpFrames, keyframeInterval);
		CHECKF(b.lastCatchUpMs > 0.0f, "catch-up time %.3f ms", b.lastCatchUpMs);
		Log("  %-34s ok: decode only %llu frames in %.2f s, stopped after %d steps, catch-up %u frames %.2f ms\n", g_context.c_str(),
			(unsigned long long)(a1.framesDecodeOnly - a0.framesDecodeOnly), tA - tA0, steps, b.lastCatchUpFrames, b.lastCatchUpMs);

		// --- phase C: stopped and visible again inside the same keyframe interval (the decoder continues from where
		// it stopped, the ring is still full of frames from before hiding: they must not block the catch-up)
		for (int i = 0; i < 30 && sim.Step(); ++i)
			p.SetVisible(true, step); // ring full again
		p.SetHiddenStopSeconds(0.1);
		for (int i = 0; i < 15; ++i)
			HiddenStep(p, clk, step);
		CHECK(p.GetVisibility() == VideoVisibility::Stopped);
		p.SetVisible(true, step);
		const uint64_t beforeC = sim.presented;
		CHECK(sim.RunUntilPresented(beforeC + 1));
		CHECK(WaitFor([&] { return p.GetStats().catchUps == 2; }, 5000));
		CHECK(p.GetFrame(fv));
		CHECKF(std::fabs(fv.pts - p.GetTime()) <= 2.0 * period + step, "frame after the short catch-up pts %.3f at clock %.3f", fv.pts, p.GetTime());
		CHECKF(p.GetStats().lastCatchUpFrames <= keyframeInterval, "short catch-up %u frames", p.GetStats().lastCatchUpFrames);
		Log("  %-34s ok: short catch-up %u frames %.2f ms\n", g_context.c_str(), p.GetStats().lastCatchUpFrames, p.GetStats().lastCatchUpMs);

		// Unload resets the visibility
		p.SetVisible(false, step);
		p.Unload();
		CHECK(p.GetVisibility() == VideoVisibility::Visible);
	}

	void TestHiddenEnd(const Clip& clip)
	{
		const double step = 1.0 / 60.0;
		for (int pass = 0; pass < 2; ++pass)
		{
			const bool loop = pass == 1;
			g_context = std::string(loop ? "hidden loop " : "hidden end ") + clip.file;
			ManualClock clk;
			PlayerCore p(MakeConfig(clk));
			if (!LoadAndWait(p, ClipPath(clip), true))
				return;
			const double duration = p.GetInfo().durationSeconds;
			p.SetHiddenStopSeconds(0.2);
			CHECK(Succeeded(p.SetLoop(loop)));
			CHECK(Succeeded(p.Play()));
			double tEvent = -1.0;
			for (int i = 0; i < static_cast<int>((duration + 2.0) / step) && tEvent < 0.0; ++i)
			{
				const double before = clk.NowSeconds();
				HiddenStep(p, clk, step);
				if (!loop && p.GetState() == PlayerState::Ended)
					tEvent = before;
				if (loop && p.GetLoopCount() == 1)
					tEvent = before;
			}
			CHECK(p.GetVisibility() == VideoVisibility::Stopped);
			CHECKF(tEvent >= duration - 1e-3 - step && tEvent <= duration + step + 1e-6, "%s at clock %.3f, duration %.3f",
				loop ? "wrap" : "end", tEvent, duration);
			if (loop)
			{
				CHECK(p.GetState() == PlayerState::Playing);
				CHECK(p.GetTime() < 0.1);
			}
			Log("  %-34s ok: %s at clock %.3f (duration %.3f)\n", g_context.c_str(), loop ? "wrap" : "Ended", tEvent, duration);
		}
	}

	// =================================================================================================================
	// M8: packed ring layout, RGBA on demand, chroma siting, reference sampler
	// =================================================================================================================
	const char* const kM8Clips[] = {"vp9_444.webm", "vp9_320x181.webm", "vp9_siting_left.webm"};

	bool ClipExists(const char* file)
	{
		FILE* f = std::fopen((g_mediaDir + "/" + file).c_str(), "rb");
		if (f == nullptr)
			return false;
		std::fclose(f);
		return true;
	}

	void TestPackedRingLayout()
	{
		g_context = "packed ring";
		{
			FrameFormat f;
			f.width = 64; f.height = 48; f.withRgba = false;
			FrameRing ring;
			CHECK(Succeeded(ring.Init(f, 3)));
			CHECK(ring.IsPacked());
			CHECK(ring.PackedWidth() == 64 && ring.PackedHeight() == 72);
			const PlaneRect y = ring.PackedPlane(0), u = ring.PackedPlane(1), v = ring.PackedPlane(2), a = ring.PackedPlane(3);
			CHECK(y.x == 0 && y.y == 0 && y.w == 64 && y.h == 48);
			CHECK(u.x == 0 && u.y == 48 && u.w == 32 && u.h == 24);
			CHECK(v.x == 32 && v.y == 48 && v.w == 32 && v.h == 24);
			CHECK(a.w == 0 && a.h == 0);
			const FrameRing::Slot& s = ring.GetSlot(0);
			CHECK(s.packed != nullptr && s.plane[0] == s.packed && s.plane[1] == s.packed + 48 * 64 && s.plane[2] == s.packed + 48 * 64 + 32);
			CHECK(s.stride[0] == 64 && s.stride[1] == 64 && s.stride[2] == 64 && s.plane[3] == nullptr);
			CHECK(s.rgba == nullptr && !s.rgbaValid && ring.RgbaBufferCount() == 0);
			// neutral content: Y 16, U / V 128
			CHECK(s.packed[0] == 16 && s.packed[48 * 64 - 1] == 16 && s.packed[48 * 64] == 128 && s.packed[72 * 64 - 1] == 128);

			// RGBA on demand: only for owned slots; Free slots can be released by anybody
			uint64_t epoch = 0;
			const int w = ring.AcquireWrite(epoch);
			CHECK(w >= 0);
			CHECK(ring.EnsureRgba(static_cast<uint32_t>(w)));
			CHECK(ring.GetSlot(static_cast<uint32_t>(w)).rgba != nullptr && ring.GetSlot(static_cast<uint32_t>(w)).rgbaStride == 64 * 4);
			CHECK(ring.RgbaBufferCount() == 1);
			CHECK(ring.EnsureRgba(static_cast<uint32_t>(w))); // already there: no second buffer
			CHECK(ring.RgbaBufferCount() == 1);
			ring.ReleaseFreeRgba();                          // the slot is Writing: untouched
			CHECK(ring.RgbaBufferCount() == 1);
			ring.Abandon(w);
			ring.ReleaseFreeRgba();                          // now Free: freed
			CHECK(ring.RgbaBufferCount() == 0 && ring.GetSlot(static_cast<uint32_t>(w)).rgba == nullptr);
		}
		{
			// odd height with alpha: ch = 24, packed height 47 + 24 + 47
			FrameFormat f;
			f.width = 64; f.height = 47; f.hasAlpha = true; f.withRgba = true;
			FrameRing ring;
			CHECK(Succeeded(ring.Init(f, 3)));
			CHECK(ring.IsPacked() && ring.PackedHeight() == 118);
			const PlaneRect u = ring.PackedPlane(1), v = ring.PackedPlane(2), a = ring.PackedPlane(3);
			CHECK(u.y == 47 && u.h == 24 && v.x == 32 && v.y == 47);
			CHECK(a.x == 0 && a.y == 71 && a.w == 64 && a.h == 47);
			const FrameRing::Slot& s = ring.GetSlot(1);
			CHECK(s.plane[3] == s.packed + 71 * 64 && s.stride[3] == 64 && s.plane[3][0] == 255);
			CHECK(s.rgba != nullptr && ring.RgbaBufferCount() == 3); // withRgba: allocated at Init
		}
		// not packable: odd width, 4:4:4, 4:2:2, 4:4:0
		const int shapes[4][3] = {{63, 1, 1}, {64, 0, 0}, {64, 1, 0}, {64, 0, 1}};
		for (const auto& sh : shapes)
		{
			FrameFormat f;
			f.width = sh[0]; f.height = 48; f.chromaShiftX = sh[1]; f.chromaShiftY = sh[2];
			FrameRing ring;
			CHECK(Succeeded(ring.Init(f, 3)));
			CHECKF(!ring.IsPacked() && ring.GetSlot(0).packed == nullptr && ring.PackedWidth() == 0, "%dx48 shift %d,%d", sh[0], sh[1], sh[2]);
			CHECK(ring.GetSlot(0).plane[1] != nullptr && ring.GetSlot(0).rgba != nullptr);
		}
		Log("  %-34s ok\n", g_context.c_str());
	}

	//! WriteFrame into a slot must reproduce the decoder's planes byte for byte (packed or not).
	void TestPackedWriteFrame(const char* file, bool expectPacked)
	{
		g_context = std::string("packed write ") + file;
		std::unique_ptr<WebmDemuxer> d;
		CHECK(Succeeded(WebmDemuxer::Open(OpenSource(g_mediaDir + "/" + file, true), d)));
		if (!d)
			return;
		std::unique_ptr<VideoDecoder> dec;
		CHECK(Succeeded(VideoDecoder::Create(d->Video(), 2, dec)));
		if (!dec)
			return;
		std::vector<uint8_t> pkt(d->Video().maxPacketSize + 16), add(d->Video().maxAdditionalSize + 16);
		FrameRing ring;
		FrameFormat fmt;
		int frames = 0;
		bool same = true;
		for (uint32_t i = 0; i < d->VideoPacketCount() && frames < 12; ++i)
		{
			const PacketRef& ref = d->VideoPacket(i);
			CHECK(Succeeded(d->ReadVideoPacket(i, pkt.data(), static_cast<uint32_t>(pkt.size()))));
			uint32_t addSize = 0;
			if (ref.additionalSize > 0 && Succeeded(d->ReadVideoAdditional(i, add.data(), static_cast<uint32_t>(add.size()))))
				addSize = ref.additionalSize;
			if (Failed(dec->Decode(pkt.data(), ref.size, addSize ? add.data() : nullptr, addSize)) || !dec->HasFrame())
				continue;
			YuvaImage img;
			CHECK(Succeeded(dec->GetFrameView(img)));
			if (!ring.IsInitialized())
			{
				fmt.width = img.width; fmt.height = img.height;
				fmt.chromaShiftX = img.chromaShiftX; fmt.chromaShiftY = img.chromaShiftY;
				fmt.hasAlpha = d->Video().hasAlpha; fmt.withRgba = false;
				CHECK(Succeeded(ring.Init(fmt, 3)));
				CHECKF(ring.IsPacked() == expectPacked, "packed %d expected %d", ring.IsPacked() ? 1 : 0, expectPacked ? 1 : 0);
			}
			FrameRing::Slot& s = ring.GetSlot(static_cast<uint32_t>(frames % 3));
			CHECK(Succeeded(dec->WriteFrame(fmt, s)));
			const int cw = fmt.ChromaWidth(), ch = fmt.ChromaHeight();
			const int rows[4] = {fmt.height, ch, ch, fmt.height};
			const int cols[4] = {fmt.width, cw, cw, fmt.width};
			for (int p = 0; p < (fmt.hasAlpha ? 4 : 3); ++p)
			{
				if (img.plane[p] == nullptr || s.plane[p] == nullptr)
				{
					same = same && p == 3 && img.plane[3] == nullptr; // alpha missing in this frame: opaque
					continue;
				}
				for (int r = 0; r < rows[p]; ++r)
					same = same && std::memcmp(s.plane[p] + static_cast<ptrdiff_t>(r) * s.stride[p], img.plane[p] + static_cast<ptrdiff_t>(r) * img.stride[p], static_cast<size_t>(cols[p])) == 0;
			}
			if (ring.IsPacked())
			{
				const PlaneRect u = ring.PackedPlane(1), v = ring.PackedPlane(2), a = ring.PackedPlane(3);
				CHECK(s.plane[0] == s.packed && s.plane[1] == s.packed + static_cast<ptrdiff_t>(u.y) * fmt.width + u.x &&
					s.plane[2] == s.packed + static_cast<ptrdiff_t>(v.y) * fmt.width + v.x);
				CHECK(!fmt.hasAlpha || s.plane[3] == s.packed + static_cast<ptrdiff_t>(a.y) * fmt.width);
			}
			++frames;
		}
		CHECK(frames >= 10);
		CHECK(same);
		Log("  %-34s %s: %d frames, %s, planes identical to libvpx: %s\n", g_context.c_str(), same ? "ok" : "FAILED", frames,
			ring.IsPacked() ? "packed" : "separate planes", same ? "yes" : "NO");
	}

	void TestChromaSiting()
	{
		g_context = "chroma siting";
		CHECK(ResolveChromaSiting(1, 2) == ChromaSiting::Left);
		CHECK(ResolveChromaSiting(2, 2) == ChromaSiting::Center);
		CHECK(ResolveChromaSiting(1, 1) == ChromaSiting::TopLeft);
		CHECK(ResolveChromaSiting(-1, -1) == ChromaSiting::Left);
		CHECK(ResolveChromaSiting(0, 0) == ChromaSiting::Left);
		CHECK(ResolveChromaSiting(2, 1) == ChromaSiting::Left); // not expressible in the contract
		CHECK(ResolveChromaSiting(1, -1) == ChromaSiting::Left);

		// not tagged
		std::unique_ptr<WebmDemuxer> d;
		CHECK(Succeeded(WebmDemuxer::Open(OpenSource(g_mediaDir + "/vp9_alpha.webm", true), d)));
		if (d)
			CHECK(d->Video().chromaSitingHorz == -1 && d->Video().chromaSitingVert == -1);

		// tagged left (1, 2), and the same file with the tag bytes changed to (2, 2) and (1, 1)
		std::vector<uint8_t> bytes;
		CHECK(ReadClipBytes("vp9_siting_left.webm", bytes));
		size_t horz = 0, vert = 0;
		for (size_t i = 0; i + 3 < bytes.size() && (horz == 0 || vert == 0); ++i)
		{
			if (bytes[i] == 0x55 && bytes[i + 1] == 0xB7 && bytes[i + 2] == 0x81)
				horz = i + 3;
			if (bytes[i] == 0x55 && bytes[i + 1] == 0xB8 && bytes[i + 2] == 0x81)
				vert = i + 3;
		}
		CHECK(horz != 0 && vert != 0);
		if (horz == 0 || vert == 0)
			return;
		const uint8_t cases[3][2] = {{1, 2}, {2, 2}, {1, 1}};
		const ChromaSiting expect[3] = {ChromaSiting::Left, ChromaSiting::Center, ChromaSiting::TopLeft};
		for (int c = 0; c < 3; ++c)
		{
			std::vector<uint8_t> copy = bytes;
			if (c == 0)
				CHECK(copy[horz] == 1 && copy[vert] == 2); // as ffmpeg wrote it
			copy[horz] = cases[c][0];
			copy[vert] = cases[c][1];
			std::unique_ptr<WebmDemuxer> dm;
			std::vector<uint8_t> forDemux = copy;
			CHECK(Succeeded(WebmDemuxer::Open(std::make_unique<MemoryByteSource>(std::move(forDemux)), dm)));
			if (dm)
				CHECKF(dm->Video().chromaSitingHorz == cases[c][0] && dm->Video().chromaSitingVert == cases[c][1], "case %d: %d %d", c,
					dm->Video().chromaSitingHorz, dm->Video().chromaSitingVert);
			ManualClock clk;
			PlayerCore p(MakeConfig(clk));
			CHECK(Succeeded(p.Load(std::make_unique<MemoryByteSource>(std::move(copy)))));
			CHECK(WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 10000));
			CHECKF(p.GetLoadState() == LoadState::Ready && p.GetInfo().chromaSiting == expect[c], "case %d: siting %d", c, static_cast<int>(p.GetInfo().chromaSiting));
		}
		ManualClock clk;
		PlayerCore p(MakeConfig(clk));
		if (LoadAndWait(p, g_mediaDir + "/vp9_alpha.webm", true))
			CHECK(p.GetInfo().chromaSiting == ChromaSiting::Left); // not tagged -> Left
		Log("  %-34s ok\n", g_context.c_str());
	}

	cevref::Packed RefFromView(const FrameView& f, const MediaInfo& info)
	{
		cevref::Packed r;
		r.data = f.packed;
		r.packedWidth = f.packedWidth;
		r.packedHeight = f.packedHeight;
		r.width = f.width;
		r.height = f.height;
		r.y = {f.rectY.x, f.rectY.y, f.rectY.w, f.rectY.h};
		r.u = {f.rectU.x, f.rectU.y, f.rectU.w, f.rectU.h};
		r.v = {f.rectV.x, f.rectV.y, f.rectV.w, f.rectV.h};
		r.a = {f.rectA.x, f.rectA.y, f.rectA.w, f.rectA.h};
		r.bt709 = info.colorStandard == ColorStandard::BT709;
		r.full = info.colorRange == ColorRange::Full;
		r.siting = static_cast<cevref::Siting>(info.chromaSiting);
		return r;
	}

	struct RgbaWatch
	{
		const MediaInfo* info = nullptr;
		uint64_t withRgba = 0, withoutRgba = 0, checked = 0, mismatched = 0;
		bool compare = false;          // run the nearest reference on every 10th frame with RGBA
		std::vector<uint8_t> scratch;  // reserved before the allocation window
	};

	void WatchFrame(const FrameView& f, uint64_t index, void* user)
	{
		RgbaWatch& w = *static_cast<RgbaWatch*>(user);
		if (f.rgba == nullptr)
		{
			++w.withoutRgba;
			return;
		}
		++w.withRgba;
		if (w.compare && f.packed != nullptr && index % 10 == 0 && w.scratch.size() >= static_cast<size_t>(f.width) * f.height * 4)
		{
			cevref::SampleNearest(RefFromView(f, *w.info), w.scratch.data(), f.width * 4);
			++w.checked;
			for (int y = 0; y < f.height; ++y)
				if (std::memcmp(w.scratch.data() + static_cast<size_t>(y) * f.width * 4, f.rgba + static_cast<ptrdiff_t>(y) * f.rgbaStride, static_cast<size_t>(f.width) * 4) != 0)
				{
					++w.mismatched;
					break;
				}
		}
	}

	bool NearestMatches(const FrameView& f, const MediaInfo& info)
	{
		if (f.rgba == nullptr || f.packed == nullptr)
			return false;
		std::vector<uint8_t> ref(static_cast<size_t>(f.width) * f.height * 4);
		cevref::SampleNearest(RefFromView(f, info), ref.data(), f.width * 4);
		for (int y = 0; y < f.height; ++y)
			if (std::memcmp(ref.data() + static_cast<size_t>(y) * f.width * 4, f.rgba + static_cast<ptrdiff_t>(y) * f.rgbaStride, static_cast<size_t>(f.width) * 4) != 0)
				return false;
		return true;
	}

	//! RGBA switched off and on while playing: frame boundaries, the held frame converted at once, no allocation on the
	//! steady path, no drop, clock untouched.
	void TestRgbaSwitch(const Clip& clip)
	{
		g_context = std::string("rgba switch ") + clip.file;
		ManualClock clk;
		PlayerConfig cfg = MakeConfig(clk);
		cfg.outputRgba = false;
		PlayerCore p(cfg);
		if (!LoadAndWait(p, ClipPath(clip), true))
			return;
		const MediaInfo& info = p.GetInfo();
		CHECK(info.packedLayout);
		CHECK(!p.GetOutputRgba());
		CHECK(Succeeded(p.SetLoop(true))); // the test runs longer than the clip
		CHECK(p.RgbaBufferCount() == 0);
		RgbaWatch watch;
		watch.info = &info;
		watch.scratch.assign(static_cast<size_t>(clip.width) * clip.height * 4, 0);
		Sim sim;
		sim.Init(p, clk);
		sim.onFrame = &WatchFrame;
		sim.onFrameUser = &watch;
		CHECK(p.Update());
		sim.presented = 1;
		FrameView fv;
		CHECK(p.GetFrame(fv));
		const int ch = (clip.height + 1) / 2;
		CHECK(fv.rgba == nullptr && fv.packed != nullptr && fv.packedWidth == clip.width && fv.packedHeight == clip.height + ch + (clip.alpha ? clip.height : 0));
		CHECK(fv.rectY.w == clip.width && fv.rectU.y == clip.height && fv.rectV.x == clip.width / 2 && (fv.rectA.w > 0) == clip.alpha);
		CHECK(fv.plane[0] == fv.packed);
		CHECK(Succeeded(p.Play()));
		CHECK(sim.RunUntilPresented(20));
		CHECK(watch.withRgba == 0 && watch.withoutRgba >= 19);
		CHECK(p.GetStats().colorBytes == 0);

		// on: the held frame has RGBA at once and matches the reference
		CHECK(Succeeded(p.SetOutputRgba(true)));
		CHECK(p.GetOutputRgba());
		CHECK(p.GetFrame(fv) && fv.rgba != nullptr);
		CHECK(NearestMatches(fv, info));
		const uint64_t noRgbaBefore = watch.withoutRgba;
		CHECK(sim.RunUntilPresented(sim.presented + 10)); // warm-up: queued frames, buffers of the other slots
		CHECKF(watch.withoutRgba - noRgbaBefore <= 4, "%llu frames without RGBA after switching on", (unsigned long long)(watch.withoutRgba - noRgbaBefore));
		watch.compare = true;
		watch.withoutRgba = 0;
		uint64_t n0 = g_newCount.load(), d0 = g_deleteCount.load();
		const uint64_t alloc0 = p.GetStats().allocs;
		CHECK(sim.RunUntilPresented(sim.presented + 60));
		CHECKF(g_newCount.load() == n0 && g_deleteCount.load() == d0, "%llu allocations / %llu frees after switching on",
			(unsigned long long)(g_newCount.load() - n0), (unsigned long long)(g_deleteCount.load() - d0));
		CHECK(p.GetStats().allocs == alloc0);
		CHECK(watch.withoutRgba == 0 && watch.checked >= 5 && watch.mismatched == 0);
		CHECK(p.RgbaBufferCount() >= 2);

		// off: no RGBA from the current frame on, buffers freed
		watch.compare = false;
		CHECK(Succeeded(p.SetOutputRgba(false)));
		CHECK(p.GetFrame(fv) && fv.rgba == nullptr);
		const uint64_t color0 = p.GetStats().colorBytes;
		CHECK(sim.RunUntilPresented(sim.presented + 10));
		CHECK(p.RgbaBufferCount() == 0);
		watch.withRgba = 0;
		n0 = g_newCount.load(); d0 = g_deleteCount.load();
		CHECK(sim.RunUntilPresented(sim.presented + 60));
		CHECKF(g_newCount.load() == n0 && g_deleteCount.load() == d0, "%llu allocations / %llu frees after switching off",
			(unsigned long long)(g_newCount.load() - n0), (unsigned long long)(g_deleteCount.load() - d0));
		CHECK(watch.withRgba == 0);
		CHECKF(p.GetStats().colorBytes <= color0 + 4ull * clip.width * clip.height * 4, "colour conversion continued after off");

		const StatsSnapshot st = p.GetStats();
		CHECK(!sim.timedOut);
		CHECKF(st.framesDropped == 0, "dropped %llu", (unsigned long long)st.framesDropped);
		CHECKF(std::fabs(p.GetTime() - std::min(clk.NowSeconds(), info.durationSeconds)) < 1e-9, "the clock was touched");
		Log("  %-34s ok: %llu frames, switches on/off at frame boundaries, 0 allocations on the steady path\n", g_context.c_str(),
			(unsigned long long)sim.presented);
	}

	//! A stream that cannot be packed keeps RGBA8 whatever is requested.
	void TestRgbaForcedOn(const char* file)
	{
		g_context = std::string("rgba forced ") + file;
		ManualClock clk;
		PlayerConfig cfg = MakeConfig(clk);
		cfg.outputRgba = false;
		PlayerCore p(cfg);
		if (!LoadAndWait(p, g_mediaDir + "/" + file, true))
			return;
		CHECK(!p.GetInfo().packedLayout);
		CHECK(p.GetOutputRgba());
		CHECK(p.SetOutputRgba(false) == Status::Unsupported);
		CHECK(p.GetOutputRgba());
		CHECK(p.Update());
		FrameView fv;
		CHECK(p.GetFrame(fv) && fv.rgba != nullptr && fv.packed == nullptr && fv.packedWidth == 0);
		Log("  %-34s ok\n", g_context.c_str());
	}

	//! The nearest reference sampler applied to the packed buffer reproduces ConvertSlot byte for byte; the bilinear
	//! (consumer) variant is reported.
	void TestReferenceSampler(const std::string& path, int samples)
	{
		const size_t slash = path.find_last_of("/\\");
		g_context = std::string("reference ") + (slash == std::string::npos ? path : path.substr(slash + 1));
		ManualClock clk;
		PlayerCore p(MakeConfig(clk));
		if (!LoadAndWait(p, path, true))
			return;
		const MediaInfo& info = p.GetInfo();
		CHECK(info.packedLayout);
		if (!info.packedLayout)
			return;
		Sim sim;
		sim.Init(p, clk);
		CHECK(p.Update());
		sim.presented = 1;
		CHECK(Succeeded(p.Play()));
		const uint64_t stride = std::max<uint64_t>(1, info.videoPacketCount / static_cast<uint32_t>(samples + 1));
		std::vector<uint8_t> nearest(static_cast<size_t>(info.width) * info.height * 4), bilinear(nearest.size());
		int compared = 0, exact = 0, maxDiff = 0;
		double sumDiff = 0.0;
		uint64_t diffCount = 0;
		for (int k = 0; k < samples; ++k)
		{
			const uint64_t target = 1 + static_cast<uint64_t>(k) * stride;
			if (target > sim.presented && !sim.RunUntilPresented(target))
				break;
			FrameView fv;
			if (!p.GetFrame(fv) || fv.rgba == nullptr || fv.packed == nullptr)
				continue;
			const cevref::Packed ref = RefFromView(fv, info);
			cevref::SampleNearest(ref, nearest.data(), info.width * 4);
			cevref::SampleBilinear(ref, bilinear.data(), info.width * 4);
			bool same = true;
			for (int y = 0; y < info.height; ++y)
			{
				const uint8_t* got = fv.rgba + static_cast<ptrdiff_t>(y) * fv.rgbaStride;
				same = same && std::memcmp(nearest.data() + static_cast<size_t>(y) * info.width * 4, got, static_cast<size_t>(info.width) * 4) == 0;
				for (int x = 0; x < info.width * 4; ++x)
				{
					const int dd = std::abs(static_cast<int>(bilinear[static_cast<size_t>(y) * info.width * 4 + x]) - static_cast<int>(got[x]));
					maxDiff = std::max(maxDiff, dd);
					sumDiff += dd;
				}
				diffCount += static_cast<uint64_t>(info.width) * 4;
			}
			++compared;
			exact += same ? 1 : 0;
		}
		CHECK(compared == samples);
		CHECKF(exact == compared, "nearest reference differs from the converter in %d of %d frames", compared - exact, compared);
		Log("  %-34s %s: %dx%d%s %s %s siting %d, %d frames, nearest == converter in %d/%d, bilinear vs converter max %d mean %.3f\n",
			g_context.c_str(), exact == compared ? "ok" : "FAILED", info.width, info.height, info.hasAlpha ? " alpha" : "",
			info.colorStandard == ColorStandard::BT709 ? "BT709" : "BT601", info.colorRange == ColorRange::Full ? "full" : "limited",
			static_cast<int>(info.chromaSiting), compared, exact, compared, maxDiff, diffCount ? sumDiff / static_cast<double>(diffCount) : 0.0);
	
	// =================================================================================================================
	// M9: decoder backends (libvpx / Media Foundation), Decode = Auto | GPU | CPU
	// =================================================================================================================
	bool g_mfChecked = false;
	bool g_mfUsable = false;
	std::string g_mfWhy;

	//! Media Foundation hardware decode usable on this machine? (opens a backend on a VP9 clip once). When not, every
	//! MF item is reported as skipped instead of failing.
	bool MfUsable()
	{
		if (g_mfChecked)
			return g_mfUsable;
		g_mfChecked = true;
		if (!MediaFoundationCompiled())
		{
			g_mfWhy = "not compiled (no Windows)";
			return false;
		}
		std::unique_ptr<WebmDemuxer> d;
		if (Failed(WebmDemuxer::Open(OpenSource(g_mediaDir + "/vp9_alpha.webm", true), d)) || !d)
		{
			g_mfWhy = "cannot open vp9_alpha.webm";
			return false;
		}
		MediaFoundationConfig mc;
		mc.stream = d->Video();
		mc.maxPacketSize = d->Video().maxPacketSize;
		mc.maxAdditionalSize = d->Video().maxAdditionalSize;
		std::unique_ptr<IVideoDecoderBackend> b;
		char why[192] = {};
		g_mfUsable = Succeeded(CreateMediaFoundationBackend(mc, b, why, sizeof(why))) && b != nullptr;
		if (g_mfUsable)
		{
			const DecodeBackendInfo bi = b->Info();
			Log("  m9: Media Foundation usable, adapter \"%s\", alpha %s\n", bi.adapter, bi.alphaHardware ? "hardware" : "libvpx");
		}
		g_mfWhy = why;
		return g_mfUsable;
	}

	void LogSkip(const char* what)
	{
		Log("  %-34s SKIPPED: no hardware VP9 decode (%s)\n", what, g_mfWhy.c_str());
	}

	struct PlaneDiff
	{
		uint64_t differing = 0;
		int maxDiff = 0;
		double sum = 0.0;
		uint64_t count = 0;
	};

	//! The packed buffer of the Media Foundation backend equals libvpx's byte for byte (Y, U, V, A), every frame.
	void TestMfBytes(const std::string& path)
	{
		const size_t slash = path.find_last_of("/\\");
		g_context = std::string("mf bytes ") + (slash == std::string::npos ? path : path.substr(slash + 1));
		if (!MfUsable())
		{
			LogSkip(g_context.c_str());
			return;
		}
		std::unique_ptr<WebmDemuxer> d;
		CHECK(Succeeded(WebmDemuxer::Open(OpenSource(path, true), d)));
		if (!d)
			return;
		const VideoStreamInfo& v = d->Video();
		FrameFormat f;
		f.width = v.width;
		f.height = v.height;
		f.hasAlpha = v.hasAlpha;
		f.withRgba = false;
		FrameRing ringL, ringM;
		CHECK(Succeeded(ringL.Init(f, 2)));
		CHECK(Succeeded(ringM.Init(f, 6)));
		CHECK(ringL.IsPacked() && ringM.IsPacked());
		std::unique_ptr<IVideoDecoderBackend> lb, mb;
		CHECK(Succeeded(CreateLibvpxBackend(v, 4, lb)));
		MediaFoundationConfig mc;
		mc.stream = v;
		mc.maxPacketSize = v.maxPacketSize;
		mc.maxAdditionalSize = v.maxAdditionalSize;
		char why[192] = {};
		CHECKF(Succeeded(CreateMediaFoundationBackend(mc, mb, why, sizeof(why))), "MF backend: %s", why);
		if (!lb || !mb)
			return;
		CHECK(mb->Info().kind == DecodeBackendKind::MediaFoundation && mb->Info().hardware);
		CHECK(mb->MaxInFlight() == 3 && lb->MaxInFlight() == 1);

		const size_t packedBytes = static_cast<size_t>(ringL.PackedWidth()) * static_cast<size_t>(ringL.PackedHeight());
		const int kHist = 8;
		std::vector<std::vector<uint8_t>> hist(kHist, std::vector<uint8_t>(packedBytes));
		std::vector<int> histId(kHist, -1);
		std::vector<uint8_t> pkt(v.maxPacketSize + 16), add(v.maxAdditionalSize + 16);
		PlaneDiff diff[4];
		int libvpxPictures = 0, compared = 0, mfPictures = 0, mfFailed = 0, missing = 0;
		uint32_t mfInFlight = 0;
		double readback = 0.0, decode = 0.0;

		auto receiveMf = [&]()
		{
			DecodeTicket t;
			Status r = Status::Ok;
			while (mb->Receive(t, r))
			{
				--mfInFlight;
				if (r == Status::Ok)
				{
					++mfPictures;
					readback += t.readbackMs;
					decode += t.decodeMs;
					const int id = static_cast<int>(t.pts);
					const int h = id % kHist;
					if (histId[static_cast<size_t>(h)] != id)
						++missing;
					else
					{
						++compared;
						for (int pl = 0; pl < (f.hasAlpha ? 4 : 3); ++pl)
						{
							const PlaneRect rc = ringM.PackedPlane(pl);
							for (int y = 0; y < rc.h; ++y)
							{
								const size_t off = static_cast<size_t>(rc.y + y) * static_cast<size_t>(ringM.PackedWidth()) + static_cast<size_t>(rc.x);
								const uint8_t* a = t.slot->packed + off;
								const uint8_t* b = hist[static_cast<size_t>(h)].data() + off;
								if (std::memcmp(a, b, static_cast<size_t>(rc.w)) != 0)
								{
									for (int x = 0; x < rc.w; ++x)
									{
										const int dd = std::abs(static_cast<int>(a[x]) - static_cast<int>(b[x]));
										diff[pl].differing += dd != 0 ? 1u : 0u;
										diff[pl].maxDiff = std::max(diff[pl].maxDiff, dd);
										diff[pl].sum += dd;
									}
								}
								diff[pl].count += static_cast<uint64_t>(rc.w);
							}
						}
					}
				}
				else
					++mfFailed;
				ringM.Abandon(t.slotIndex);
			}
		};

		const uint32_t n = d->VideoPacketCount();
		for (uint32_t i = 0; i < n; ++i)
		{
			const PacketRef& ref = d->VideoPacket(i);
			CHECK(Succeeded(d->ReadVideoPacket(i, pkt.data(), static_cast<uint32_t>(pkt.size()))));
			const bool alpha = v.hasAlpha && ref.additionalSize > 0 && Succeeded(d->ReadVideoAdditional(i, add.data(), static_cast<uint32_t>(add.size())));
			DecodePacket cp{pkt.data(), ref.size, ref.keyframe};
			DecodePacket ap{add.data(), alpha ? ref.additionalSize : 0u, ref.keyframe};

			// libvpx: synchronous
			uint64_t e = 0;
			const int ls = ringL.AcquireWrite(e);
			DecodeTicket lt;
			lt.epoch = e;
			lt.pts = static_cast<double>(i);
			lt.slotIndex = ls;
			lt.slot = &ringL.GetSlot(static_cast<uint32_t>(ls));
			CHECK(Succeeded(lb->Submit(cp, alpha ? &ap : nullptr, lt)));
			DecodeTicket done;
			Status r = Status::Ok;
			CHECK(lb->Receive(done, r) && done.slotIndex == ls);
			if (r == Status::Ok)
			{
				++libvpxPictures;
				std::memcpy(hist[i % kHist].data(), done.slot->packed, packedBytes);
				histId[i % kHist] = static_cast<int>(i);
			}
			ringL.Abandon(ls);

			// Media Foundation: asynchronous, at most MaxInFlight() pictures in flight
			for (int guard = 0; guard < 5000; ++guard)
			{
				receiveMf();
				if (mfInFlight >= mb->MaxInFlight())
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
				const int ms = ringM.AcquireWrite(e);
				CHECK(ms >= 0);
				DecodeTicket mt;
				mt.epoch = e;
				mt.pts = static_cast<double>(i);
				mt.slotIndex = ms;
				mt.slot = &ringM.GetSlot(static_cast<uint32_t>(ms));
				const Status st = mb->Submit(cp, alpha ? &ap : nullptr, mt);
				if (st == Status::Busy)
				{
					ringM.Abandon(ms);
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
					continue;
				}
				CHECKF(st == Status::Ok, "MF submit %u: %s", i, StatusToString(st));
				if (st == Status::Ok)
					++mfInFlight;
				else
					ringM.Abandon(ms);
				break;
			}
		}
		mb->Drain();
		CHECK(WaitFor([&] { receiveMf(); return mfInFlight == 0; }, 10000));
		CHECKF(mfFailed == 0 && missing == 0, "MF failed %d, unmatched %d", mfFailed, missing);
		CHECKF(compared == libvpxPictures && mfPictures == libvpxPictures, "compared %d, MF pictures %d, libvpx pictures %d", compared, mfPictures, libvpxPictures);
		const char* names[4] = {"Y", "U", "V", "A"};
		bool identical = true;
		char line[512];
		int len = 0;
		line[0] = 0;
		for (int pl = 0; pl < (f.hasAlpha ? 4 : 3); ++pl)
		{
			identical = identical && diff[pl].differing == 0;
			len += std::snprintf(line + len, sizeof(line) - static_cast<size_t>(len), " %s: %llu/%llu differ, max %d, mean %.6f;", names[pl],
				(unsigned long long)diff[pl].differing, (unsigned long long)diff[pl].count, diff[pl].maxDiff,
				diff[pl].count ? diff[pl].sum / static_cast<double>(diff[pl].count) : 0.0);
		}
		CHECKF(identical, "packed buffers differ:%s", line);
		Log("  %-34s %s: %d frames byte compared, MF decode %.3f ms, read-back %.3f ms per frame;%s\n", g_context.c_str(),
			identical ? "identical" : "DIFFERENT", compared, compared ? decode / compared : 0.0, compared ? readback / compared : 0.0, line);
	}

	PlayerConfig MakeConfigMode(ManualClock& clk, DecodeMode mode)
	{
		PlayerConfig cfg = MakeConfig(clk);
		cfg.decodeMode = mode;
		cfg.outputRgba = false;
		return cfg;
	}

	bool PackedEqual(const FrameView& a, const FrameView& b)
	{
		if (a.packed == nullptr || b.packed == nullptr || a.packedWidth != b.packedWidth || a.packedHeight != b.packedHeight)
			return false;
		return std::memcmp(a.packed, b.packed, static_cast<size_t>(a.packedWidth) * static_cast<size_t>(a.packedHeight)) == 0;
	}

	//! The selection rules and the fallbacks of PlayerCore (VP8, Auto threshold, simulated missing MFT and failing
	//! SetInputType).
	void TestDecodeSelection()
	{
		g_context = "m9 selection";
		ManualClock clk;
		{
			PlayerCore p(MakeConfigMode(clk, DecodeMode::Cpu));
			if (LoadAndWait(p, g_mediaDir + "/vp9_alpha.webm", true))
			{
				const DecodeBackendInfo bi = p.GetDecodeBackend();
				CHECK(bi.kind == DecodeBackendKind::Libvpx && !bi.hardware && bi.reason[0] == 0);
			}
		}
		{
			PlayerCore p(MakeConfigMode(clk, DecodeMode::Gpu));
			if (LoadAndWait(p, g_mediaDir + "/vp8_vorbis.webm", true))
			{
				const DecodeBackendInfo bi = p.GetDecodeBackend();
				CHECKF(bi.kind == DecodeBackendKind::Libvpx && !bi.hardware && bi.reason[0] != 0, "VP8 with GPU: kind %d reason '%s'",
					static_cast<int>(bi.kind), bi.reason);
				Log("  %-34s ok: VP8 + GPU -> libvpx (%s)\n", g_context.c_str(), bi.reason);
			}
		}
		{
			// Auto below the threshold (360p < 1080): libvpx
			PlayerCore p(MakeConfigMode(clk, DecodeMode::Auto));
			if (LoadAndWait(p, g_mediaDir + "/vp9_alpha.webm", true))
				CHECK(p.GetDecodeBackend().kind == DecodeBackendKind::Libvpx);
		}
		const MediaFoundationTestFault faults[2] = {MediaFoundationTestFault::NoDecoderMft, MediaFoundationTestFault::SetInputTypeFails};
		for (MediaFoundationTestFault fault : faults)
		{
			SetMediaFoundationTestFault(fault);
			PlayerCore p(MakeConfigMode(clk, DecodeMode::Gpu));
			if (LoadAndWait(p, g_mediaDir + "/vp9_alpha.webm", true))
			{
				const DecodeBackendInfo bi = p.GetDecodeBackend();
				CHECKF(bi.kind == DecodeBackendKind::Libvpx && bi.reason[0] != 0, "fault %d: kind %d reason '%s'", static_cast<int>(fault),
					static_cast<int>(bi.kind), bi.reason);
				Sim sim;
				sim.Init(p, clk);
				CHECK(p.Update());
				sim.presented = 1;
				CHECK(Succeeded(p.Play()));
				CHECK(sim.RunUntilPresented(30));
				Log("  %-34s ok: fault %d -> libvpx (%s)\n", g_context.c_str(), static_cast<int>(fault), bi.reason);
			}
			SetMediaFoundationTestFault(MediaFoundationTestFault::None);
		}
		CHECK(MediaFoundationStartupCount() == 0);
		if (!MfUsable())
		{
			LogSkip("m9 selection (GPU / Auto 1080p)");
			return;
		}
		{
			PlayerCore p(MakeConfigMode(clk, DecodeMode::Gpu));
			if (LoadAndWait(p, g_mediaDir + "/vp9_alpha.webm", true))
			{
				const DecodeBackendInfo bi = p.GetDecodeBackend();
				CHECKF(bi.kind == DecodeBackendKind::MediaFoundation && bi.hardware && bi.reason[0] == 0, "GPU: kind %d reason '%s'",
					static_cast<int>(bi.kind), bi.reason);
				CHECK(MediaFoundationStartupCount() == 1);
			}
		}
		CHECK(MediaFoundationStartupCount() == 0);
		{
			PlayerCore p(MakeConfigMode(clk, DecodeMode::Auto));
			if (LoadAndWait(p, g_mediaDir + "/vp9_1080p30_opus.webm", true))
				CHECK(p.GetDecodeBackend().kind == DecodeBackendKind::MediaFoundation);
		}
	}

	//! 20 seeks: the first frame shown by the GPU player equals the CPU player's byte for byte.
	void TestMfSeekCompare(const std::string& path)
	{
		const size_t slash = path.find_last_of("/\\");
		g_context = std::string("mf seek ") + (slash == std::string::npos ? path : path.substr(slash + 1));
		if (!MfUsable())
		{
			LogSkip(g_context.c_str());
			return;
		}
		ManualClock ca, cb;
		PlayerCore cpu(MakeConfigMode(ca, DecodeMode::Cpu));
		PlayerCore gpu(MakeConfigMode(cb, DecodeMode::Gpu));
		if (!LoadAndWait(cpu, path, true) || !LoadAndWait(gpu, path, true))
			return;
		CHECK(gpu.GetDecodeBackend().kind == DecodeBackendKind::MediaFoundation);
		const double duration = cpu.GetInfo().durationSeconds;
		CHECK(Succeeded(cpu.Play()) && Succeeded(cpu.Pause()));
		CHECK(Succeeded(gpu.Play()) && Succeeded(gpu.Pause()));
		uint32_t seed = 12345u;
		int same = 0;
		for (int k = 0; k < 20; ++k)
		{
			seed = seed * 1664525u + 1013904223u;
			const double t = (static_cast<double>(seed >> 8) / static_cast<double>(1u << 24)) * std::max(0.1, duration - 0.5);
			CHECK(Succeeded(cpu.Seek(t)) && Succeeded(gpu.Seek(t)));
			const bool ra = WaitFor([&] { return cpu.VideoBufferedFrame(); }, 10000);
			const bool rb = WaitFor([&] { return gpu.VideoBufferedFrame(); }, 10000);
			CHECKF(ra && rb, "seek %.3f: frame buffered cpu %d gpu %d", t, ra ? 1 : 0, rb ? 1 : 0);
			cpu.Update();
			gpu.Update();
			FrameView fa, fb;
			CHECK(cpu.GetFrame(fa) && gpu.GetFrame(fb));
			const bool eq = std::fabs(fa.pts - fb.pts) < 1e-6 && PackedEqual(fa, fb);
			CHECKF(eq, "seek %.3f: cpu pts %.3f gpu pts %.3f, packed %s", t, fa.pts, fb.pts, PackedEqual(fa, fb) ? "equal" : "different");
			same += eq ? 1 : 0;
		}
		Log("  %-34s %s: %d/20 seeks identical\n", g_context.c_str(), same == 20 ? "ok" : "FAILED", same);
	}

	//! A run of the existing playback tests with the GPU backend (loop three rounds, M7 hidden / stopped / catch-up,
	//! seek, steady state allocations).
	void TestMfReuse()
	{
		if (!MfUsable())
		{
			LogSkip("m9 loop / hidden / seek on MF");
			return;
		}
		g_decodeMode = DecodeMode::Gpu;
		TestLoop(*FindClip("vp9_alpha.webm"));
		TestHiddenCatchUp(*FindClip("vp9_1080p30_opus.webm"), 300);
		TestHiddenEnd(*FindClip("vp9_alpha.webm"));
		TestSeek(*FindClip("vp9_alpha.webm"), 3.3);
		TestSeek(*FindClip("vp9_1080p30_opus.webm"), 5.0);
		TestPlayback(*FindClip("vp9_1080p30_opus.webm"));
		TestSteadyStateAllocations(*FindClip("vp9_alpha.webm"));
		g_decodeMode = DecodeMode::Cpu;
		// the runs above really used Media Foundation
		g_context = "m9 reuse";
		ManualClock clk;
		PlayerCore p(MakeConfigMode(clk, DecodeMode::Gpu));
		if (LoadAndWait(p, ClipPath(*FindClip("vp9_alpha.webm")), true))
			CHECK(p.GetDecodeBackend().kind == DecodeBackendKind::MediaFoundation);
	}

	//! The device disappears during playback: the player continues on libvpx from where it was.
	void TestMfDeviceLost()
	{
		g_context = "mf device lost";
		if (!MfUsable())
		{
			LogSkip(g_context.c_str());
			return;
		}
		SetMediaFoundationTestFault(MediaFoundationTestFault::DeviceLostAfter, 40);
		ManualClock clk;
		const Clip& clip = *FindClip("vp9_alpha.webm");
		PlayerCore p(MakeConfigMode(clk, DecodeMode::Gpu));
		if (LoadAndWait(p, ClipPath(clip), true))
		{
			CHECK(p.GetDecodeBackend().kind == DecodeBackendKind::MediaFoundation);
			Sim sim;
			sim.Init(p, clk);
			CHECK(p.Update());
			sim.presented = 1;
			CHECK(Succeeded(p.Play()));
			CHECK(sim.RunToEnd(clip.duration + 2.0));
			CHECK(!sim.timedOut);
			CHECK(p.GetState() == PlayerState::Ended);
			const DecodeBackendInfo bi = p.GetDecodeBackend();
			CHECKF(bi.kind == DecodeBackendKind::Libvpx && std::strstr(bi.reason, "device") != nullptr, "after loss: kind %d reason '%s'",
				static_cast<int>(bi.kind), bi.reason);
			CHECKF(sim.presented + 3 >= clip.videoFrames && sim.presented <= clip.videoFrames, "presented %llu of %u",
				(unsigned long long)sim.presented, clip.videoFrames);
			Log("  %-34s ok: %llu/%u frames, continued on libvpx (%s)\n", g_context.c_str(), (unsigned long long)sim.presented, clip.videoFrames, bi.reason);
		}
		SetMediaFoundationTestFault(MediaFoundationTestFault::None);
	}

	//! Six 1080p players with Decode = GPU at once: all load and play (the ones that get no hardware decoder fall back),
	//! destruction is bounded, MFStartup / MFShutdown stay paired.
	void TestMfSixPlayers(const std::string& path)
	{
		g_context = "mf six players";
		if (!MfUsable())
		{
			LogSkip(g_context.c_str());
			return;
		}
		const int kPlayers = 6;
		ManualClock clk[kPlayers];
		std::unique_ptr<PlayerCore> p[kPlayers];
		for (int i = 0; i < kPlayers; ++i)
		{
			p[i] = std::make_unique<PlayerCore>(MakeConfigMode(clk[i], DecodeMode::Gpu));
			std::unique_ptr<IByteSource> src = OpenSource(path, true);
			CHECK(src != nullptr);
			if (src)
				CHECK(Succeeded(p[i]->Load(std::move(src))));
		}
		int hw = 0, ready = 0;
		for (int i = 0; i < kPlayers; ++i)
		{
			CHECK(WaitFor([&] { return p[i]->GetLoadState() != LoadState::Loading; }, 20000));
			if (p[i]->GetLoadState() == LoadState::Ready)
			{
				++ready;
				const DecodeBackendInfo bi = p[i]->GetDecodeBackend();
				hw += bi.hardware ? 1 : 0;
				if (!bi.hardware)
					Log("  %-34s player %d on libvpx: %s\n", g_context.c_str(), i, bi.reason);
			}
		}
		CHECK(ready == kPlayers);
		std::vector<Sim> sim(kPlayers);
		for (int i = 0; i < kPlayers; ++i)
		{
			sim[i].Init(*p[i], clk[i]);
			CHECK(p[i]->Update());
			sim[i].presented = 1;
			CHECK(Succeeded(p[i]->Play()));
		}
		for (int s = 0; s < 120; ++s)
			for (int i = 0; i < kPlayers; ++i)
				CHECK(sim[i].Step());
		for (int i = 0; i < kPlayers; ++i)
			CHECKF(sim[i].presented >= 55, "player %d presented %llu", i, (unsigned long long)sim[i].presented);
		const auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < kPlayers; ++i)
			p[i].reset();
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		CHECKF(ms < 3000.0, "destroying six players took %.0f ms", ms);
		CHECK(MediaFoundationStartupCount() == 0);
		Log("  %-34s ok: %d/%d ready, %d with hardware decode, destroyed in %.0f ms\n", g_context.c_str(), ready, kPlayers, hw, ms);
	}
}

int main(int argc, char** argv)
{
#if defined(CEV_DEFAULT_MEDIA_DIR)
	g_mediaDir = CEV_DEFAULT_MEDIA_DIR;
#endif
	const char* only = nullptr;
	std::string benchDir; // M8: directory of the six measurement clips (reference sampler on real content)
	int stressIterations = 20;
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--media") == 0 && i + 1 < argc)
			g_mediaDir = argv[++i];
		else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc)
			only = argv[++i];
		else if (std::strcmp(argv[i], "--stress") == 0 && i + 1 < argc)
			stressIterations = std::atoi(argv[++i]);
		else if (std::strcmp(argv[i], "--bench") == 0 && i + 1 < argc)
			benchDir = argv[++i];
		else if (std::strcmp(argv[i], "--verbose") == 0)
			g_verbose = true;
		else
		{
			std::printf("usage: cevcore_selftest [--media <dir>] [--only <substring>] [--stress <n>] [--bench <dir>] [--verbose]\n");
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
	if (wanted("descriptor")) TestDescriptorPlan();

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
	if (wanted("m2fix2"))
		TestM2Fix2();
	if (wanted("harden"))
	{
		TestMalformedIndex();
		TestAudioRateAndRing();
		TestBogusTimestamps();
		TestMidStreamResize();
		TestAudioThreadVersusUnload();
		TestWorkerExceptions();
	}
	if (wanted("hidden"))
	{
		TestHiddenCatchUp(*FindClip("vp8_vorbis.webm"), 50); // -g 50: a keyframe every 2 s
		TestHiddenEnd(*FindClip("vp9_odd_321x181.webm"));
	}
	if (wanted("packed"))
	{
		for (const char* f : kM8Clips)
		{
			g_context = f;
			CHECKF(ClipExists(f), "missing clip %s/%s (tools/make_test_media.sh)", g_mediaDir.c_str(), f);
		}
		TestPackedRingLayout();
		TestPackedWriteFrame("vp9_alpha.webm", true);
		TestPackedWriteFrame("vp9_1080p30_opus.webm", true);
		TestPackedWriteFrame("vp8_vorbis.webm", true);
		TestPackedWriteFrame("vp9_320x181.webm", true);
		TestPackedWriteFrame("vp9_odd_321x181.webm", false);
		TestPackedWriteFrame("vp9_444.webm", false);
		TestChromaSiting();
		TestRgbaSwitch(*FindClip("vp9_alpha.webm"));
		TestRgbaSwitch(*FindClip("vp8_vorbis.webm"));
		TestRgbaForcedOn("vp9_odd_321x181.webm");
		TestRgbaForcedOn("vp9_444.webm");
		TestReferenceSampler(g_mediaDir + "/vp9_alpha.webm", 5);
		TestReferenceSampler(g_mediaDir + "/vp9_320x181.webm", 5);
		TestReferenceSampler(g_mediaDir + "/vp8_vorbis.webm", 5);
		if (!benchDir.empty())
			for (const char* f : { "vp9_1080p30.webm", "vp9_1080p30_alpha.webm", "vp9_1080p60.webm", "vp9_1080p60_alpha.webm", "vp9_2160p30.webm", "vp9_2160p30_alpha.webm" })
				TestReferenceSampler(benchDir + "/" + f, 4);
	}
	if (wanted("m9"))
	{
		TestDecodeSelection();
		if (!benchDir.empty())
			for (const char* f : { "vp9_1080p30.webm", "vp9_1080p30_alpha.webm", "vp9_1080p60.webm", "vp9_1080p60_alpha.webm", "vp9_2160p30.webm", "vp9_2160p30_alpha.webm" })
				TestMfBytes(benchDir + "/" + f);
		else
			Log("  m9 bytes                           SKIPPED: no --bench directory\n");
		TestMfBytes(g_mediaDir + "/vp9_alpha.webm");
		TestMfBytes(g_mediaDir + "/vp9_1080p30_opus.webm");
		TestMfSeekCompare(g_mediaDir + "/vp9_1080p30_opus.webm");
		TestMfSeekCompare(g_mediaDir + "/vp9_alpha.webm");
		if (!benchDir.empty())
			TestMfSeekCompare(benchDir + "/vp9_2160p30_alpha.webm");
		TestMfReuse();
		TestMfDeviceLost();
		TestMfSixPlayers(g_mediaDir + "/vp9_1080p30_opus.webm");
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
