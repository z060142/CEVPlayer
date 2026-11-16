// CEVPlayerOffline selftest. Plain executable (no engine, no test framework).
//
//   cevoffline_selftest [--media <dir>] [--only <substring>] [--out <dir>] [--keep] [--verbose]
//
// The media directory holds the clips produced by tools/make_test_media.sh. Every output is loaded back through
// cev::PlayerCore / libvorbisfile and compared with the embedded stream.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <vorbis/vorbisfile.h>

#include "CEVCore/IClock.h"
#include "CEVCore/IByteSource.h"
#include "CEVCore/PlayerCore.h"
#include "CEVCore/Resampler.h"
#include "CEVCore/WebmDemuxer.h"
#include "CEVOffline/Offline.h"

namespace fs = std::filesystem;
using namespace cev;
using namespace cev::offline;

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

	void Fail(const char* file, int line, const char* expr, const char* fmt = nullptr, ...)
	{
		++g_failures;
		std::printf("  FAIL [%s] %s:%d: %s", g_context.c_str(), file, line, expr);
		if (fmt != nullptr)
		{
			std::printf(" -- ");
			va_list ap;
			va_start(ap, fmt);
			std::vprintf(fmt, ap);
			va_end(ap);
		}
		std::printf("\n");
		std::fflush(stdout);
	}

#define CHECK(cond) do { ++g_checks; if (!(cond)) Fail(__FILE__, __LINE__, #cond); } while (0)
#define CHECKF(cond, ...) do { ++g_checks; if (!(cond)) Fail(__FILE__, __LINE__, #cond, __VA_ARGS__); } while (0)

	template <typename Pred>
	bool WaitFor(Pred pred, int timeoutMs)
	{
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (!pred())
		{
			if (std::chrono::steady_clock::now() > end)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return true;
	}

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
	struct Clip
	{
		const char* file;
		uint32_t videoFrames;   // 0 = no video
		int width, height;
		bool alpha;
		AudioCodec audio;       // None = no audio track
		double tone;            // Hz of the sine in the audio (0 = beep clip / none)
		double toneStart;       // seconds on the video timeline where the sine starts: the first audio block time (-itsoffset 1.5 ends up as 1.501 s in the file, ms muxer rounding)
		const char* language;   // expected Matroska language ("" = not checked)
	};

	const Clip kClips[] = {
		{"vp9_1080p30_opus.webm", 300, 1920, 1080, false, AudioCodec::Opus, 440.0, 0.0, ""},
		{"vp8_vorbis.webm", 125, 640, 360, false, AudioCodec::Vorbis, 523.0, 0.0, ""},
		{"opus_preskip.webm", 150, 320, 180, false, AudioCodec::Opus, 330.0, 0.0, ""},
		{"audio_start_offset.webm", 150, 640, 360, false, AudioCodec::Opus, 660.0, 1.501, ""},
		{"av_sync_flash_beep.webm", 300, 1280, 720, false, AudioCodec::Opus, 0.0, 0.0, ""},
		{"vp9_alpha.webm", 150, 640, 360, true, AudioCodec::None, 0.0, 0.0, ""},
		{"audio_en.webm", 0, 0, 0, false, AudioCodec::Opus, 440.0, 0.0, "eng"},
	};

	std::string g_mediaDir;
	std::string g_outDir;
	bool g_keep = false;

	std::string ClipPath(const Clip& c) { return g_mediaDir + "/" + c.file; }
	std::string OutPath(const std::string& name) { return g_outDir + "/" + name; }

	bool FileExists(const std::string& p)
	{
		std::error_code ec;
		return fs::exists(fs::u8path(p), ec);
	}

	uint64_t FileSizeOf(const std::string& p)
	{
		std::error_code ec;
		const auto s = fs::file_size(fs::u8path(p), ec);
		return ec ? 0 : static_cast<uint64_t>(s);
	}

	bool OutputGone(const std::string& p) { return !FileExists(p) && !FileExists(p + ".tmp"); }

	void RemoveOutput(const std::string& p)
	{
		std::error_code ec;
		fs::remove(fs::u8path(p), ec);
		fs::remove(fs::u8path(p + ".tmp"), ec);
	}

	std::vector<uint8_t> ReadFile(const std::string& p)
	{
		std::vector<uint8_t> data;
		std::FILE* f = std::fopen(fs::u8path(p).string().c_str(), "rb");
		if (f == nullptr)
			return data;
		std::fseek(f, 0, SEEK_END);
		const long n = std::ftell(f);
		std::fseek(f, 0, SEEK_SET);
		if (n > 0)
		{
			data.resize(static_cast<size_t>(n));
			if (std::fread(data.data(), 1, data.size(), f) != data.size())
				data.clear();
		}
		std::fclose(f);
		return data;
	}

	void WriteBytes(const std::string& p, const void* data, size_t size)
	{
		std::FILE* f = std::fopen(fs::u8path(p).string().c_str(), "wb");
		if (f == nullptr)
			return;
		if (size > 0)
			std::fwrite(data, 1, size, f);
		std::fclose(f);
	}

	bool ResultIs(const Result& r, Status s)
	{
		if (r.status != s)
			Log("    got %s (%s), expected %s\n", StatusToString(r.status), r.message.c_str(), StatusToString(s));
		return r.status == s;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Playback of a file through PlayerCore: the audio exactly as the engine plays it, from t = 0 (leading silence
	// for a late start included).
	// ---------------------------------------------------------------------------------------------------------------
	struct Pcm
	{
		std::vector<float> data;
		uint32_t rate = 0;
		uint32_t channels = 0;
		uint64_t Frames() const { return channels != 0 ? data.size() / channels : 0; }
	};

	bool LoadReady(PlayerCore& p, const std::string& path)
	{
		std::unique_ptr<IByteSource> src;
		if (Failed(FileByteSource::Open(path.c_str(), false, src)))
			return false;
		if (Failed(p.Load(std::move(src))))
			return false;
		if (!WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 20000))
			return false;
		return p.GetLoadState() == LoadState::Ready;
	}

	bool CaptureAudio(const std::string& path, Pcm& out)
	{
		out = Pcm();
		ManualClock clock;
		PlayerConfig cfg;
		cfg.decodeVideo = false;
		cfg.externalClock = &clock;
		PlayerCore p(cfg);
		if (!LoadReady(p, path) || !p.GetInfo().hasAudio)
			return false;
		out.rate = p.GetInfo().audioSampleRate;
		out.channels = p.GetInfo().audioChannels;
		if (Failed(p.Play()))
			return false;
		std::vector<float> buf(static_cast<size_t>(4096) * out.channels);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		for (;;)
		{
			// Update() is what wakes the decode worker once the ring has room again (no video to pace it here).
			if (!WaitFor([&] { p.Update(); return p.AudioBufferedFrames() >= 4096 || p.AudioDecoderAtEnd(); }, 20000))
				return false;
			const uint32_t got = p.ReadAudio(buf.data(), 4096);
			out.data.insert(out.data.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(got) * out.channels));
			if (p.AudioEnded())
				break;
			if (got == 0 && p.AudioDecoderAtEnd() && p.AudioBufferedFrames() == 0)
				break;
			if (std::chrono::steady_clock::now() > deadline)
				return false;
		}
		p.Unload();
		return true;
	}

	uint64_t Fnv(uint64_t h, const uint8_t* d, size_t n)
	{
		for (size_t i = 0; i < n; ++i)
			h = (h ^ d[i]) * 1099511628211ull;
		return h;
	}

	struct FrameHash
	{
		bool ok = false;
		bool alphaPlane = false;
		uint64_t y = 0;
		uint64_t a = 0;
	};

	// Hash of the Y and A planes of the first presented frame.
	FrameHash FirstFrame(const std::string& path)
	{
		FrameHash fh;
		ManualClock clock;
		PlayerConfig cfg;
		cfg.decodeAudio = false;
		cfg.outputRgba = false;
		cfg.externalClock = &clock;
		PlayerCore p(cfg);
		if (!LoadReady(p, path) || !p.GetInfo().hasVideo)
			return fh;
		if (Failed(p.Play()))
			return fh;
		for (int i = 0; i < 300; ++i)
		{
			WaitFor([&] { return p.VideoBufferedFrame() || p.VideoDecoderAtEnd(); }, 15000);
			if (p.Update())
			{
				FrameView fv;
				if (!p.GetFrame(fv))
					return fh;
				fh.y = 1469598103934665603ull;
				for (int row = 0; row < fv.height; ++row)
					fh.y = Fnv(fh.y, fv.plane[0] + static_cast<size_t>(row) * static_cast<size_t>(fv.stride[0]), static_cast<size_t>(fv.width));
				if (fv.plane[3] != nullptr)
				{
					fh.alphaPlane = true;
					fh.a = 1469598103934665603ull;
					for (int row = 0; row < fv.height; ++row)
						fh.a = Fnv(fh.a, fv.plane[3] + static_cast<size_t>(row) * static_cast<size_t>(fv.stride[3]), static_cast<size_t>(fv.width));
				}
				fh.ok = true;
				return fh;
			}
			clock.Advance(1.0 / 60.0);
		}
		return fh;
	}

	int16_t ToS16(float x)
	{
		float v = x * 32767.0f;
		if (v > 32767.0f) v = 32767.0f;
		if (v < -32768.0f) v = -32768.0f;
		return static_cast<int16_t>(std::lroundf(v));
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Signal analysis
	// ---------------------------------------------------------------------------------------------------------------
	// Least squares fit of  a sin(w (n - t0)) + b cos(w (n - t0))  over [from, to) of channel 0; returns the delay d in
	// samples such that the signal is  R sin(w (n - t0 - d))  (d > 0: the audio is late), and the amplitude R.
	double FitDelay(const std::vector<double>& x, size_t from, size_t to, double freq, double rate, double t0, double* amp)
	{
		const double w = 2.0 * 3.14159265358979323846 * freq / rate;
		double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
		for (size_t n = from; n < to && n < x.size(); ++n)
		{
			const double ph = w * (static_cast<double>(n) - t0);
			const double s = std::sin(ph), c = std::cos(ph);
			ss += s * s; sc += s * c; cc += c * c;
			ys += x[n] * s; yc += x[n] * c;
		}
		const double det = ss * cc - sc * sc;
		const double a = (ys * cc - yc * sc) / det;
		const double b = (yc * ss - ys * sc) / det;
		if (amp != nullptr)
			*amp = std::sqrt(a * a + b * b);
		return std::atan2(-b, a) / w;
	}

	// Channel 0 of interleaved int16 / float as double.
	std::vector<double> Channel0(const std::vector<int16_t>& pcm, uint32_t channels)
	{
		std::vector<double> r(pcm.size() / channels);
		for (size_t i = 0; i < r.size(); ++i)
			r[i] = pcm[i * channels] / 32767.0;
		return r;
	}
	std::vector<double> Channel0(const std::vector<float>& pcm, uint32_t channels)
	{
		std::vector<double> r(pcm.size() / channels);
		for (size_t i = 0; i < r.size(); ++i)
			r[i] = pcm[i * channels];
		return r;
	}

	// Shift s in [-maxShift, maxShift] that minimises sum |a[i] - b[i + s]| over the interior.
	int BestShift(const std::vector<int16_t>& a, const std::vector<int16_t>& b, uint32_t channels, int maxShift)
	{
		const size_t n = std::min(a.size(), b.size()) / channels;
		int best = 0;
		double bestErr = 1e300;
		for (int s = -maxShift; s <= maxShift; ++s)
		{
			double err = 0;
			for (size_t i = 1000; i + 1000 < n; i += 3)
			{
				const int64_t j = static_cast<int64_t>(i) + s;
				if (j < 0 || j >= static_cast<int64_t>(n))
					continue;
				err += std::fabs(static_cast<double>(a[i * channels]) - static_cast<double>(b[static_cast<size_t>(j) * channels]));
			}
			if (err < bestErr)
			{
				bestErr = err;
				best = s;
			}
		}
		return best;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// WAV
	// ---------------------------------------------------------------------------------------------------------------
	struct Wav
	{
		bool ok = false;
		uint32_t channels = 0;
		uint32_t rate = 0;
		uint32_t bits = 0;
		std::vector<int16_t> pcm;
		uint64_t Frames() const { return channels != 0 ? pcm.size() / channels : 0; }
	};

	uint32_t Rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
	uint32_t Rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }

	Wav ParseWav(const std::string& path)
	{
		Wav w;
		const std::vector<uint8_t> d = ReadFile(path);
		CHECK(d.size() >= 44);
		if (d.size() < 44)
			return w;
		CHECK(std::memcmp(d.data(), "RIFF", 4) == 0);
		CHECK(std::memcmp(d.data() + 8, "WAVEfmt ", 8) == 0);
		CHECK(Rd32(d.data() + 16) == 16);
		CHECK(Rd16(d.data() + 20) == 1);
		CHECK(std::memcmp(d.data() + 36, "data", 4) == 0);
		w.channels = Rd16(d.data() + 22);
		w.rate = Rd32(d.data() + 24);
		w.bits = Rd16(d.data() + 34);
		const uint32_t dataBytes = Rd32(d.data() + 40);
		CHECKF(Rd32(d.data() + 4) == d.size() - 8, "RIFF size %u vs file %zu", Rd32(d.data() + 4), d.size());
		CHECKF(dataBytes == d.size() - 44, "data size %u vs file %zu", dataBytes, d.size());
		CHECK(Rd32(d.data() + 28) == w.rate * w.channels * 2);
		CHECK(Rd16(d.data() + 32) == w.channels * 2);
		if (w.channels < 1 || w.channels > 2 || dataBytes != d.size() - 44)
			return w;
		w.pcm.resize(dataBytes / 2);
		for (size_t i = 0; i < w.pcm.size(); ++i)
			w.pcm[i] = static_cast<int16_t>(Rd16(d.data() + 44 + i * 2));
		w.ok = true;
		return w;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Packet level comparison through Core's own demuxer
	// ---------------------------------------------------------------------------------------------------------------
	std::unique_ptr<WebmDemuxer> OpenDemux(const std::string& path)
	{
		std::unique_ptr<IByteSource> src;
		std::unique_ptr<WebmDemuxer> d;
		if (Failed(FileByteSource::Open(path.c_str(), false, src)))
			return nullptr;
		if (Failed(WebmDemuxer::Open(std::move(src), d)))
			return nullptr;
		return d;
	}

	void CompareAudioPackets(WebmDemuxer& a, WebmDemuxer& b)
	{
		CHECK(a.HasAudio() && b.HasAudio());
		if (!a.HasAudio() || !b.HasAudio())
			return;
		const AudioStreamInfo& x = a.Audio();
		const AudioStreamInfo& y = b.Audio();
		CHECK(x.codec == y.codec);
		CHECK(x.sampleRate == y.sampleRate);
		CHECK(x.channels == y.channels);
		CHECK(x.codecPrivate == y.codecPrivate);
		CHECK(x.codecDelayNs == y.codecDelayNs);
		CHECK(x.seekPreRollNs == y.seekPreRollNs);
		CHECKF(x.packetCount == y.packetCount, "%u vs %u packets", x.packetCount, y.packetCount);
		if (x.packetCount != y.packetCount)
			return;
		std::vector<uint8_t> bufA(x.maxPacketSize), bufB(y.maxPacketSize);
		uint32_t badPts = 0, badSize = 0, badPad = 0, badData = 0;
		for (uint32_t i = 0; i < x.packetCount; ++i)
		{
			const PacketRef& pa = a.AudioPacket(i);
			const PacketRef& pb = b.AudioPacket(i);
			badPts += pa.ptsNs != pb.ptsNs;
			badSize += pa.size != pb.size;
			badPad += pa.discardPaddingNs != pb.discardPaddingNs;
			if (pa.size == pb.size && Succeeded(a.ReadAudioPacket(i, bufA.data(), static_cast<uint32_t>(bufA.size()))) &&
				Succeeded(b.ReadAudioPacket(i, bufB.data(), static_cast<uint32_t>(bufB.size()))))
				badData += std::memcmp(bufA.data(), bufB.data(), pa.size) != 0;
		}
		CHECKF(badPts == 0, "%u packets with a different pts", badPts);
		CHECKF(badSize == 0, "%u packets with a different size", badSize);
		CHECKF(badPad == 0, "%u packets with a different DiscardPadding", badPad);
		CHECKF(badData == 0, "%u packets with different payload", badData);
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Tests
	// ---------------------------------------------------------------------------------------------------------------
	bool Selected(const char* only, const char* name) { return only == nullptr || std::strstr(name, only) != nullptr; }

	void TestProbe(const Clip& c)
	{
		g_context = std::string("probe ") + c.file;
		MediaProbe pr;
		const Result r = ProbeMedia(ClipPath(c), pr);
		CHECKF(r.Ok(), "%s", r.message.c_str());
		if (!r.Ok())
			return;
		CHECK(pr.hasVideo == (c.videoFrames != 0));
		if (c.videoFrames != 0)
		{
			CHECK(pr.videoCodec == VideoCodec::VP8 || pr.videoCodec == VideoCodec::VP9);
			CHECK(pr.width == c.width && pr.height == c.height);
			CHECK(pr.hasAlpha == c.alpha);
			CHECKF(pr.videoFrameCount == c.videoFrames, "%u frames", pr.videoFrameCount);
			CHECKF(pr.frameRate > 20.0 && pr.frameRate < 61.0, "fps %.3f", pr.frameRate);
			CHECK(pr.videoFirstPts >= 0.0 && pr.videoFirstPts < 0.1);
		}
		CHECK(pr.durationSeconds > 4.0 && pr.durationSeconds < 11.0);
		CHECK(pr.fileSize == FileSizeOf(ClipPath(c)));
		const size_t expectTracks = c.audio == AudioCodec::None ? 0u : 1u;
		CHECKF(pr.audioTracks.size() == expectTracks, "%zu audio tracks", pr.audioTracks.size());
		if (pr.audioTracks.size() != 1 || expectTracks != 1)
			return;
		const AudioTrackProbe& a = pr.audioTracks[0];
		CHECK(a.index == 0);
		CHECK(a.codec == c.audio);
		CHECK(a.channels >= 1 && a.channels <= 2);
		CHECK(a.packetCount > 100);
		if (c.audio == AudioCodec::Opus)
		{
			CHECK(a.decodedSampleRate == 48000);
			CHECK(a.codecId == "A_OPUS");
			CHECKF(a.preSkipFrames > 0 && a.preSkipFrames < 2000, "pre-skip %u", a.preSkipFrames);
			CHECKF(std::fabs(a.codecDelaySeconds * 48000.0 - a.preSkipFrames) < 1.0, "CodecDelay %.6f s vs pre-skip %u", a.codecDelaySeconds, a.preSkipFrames);
			CHECKF(a.seekPreRollSeconds > 0.07 && a.seekPreRollSeconds < 0.09, "SeekPreRoll %.4f", a.seekPreRollSeconds);
		}
		else
		{
			CHECK(a.decodedSampleRate == 44100 && a.sampleRate == 44100);
			CHECK(a.codecId == "A_VORBIS");
			CHECK(a.preSkipFrames == 0 && a.codecDelaySeconds == 0.0);
		}
		CHECKF(std::fabs(a.startOffsetSeconds - c.toneStart) < 0.002, "start offset %.5f s (expected %.3f)", a.startOffsetSeconds, c.toneStart);
		if (std::strcmp(c.file, "audio_start_offset.webm") == 0)
			CHECKF(std::fabs(a.startOffsetSeconds - 1.5) <= 0.0015, "start offset %.5f s, nominal 1.5 s", a.startOffsetSeconds);
		CHECKF(a.durationSeconds > 4.9 && a.durationSeconds < 10.1, "audio end %.3f", a.durationSeconds);
		CHECK(std::strlen(c.language) == 0 || a.language == c.language);
		Log("  probe %-26s %s %dx%d %.2f fps alpha=%d frames=%u  audio: %s %u Hz %uch start=%.6f s (%lld frames) preskip=%u lang='%s'\n", c.file,
			pr.videoCodec == VideoCodec::VP9 ? "VP9" : (pr.videoCodec == VideoCodec::VP8 ? "VP8" : "-"), pr.width, pr.height, pr.frameRate,
			pr.hasAlpha ? 1 : 0, pr.videoFrameCount, a.codecId.c_str(), a.decodedSampleRate, a.channels, a.startOffsetSeconds,
			static_cast<long long>(a.startFrames), a.preSkipFrames, a.language.c_str());
	}

	void TestProbeAlpha(const Clip& c)
	{
		g_context = std::string("probe ") + c.file;
		MediaProbe pr;
		const Result r = ProbeMedia(ClipPath(c), pr);
		CHECK(r.Ok());
		CHECK(pr.hasAlpha == c.alpha);
		CHECK(pr.audioTracks.empty());
	}

	void TestExtract(const Clip& c)
	{
		g_context = std::string("extract ") + c.file;
		const std::string dst = OutPath(std::string("audio_") + c.file);
		RemoveOutput(dst);
		std::vector<double> fractions;
		Control ctl;
		ctl.progress = [&](double f) { fractions.push_back(f); };
		const Result r = ExtractAudioWebm(ClipPath(c), 0, dst, ctl);
		CHECKF(r.Ok(), "%s", r.message.c_str());
		if (!r.Ok())
			return;
		CHECK(!FileExists(dst + ".tmp"));
		CHECK(!fractions.empty() && fractions.back() == 1.0);
		bool monotonic = true;
		for (size_t i = 1; i < fractions.size(); ++i)
			monotonic = monotonic && fractions[i] > fractions[i - 1];
		CHECK(monotonic);

		MediaProbe src, out;
		CHECK(ProbeMedia(ClipPath(c), src).Ok());
		const Result pr = ProbeMedia(dst, out);
		CHECKF(pr.Ok(), "%s", pr.message.c_str());
		CHECK(!out.hasVideo);
		CHECK(out.audioTracks.size() == 1);
		CHECKF(std::fabs(out.durationSeconds - src.durationSeconds) < 0.05, "duration %.4f s vs source %.4f s", out.durationSeconds, src.durationSeconds);
		if (out.audioTracks.size() == 1 && src.audioTracks.size() == 1)
		{
			CHECK(out.audioTracks[0].language == src.audioTracks[0].language);
			CHECK(out.audioTracks[0].startFrames == src.audioTracks[0].startFrames);
			CHECK(out.audioTracks[0].packetCount == src.audioTracks[0].packetCount);
		}

		// Same packets (payload, pts, DiscardPadding) and stream parameters through Core's demuxer.
		std::unique_ptr<WebmDemuxer> a = OpenDemux(ClipPath(c));
		std::unique_ptr<WebmDemuxer> b = OpenDemux(dst);
		CHECK(a != nullptr && b != nullptr);
		if (a && b)
		{
			CompareAudioPackets(*a, *b);
			CHECK(!b->HasVideo());
		}

		// Decodes to the same samples as the embedded track, loaded through PlayerCore.
		Pcm emb, ext;
		CHECK(CaptureAudio(ClipPath(c), emb));
		CHECK(CaptureAudio(dst, ext));
		CHECK(emb.rate == ext.rate && emb.channels == ext.channels);
		CHECKF(emb.data.size() == ext.data.size(), "%zu vs %zu samples", emb.data.size(), ext.data.size());
		double maxDiff = 0.0;
		size_t bad = 0;
		for (size_t i = 0; i < std::min(emb.data.size(), ext.data.size()); ++i)
		{
			const double d = std::fabs(static_cast<double>(emb.data[i]) - ext.data[i]);
			maxDiff = std::max(maxDiff, d);
			bad += d != 0.0;
		}
		CHECKF(bad == 0, "%zu samples differ (max %.3g)", bad, maxDiff);
		Log("  extract %-26s %u packets, %llu frames: PCM identical to the embedded track (%zu mismatches, max diff %.3g)\n", c.file,
			out.audioTracks.empty() ? 0u : out.audioTracks[0].packetCount, static_cast<unsigned long long>(emb.Frames()), bad, maxDiff);
		if (!g_keep)
			RemoveOutput(dst);
	}

	void TestStrip(const Clip& c)
	{
		g_context = std::string("strip ") + c.file;
		const std::string dst = OutPath(std::string("video_") + c.file);
		RemoveOutput(dst);
		std::vector<double> fractions;
		Control ctl;
		ctl.progress = [&](double f) { fractions.push_back(f); };
		const Result r = StripAudio(ClipPath(c), dst, ctl);
		CHECKF(r.Ok(), "%s", r.message.c_str());
		if (!r.Ok())
			return;
		CHECK(!FileExists(dst + ".tmp"));
		CHECK(!fractions.empty() && fractions.back() == 1.0);

		MediaProbe out;
		CHECK(ProbeMedia(dst, out).Ok());
		CHECK(out.hasVideo);
		CHECKF(out.videoFrameCount == c.videoFrames, "%u frames, expected %u", out.videoFrameCount, c.videoFrames);
		CHECK(out.width == c.width && out.height == c.height);
		CHECK(out.hasAlpha == c.alpha);
		CHECK(out.audioTracks.empty());
		{
			MediaProbe src;
			CHECK(ProbeMedia(ClipPath(c), src).Ok());
			CHECKF(std::fabs(out.durationSeconds - src.durationSeconds) < 0.05, "duration %.4f s vs source %.4f s", out.durationSeconds, src.durationSeconds);
		}

		std::unique_ptr<WebmDemuxer> a = OpenDemux(ClipPath(c));
		std::unique_ptr<WebmDemuxer> b = OpenDemux(dst);
		CHECK(a != nullptr && b != nullptr);
		if (a && b)
		{
			CHECK(!b->HasAudio());
			CHECK(a->Video().codec == b->Video().codec);
			CHECK(a->Video().hasAlpha == b->Video().hasAlpha);
			CHECK(a->Video().matrixCoefficients == b->Video().matrixCoefficients);
			CHECK(a->Video().colorRange == b->Video().colorRange);
			CHECK(a->VideoPacketCount() == b->VideoPacketCount());
			uint32_t badPts = 0, badKey = 0, badData = 0, badAdd = 0, withAlpha = 0;
			if (a->VideoPacketCount() == b->VideoPacketCount())
			{
				std::vector<uint8_t> fa(a->Video().maxPacketSize), fb(b->Video().maxPacketSize);
				std::vector<uint8_t> aa(std::max<uint32_t>(a->Video().maxAdditionalSize, 1)), ab(std::max<uint32_t>(b->Video().maxAdditionalSize, 1));
				for (uint32_t i = 0; i < a->VideoPacketCount(); ++i)
				{
					const PacketRef& pa = a->VideoPacket(i);
					const PacketRef& pb = b->VideoPacket(i);
					badPts += pa.ptsNs != pb.ptsNs;
					badKey += pa.keyframe != pb.keyframe;
					if (pa.size != pb.size || pa.additionalSize != pb.additionalSize)
					{
						++badData;
						continue;
					}
					a->ReadVideoPacket(i, fa.data(), static_cast<uint32_t>(fa.size()));
					b->ReadVideoPacket(i, fb.data(), static_cast<uint32_t>(fb.size()));
					badData += std::memcmp(fa.data(), fb.data(), pa.size) != 0;
					if (pa.additionalSize > 0)
					{
						++withAlpha;
						a->ReadVideoAdditional(i, aa.data(), static_cast<uint32_t>(aa.size()));
						b->ReadVideoAdditional(i, ab.data(), static_cast<uint32_t>(ab.size()));
						badAdd += std::memcmp(aa.data(), ab.data(), pa.additionalSize) != 0;
					}
				}
			}
			CHECKF(badPts == 0, "%u video packets with a different pts", badPts);
			CHECKF(badKey == 0, "%u video packets with a different key flag", badKey);
			CHECKF(badData == 0, "%u video packets with different payload", badData);
			CHECKF(badAdd == 0, "%u alpha BlockAdditions differ", badAdd);
			CHECK(!c.alpha || withAlpha > 100);
			Log("  strip   %-26s %u video packets identical (pts, key, payload), %u with alpha BlockAdditions, no audio, %llu -> %llu bytes\n", c.file,
				b->VideoPacketCount(), withAlpha, static_cast<unsigned long long>(FileSizeOf(ClipPath(c))), static_cast<unsigned long long>(FileSizeOf(dst)));
		}

		// Decodes: the first presented frame (Y and alpha plane) equals the source's.
		const FrameHash fa = FirstFrame(ClipPath(c));
		const FrameHash fb = FirstFrame(dst);
		CHECK(fa.ok && fb.ok);
		CHECK(fa.y == fb.y);
		CHECK(fa.alphaPlane == c.alpha && fb.alphaPlane == c.alpha);
		CHECK(fa.a == fb.a);
		if (!g_keep)
			RemoveOutput(dst);
	}

	void TestWav(const Clip& c)
	{
		g_context = std::string("wav ") + c.file;
		const std::string dst = OutPath(std::string("audio_") + c.file + ".wav");
		RemoveOutput(dst);
		AudioExportInfo info;
		std::vector<double> fractions;
		Control ctl;
		ctl.progress = [&](double f) { fractions.push_back(f); };
		const Result r = ExportWav(ClipPath(c), 0, dst, ctl, &info);
		CHECKF(r.Ok(), "%s", r.message.c_str());
		if (!r.Ok())
			return;
		CHECK(!FileExists(dst + ".tmp"));
		CHECK(!fractions.empty() && fractions.back() == 1.0);
		const Wav wav = ParseWav(dst);
		CHECK(wav.ok);
		if (!wav.ok)
			return;
		CHECK(wav.rate == 48000 && wav.bits == 16);
		CHECK(info.sampleRate == 48000 && info.channels == wav.channels && info.frames == wav.Frames());

		Pcm emb;
		CHECK(CaptureAudio(ClipPath(c), emb));
		CHECK(wav.channels == std::min<uint32_t>(emb.channels, 2));

		// Reference: the embedded track as PlayerCore plays it from t = 0, brought to 48 kHz int16.
		std::vector<int16_t> ref;
		if (emb.rate == 48000)
		{
			ref.resize(emb.data.size());
			for (size_t i = 0; i < ref.size(); ++i)
				ref[i] = ToS16(emb.data[i]);
		}
		else
		{
			Resampler rs;
			CHECK(Succeeded(rs.Init(emb.rate, emb.channels, 48000, wav.channels, 4096)));
			const uint64_t inFrames = emb.Frames();
			const uint64_t outFrames = (inFrames * 48000 + emb.rate - 1) / emb.rate;
			std::vector<float> out;
			std::vector<float> chunk(static_cast<size_t>(16384) * wav.channels);
			uint64_t pos = 0;
			bool flushed = false;
			while (out.size() < outFrames * wav.channels)
			{
				uint32_t c2 = 0, p2 = 0;
				const uint32_t take = static_cast<uint32_t>(std::min<uint64_t>(inFrames - pos, 4096));
				if (take == 0 && !flushed)
				{
					rs.Flush();
					flushed = true;
				}
				rs.Process(take > 0 ? emb.data.data() + pos * emb.channels : nullptr, take, chunk.data(), 16384, &c2, &p2);
				pos += c2;
				out.insert(out.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(p2) * wav.channels));
				if (c2 == 0 && p2 == 0 && flushed)
					break;
			}
			out.resize(static_cast<size_t>(outFrames) * wav.channels);
			ref.resize(out.size());
			for (size_t i = 0; i < ref.size(); ++i)
				ref[i] = ToS16(out[i]);
		}
		CHECKF(wav.pcm.size() == ref.size(), "WAV has %zu samples, the embedded track (as played) %zu", wav.pcm.size(), ref.size());
		size_t mismatches = 0;
		for (size_t i = 0; i < std::min(wav.pcm.size(), ref.size()); ++i)
			mismatches += std::abs(static_cast<int>(wav.pcm[i]) - static_cast<int>(ref[i])) > 1;
		const int shift = BestShift(wav.pcm, ref, wav.channels, 8);
		CHECKF(mismatches == 0, "%zu samples differ by more than 1 LSB from the embedded track", mismatches);
		CHECKF(shift == 0, "best alignment shift vs the embedded track is %d samples", shift);

		MediaProbe pr;
		CHECK(ProbeMedia(ClipPath(c), pr).Ok());
		CHECKF(std::fabs(static_cast<double>(wav.Frames()) / 48000.0 - pr.durationSeconds) < 0.05, "WAV %.4f s vs container %.4f s",
			static_cast<double>(wav.Frames()) / 48000.0, pr.durationSeconds);
		if (!pr.audioTracks.empty())
		{
			const double expectStart = pr.audioTracks[0].startOffsetSeconds * 48000.0;
			// everything before the start of the audio is digital silence
			size_t lead = static_cast<size_t>(std::llround(expectStart));
			bool silent = true;
			for (size_t i = 0; i < std::min<size_t>(lead, wav.Frames()); ++i)
				silent = silent && wav.pcm[i * wav.channels] == 0;
			CHECK(silent);
		}

		// Ground truth from the generator: the sine starts at toneStart with phase 0.
		const std::vector<double> ch0 = Channel0(wav.pcm, wav.channels);
		if (c.tone > 0.0)
		{
			const double t0 = c.toneStart * 48000.0;
			const size_t from = static_cast<size_t>(t0) + 48000;
			double amp = 0.0;
			const double d = FitDelay(ch0, from, from + 96000, c.tone, 48000.0, t0, &amp);
			CHECKF(std::fabs(d) <= 1.0, "%.0f Hz tone phase: audio %.3f samples late", c.tone, d);
			CHECKF(amp > 0.06 && amp < 0.12, "tone amplitude %.4f", amp); // 0.125 / sqrt(2): ffmpeg's mono sine mixed to stereo
			Log("  wav     %-26s %llu frames x %uch  vs embedded: shift %d, %zu mismatches (>1 LSB); %.0f Hz tone phase error %+.3f samples (start %lld)\n", c.file,
				static_cast<unsigned long long>(wav.Frames()), wav.channels, shift, mismatches, c.tone, d, static_cast<long long>(info.startFrames));
		}
		else if (std::strcmp(c.file, "av_sync_flash_beep.webm") == 0)
		{
			// beeps: 0.8 * sin(2 pi 1000 t) for t mod 1 < 0.05. Compare the onset sample with the ideal signal's.
			auto onset = [&](const std::vector<double>& sig, size_t k, double thr) -> int64_t {
				const int64_t base = static_cast<int64_t>(k) * 48000;
				for (int64_t n = base - 480; n < base + 480; ++n)
					if (n >= 0 && n < static_cast<int64_t>(sig.size()) && std::fabs(sig[static_cast<size_t>(n)]) > thr)
						return n;
				return -1;
			};
			std::vector<double> ideal(480000);
			for (size_t n = 0; n < ideal.size(); ++n)
			{
				const double t = static_cast<double>(n) / 48000.0;
				ideal[n] = std::fmod(t, 1.0) < 0.05 ? 0.8 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * t) : 0.0;
			}
			int beeps = 0;
			double worstOnset = 0.0, worstPhase = 0.0;
			for (size_t k = 0; k < 10; ++k)
			{
				const int64_t o = onset(ch0, k, 0.35);
				const int64_t oi = onset(ideal, k, 0.35);
				CHECKF(o >= 0 && oi >= 0, "no beep onset near %zu s", k);
				if (o < 0 || oi < 0)
					continue;
				++beeps;
				worstOnset = std::max(worstOnset, std::fabs(static_cast<double>(o - oi)));
				const size_t from = k * 48000 + 480;
				double amp = 0.0;
				const double d = FitDelay(ch0, from, from + 1920, 1000.0, 48000.0, static_cast<double>(k) * 48000.0, &amp);
				worstPhase = std::max(worstPhase, std::fabs(d));
			}
			CHECKF(beeps == 10, "%d beeps found", beeps);
			CHECKF(worstOnset <= 1.0, "beep onset vs the ideal signal: %.0f samples", worstOnset);
			CHECKF(worstPhase <= 1.0, "beep tone phase: %.3f samples", worstPhase);
			Log("  wav     %-26s %llu frames x %uch  vs embedded: shift %d, %zu mismatches (>1 LSB); %d beeps at k*1.0 s: worst onset error %.0f samples, worst tone phase error %.3f samples\n",
				c.file, static_cast<unsigned long long>(wav.Frames()), wav.channels, shift, mismatches, beeps, worstOnset, worstPhase);
		}
		if (!g_keep)
			RemoveOutput(dst);
	}

	// ---- Ogg ----
	struct OggRead
	{
		bool ok = false;
		long channels = 0;
		long rate = 0;
		int64_t total = 0;
		std::vector<float> pcm; // interleaved
	};

	OggRead ReadOgg(const std::string& path)
	{
		OggRead o;
		std::FILE* f = std::fopen(fs::u8path(path).string().c_str(), "rb");
		CHECK(f != nullptr);
		if (f == nullptr)
			return o;
		OggVorbis_File vf;
		const int rc = ov_open_callbacks(f, &vf, nullptr, 0, OV_CALLBACKS_NOCLOSE);
		CHECKF(rc == 0, "ov_open_callbacks %d", rc);
		if (rc != 0)
		{
			std::fclose(f);
			return o;
		}
		CHECK(ov_streams(&vf) == 1);
		const vorbis_info* vi = ov_info(&vf, -1);
		o.channels = vi->channels;
		o.rate = vi->rate;
		o.total = ov_pcm_total(&vf, -1);
		CHECK(ov_seekable(&vf) != 0);
		for (;;)
		{
			float** planes = nullptr;
			int section = 0;
			const long n = ov_read_float(&vf, &planes, 4096, &section);
			if (n < 0)
			{
				CHECKF(false, "ov_read_float error %ld", n);
				break;
			}
			if (n == 0)
				break;
			for (long i = 0; i < n; ++i)
				for (long ch = 0; ch < o.channels; ++ch)
					o.pcm.push_back(planes[ch][i]);
		}
		ov_clear(&vf);
		std::fclose(f);
		o.ok = true;
		return o;
	}

	void TestOgg(const Clip& c)
	{
		g_context = std::string("ogg ") + c.file;
		const std::string dst = OutPath(std::string("audio_") + c.file + ".ogg");
		RemoveOutput(dst);
		AudioExportInfo info;
		std::vector<double> fractions;
		Control ctl;
		ctl.progress = [&](double f) { fractions.push_back(f); };
		const Result r = ExportOggVorbis(ClipPath(c), 0, dst, 0.4f, ctl, &info);
		CHECKF(r.Ok(), "%s", r.message.c_str());
		if (!r.Ok())
			return;
		CHECK(!FileExists(dst + ".tmp"));
		CHECK(!fractions.empty() && fractions.back() == 1.0);
		const bool expectRemux = c.audio == AudioCodec::Vorbis;
		CHECKF(info.remuxed == expectRemux, "remuxed=%d, expected %d", info.remuxed ? 1 : 0, expectRemux ? 1 : 0);

		Pcm emb;
		CHECK(CaptureAudio(ClipPath(c), emb));
		const OggRead ogg = ReadOgg(dst);
		CHECK(ogg.ok);
		if (!ogg.ok)
			return;
		CHECK(static_cast<uint32_t>(ogg.rate) == emb.rate);
		CHECK(static_cast<uint32_t>(ogg.channels) == emb.channels);
		CHECKF(static_cast<uint64_t>(ogg.total) == emb.Frames(), "ov_pcm_total %lld vs %llu frames played by PlayerCore", static_cast<long long>(ogg.total),
			static_cast<unsigned long long>(emb.Frames()));
		CHECK(info.frames == static_cast<uint64_t>(ogg.total));
		CHECKF(ogg.pcm.size() == static_cast<size_t>(ogg.total) * static_cast<size_t>(ogg.channels), "decoded %zu samples, ov_pcm_total %lld",
			ogg.pcm.size(), static_cast<long long>(ogg.total));

		if (expectRemux)
		{
			// lossless: libvorbisfile decodes exactly what PlayerCore decoded from the WebM
			size_t bad = 0;
			double maxDiff = 0.0;
			for (size_t i = 0; i < std::min(ogg.pcm.size(), emb.data.size()); ++i)
			{
				const double d = std::fabs(static_cast<double>(ogg.pcm[i]) - emb.data[i]);
				maxDiff = std::max(maxDiff, d);
				bad += d > 1e-6;
			}
			CHECKF(bad == 0, "%zu samples differ from the embedded Vorbis decode (max %.3g)", bad, maxDiff);
			Log("  ogg     %-26s REMUX: %lld frames, %ld Hz %ldch, libvorbisfile PCM == embedded decode (%zu mismatches, max diff %.3g)\n", c.file,
				static_cast<long long>(ogg.total), ogg.rate, ogg.channels, bad, maxDiff);
		}
		else
		{
			// re-encoded: the 1 kHz-ish tone keeps its phase (the Vorbis encoder has no delay)
			double d = 0.0;
			if (c.tone > 0.0)
			{
				const std::vector<double> ch0 = Channel0(ogg.pcm, static_cast<uint32_t>(ogg.channels));
				const double t0 = c.toneStart * 48000.0;
				const size_t from = static_cast<size_t>(t0) + 48000;
				double amp = 0.0;
				d = FitDelay(ch0, from, from + 96000, c.tone, 48000.0, t0, &amp);
				CHECKF(std::fabs(d) <= 1.0, "%.0f Hz tone phase: audio %.3f samples late", c.tone, d);
				CHECKF(amp > 0.06 && amp < 0.12, "tone amplitude %.4f", amp); // 0.125 / sqrt(2): ffmpeg's mono sine mixed to stereo
				// no sound before the start of the tone (allow codec pre-echo)
				float peak = 0.0f;
				for (size_t i = 0; i + 2000 < static_cast<size_t>(t0) && i < ch0.size(); ++i)
					peak = std::max(peak, static_cast<float>(std::fabs(ch0[i])));
				CHECKF(peak < 0.01f, "peak %.4f before the tone starts", peak);
			}
			Log("  ogg     %-26s ENCODE (vorbisenc q0.4): %lld frames == %llu played by PlayerCore, %ld Hz %ldch, tone phase error %+.3f samples (start %lld)\n", c.file,
				static_cast<long long>(ogg.total), static_cast<unsigned long long>(emb.Frames()), ogg.rate, ogg.channels, d, static_cast<long long>(info.startFrames));
		}
		if (!g_keep)
			RemoveOutput(dst);
	}

	void TestCancel()
	{
		g_context = "cancel";
		const Clip& big = kClips[0]; // 1080p, 300 frames
		const std::string src = ClipPath(big);
		std::atomic<bool> cancel{true};
		Control pre;
		pre.cancel = &cancel;

		const std::string d1 = OutPath("cancel_a.webm"), d2 = OutPath("cancel_b.webm"), d3 = OutPath("cancel_c.wav"), d4 = OutPath("cancel_d.ogg");
		for (const std::string& d : {d1, d2, d3, d4})
			RemoveOutput(d);
		MediaProbe pr;
		CHECK(ResultIs(ProbeMedia(src, pr, pre), Status::Cancelled));
		CHECK(ResultIs(ExtractAudioWebm(src, 0, d1, pre), Status::Cancelled));
		CHECK(ResultIs(StripAudio(src, d2, pre), Status::Cancelled));
		CHECK(ResultIs(ExportWav(src, 0, d3, pre), Status::Cancelled));
		CHECK(ResultIs(ExportOggVorbis(src, 0, d4, 0.4f, pre), Status::Cancelled));
		for (const std::string& d : {d1, d2, d3, d4})
			CHECKF(OutputGone(d), "%s (or its .tmp) exists after a cancelled run", d.c_str());

		// cancel from the progress callback, half way through
		struct Case
		{
			const char* name;
			std::string dst;
		};
		const auto runMid = [&](const char* what, const std::string& dst, auto&& fn) {
			std::atomic<bool> flag{false};
			Control ctl;
			ctl.cancel = &flag;
			double last = 0.0;
			ctl.progress = [&](double f) {
				last = f;
				if (f >= 0.5)
					flag.store(true);
			};
			const auto t0 = std::chrono::steady_clock::now();
			const Result r = fn(ctl);
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			CHECKF(ResultIs(r, Status::Cancelled), "%s", what);
			CHECKF(last < 0.75, "%s: progress reached %.2f after the cancel request", what, last);
			CHECKF(OutputGone(dst), "%s: partial output left behind", what);
			Log("  cancel  %-8s at 50%%: Cancelled after %.0f ms (last progress %.2f), no output / .tmp left\n", what, ms, last);
		};
		runMid("strip", d2, [&](const Control& ctl) { return StripAudio(src, d2, ctl); });
		runMid("extract", d1, [&](const Control& ctl) { return ExtractAudioWebm(src, 0, d1, ctl); });
		runMid("wav", d3, [&](const Control& ctl) { return ExportWav(src, 0, d3, ctl); });
		runMid("ogg", d4, [&](const Control& ctl) { return ExportOggVorbis(src, 0, d4, 0.4f, ctl); });

		// an existing destination survives a cancelled / failed run
		const char keep[] = "existing";
		WriteBytes(d3, keep, sizeof(keep));
		CHECK(ResultIs(ExportWav(src, 0, d3, pre), Status::Cancelled));
		CHECK(FileSizeOf(d3) == sizeof(keep));
		CHECK(ResultIs(ExportWav(src, 7, d3), Status::OutOfRange));
		CHECK(FileSizeOf(d3) == sizeof(keep));
		RemoveOutput(d3);
	}

	void TestBadInput()
	{
		g_context = "bad input";
		const std::string good = ClipPath(kClips[1]); // vp8_vorbis
		const std::string dst = OutPath("bad.out");
		MediaProbe pr;
		RemoveOutput(dst);

		CHECK(ResultIs(ProbeMedia(OutPath("does_not_exist.webm"), pr), Status::IoError));
		CHECK(ResultIs(ExtractAudioWebm(OutPath("does_not_exist.webm"), 0, dst), Status::IoError));
		CHECK(ResultIs(StripAudio(OutPath("does_not_exist.webm"), dst), Status::IoError));
		CHECK(ResultIs(ExportWav(OutPath("does_not_exist.webm"), 0, dst), Status::IoError));
		CHECK(ResultIs(ExportOggVorbis(OutPath("does_not_exist.webm"), 0, dst), Status::IoError));
		CHECK(ResultIs(ProbeMedia("", pr), Status::IoError));

		// garbage / empty / tiny files
		std::vector<uint8_t> junk(8192);
		for (size_t i = 0; i < junk.size(); ++i)
			junk[i] = static_cast<uint8_t>((i * 2654435761u) >> 13);
		const std::string junkPath = OutPath("junk.webm");
		WriteBytes(junkPath, junk.data(), junk.size());
		CHECK(ResultIs(ProbeMedia(junkPath, pr), Status::InvalidData));
		CHECK(ResultIs(ExtractAudioWebm(junkPath, 0, dst), Status::InvalidData));
		CHECK(ResultIs(StripAudio(junkPath, dst), Status::InvalidData));
		CHECK(ResultIs(ExportWav(junkPath, 0, dst), Status::InvalidData));
		CHECK(ResultIs(ExportOggVorbis(junkPath, 0, dst), Status::InvalidData));
		const std::string emptyPath = OutPath("empty.webm");
		WriteBytes(emptyPath, nullptr, 0);
		CHECK(ResultIs(ProbeMedia(emptyPath, pr), Status::InvalidData));
		CHECK(ResultIs(ExportWav(emptyPath, 0, dst), Status::InvalidData));
		CHECK(OutputGone(dst));

		// a WAV file is not a WebM file
		const std::string wavPath = OutPath("not_webm.wav");
		const uint8_t hdr[44] = {'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0, 0x80, 0xBB, 0, 0, 0, 0x77, 1, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
		WriteBytes(wavPath, hdr, sizeof(hdr));
		CHECK(ResultIs(ProbeMedia(wavPath, pr), Status::InvalidData));

		// truncated and corrupted WebM: must neither crash nor hang; a success must produce a loadable file
		const std::vector<uint8_t> whole = ReadFile(good);
		CHECK(whole.size() > 10000);
		const std::string truncPath = OutPath("truncated.webm");
		WriteBytes(truncPath, whole.data(), whole.size() / 2);
		{
			const Result r = ExportWav(truncPath, 0, dst);
			Log("  bad     truncated file (50%%): ExportWav -> %s '%s'\n", StatusToString(r.status), r.message.c_str());
			CHECK(r.Ok() || r.status == Status::InvalidData || r.status == Status::IoError || r.status == Status::DecodeError);
			RemoveOutput(dst);
			const Result r2 = StripAudio(truncPath, dst);
			CHECK(r2.Ok() || r2.status == Status::InvalidData || r2.status == Status::IoError);
			RemoveOutput(dst);
		}
		std::vector<uint8_t> corrupt = whole;
		for (size_t i = corrupt.size() / 3; i < corrupt.size() / 3 + 400; ++i)
			corrupt[i] = static_cast<uint8_t>(corrupt[i] ^ 0x5A);
		const std::string corruptPath = OutPath("corrupt.webm");
		WriteBytes(corruptPath, corrupt.data(), corrupt.size());
		{
			const Result r = ExportOggVorbis(corruptPath, 0, dst);
			Log("  bad     corrupted payload: ExportOggVorbis -> %s '%s'\n", StatusToString(r.status), r.message.c_str());
			RemoveOutput(dst);
			const Result r2 = ExportWav(corruptPath, 0, dst);
			RemoveOutput(dst);
			(void)r2;
		}

		// bad arguments
		CHECK(ResultIs(ExtractAudioWebm(good, 5, dst), Status::OutOfRange));
		CHECK(ResultIs(ExportWav(good, 1, dst), Status::OutOfRange));
		CHECK(ResultIs(ExportOggVorbis(good, 0xFFFFFFFFu, dst), Status::OutOfRange));
		CHECK(ResultIs(ExtractAudioWebm(good, 0, ""), Status::InvalidArgument));
		CHECK(ResultIs(StripAudio(good, ""), Status::InvalidArgument));
		CHECK(ResultIs(ExportWav(good, 0, ""), Status::InvalidArgument));
		CHECK(ResultIs(ExportOggVorbis(good, 0, ""), Status::InvalidArgument));
		CHECK(ResultIs(ExportWav(good, 0, OutPath("no_such_dir/x.wav")), Status::IoError));
		// video only file has no audio; audio only file has no video
		const std::string alpha = ClipPath(kClips[5]);
		const std::string audioOnly = ClipPath(kClips[6]);
		CHECK(ResultIs(ExtractAudioWebm(alpha, 0, dst), Status::InvalidData));
		CHECK(ResultIs(ExportWav(alpha, 0, dst), Status::InvalidData));
		CHECK(ResultIs(ExportOggVorbis(alpha, 0, dst), Status::InvalidData));
		CHECK(ResultIs(StripAudio(audioOnly, dst), Status::InvalidData));
		CHECK(OutputGone(dst));
		Log("  bad     input: missing / empty / garbage / non-WebM / truncated / corrupt files, bad indices and paths -> clean errors, no output left\n");
	}

	// Non ASCII path (UTF-8) for source and destination, plus a quality sweep sanity check.
	void TestUtf8Paths()
	{
		g_context = "utf8 paths";
		const std::string dirName = OutPath("t\xC3\xA9st_\xE6\x97\xA5\xE6\x9C\xAC_\xD0\xB2\xD0\xB8\xD0\xB4\xD0\xB5\xD0\xBE");
		std::error_code ec;
		fs::create_directories(fs::u8path(dirName), ec);
		const std::string copy = dirName + "/cl\xC3\xADp_\xE6\x97\xA5.webm";
		fs::copy_file(fs::u8path(ClipPath(kClips[2])), fs::u8path(copy), fs::copy_options::overwrite_existing, ec);
		CHECK(!ec);
		MediaProbe pr;
		CHECK(ProbeMedia(copy, pr).Ok());
		const std::string wav = dirName + "/\xE9\x9F\xB3.wav";
		const std::string ogg = dirName + "/\xE9\x9F\xB3.ogg";
		const std::string webm = dirName + "/\xE9\x9F\xB3.webm";
		const std::string vid = dirName + "/\xE6\x98\xA0\xE5\x83\x8F.webm";
		CHECK(ExportWav(copy, 0, wav).Ok());
		CHECK(ExportOggVorbis(copy, 0, ogg).Ok());
		CHECK(ExtractAudioWebm(copy, 0, webm).Ok());
		CHECK(StripAudio(copy, vid).Ok());
		CHECK(FileExists(wav) && FileExists(ogg) && FileExists(webm) && FileExists(vid));
		CHECK(OutputGone(wav + ".none"));
		Log("  utf8    non-ASCII directory / file names: probe, WAV, OGG, extract, strip OK\n");
		if (!g_keep)
			fs::remove_all(fs::u8path(dirName), ec);
	}

	void TestQuality()
	{
		g_context = "ogg quality";
		const Clip& c = kClips[0];
		const std::string lo = OutPath("q_lo.ogg"), hi = OutPath("q_hi.ogg");
		AudioExportInfo a, b;
		CHECK(ExportOggVorbis(ClipPath(c), 0, lo, -0.1f, Control(), &a).Ok());
		CHECK(ExportOggVorbis(ClipPath(c), 0, hi, 1.0f, Control(), &b).Ok());
		CHECK(ExportOggVorbis(ClipPath(c), 0, hi, 7.0f).Ok()); // clamped
		CHECK(a.frames == b.frames);
		CHECKF(FileSizeOf(lo) < FileSizeOf(hi), "q-0.1: %llu bytes, q1.0: %llu bytes", static_cast<unsigned long long>(FileSizeOf(lo)),
			static_cast<unsigned long long>(FileSizeOf(hi)));
		Log("  ogg     quality: q=-0.1 -> %llu bytes, q=1.0 -> %llu bytes, same length (%llu frames)\n", static_cast<unsigned long long>(FileSizeOf(lo)),
			static_cast<unsigned long long>(FileSizeOf(hi)), static_cast<unsigned long long>(a.frames));
		RemoveOutput(lo);
		RemoveOutput(hi);
	}
}

int main(int argc, char** argv)
{
	g_mediaDir =
#ifdef CEV_DEFAULT_MEDIA_DIR
		CEV_DEFAULT_MEDIA_DIR;
#else
		"test_media";
#endif
	const char* only = nullptr;
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--media") == 0 && i + 1 < argc)
			g_mediaDir = argv[++i];
		else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc)
			only = argv[++i];
		else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
			g_outDir = argv[++i];
		else if (std::strcmp(argv[i], "--keep") == 0)
			g_keep = true;
		else if (std::strcmp(argv[i], "--verbose") == 0)
			g_verbose = true;
		else
		{
			std::printf("usage: cevoffline_selftest [--media <dir>] [--only <substring>] [--out <dir>] [--keep] [--verbose]\n");
			return 2;
		}
	}
	(void)g_verbose;
	while (g_mediaDir.size() > 1 && (g_mediaDir.back() == '/' || g_mediaDir.back() == '\\'))
		g_mediaDir.pop_back();

	for (const Clip& c : kClips)
	{
		if (!FileExists(ClipPath(c)))
		{
			std::printf("missing test clip %s (run tools/make_test_media.sh, or pass --media <dir>)\n", ClipPath(c).c_str());
			return 2;
		}
	}
	if (g_outDir.empty())
	{
		std::error_code ec;
		g_outDir = (fs::temp_directory_path(ec) / "cevoffline_selftest").u8string();
	}
	{
		std::error_code ec;
		fs::remove_all(fs::u8path(g_outDir), ec);
		fs::create_directories(fs::u8path(g_outDir), ec);
	}

	Log("CEVPlayerOffline selftest, media: %s, output: %s\n", g_mediaDir.c_str(), g_outDir.c_str());
	const auto t0 = std::chrono::steady_clock::now();

	Log("== probe\n");
	for (const Clip& c : kClips)
		if (Selected(only, c.file))
			c.audio != AudioCodec::None ? TestProbe(c) : TestProbeAlpha(c);

	Log("== extract audio WebM (packet copy)\n");
	for (const Clip& c : kClips)
		if (c.audio != AudioCodec::None && Selected(only, c.file))
			TestExtract(c);

	Log("== strip audio (video only)\n");
	for (const Clip& c : kClips)
		if (c.videoFrames != 0 && Selected(only, c.file))
			TestStrip(c);

	Log("== export WAV (48 kHz / 16 bit, aligned to video t = 0)\n");
	for (const Clip& c : kClips)
		if (c.audio != AudioCodec::None && Selected(only, c.file))
			TestWav(c);

	Log("== export Ogg Vorbis\n");
	for (const Clip& c : kClips)
		if (c.audio != AudioCodec::None && Selected(only, c.file))
			TestOgg(c);

	if (only == nullptr || std::strstr("quality", only) != nullptr)
		TestQuality();
	if (only == nullptr || std::strstr("utf8", only) != nullptr)
		TestUtf8Paths();
	if (only == nullptr || std::strstr("cancel", only) != nullptr)
	{
		Log("== cancel / progress\n");
		TestCancel();
	}
	if (only == nullptr || std::strstr("bad", only) != nullptr)
	{
		Log("== bad input\n");
		TestBadInput();
	}

	if (!g_keep)
	{
		std::error_code ec;
		fs::remove_all(fs::u8path(g_outDir), ec);
	}
	const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	Log("\n%s: %d checks, %d failures (%.1f s)\n", g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures, sec);
	return g_failures == 0 ? 0 : 1;
}
