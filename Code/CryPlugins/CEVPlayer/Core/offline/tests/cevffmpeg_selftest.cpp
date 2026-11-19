// CEVPlayerFFmpeg selftest. Plain executable (no engine, no test framework).
//
//   cevffmpeg_selftest [--media <dir>] [--ffmpeg <ffmpeg.exe>] [--only <substring>] [--out <dir>] [--keep]
//                      [--fetch-url <url> --fetch-sha256 <hex>]
//
// Part 1 needs nothing but this executable: SHA-256 vectors, argument quoting (also through real child
// processes: the executable spawns itself in a few hidden helper modes), -progress parsing, capability parsing on
// canned text, locator order with fake folders and a fake probe, argument builders, installer on stored zips.
// Part 2 runs a real ffmpeg (--ffmpeg, else the one the locator finds on PATH; skipped with a message when there is
// none) on generated sources (MP4 H.264 + AAC, PNG sequence with alpha, ProRes 4444) and the WebM clips in --media,
// and loads every output back through cev::PlayerCore / ProbeMedia.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib")
#endif

#include "CEVCore/IByteSource.h"
#include "CEVCore/IClock.h"
#include "CEVCore/PlayerCore.h"
#include "CEVOffline/FfmpegCommands.h"
#include "CEVOffline/FfmpegInstall.h"
#include "CEVOffline/FfmpegLocator.h"
#include "CEVOffline/FfmpegRunner.h"
#include "CEVOffline/Offline.h"

namespace fs = std::filesystem;
using namespace cev;
using namespace cev::offline;
using namespace cev::offline::ffmpeg;

namespace
{
	int g_failures = 0;
	int g_checks = 0;
	int g_skipped = 0;
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

	double Since(std::chrono::steady_clock::time_point t0)
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	}

	bool Contains(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

	bool HasArg(const std::vector<std::string>& v, const std::string& a) { return std::find(v.begin(), v.end(), a) != v.end(); }

	// Value following `flag`, empty when absent.
	std::string ArgAfter(const std::vector<std::string>& v, const std::string& flag)
	{
		for (size_t i = 0; i + 1 < v.size(); ++i)
			if (v[i] == flag)
				return v[i + 1];
		return std::string();
	}

	long IndexOf(const std::vector<std::string>& v, const std::string& a)
	{
		const auto it = std::find(v.begin(), v.end(), a);
		return it == v.end() ? -1 : static_cast<long>(it - v.begin());
	}

	bool ResultIs(const Result& r, Status s)
	{
		if (r.status != s)
			Log("    got %s (%s), expected %s\n", StatusToString(r.status), r.message.c_str(), StatusToString(s));
		return r.status == s;
	}

	bool FileExists(const std::string& p)
	{
		std::error_code ec;
		return fs::exists(fs::u8path(p), ec);
	}

	std::string g_outDir;
	std::string g_mediaDir;
	std::string g_ffmpegArg;
	bool g_keep = false;

	std::string Out(const std::string& name) { return (fs::u8path(g_outDir) / fs::u8path(name)).u8string(); }

	void WriteText(const std::string& p, const std::string& text)
	{
		std::ofstream f(fs::u8path(p), std::ios::binary);
		f.write(text.data(), static_cast<std::streamsize>(text.size()));
	}

	std::string ReadText(const std::string& p)
	{
		std::ifstream f(fs::u8path(p), std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	}

	std::string Hex(const std::string& bytes)
	{
		static const char* const d = "0123456789abcdef";
		std::string out;
		for (const unsigned char c : bytes)
		{
			out.push_back(d[c >> 4]);
			out.push_back(d[c & 15]);
		}
		return out;
	}

	std::string Unhex(const std::string& hex)
	{
		std::string out;
		for (size_t i = 0; i + 1 < hex.size(); i += 2)
			out.push_back(static_cast<char>(std::strtol(hex.substr(i, 2).c_str(), nullptr, 16)));
		return out;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Hidden helper modes: this executable spawns itself so that the runner can be tested without any other tool.
	// ---------------------------------------------------------------------------------------------------------------
	int HelperMain(const std::string& mode, int argc, char** argv, int index)
	{
		if (mode == "--helper-sleep")
		{
			const int ms = index < argc ? std::atoi(argv[index]) : 1000;
			std::this_thread::sleep_for(std::chrono::milliseconds(ms));
			return 0;
		}
		if (mode == "--helper-exit")
		{
			std::fprintf(stderr, "first line\nthe last words\n");
			return index < argc ? std::atoi(argv[index]) : 1;
		}
		if (mode == "--helper-chatter") // endless stderr output, ignores stdin
		{
			for (int i = 0; i < 600; ++i)
			{
				std::fprintf(stderr, "chatter %d\n", i);
				std::fflush(stderr);
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
			return 0;
		}
		if (mode == "--helper-flood")
		{
			const std::string chunk(65536, 'x');
			for (int i = 0; i < 200; ++i)
			{
				std::fwrite(chunk.data(), 1, chunk.size(), stderr);
				std::fwrite(chunk.data(), 1, chunk.size(), stdout);
			}
			std::fprintf(stderr, "\nFLOOD-END\n");
			return 3;
		}
		if (mode == "--helper-progress") // a canned ffmpeg style -progress stream, 2.0 s of media
		{
			std::fprintf(stderr, "ffmpeg version fake\n");
			std::fflush(stderr);
			std::printf("frame=0\nout_time_us=-9223372036854775807\nout_time_ms=-9223372036854775807\nspeed=N/A\nprogress=continue\n");
			std::fflush(stdout);
			for (int i = 1; i <= 8; ++i)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(60));
				std::printf("frame=%d\nfps=30.00\ntotal_size=%d\nout_time_us=%d\nout_time_ms=%d\nspeed=1.5x\nprogress=continue\n", i * 7, i * 1000, i * 250000, i * 250000);
				std::fflush(stdout);
			}
			std::printf("frame=60\nout_time_us=2000000\nspeed=1.4x\nprogress=end\n");
			std::fflush(stdout);
			return 0;
		}
#if defined(_WIN32)
		if (mode == "--helper-echo-args") // every argument of the real command line as hex of its UTF-8 bytes
		{
			int n = 0;
			wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &n);
			bool afterMode = false;
			for (int i = 0; wargv != nullptr && i < n; ++i)
			{
				if (afterMode)
				{
					const std::wstring w = wargv[i];
					std::string utf8(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr)), '\0');
					WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &utf8[0], static_cast<int>(utf8.size()), nullptr, nullptr);
					std::printf("%s\n", Hex(utf8).c_str());
				}
				else if (std::wstring(wargv[i]) == L"--helper-echo-args")
				{
					afterMode = true;
				}
			}
			LocalFree(wargv);
			return 0;
		}
#endif
		return 99;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Canned ffmpeg listings
	// ---------------------------------------------------------------------------------------------------------------
	std::string Listing(const char* title, const std::vector<std::string>& names, bool formats)
	{
		std::string s;
		if (formats)
		{
			s += "File formats:\n D. = Demuxing supported\n .E = Muxing supported\n ---\n";
			for (const std::string& n : names)
				s += "  E " + n + "   description of " + n + "\n";
		}
		else
		{
			s += std::string(title) + ":\n V..... = Video\n A..... = Audio\n ------\n";
			for (const std::string& n : names)
				s += " V....D " + n + "   description (codec " + n + ")\n";
		}
		return s;
	}

	struct Profile
	{
		std::string version = "ffmpeg version 7.1-test Copyright (c) 2000-2024 the FFmpeg developers\r\nbuilt with gcc\r\nconfiguration: --disable-everything --enable-libvpx\r\n";
		std::vector<std::string> encoders, decoders, muxers, demuxers;
	};

	Profile MinimalBuild()
	{
		Profile p;
		p.encoders = {"libvpx-vp9", "libopus", "libvorbis", "pcm_s16le"};
		p.decoders = {"h264", "hevc", "prores", "vp8", "vp9", "libvpx-vp9", "libdav1d", "png", "aac", "mp3", "opus", "vorbis", "flac", "pcm_s16le"};
		p.muxers = {"webm", "ogg", "wav"};
		p.demuxers = {"mov,mp4,m4a,3gp,3g2,mj2", "matroska,webm", "avi", "image2"};
		return p;
	}

	Profile FullBuild()
	{
		Profile p = MinimalBuild();
		p.version = "ffmpeg version 8.0-full_build-www.gyan.dev Copyright (c) 2000-2025 the FFmpeg developers\r\nconfiguration: --enable-gpl --enable-version3 --enable-libx264 --enable-libvpx\r\n";
		p.encoders.insert(p.encoders.end(), {"libx264", "aac", "prores_ks", "libopus_extra"});
		p.muxers.insert(p.muxers.end(), {"mp4", "matroska", "mov"});
		return p;
	}

	Profile WithoutEncoder(Profile p, const std::string& name)
	{
		p.encoders.erase(std::remove(p.encoders.begin(), p.encoders.end(), name), p.encoders.end());
		return p;
	}

	// Answers `exe -hide_banner <what>` from a profile map (path substring -> profile). "hello" for a path that maps to
	// a null profile (not ffmpeg at all).
	ProbeExec FakeExec(const std::map<std::string, std::shared_ptr<Profile>>& byPathPart, std::vector<std::string>* probed = nullptr)
	{
		return [byPathPart, probed](const std::string& exe, const std::vector<std::string>& args, std::string& text) -> Result
		{
			if (probed != nullptr && args.size() >= 2 && args[1] == "-version")
				probed->push_back(exe);
			const Profile* profile = nullptr;
			bool known = false;
			for (const auto& kv : byPathPart)
			{
				if (exe.find(kv.first) != std::string::npos)
				{
					known = true;
					profile = kv.second.get();
					break;
				}
			}
			if (!known)
				return Result{Status::IoError, "cannot run " + exe};
			if (profile == nullptr)
			{
				text = "hello world\r\n";
				return Result();
			}
			const std::string& what = args.back();
			if (what == "-version") text = profile->version;
			else if (what == "-encoders") text = Listing("Encoders", profile->encoders, false);
			else if (what == "-decoders") text = Listing("Decoders", profile->decoders, false);
			else if (what == "-muxers") text = Listing("Muxers", profile->muxers, true);
			else if (what == "-demuxers") text = Listing("Demuxers", profile->demuxers, true);
			return Result();
		};
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Part 1
	// ---------------------------------------------------------------------------------------------------------------
	void TestSha256()
	{
		g_context = "sha256";
		CHECK(Sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
		CHECK(Sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
		const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
		CHECK(Sha256Hex(two.data(), two.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
		// one million 'a', fed in odd chunk sizes across block boundaries
		Sha256 h;
		const std::string chunk(997, 'a');
		size_t left = 1000000;
		while (left > 0)
		{
			const size_t n = std::min(left, chunk.size());
			h.Update(chunk.data(), n);
			left -= n;
		}
		CHECK(h.FinalHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
		// lengths around the padding edge (55, 56, 63, 64, 65 bytes) against one-shot vs byte-wise
		for (size_t len : {55u, 56u, 57u, 63u, 64u, 65u, 119u, 120u})
		{
			const std::string data(len, 'q');
			Sha256 a, b;
			a.Update(data.data(), data.size());
			for (const char c : data)
				b.Update(&c, 1);
			CHECKF(a.FinalHex() == b.FinalHex(), "len %zu", len);
		}
		// file
		const std::string p = Out("sha_file.bin");
		WriteText(p, "abc");
		std::string hex;
		CHECK(Sha256FileHex(p, hex).Ok());
		CHECK(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
		CHECK(ResultIs(Sha256FileHex(Out("does_not_exist.bin"), hex), Status::IoError));
	}

	std::vector<std::string> TrickyArgs()
	{
		return {
			"plain", "with space", "tab\there", "", "quote\"inside", "\"fully quoted\"", "back\\slash", "trailing\\", "trailing space \\",
			"C:\\path with space\\file name.mp4", "C:\\dir\\", "two\\\\slashes\\\\\"quote", "100%", "%PATH%", "a&b|c<d>e^f", "semi;colon,comma",
			"-i", "--flag=value with space", "\xE5\xBD\xB1\xE7\x89\x87 \xC3\xB1 \xF0\x9F\x8E\xAC.webm", "caf\xC3\xA9", "x=y z", "'single quotes'", "new\nline"};
	}

	void TestQuoting()
	{
		g_context = "quoting";
		CHECK(QuoteWindowsArg("plain") == "plain");
		CHECK(QuoteWindowsArg("") == "\"\"");
		CHECK(QuoteWindowsArg("a b") == "\"a b\"");
		CHECK(QuoteWindowsArg("a\"b") == "\"a\\\"b\"");
		CHECK(QuoteWindowsArg("dir\\") == "dir\\"); // no space, no quote: backslashes stay
		CHECK(QuoteWindowsArg("a b\\") == "\"a b\\\\\"");
		CHECK(QuoteWindowsArg("a\\\"b") == "\"a\\\\\\\"b\"");
		CHECK(BuildWindowsCommandLine("C:\\Program Files\\ffmpeg.exe", {"-i", "in put.mp4"}) == "\"C:\\Program Files\\ffmpeg.exe\" -i \"in put.mp4\"");

#if defined(_WIN32)
		// Parsed back by the OS parser, in process ...
		const std::vector<std::string> args = TrickyArgs();
		{
			const std::string line = BuildWindowsCommandLine("C:\\x\\prog.exe", args);
			std::wstring wline(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0)), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), &wline[0], static_cast<int>(wline.size()));
			int n = 0;
			wchar_t** w = CommandLineToArgvW(wline.c_str(), &n);
			CHECKF(w != nullptr && static_cast<size_t>(n) == args.size() + 1, "argc %d", n);
			for (int i = 1; w != nullptr && i < n && static_cast<size_t>(i) <= args.size(); ++i)
			{
				std::string utf8(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr)), '\0');
				WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &utf8[0], static_cast<int>(utf8.size()), nullptr, nullptr);
				utf8.resize(std::strlen(utf8.c_str()));
				CHECKF(utf8 == args[static_cast<size_t>(i) - 1], "arg %d: got [%s]", i, utf8.c_str());
			}
			if (w != nullptr)
				LocalFree(w);
		}
		// ... and through a real CreateProcessW round trip (this executable echoes its argv).
		RunOptions o;
		o.exe = CurrentExecutablePath();
		o.args.push_back("--helper-echo-args");
		o.args.insert(o.args.end(), args.begin(), args.end());
		const RunResult r = RunProcess(o);
		CHECK(r.result.Ok());
		std::vector<std::string> got;
		size_t start = 0;
		while (start < r.stdoutText.size())
		{
			size_t end = r.stdoutText.find('\n', start);
			if (end == std::string::npos)
				end = r.stdoutText.size();
			std::string line = r.stdoutText.substr(start, end - start);
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			got.push_back(Unhex(line));
			start = end + 1;
		}
		CHECKF(got.size() == args.size(), "echoed %zu of %zu args", got.size(), args.size());
		for (size_t i = 0; i < got.size() && i < args.size(); ++i)
			CHECKF(got[i] == args[i], "arg %zu: [%s]", i, got[i].c_str());
#endif
	}

	void TestProgressParser()
	{
		g_context = "progress parser";
		const std::string stream =
			"frame=0\nfps=0.00\nout_time_us=-9223372036854775807\nout_time_ms=-9223372036854775807\nspeed=N/A\nprogress=continue\n"
			"frame=30\nfps=60.5\ntotal_size=40960\nout_time_us=1000000\nout_time_ms=1000000\nout_time=00:00:01.000000\nspeed=2.5x\nprogress=continue\r\n"
			"frame=60\nout_time_us=N/A\nspeed= 1.0x\nprogress=continue\n"
			"frame=90\ntotal_size=N/A\nout_time_us=3000000\nprogress=end\n";
		// every possible split of the stream into two chunks gives the same blocks
		for (size_t cut = 0; cut <= stream.size(); cut += 7)
		{
			std::vector<ProgressBlock> blocks;
			ProgressParser p([&blocks](const ProgressBlock& b) { blocks.push_back(b); });
			p.Feed(stream.data(), cut);
			p.Feed(stream.data() + cut, stream.size() - cut);
			CHECKF(blocks.size() == 4, "cut %zu: %zu blocks", cut, blocks.size());
			if (blocks.size() != 4)
				continue;
			CHECK(!blocks[0].hasTime && blocks[0].frame == 0 && !blocks[0].ended);
			CHECK(blocks[1].hasTime && blocks[1].outTimeSeconds == 1.0 && blocks[1].frame == 30 && blocks[1].fps == 60.5 && blocks[1].speed == 2.5 && blocks[1].totalSize == 40960);
			CHECK(!blocks[2].hasTime && blocks[2].frame == 60 && blocks[2].speed == 1.0);
			CHECK(blocks[3].hasTime && blocks[3].outTimeSeconds == 3.0 && blocks[3].ended && blocks[3].totalSize == -1);
		}
		// byte at a time
		{
			std::vector<ProgressBlock> blocks;
			ProgressParser p([&blocks](const ProgressBlock& b) { blocks.push_back(b); });
			for (const char c : stream)
				p.Feed(&c, 1);
			CHECK(blocks.size() == 4);
		}
		// garbage lines do not break it
		{
			std::vector<ProgressBlock> blocks;
			ProgressParser p([&blocks](const ProgressBlock& b) { blocks.push_back(b); });
			p.Feed("\n\n=\nnonsense\n=5\nprogress=end\n");
			CHECK(blocks.size() == 1 && blocks[0].ended);
		}
		CHECK(ProgressFraction(0.0, 10.0) == 0.0);
		CHECK(ProgressFraction(2.5, 10.0) == 0.25);
		CHECK(ProgressFraction(12.0, 10.0) < 1.0);
		CHECK(ProgressFraction(5.0, 0.0) == 0.0);
		CHECK(ProgressFraction(-1.0, 10.0) == 0.0);
	}

	void TestCapabilityParsing()
	{
		g_context = "capabilities";
		// Real ffmpeg 8 layout (verbatim lines).
		const std::string realEncoders =
			"Encoders:\n V..... = Video\n A..... = Audio\n S..... = Subtitle\n .F.... = Frame-level multithreading\n ..S... = Slice-level multithreading\n"
			" ...X.. = Codec is experimental\n ....B. = Supports draw_horiz_band\n .....D = Supports direct rendering method 1\n ------\n"
			" V....D a64multi             Multicolor charset for Commodore 64 (codec a64_multi)\n"
			" V....D libvpx-vp9           libvpx VP9 (codec vp9)\n"
			" A....D libopus              libopus Opus (codec opus)\n"
			" A..... pcm_s16le            PCM signed 16-bit little-endian\n";
		const std::set<std::string> enc = ParseNameList(realEncoders, false);
		CHECK(enc.size() == 4 && enc.count("libvpx-vp9") == 1 && enc.count("libopus") == 1 && enc.count("pcm_s16le") == 1 && enc.count("a64multi") == 1);
		CHECK(enc.count("=") == 0 && enc.count("Video") == 0);

		const std::string realDemuxers =
			"Formats:\n D.. = Demuxing supported\n .E. = Muxing supported\n ..d = Is a device\n ---\n"
			" D   avi             AVI (Audio Video Interleaved)\n D   image2          image2 sequence\n"
			" D   matroska,webm   Matroska / WebM\n D   mov,mp4,m4a,3gp,3g2,mj2 QuickTime / MOV\n DE  mpegts          MPEG-TS (MPEG-2 Transport Stream)\n"
			"  E  webm            WebM\n";
		const std::set<std::string> dem = ParseNameList(realDemuxers, true);
		CHECK(dem.count("matroska") == 1 && dem.count("webm") == 1 && dem.count("mov") == 1 && dem.count("mp4") == 1 && dem.count("mj2") == 1);
		CHECK(dem.count("image2") == 1 && dem.count("avi") == 1 && dem.count("mpegts") == 1);
		CHECK(dem.count("matroska,webm") == 0);
		CHECK(ParseNameList(realDemuxers, false).count("matroska,webm") == 1);

		// version / configuration
		CHECK(ParseVersion("ffmpeg version 8.0-full_build-www.gyan.dev Copyright (c) 2000-2025\r\nbuilt with gcc\r\n") == "8.0-full_build-www.gyan.dev");
		CHECK(ParseVersion("ffmpeg version N-118000-g1234abcd Copyright") == "N-118000-g1234abcd");
		CHECK(ParseVersion("ffprobe version 7.0") == "");
		CHECK(ParseVersion("hello") == "");
		CHECK(ParseConfiguration("ffmpeg version 1\nconfiguration: --enable-gpl --x\n") == "--enable-gpl --x");

		// full build
		{
			FfmpegCapabilities caps;
			const Profile p = FullBuild();
			caps.encoders = ParseNameList(Listing("Encoders", p.encoders, false), false);
			caps.decoders = ParseNameList(Listing("Decoders", p.decoders, false), false);
			caps.muxers = ParseNameList(Listing("Muxers", p.muxers, true), true);
			caps.demuxers = ParseNameList(Listing("Demuxers", p.demuxers, true), true);
			const CapabilityReport r = EvaluateCapabilities(caps);
			CHECK(r.sufficient && r.missingRequired.empty() && r.missingRecommended.empty() && r.importComplete());
		}
		// minimal build (exactly the scope of our build)
		{
			FfmpegCapabilities caps;
			const Profile p = MinimalBuild();
			caps.encoders = ParseNameList(Listing("Encoders", p.encoders, false), false);
			caps.decoders = ParseNameList(Listing("Decoders", p.decoders, false), false);
			caps.muxers = ParseNameList(Listing("Muxers", p.muxers, true), true);
			caps.demuxers = ParseNameList(Listing("Demuxers", p.demuxers, true), true);
			const CapabilityReport r = EvaluateCapabilities(caps);
			CHECK(r.sufficient);
			CHECKF(r.missingRecommended.empty() && r.importComplete(), "%s", DescribeMissing(r.missingImport).c_str());
		}
		// insufficient: no libopus, no webm muxer, no vp9 encoder
		{
			FfmpegCapabilities caps;
			caps.encoders = {"libx264", "aac", "libopus_nope"};
			caps.muxers = {"mp4"};
			const CapabilityReport r = EvaluateCapabilities(caps);
			CHECK(!r.sufficient);
			CHECK(r.missingRequired.size() == 3);
			CHECK(IndexOf(r.missingRequired, "encoder:libvpx-vp9") >= 0 && IndexOf(r.missingRequired, "encoder:libopus") >= 0 && IndexOf(r.missingRequired, "muxer:webm") >= 0);
			CHECK(DescribeMissing(r.missingRequired) == "missing encoder:libvpx-vp9, encoder:libopus, muxer:webm");
		}
		// broken output: no legend / empty / HTML error page
		CHECK(ParseNameList("", false).empty());
		CHECK(ParseNameList("Unrecognized option 'encoders'.\nError splitting the argument list: Option not found\n", false).empty());
		CHECK(ParseNameList(" V....D libvpx-vp9 no dashes line so no list\n", false).empty());
		CHECK(!EvaluateCapabilities(FfmpegCapabilities()).sufficient);
	}

	void TestLocatorOrder()
	{
		g_context = "locator";
		const fs::path root = fs::u8path(Out("locator"));
		std::error_code ec;
		fs::remove_all(root, ec);
		for (const char* d : {"loc_cfg", "loc_dl/bin", "loc_p1", "loc_p2", "loc_dlflat", "loc_empty"})
			fs::create_directories(root / d, ec);
		for (const char* f : {"loc_cfg/ffmpeg.exe", "loc_dl/bin/ffmpeg.exe", "loc_p1/ffmpeg.exe", "loc_p2/ffmpeg.exe", "loc_dlflat/ffmpeg.exe"})
			WriteText((root / f).u8string(), "x");
#if !defined(_WIN32)
		g_skipped++;
		Log("  (locator folder tests use ffmpeg.exe names: skipped)\n");
		return;
#endif
		const std::string cfg = (root / "loc_cfg").u8string();
		const std::string cfgExe = (root / "loc_cfg" / "ffmpeg.exe").u8string();
		const std::string dl = (root / "loc_dl").u8string();
		const std::string p1 = (root / "loc_p1").u8string();
		const std::string p2 = (root / "loc_p2").u8string();
		const std::string pathList = p1 + ";" + p2;
		auto prof = [](Profile p) { return std::make_shared<Profile>(std::move(p)); };

		auto base = [&]
		{
			LocateOptions o;
			o.settingsPath = cfg;
			o.downloadedDir = dl;
			o.pathOverrideSet = true;
			o.pathOverride = pathList;
			return o;
		};

		// 1. everything is good: configured wins, nothing else is probed
		{
			std::vector<std::string> probed;
			LocateOptions o = base();
			o.exec = FakeExec({{"loc_cfg", prof(FullBuild())}, {"loc_dl", prof(MinimalBuild())}, {"loc_p1", prof(FullBuild())}, {"loc_p2", prof(FullBuild())}}, &probed);
			const LocateResult r = Locate(o);
			CHECK(r.found && r.source == FfmpegSource::Configured && r.rejected.empty());
			CHECK(Contains(r.info.path, "loc_cfg") && r.info.version == "8.0-full_build-www.gyan.dev" && r.info.gpl && !r.info.nonfree);
			CHECK(probed.size() == 1);
		}
		// 1b. the configured value may also be the executable itself
		{
			LocateOptions o = base();
			o.settingsPath = cfgExe;
			o.exec = FakeExec({{"loc_cfg", prof(MinimalBuild())}});
			const LocateResult r = Locate(o);
			CHECK(r.found && r.source == FfmpegSource::Configured && !r.info.gpl);
		}
		// 2. configured is insufficient -> reported, the downloaded copy (bin/ layout) is used
		{
			LocateOptions o = base();
			o.exec = FakeExec({{"loc_cfg", prof(WithoutEncoder(FullBuild(), "libopus"))}, {"loc_dl", prof(MinimalBuild())}, {"loc_p1", prof(FullBuild())}});
			const LocateResult r = Locate(o);
			CHECK(r.found && r.source == FfmpegSource::Downloaded && Contains(r.info.path, "loc_dl"));
			CHECK(r.rejected.size() == 1 && r.rejected[0].source == FfmpegSource::Configured && Contains(r.rejected[0].reason, "encoder:libopus"));
		}
		// 3. configured path does not exist -> noted, downloaded wins
		{
			LocateOptions o = base();
			o.settingsPath = (root / "nowhere").u8string();
			o.exec = FakeExec({{"loc_dl", prof(MinimalBuild())}});
			const LocateResult r = Locate(o);
			CHECK(r.found && r.source == FfmpegSource::Downloaded);
			CHECK(r.rejected.size() == 1 && Contains(r.rejected[0].reason, "no ffmpeg"));
		}
		// 4. nothing configured, nothing downloaded -> first good ffmpeg on PATH (the first one is insufficient)
		{
			LocateOptions o = base();
			o.settingsPath.clear();
			o.downloadedDir = (root / "loc_empty").u8string();
			o.exec = FakeExec({{"loc_p1", prof(WithoutEncoder(FullBuild(), "libvpx-vp9"))}, {"loc_p2", prof(FullBuild())}});
			const LocateResult r = Locate(o);
			CHECK(r.found && r.source == FfmpegSource::System && Contains(r.info.path, "loc_p2"));
			CHECK(r.rejected.size() == 2); // the empty downloaded folder and loc_p1
		}
		// 5. not ffmpeg at all / cannot run
		{
			LocateOptions o = base();
			o.exec = FakeExec({{"loc_cfg", nullptr}, {"loc_dl", prof(MinimalBuild())}});
			const LocateResult r = Locate(o);
			CHECK(r.found && r.source == FfmpegSource::Downloaded);
			CHECK(r.rejected.size() == 1 && Contains(r.rejected[0].reason, "not ffmpeg"));
		}
		// 6. nothing usable
		{
			LocateOptions o = base();
			o.exec = FakeExec({{"loc_cfg", prof(WithoutEncoder(MinimalBuild(), "libopus"))}});
			o.searchSystemPath = true;
			const LocateResult r = Locate(o);
			CHECK(!r.found && r.source == FfmpegSource::None);
			CHECK(r.rejected.size() == 4); // cfg insufficient, dl cannot run, p1 / p2 cannot run
			CHECK(Contains(r.message, "no usable ffmpeg") && Contains(r.message, "encoder:libopus"));
		}
		{
			LocateOptions o;
			o.pathOverrideSet = true;
			o.pathOverride = (root / "loc_empty").u8string();
			o.exec = FakeExec({});
			const LocateResult r = Locate(o);
			CHECK(!r.found && r.rejected.empty() && Contains(r.message, "not found"));
		}
		// 7. PATH search can be switched off
		{
			LocateOptions o;
			o.pathOverrideSet = true;
			o.pathOverride = pathList;
			o.searchSystemPath = false;
			o.exec = FakeExec({{"loc_p1", prof(FullBuild())}});
			CHECK(!Locate(o).found);
		}
		// 8. the same executable is probed once
		{
			std::vector<std::string> probed;
			LocateOptions o = base();
			o.settingsPath = cfg;
			o.downloadedDir = cfg;
			o.pathOverride = cfg;
			o.exec = FakeExec({{"loc_cfg", prof(WithoutEncoder(MinimalBuild(), "libopus"))}}, &probed);
			const LocateResult r = Locate(o);
			CHECK(!r.found && probed.size() == 1);
		}
		CHECK(ResolveExecutable((root / "loc_dl").u8string()).find("bin") != std::string::npos);
		CHECK(ResolveExecutable((root / "loc_dlflat").u8string()).find("bin") == std::string::npos && !ResolveExecutable((root / "loc_dlflat").u8string()).empty());
		CHECK(ResolveExecutable((root / "loc_empty").u8string()).empty());
		CHECK(FindOnPath(p2 + ";" + p1).find("loc_p2") != std::string::npos);
		CHECK(FindOnPath("").empty());
	}

	void TestCommandBuilders()
	{
		g_context = "builders";
		// default
		{
			const WebmTranscodeOptions o;
			const TranscodePlan p = BuildTranscodeToWebm("C:/in/a b.mp4", "C:/out/x.webm", o);
			CHECK(p.commands.size() == 1 && p.finalOutput == "C:/out/x.webm" && p.tempOutput == "C:/out/x.webm.tmp" && !p.alphaOutput);
			const std::vector<std::string>& a = p.commands[0].args;
			CHECK(ArgAfter(a, "-i") == "C:/in/a b.mp4");
			CHECK(ArgAfter(a, "-c:v") == "libvpx-vp9" && ArgAfter(a, "-crf") == "32" && ArgAfter(a, "-b:v") == "0");
			CHECK(ArgAfter(a, "-c:a") == "libopus" && ArgAfter(a, "-b:a") == "96k");
			CHECK(ArgAfter(a, "-progress") == "pipe:1" && HasArg(a, "-nostats") && HasArg(a, "-y"));
			CHECK(ArgAfter(a, "-f") == "webm" && a.back() == "C:/out/x.webm.tmp"); // never writes to dst directly
			CHECK(IndexOf(a, "-i") < IndexOf(a, "-map") && ArgAfter(a, "-vf").find("format=yuv420p") != std::string::npos);
			CHECK(!HasArg(a, "-auto-alt-ref") && !HasArg(a, "-threads") && !HasArg(a, "-pass"));
			CHECK(ArgAfter(a, "-vf").find("scale=") == 0 && ArgAfter(a, "-vf").find("out_color_matrix=bt709") != std::string::npos);
			CHECK(ArgAfter(a, "-vf").find("-2") == std::string::npos);
			CHECK(ArgAfter(a, "-colorspace") == "bt709");
			// only features of the minimal build: no filter other than scale / format
			const std::string vf = ArgAfter(a, "-vf");
			CHECK(vf.find("fps") == std::string::npos && vf.find("pad") == std::string::npos && vf.find("setsar") == std::string::npos);
		}
		// size, quality, threads, bitrate, no colour tags
		{
			WebmTranscodeOptions o;
			o.crf = 99;
			o.width = 640;
			o.threads = 4;
			o.videoBitrateKbps = 1500;
			o.cpuUsed = 12;
			o.colorMatrix.clear();
			o.audioBitrateKbps = 128;
			const TranscodePlan p = BuildTranscodeToWebm("in.mkv", "out.webm", o);
			const std::vector<std::string>& a = p.commands[0].args;
			CHECK(ArgAfter(a, "-crf") == "63" && ArgAfter(a, "-b:v") == "1500k" && ArgAfter(a, "-threads") == "4" && ArgAfter(a, "-cpu-used") == "8");
			CHECK(ArgAfter(a, "-vf") == "scale=640:-2:flags=lanczos,format=yuv420p");
			CHECK(!HasArg(a, "-colorspace") && ArgAfter(a, "-b:a") == "128k");
			o.width = 0;
			o.height = 360;
			CHECK(ArgAfter(BuildTranscodeToWebm("in.mkv", "out.webm", o).commands[0].args, "-vf") == "scale=-2:360:flags=lanczos,format=yuv420p");
			o.width = 321;
			CHECK(ArgAfter(BuildTranscodeToWebm("in.mkv", "out.webm", o).commands[0].args, "-vf") == "scale=321:360:flags=lanczos,format=yuv420p");
		}
		// alpha
		{
			WebmTranscodeOptions o;
			o.sourceHasAlpha = true;
			o.colorMatrix.clear();
			TranscodePlan p = BuildTranscodeToWebm("in.mov", "out.webm", o);
			CHECK(p.alphaOutput && ArgAfter(p.commands[0].args, "-vf") == "format=yuva420p" && ArgAfter(p.commands[0].args, "-auto-alt-ref") == "0");
			o.alpha = AlphaMode::Discard;
			p = BuildTranscodeToWebm("in.mov", "out.webm", o);
			CHECK(!p.alphaOutput && ArgAfter(p.commands[0].args, "-vf") == "format=yuv420p" && !HasArg(p.commands[0].args, "-auto-alt-ref"));
			o.alpha = AlphaMode::Preserve;
			o.sourceHasAlpha = false;
			CHECK(BuildTranscodeToWebm("in.mov", "out.webm", o).alphaOutput);
		}
		// audio handling
		{
			WebmTranscodeOptions o;
			o.sourceHasAudio = false;
			CHECK(HasArg(BuildTranscodeToWebm("a", "b", o).commands[0].args, "-an") && !HasArg(BuildTranscodeToWebm("a", "b", o).commands[0].args, "-c:a"));
			o.sourceHasAudio = true;
			o.keepAudio = false;
			CHECK(HasArg(BuildTranscodeToWebm("a", "b", o).commands[0].args, "-an"));
			o.keepAudio = true;
			CHECK(ArgAfter(BuildTranscodeToWebm("a", "b", o).commands[0].args, "-map").size() > 0);
		}
		// PNG sequence: -framerate comes before -i
		{
			WebmTranscodeOptions o;
			o.inputFrameRate = 24;
			o.sourceHasAlpha = true;
			o.sourceHasAudio = false;
			const TranscodePlan p = BuildTranscodeToWebm("C:/seq/frame_%04d.png", "out.webm", o);
			const std::vector<std::string>& a = p.commands[0].args;
			CHECK(ArgAfter(a, "-framerate") == "24" && IndexOf(a, "-framerate") < IndexOf(a, "-i") && ArgAfter(a, "-i") == "C:/seq/frame_%04d.png");
		}
		// two pass
		{
			WebmTranscodeOptions o;
			o.twoPass = true;
			const TranscodePlan p = BuildTranscodeToWebm("in.mp4", "C:/o/out.webm", o);
			CHECK(p.commands.size() == 2 && p.passLogPrefix == "C:/o/out.webm.2pass");
			const std::vector<std::string>& p1 = p.commands[0].args;
			const std::vector<std::string>& p2 = p.commands[1].args;
			CHECK(ArgAfter(p1, "-pass") == "1" && ArgAfter(p2, "-pass") == "2" && ArgAfter(p1, "-passlogfile") == p.passLogPrefix && ArgAfter(p2, "-passlogfile") == p.passLogPrefix);
			CHECK(HasArg(p1, "-an") && !HasArg(p1, "-c:a") && ArgAfter(p2, "-c:a") == "libopus");
			CHECK(p1.back() != p.tempOutput && ArgAfter(p1, "-f") == "webm"); // pass 1 goes to the null device, as a webm
			CHECK(p2.back() == p.tempOutput);
			CHECK(p.commands[0].weight + p.commands[1].weight > 0.99 && p.commands[0].weight + p.commands[1].weight < 1.01);
			o.passLogPrefix = "C:/tmp/log";
			CHECK(BuildTranscodeToWebm("in.mp4", "x.webm", o).passLogPrefix == "C:/tmp/log");
		}
		// variants
		{
			WebmTranscodeOptions o;
			o.width = 999; // ignored: a variant is defined by its height
			const TranscodePlan p = BuildMakeVariant("in.webm", "in_360.webm", 360, o);
			CHECK(p.commands.size() == 1 && ArgAfter(p.commands[0].args, "-vf").find("scale=-2:360:") == 0);
			CHECK(ArgAfter(p.commands[0].args, "-c:v") == "libvpx-vp9" && !HasArg(p.commands[0].args, "-framerate"));
			CHECK(IndexOf(p.commands[0].args, "-c:v") > IndexOf(p.commands[0].args, "-i")); // output codec, no forced decoder
			o.sourceHasAlpha = true;
			const TranscodePlan pa = BuildMakeVariant("in.webm", "in_360.webm", 360, o);
			const std::vector<std::string>& a = pa.commands[0].args;
			CHECK(a.size() > 2 && IndexOf(a, "-c:v") < IndexOf(a, "-i") && a[static_cast<size_t>(IndexOf(a, "-c:v")) + 1] == "libvpx-vp9"); // libvpx decoder for the alpha channel
			CHECK(pa.alphaOutput);
			CHECK(BuildMakeVariant("in.webm", "out.webm", 0, o).Empty());
			CHECK(BuildMakeVariant("", "out.webm", 360, o).Empty());
		}
		// M7: keyframe interval -g = round(frame rate x 2 s), at least 1, 60 when the rate is unknown; no -keyint_min
		{
			CHECK(KeyframeIntervalFrames(30.0) == 60 && KeyframeIntervalFrames(29.97) == 60 && KeyframeIntervalFrames(23.976) == 48);
			CHECK(KeyframeIntervalFrames(60.0) == 120 && KeyframeIntervalFrames(0.2) == 1 && KeyframeIntervalFrames(0.0) == 60 && KeyframeIntervalFrames(-5.0) == 60);
			WebmTranscodeOptions o;
			std::vector<std::string> a = BuildTranscodeToWebm("in.mp4", "out.webm", o).commands[0].args;
			CHECK(ArgAfter(a, "-g") == "60" && !HasArg(a, "-keyint_min")); // unknown rate
			CHECK(IndexOf(a, "-g") > IndexOf(a, "-c:v") && IndexOf(a, "-g") < IndexOf(a, "-f"));
			o.sourceFrameRate = 25.0;
			CHECK(ArgAfter(BuildTranscodeToWebm("in.mp4", "out.webm", o).commands[0].args, "-g") == "50");
			CHECK(ArgAfter(BuildMakeVariant("in.webm", "v.webm", 360, o).commands[0].args, "-g") == "50"); // variants too
			o.twoPass = true;
			const TranscodePlan p2 = BuildTranscodeToWebm("in.mp4", "out.webm", o);
			CHECK(ArgAfter(p2.commands[0].args, "-g") == "50" && ArgAfter(p2.commands[1].args, "-g") == "50");
			WebmTranscodeOptions png;
			png.inputFrameRate = 24.0; // PNG sequence: the rate given for the input
			CHECK(ArgAfter(BuildTranscodeToWebm("C:/seq/f_%04d.png", "out.webm", png).commands[0].args, "-g") == "48");
		}
	}

	void TestSourceProbeParsing()
	{
		g_context = "source probe";
		const std::string webmAlpha =
			"Input #0, matroska,webm, from 'a.webm':\n  Metadata:\n    ENCODER         : Lavf62.3.100\n  Duration: 00:00:05.00, start: 0.000000, bitrate: 811 kb/s\n"
			"  Stream #0:0: Video: vp9 (Profile 0), yuv420p(tv, progressive), 640x360, SAR 1:1 DAR 16:9, 30 fps, 30 tbr, 1k tbn\n    Metadata:\n      alpha_mode      : 1\n      ENCODER         : Lavc62.11.100 libvpx-vp9\n"
			"At least one output file must be specified\n";
		SourceProbe p = ParseSourceProbe(webmAlpha);
		CHECK(p.ok && p.hasVideo && !p.hasAudio && p.width == 640 && p.height == 360 && p.durationSeconds == 5.0 && p.videoCodec == "vp9" && p.pixelFormat == "yuv420p" && p.hasAlpha && p.frameRate == 30.0);

		const std::string prores =
			"Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'p.mov':\n  Duration: 00:01:02.50, start: 0.000000, bitrate: 200000 kb/s\n"
			"  Stream #0:0[0x1](eng): Video: prores (4444) (ap4h / 0x68347061), yuva444p12le(progressive), 1920x1080, 190000 kb/s, 24 fps, 24 tbr, 24 tbn (default)\n"
			"  Stream #0:1[0x2](eng): Audio: pcm_s24le (in24 / 0x34326E69), 48000 Hz, stereo, s32 (24 bit), 2304 kb/s (default)\n";
		p = ParseSourceProbe(prores);
		CHECK(p.ok && p.hasVideo && p.hasAudio && p.audioStreams == 1 && p.width == 1920 && p.height == 1080 && p.durationSeconds == 62.5 && p.videoCodec == "prores" && p.pixelFormat == "yuva444p12le" && p.hasAlpha);

		const std::string png =
			"Input #0, image2, from 'seq_%03d.png':\n  Duration: 00:00:00.40, start: 0.000000, bitrate: N/A\n"
			"  Stream #0:0: Video: png, rgba(pc, gbr/unknown/unknown), 160x90, 10 fps, 10 tbr, 10 tbn\n";
		p = ParseSourceProbe(png);
		CHECK(p.ok && p.hasVideo && !p.hasAudio && p.pixelFormat == "rgba" && p.hasAlpha && p.width == 160 && p.height == 90 && p.frameRate == 10.0 && p.durationSeconds == 0.4);

		const std::string mp4 =
			"Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'a.mp4':\n  Duration: 00:00:02.02, start: 0.000000, bitrate: 500 kb/s\n"
			"  Stream #0:0[0x1](und): Video: h264 (High) (avc1 / 0x31637661), yuv420p(progressive), 320x240 [SAR 1:1 DAR 4:3], 400 kb/s, 30 fps, 30 tbr, 15360 tbn (default)\n"
			"  Stream #0:1[0x2](und): Audio: aac (LC) (mp4a / 0x6134706D), 44100 Hz, mono, fltp, 69 kb/s (default)\n"
			"  Stream #0:2: Video: mjpeg (Baseline), yuvj420p, 100x100, 90k tbr, 90k tbn (attached pic)\n";
		p = ParseSourceProbe(mp4);
		CHECK(p.ok && p.hasVideo && p.hasAudio && !p.hasAlpha && p.width == 320 && p.height == 240 && p.videoCodec == "h264" && p.pixelFormat == "yuv420p" && p.frameRate == 30.0);

		p = ParseSourceProbe("x.txt: Invalid data found when processing input\n");
		CHECK(!p.ok);
		p = ParseSourceProbe("  Duration: N/A, bitrate: N/A\n  Stream #0:0: Audio: mp3, 44100 Hz, stereo, fltp, 128 kb/s\n");
		CHECK(p.ok && p.hasAudio && !p.hasVideo && p.durationSeconds == 0.0);
		CHECK(PixelFormatHasAlpha("yuva420p") && PixelFormatHasAlpha("rgba") && PixelFormatHasAlpha("gbrap10le") && PixelFormatHasAlpha("bgra") && PixelFormatHasAlpha("ya8"));
		CHECK(!PixelFormatHasAlpha("yuv420p") && !PixelFormatHasAlpha("rgb24") && !PixelFormatHasAlpha("pal8") && !PixelFormatHasAlpha(""));
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Runner (real processes: this executable in helper modes)
	// ---------------------------------------------------------------------------------------------------------------
	void TestRunner()
	{
		g_context = "runner";
#if !defined(_WIN32)
		g_skipped++;
		return;
#else
		const std::string self = CurrentExecutablePath();
		CHECK(!self.empty());

		// -progress stream through a real pipe: fractions rise monotonically, end at 1.0, no stdout kept
		{
			RunOptions o;
			o.exe = self;
			o.args = {"--helper-progress"};
			o.parseProgress = true;
			o.durationSeconds = 2.0;
			std::vector<double> fractions;
			o.progress = [&fractions](double f) { fractions.push_back(f); };
			const RunResult r = RunProcess(o);
			CHECK(r.result.Ok() && r.exitCode == 0 && r.started && !r.cancelled && !r.timedOut);
			CHECK(r.stdoutText.empty() && Contains(r.stderrLog, "ffmpeg version fake"));
			CHECKF(fractions.size() >= 3, "%zu progress calls", fractions.size());
			bool mono = true;
			for (size_t i = 1; i < fractions.size(); ++i)
				mono = mono && fractions[i] > fractions[i - 1];
			CHECK(mono && !fractions.empty() && fractions.back() == 1.0);
			CHECK(fractions.empty() || fractions.front() > 0.0);
			CHECKF(r.lastOutTimeSeconds >= 1.99, "last out time %f", r.lastOutTimeSeconds);
		}
		// non-zero exit: InvalidData, the last stderr line is in the message
		{
			RunOptions o;
			o.exe = self;
			o.args = {"--helper-exit", "7"};
			const RunResult r = RunProcess(o);
			CHECK(ResultIs(r.result, Status::InvalidData) && r.exitCode == 7 && Contains(r.result.message, "the last words") && Contains(r.stderrLog, "first line"));
		}
		// cannot start
		{
			RunOptions o;
			o.exe = Out("no_such_dir/nothing.exe");
			const RunResult r = RunProcess(o);
			CHECK(ResultIs(r.result, Status::IoError) && !r.started);
			RunOptions o2;
			CHECK(ResultIs(RunProcess(o2).result, Status::InvalidArgument));
		}
		// hung process (no output at all): killed by the no-progress timeout, quickly
		{
			RunOptions o;
			o.exe = self;
			o.args = {"--helper-sleep", "60000"};
			o.noProgressTimeoutMs = 500;
			const auto t0 = std::chrono::steady_clock::now();
			const RunResult r = RunProcess(o);
			const double s = Since(t0);
			CHECK(ResultIs(r.result, Status::IoError) && r.timedOut && r.killed && !r.cancelled);
			CHECK(Contains(r.result.message, "hung"));
			CHECKF(s > 0.4 && s < 5.0, "took %.2f s", s);
		}
		// total time limit even though the process keeps talking
		{
			RunOptions o;
			o.exe = self;
			o.args = {"--helper-chatter"};
			o.noProgressTimeoutMs = 2000;
			o.totalTimeoutMs = 700;
			const auto t0 = std::chrono::steady_clock::now();
			const RunResult r = RunProcess(o);
			const double s = Since(t0);
			CHECK(ResultIs(r.result, Status::IoError) && r.timedOut && Contains(r.result.message, "time limit"));
			CHECKF(s > 0.6 && s < 5.0, "took %.2f s", s);
			CHECK(Contains(r.stderrLog, "chatter 0"));
		}
		// a short quiet process is not a hang
		{
			RunOptions o;
			o.exe = self;
			o.args = {"--helper-sleep", "300"};
			o.noProgressTimeoutMs = 3000;
			CHECK(RunProcess(o).result.Ok());
		}
		// cancel: 'q' is ignored by the helper, so the process tree is killed after the grace time
		{
			std::atomic<bool> cancel{false};
			RunOptions o;
			o.exe = self;
			o.args = {"--helper-chatter"};
			o.cancel = &cancel;
			o.cancelGraceMs = 300;
			o.noProgressTimeoutMs = 0;
			std::thread t([&cancel] { std::this_thread::sleep_for(std::chrono::milliseconds(400)); cancel = true; });
			const auto t0 = std::chrono::steady_clock::now();
			const RunResult r = RunProcess(o);
			const double s = Since(t0);
			t.join();
			CHECK(ResultIs(r.result, Status::Cancelled) && r.cancelled && r.killed && !r.timedOut);
			CHECKF(s > 0.4 && s < 3.0, "took %.2f s (cancel at 0.4 s, grace 0.3 s)", s);
		}
		// cancel flag already set: returns right away
		{
			std::atomic<bool> cancel{true};
			RunOptions o;
			o.exe = self;
			o.args = {"--helper-sleep", "30000"};
			o.cancel = &cancel;
			o.cancelGraceMs = 100;
			const auto t0 = std::chrono::steady_clock::now();
			const RunResult r = RunProcess(o);
			CHECK(ResultIs(r.result, Status::Cancelled));
			CHECKF(Since(t0) < 3.0, "took %.2f s", Since(t0));
		}
		// 12 MB on stderr and stdout: no deadlock, capped capture with a marker, the tail is kept
		{
			RunOptions o;
			o.exe = self;
			o.args = {"--helper-flood"};
			o.maxStderrBytes = 1u << 20;
			o.maxStdoutBytes = 1u << 20;
			const RunResult r = RunProcess(o);
			CHECK(ResultIs(r.result, Status::InvalidData) && r.exitCode == 3);
			CHECKF(r.stderrLog.size() < (2u << 20) && Contains(r.stderrLog, "bytes dropped"), "stderr %zu", r.stderrLog.size());
			CHECK(r.stderrLog.size() > (1u << 19));
			CHECK(Contains(r.stderrLog, "FLOOD-END") && Contains(r.result.message, "FLOOD-END"));
			CHECK(r.stdoutText.size() < (2u << 20) && Contains(r.stdoutText, "bytes dropped"));
		}
		// RunCapture
		{
			const RunResult r = RunCapture(self, {"--helper-exit", "0"}, 5000);
			CHECK(r.result.Ok() && Contains(r.stderrLog, "the last words"));
		}
#endif
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Installer
	// ---------------------------------------------------------------------------------------------------------------
	uint32_t Crc32(const std::string& data)
	{
		uint32_t table[256];
		for (uint32_t i = 0; i < 256; ++i)
		{
			uint32_t c = i;
			for (int k = 0; k < 8; ++k)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		uint32_t crc = 0xFFFFFFFFu;
		for (const unsigned char b : data)
			crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
		return crc ^ 0xFFFFFFFFu;
	}

	// Minimal "stored" zip writer: lets the test create entries with any name (including ../).
	void WriteZip(const std::string& path, const std::vector<std::pair<std::string, std::string>>& entries)
	{
		std::string out;
		std::string central;
		auto u16 = [](std::string& s, uint32_t v) { s.push_back(static_cast<char>(v & 0xFF)); s.push_back(static_cast<char>((v >> 8) & 0xFF)); };
		auto u32 = [&u16](std::string& s, uint32_t v) { u16(s, v & 0xFFFF); u16(s, v >> 16); };
		for (const auto& e : entries)
		{
			const uint32_t crc = Crc32(e.second);
			const uint32_t offset = static_cast<uint32_t>(out.size());
			u32(out, 0x04034b50); u16(out, 20); u16(out, 0); u16(out, 0); u16(out, 0); u16(out, 0x21);
			u32(out, crc); u32(out, static_cast<uint32_t>(e.second.size())); u32(out, static_cast<uint32_t>(e.second.size()));
			u16(out, static_cast<uint32_t>(e.first.size())); u16(out, 0);
			out += e.first;
			out += e.second;
			u32(central, 0x02014b50); u16(central, 20); u16(central, 20); u16(central, 0); u16(central, 0); u16(central, 0); u16(central, 0x21);
			u32(central, crc); u32(central, static_cast<uint32_t>(e.second.size())); u32(central, static_cast<uint32_t>(e.second.size()));
			u16(central, static_cast<uint32_t>(e.first.size())); u16(central, 0); u16(central, 0); u16(central, 0); u16(central, 0); u32(central, 0); u32(central, offset);
			central += e.first;
		}
		const uint32_t cdOffset = static_cast<uint32_t>(out.size());
		out += central;
		u32(out, 0x06054b50); u16(out, 0); u16(out, 0); u16(out, static_cast<uint32_t>(entries.size())); u16(out, static_cast<uint32_t>(entries.size()));
		u32(out, static_cast<uint32_t>(central.size())); u32(out, cdOffset); u16(out, 0);
		WriteText(path, out);
	}

	void TestInstaller()
	{
		g_context = "installer";
		// Constants: download is on exactly when a release is pinned; otherwise it is off and says why.
		CHECK(FfmpegDownloadEnabled() == (std::string(kFfmpegMinimalVersion) != "unreleased" && std::strlen(kFfmpegMinimalSha256) == 64));
		CHECK(FfmpegDownloadEnabled() == FfmpegDownloadDisabledReason().empty());
		CHECK(!FfmpegDownloadEnabled("unreleased", std::string(64, 'a')));
		CHECK(!FfmpegDownloadEnabled("1.0", ""));
		CHECK(!FfmpegDownloadEnabled("1.0", std::string(63, 'a')));
		CHECK(!FfmpegDownloadEnabled("1.0", std::string(64, 'g')));
		CHECK(!FfmpegDownloadEnabled("../evil", std::string(64, 'a')));
		CHECK(FfmpegDownloadEnabled("8.0.1-1", std::string(64, 'A')));
		CHECK(FfmpegMinimalZipName("8.0.1-1") == "ffmpeg-minimal-8.0.1-1-win64.zip");
		CHECK(FfmpegMinimalDownloadUrl("8.0.1-1") == "https://github.com/z060142/CEVPlayer/releases/download/ffmpeg-minimal-v8.0.1-1/ffmpeg-minimal-8.0.1-1-win64.zip");
		if (!FfmpegDownloadEnabled())
		{
			CHECK(Contains(FfmpegDownloadDisabledReason(), "not available yet") && PinnedFfmpegPackage().url.empty());
			LocalFileFetcher fetcher;
			FfmpegPackage pkg;
			pkg.version = kFfmpegMinimalVersion;
			pkg.sha256 = kFfmpegMinimalSha256;
			pkg.url = "x";
			CHECK(ResultIs(DownloadAndInstallFfmpeg(pkg, fetcher, Out("inst_disabled")), Status::Unsupported));
			CHECK(!FileExists(Out("inst_disabled")));
		}
		else
		{
			// Pinned release ffmpeg-minimal-v8.0.3-cev1.
			CHECK(std::string(kFfmpegMinimalVersion) == "8.0.3-cev1");
			CHECK(std::string(kFfmpegMinimalSha256) == "b89f1dee75a7f19ed2fbcc80f72d718fe415515399bc7199a1a1e88cded64033");
			const FfmpegPackage pinned = PinnedFfmpegPackage();
			CHECK(pinned.version == kFfmpegMinimalVersion && pinned.sha256 == kFfmpegMinimalSha256);
			CHECK(pinned.url == "https://github.com/z060142/CEVPlayer/releases/download/ffmpeg-minimal-v8.0.3-cev1/ffmpeg-minimal-8.0.3-cev1-win64.zip");
		}
		CHECK(FfmpegInstallDir("", "1.0").empty() && FfmpegInstallDir("C:/root", "../x").empty() && !FfmpegInstallDir("C:/root", "1.0").empty());
#if defined(_WIN32)
		CHECK(!DefaultFfmpegInstallRoot().empty());
#endif

#if !defined(_WIN32)
		g_skipped++;
		return;
#endif
		const std::string base = Out("inst");
		std::error_code ec;
		fs::remove_all(fs::u8path(base), ec);
		fs::create_directories(fs::u8path(base), ec);
		const std::string fakeExe = "MZ-FAKE-FFMPEG-EXECUTABLE";

		// nested top folder (the usual release layout)
		{
			const std::string zip = base + "/nested.zip";
			WriteZip(zip, {{"ffmpeg-minimal-1.0-win64/ffmpeg.exe", fakeExe}, {"ffmpeg-minimal-1.0-win64/LICENSE", "LGPL"}, {"ffmpeg-minimal-1.0-win64/BUILD.txt", "versions"}});
			std::string sha;
			CHECK(Sha256FileHex(zip, sha).Ok());
			const std::string dir = base + "/target_nested";
			std::string exe;
			const Result r = InstallFfmpegFromZip(zip, sha, dir, &exe);
			CHECK(r.Ok());
			CHECK(!exe.empty() && ReadText(exe) == fakeExe && ReadText(dir + "/LICENSE") == "LGPL" && ReadText(dir + "/BUILD.txt") == "versions");
			CHECK(!FileExists(dir + ".staging") && FileExists(zip));
			// upper case hash is accepted, an existing install is replaced
			std::string upper = sha;
			for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			CHECK(InstallFfmpegFromZip(zip, upper, dir, &exe).Ok());
			// and the locator finds it through the downloaded folder
			LocateOptions lo;
			lo.downloadedDir = dir;
			lo.searchSystemPath = false;
			lo.exec = FakeExec({{"target_nested", std::make_shared<Profile>(MinimalBuild())}});
			const LocateResult lr = Locate(lo);
			CHECK(lr.found && lr.source == FfmpegSource::Downloaded);
		}
		// bin/ layout: the folder above bin is the install folder
		{
			const std::string zip = base + "/bin.zip";
			WriteZip(zip, {{"pkg/bin/ffmpeg.exe", fakeExe}, {"pkg/LICENSE", "L"}});
			std::string sha;
			Sha256FileHex(zip, sha);
			const std::string dir = base + "/target_bin";
			std::string exe;
			CHECK(InstallFfmpegFromZip(zip, sha, dir, &exe).Ok());
			CHECK(ReadText(dir + "/bin/ffmpeg.exe") == fakeExe && ReadText(dir + "/LICENSE") == "L" && Contains(exe, "bin"));
		}
		// flat layout
		{
			const std::string zip = base + "/flat.zip";
			WriteZip(zip, {{"ffmpeg.exe", fakeExe}});
			std::string sha;
			Sha256FileHex(zip, sha);
			std::string exe;
			CHECK(InstallFfmpegFromZip(zip, sha, base + "/target_flat", &exe).Ok() && ReadText(base + "/target_flat/ffmpeg.exe") == fakeExe);
		}
		// SHA-256 mismatch: refused, nothing extracted, no staging folder, an older install survives
		{
			const std::string zip = base + "/mismatch.zip";
			WriteZip(zip, {{"ffmpeg.exe", fakeExe}});
			std::string sha;
			Sha256FileHex(zip, sha);
			const std::string dir = base + "/target_mismatch";
			fs::create_directories(fs::u8path(dir), ec);
			WriteText(dir + "/ffmpeg.exe", "OLD");
			std::string bad = sha;
			bad[10] = bad[10] == '0' ? '1' : '0';
			std::string exe = "untouched";
			const Result r = InstallFfmpegFromZip(zip, bad, dir, &exe);
			CHECK(ResultIs(r, Status::InvalidData) && Contains(r.message, "SHA-256 mismatch") && exe.empty());
			CHECK(ReadText(dir + "/ffmpeg.exe") == "OLD" && !FileExists(dir + ".staging"));
			// no / malformed expected hash: refused as well (never install unverified)
			CHECK(ResultIs(InstallFfmpegFromZip(zip, "", base + "/target_nohash"), Status::InvalidArgument));
			CHECK(ResultIs(InstallFfmpegFromZip(zip, "abc", base + "/target_nohash"), Status::InvalidArgument));
			CHECK(!FileExists(base + "/target_nohash"));
			// tampered after the hash was taken
			std::string data = ReadText(zip);
			data[data.size() / 2] = static_cast<char>(data[data.size() / 2] ^ 0x55);
			WriteText(zip, data);
			CHECK(ResultIs(InstallFfmpegFromZip(zip, sha, base + "/target_tampered"), Status::InvalidData));
			CHECK(!FileExists(base + "/target_tampered"));
			CHECK(ResultIs(InstallFfmpegFromZip(base + "/missing.zip", sha, base + "/target_x"), Status::IoError));
		}
		// no ffmpeg.exe inside
		{
			const std::string zip = base + "/empty.zip";
			WriteZip(zip, {{"readme.txt", "hi"}});
			std::string sha;
			Sha256FileHex(zip, sha);
			CHECK(ResultIs(InstallFfmpegFromZip(zip, sha, base + "/target_empty"), Status::InvalidData));
			CHECK(!FileExists(base + "/target_empty") && !FileExists(base + "/target_empty.staging"));
		}
		// a not-a-zip file with a matching hash: tar refuses, no install
		{
			const std::string junk = base + "/junk.zip";
			WriteText(junk, "this is not a zip archive at all");
			std::string sha;
			Sha256FileHex(junk, sha);
			const Result r = InstallFfmpegFromZip(junk, sha, base + "/target_junk");
			CHECK(!r.Ok() && !FileExists(base + "/target_junk") && !FileExists(base + "/target_junk.staging"));
		}
		// zip slip: an entry named ../evil.txt must not land outside the staging folder
		{
			const std::string zip = base + "/slip.zip";
			WriteZip(zip, {{"../evil.txt", "evil"}, {"..\\evil2.txt", "evil"}, {"sub/../../evil3.txt", "evil"}, {"ffmpeg.exe", fakeExe}});
			std::string sha;
			Sha256FileHex(zip, sha);
			const std::string dir = base + "/deep/target_slip";
			const Result r = InstallFfmpegFromZip(zip, sha, dir);
			Log("    zip slip install: %s (%s)\n", StatusToString(r.status), r.message.c_str());
			for (const char* evil : {"evil.txt", "evil2.txt", "evil3.txt"})
			{
				CHECK(!FileExists(base + "/deep/" + evil) && !FileExists(base + "/" + evil) && !FileExists(base + "/deep/target_slip.staging/../" + evil) && !FileExists(dir + "/" + evil));
			}
			CHECK(!FileExists(base + "/deep/target_slip.staging"));
		}
		// DownloadAndInstall through the local-file fetcher: good, mismatched, missing, cancelled
		{
			const std::string zip = base + "/dl.zip";
			WriteZip(zip, {{"ffmpeg.exe", fakeExe}, {"LICENSE", "L"}});
			std::string sha;
			Sha256FileHex(zip, sha);
			LocalFileFetcher fetcher;
			FfmpegPackage pkg;
			pkg.version = "9.9.9-test";
			pkg.sha256 = sha;
			pkg.url = zip;
			std::vector<double> prog;
			Control ctl;
			ctl.progress = [&prog](double f) { prog.push_back(f); };
			const std::string dir = base + "/dl_target";
			std::string exe;
			CHECK(DownloadAndInstallFfmpeg(pkg, fetcher, dir, &exe, ctl).Ok());
			CHECK(ReadText(dir + "/ffmpeg.exe") == fakeExe && !FileExists(dir + ".download") && !prog.empty() && prog.back() == 1.0);
			bool mono = true;
			for (size_t i = 1; i < prog.size(); ++i)
				mono = mono && prog[i] >= prog[i - 1];
			CHECK(mono);

			FfmpegPackage bad = pkg;
			bad.sha256 = std::string(64, '0');
			const std::string dir2 = base + "/dl_bad";
			CHECK(ResultIs(DownloadAndInstallFfmpeg(bad, fetcher, dir2), Status::InvalidData));
			CHECK(!FileExists(dir2) && !FileExists(dir2 + ".download") && !FileExists(dir2 + ".staging")); // the refused download is deleted

			FfmpegPackage missing = pkg;
			missing.url = base + "/nothing.zip";
			CHECK(ResultIs(DownloadAndInstallFfmpeg(missing, fetcher, base + "/dl_missing"), Status::IoError));
			CHECK(!FileExists(base + "/dl_missing.download"));

			std::atomic<bool> cancel{true};
			Control cc;
			cc.cancel = &cancel;
			CHECK(ResultIs(DownloadAndInstallFfmpeg(pkg, fetcher, base + "/dl_cancel", nullptr, cc), Status::Cancelled));
			CHECK(!FileExists(base + "/dl_cancel") && !FileExists(base + "/dl_cancel.download"));

			// the file:/// form
			FfmpegPackage fileUrl = pkg;
			fileUrl.url = "file:///" + zip;
			CHECK(DownloadAndInstallFfmpeg(fileUrl, fetcher, base + "/dl_fileurl").Ok());
		}
	}

	// Optional: --fetch-url <url> --fetch-sha256 <hex>: WinHTTP against a real (e.g. local) HTTP server.
	void TestWinHttp(const std::string& url, const std::string& sha256)
	{
		g_context = "winhttp";
		const std::string dst = Out("http_download.bin");
		WinHttpFetcher fetcher;
		std::vector<double> prog;
		Control ctl;
		ctl.progress = [&prog](double f) { prog.push_back(f); };
		const Result r = fetcher.Fetch(url, dst, ctl);
		CHECK(r.Ok());
		if (!r.Ok())
		{
			Log("    %s\n", r.message.c_str());
			return;
		}
		std::string hex;
		CHECK(Sha256FileHex(dst, hex).Ok() && hex == sha256);
		CHECK(!prog.empty() && prog.back() == 1.0);
		CHECK(!fetcher.Fetch(url + "-does-not-exist", Out("http_404.bin"), Control()).Ok());
		CHECK(!FileExists(Out("http_404.bin")));
		CHECK(ResultIs(fetcher.Fetch("http://example.com/x.zip", Out("http_nothttps.bin"), Control()), Status::InvalidArgument));
		CHECK(ResultIs(fetcher.Fetch("ftp://example.com/x.zip", Out("http_ftp.bin"), Control()), Status::InvalidArgument));
		CHECK(ResultIs(fetcher.Fetch("not a url", Out("http_bad.bin"), Control()), Status::InvalidArgument));
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Part 2: real ffmpeg
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

	struct Played
	{
		bool loaded = false;
		MediaInfo info;
		uint32_t presented = 0;       // frames the consumer saw
		uint32_t decodeErrors = 0;
		bool hasAlphaPlane = false;
		int alphaInside = -1;         // alpha at the centre of the first frame
		int alphaOutside = -1;        // alpha near the top left corner of the first frame
		int alphaMin = 255, alphaMax = 0;
	};

	// Plays the file through PlayerCore (manual clock, video only) and records what the consumer sees.
	Played PlayThrough(const std::string& path)
	{
		Played out;
		ManualClock clock;
		PlayerConfig cfg;
		cfg.decodeAudio = false;
		cfg.outputRgba = false;
		cfg.externalClock = &clock;
		PlayerCore p(cfg);
		std::unique_ptr<IByteSource> src;
		if (Failed(FileByteSource::Open(path.c_str(), false, src)) || Failed(p.Load(std::move(src))))
			return out;
		if (!WaitFor([&] { return p.GetLoadState() != LoadState::Loading; }, 30000) || p.GetLoadState() != LoadState::Ready)
			return out;
		out.loaded = true;
		out.info = p.GetInfo();
		if (!out.info.hasVideo || Failed(p.Play()))
			return out;
		// Half frame steps: a frame is never skipped because two became due in one step.
		const double dt = 0.5 / (out.info.frameRate > 1.0 ? out.info.frameRate : 30.0);
		for (int i = 0; i < 100000; ++i)
		{
			WaitFor([&] { return p.VideoBufferedFrame() || p.VideoDecoderAtEnd(); }, 15000);
			if (p.Update())
			{
				FrameView fv;
				if (p.GetFrame(fv))
				{
					++out.presented;
					if (fv.plane[3] != nullptr)
					{
						out.hasAlphaPlane = true;
						if (out.presented == 1)
						{
							out.alphaInside = fv.plane[3][static_cast<size_t>(fv.height / 2) * static_cast<size_t>(fv.stride[3]) + static_cast<size_t>(fv.width / 2)];
							out.alphaOutside = fv.plane[3][static_cast<size_t>(fv.height / 20) * static_cast<size_t>(fv.stride[3]) + static_cast<size_t>(fv.width / 20)];
							for (int y = 0; y < fv.height; ++y)
								for (int x = 0; x < fv.width; ++x)
								{
									const int a = fv.plane[3][static_cast<size_t>(y) * static_cast<size_t>(fv.stride[3]) + static_cast<size_t>(x)];
									out.alphaMin = std::min(out.alphaMin, a);
									out.alphaMax = std::max(out.alphaMax, a);
								}
						}
					}
				}
			}
			else if (p.VideoDecoderAtEnd() && !p.VideoBufferedFrame())
			{
				break;
			}
			clock.Advance(dt);
		}
		out.decodeErrors = p.DecodeErrors();
		p.Unload();
		return out;
	}

	bool RunFfmpeg(const std::string& exe, const std::vector<std::string>& args, std::string* log = nullptr)
	{
		RunOptions o;
		o.exe = exe;
		o.args = args;
		o.noProgressTimeoutMs = 120000;
		const RunResult r = RunProcess(o);
		if (!r.result.Ok())
			Log("    ffmpeg failed: %s\n%s\n", r.result.message.c_str(), r.stderrLog.c_str());
		if (log != nullptr)
			*log = r.stderrLog;
		return r.result.Ok();
	}

	struct Sources
	{
		std::string mp4;       // H.264 + AAC (mono 44.1 kHz), 320x240, 30 fps, 2 s = 60 frames
		std::string pngSeq;    // seq_%03d.png, RGBA 160x90, 10 frames, red box on transparent
		std::string prores;    // ProRes 4444 with alpha, 160x90, 10 frames
		bool haveMp4 = false, havePng = false, haveProres = false;
	};

	Sources MakeSources(const std::string& ffmpegExe)
	{
		Sources s;
		const std::string dir = Out("src");
		std::error_code ec;
		fs::create_directories(fs::u8path(dir), ec);
		s.mp4 = dir + "/h264_aac.mp4";
		s.haveMp4 = RunFfmpeg(ffmpegExe, {"-hide_banner", "-y", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=30:duration=2", "-f", "lavfi", "-i",
			"sine=frequency=440:sample_rate=44100:duration=2", "-ac", "1", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", s.mp4});
		const std::string box = "color=c=black@0.0:size=160x90:rate=10,format=rgba,drawbox=x=40:y=20:w=80:h=50:color=red@1.0:t=fill:replace=1";
		s.pngSeq = dir + "/seq_%03d.png";
		s.havePng = RunFfmpeg(ffmpegExe, {"-hide_banner", "-y", "-loglevel", "error", "-f", "lavfi", "-i", box, "-frames:v", "10", "-start_number", "1", "-pix_fmt", "rgba", s.pngSeq});
		s.prores = dir + "/prores4444.mov";
		s.haveProres = RunFfmpeg(ffmpegExe, {"-hide_banner", "-y", "-loglevel", "error", "-f", "lavfi", "-i", box, "-frames:v", "10", "-c:v", "prores_ks", "-profile:v", "4444", "-pix_fmt", "yuva444p10le", s.prores});
		return s;
	}

	void TestRealFfmpeg(const std::string& ffmpegExe, const std::string& only)
	{
		auto selected = [&only](const char* name) { return only.empty() || std::string(name).find(only) != std::string::npos; };
		const std::string dir = Out("real");
		std::error_code ec;
		fs::create_directories(fs::u8path(dir), ec);
		Log("== real ffmpeg: %s\n", ffmpegExe.c_str());

		g_context = "probe";
		const Sources src = MakeSources(ffmpegExe);
		CHECK(src.haveMp4 && src.havePng && src.haveProres);
		{
			SourceProbe pr;
			CHECK(src.haveMp4 && ProbeSource(ffmpegExe, src.mp4, pr).Ok());
			CHECK(pr.hasVideo && pr.hasAudio && pr.width == 320 && pr.height == 240 && !pr.hasAlpha && pr.durationSeconds > 1.9 && pr.durationSeconds < 2.2 && pr.videoCodec == "h264");
			CHECK(src.havePng && ProbeSource(ffmpegExe, src.pngSeq, pr, 30000, {"-framerate", "10"}).Ok());
			CHECK(pr.hasVideo && pr.hasAlpha && pr.width == 160 && pr.height == 90 && pr.pixelFormat == "rgba");
			CHECKF(pr.durationSeconds > 0.9 && pr.durationSeconds < 1.1, "png seq duration %f", pr.durationSeconds);
			CHECK(src.haveProres && ProbeSource(ffmpegExe, src.prores, pr).Ok());
			CHECK(pr.hasAlpha && pr.videoCodec == "prores" && Contains(pr.pixelFormat, "yuva"));
			CHECK(g_mediaDir.empty() || ProbeSource(ffmpegExe, g_mediaDir + "/vp9_alpha.webm", pr).Ok());
			CHECK(g_mediaDir.empty() || (pr.hasAlpha && pr.videoCodec == "vp9")); // WebM alpha_mode tag
			CHECK(!ProbeSource(ffmpegExe, Out("nothing.mp4"), pr).Ok());
			WriteText(Out("garbage.mp4"), std::string(2000, 'z'));
			const Result gr = ProbeSource(ffmpegExe, Out("garbage.mp4"), pr);
			CHECK(ResultIs(gr, Status::InvalidData));
		}

		g_context = "transcode mp4";
		if (src.haveMp4 && selected("mp4"))
		{
			const std::string dst = dir + "/mp4.webm";
			std::vector<double> prog;
			Control ctl;
			ctl.progress = [&prog](double f) { prog.push_back(f); };
			PlanRunReport rep;
			WebmTranscodeOptions o;
			o.cpuUsed = 5;
			const Result r = TranscodeToWebm(ffmpegExe, src.mp4, dst, o, ctl, &rep);
			CHECKF(r.Ok(), "%s\n%s", r.message.c_str(), rep.log.c_str());
			CHECK(FileExists(dst) && !FileExists(dst + ".tmp"));
			CHECK(!prog.empty() && prog.back() == 1.0);
			bool mono = true;
			for (size_t i = 1; i < prog.size(); ++i)
				mono = mono && prog[i] > prog[i - 1];
			CHECK(mono);
			MediaProbe mp;
			CHECK(ProbeMedia(dst, mp).Ok());
			CHECK(mp.hasVideo && mp.videoCodec == VideoCodec::VP9 && mp.width == 320 && mp.height == 240 && !mp.hasAlpha);
			CHECKF(mp.videoFrameCount == 60, "%u frames", mp.videoFrameCount);
			CHECK(mp.audioTracks.size() == 1 && mp.audioTracks[0].codec == AudioCodec::Opus);
			CHECK(mp.durationSeconds > 1.9 && mp.durationSeconds < 2.2);
			const Played pl = PlayThrough(dst);
			CHECK(pl.loaded && pl.info.hasVideo && pl.decodeErrors == 0);
			CHECKF(pl.presented >= 55 && pl.presented <= 60, "presented %u", pl.presented);
			CHECK(!pl.hasAlphaPlane);
		}

		g_context = "transcode mp4 two pass + resize";
		if (src.haveMp4 && selected("twopass"))
		{
			const std::string dst = dir + "/mp4_2pass.webm";
			WebmTranscodeOptions o;
			o.twoPass = true;
			o.height = 120;
			o.cpuUsed = 5;
			o.crf = 40;
			std::vector<double> prog;
			Control ctl;
			ctl.progress = [&prog](double f) { prog.push_back(f); };
			PlanRunReport rep;
			const Result r = TranscodeToWebm(ffmpegExe, src.mp4, dst, o, ctl, &rep);
			CHECKF(r.Ok(), "%s\n%s", r.message.c_str(), rep.log.c_str());
			CHECK(Contains(rep.log, "pass 1 of 2") && Contains(rep.log, "pass 2 of 2"));
			CHECK(!FileExists(dst + ".2pass-0.log") && !FileExists(dst + ".tmp"));
			bool mono = true;
			for (size_t i = 1; i < prog.size(); ++i)
				mono = mono && prog[i] > prog[i - 1];
			CHECK(mono && !prog.empty() && prog.back() == 1.0);
			CHECKF(prog.size() >= 2, "%zu progress calls", prog.size());
			MediaProbe mp;
			CHECK(ProbeMedia(dst, mp).Ok());
			CHECKF(mp.width == 160 && mp.height == 120 && mp.videoFrameCount == 60, "%dx%d %u frames", mp.width, mp.height, mp.videoFrameCount);
			const Played pl = PlayThrough(dst);
			CHECK(pl.loaded && pl.decodeErrors == 0 && pl.presented >= 55);
		}

		g_context = "transcode png sequence (alpha)";
		if (src.havePng && selected("png"))
		{
			const std::string dst = dir + "/png_alpha.webm";
			WebmTranscodeOptions o;
			o.inputFrameRate = 10;
			o.cpuUsed = 4;
			o.crf = 20;
			PlanRunReport rep;
			const Result r = TranscodeToWebm(ffmpegExe, src.pngSeq, dst, o, Control(), &rep);
			CHECKF(r.Ok(), "%s\n%s", r.message.c_str(), rep.log.c_str());
			MediaProbe mp;
			CHECK(ProbeMedia(dst, mp).Ok());
			CHECK(mp.hasAlpha && mp.videoCodec == VideoCodec::VP9 && mp.width == 160 && mp.height == 90 && mp.audioTracks.empty());
			CHECKF(mp.videoFrameCount == 10, "%u frames", mp.videoFrameCount);
			const Played pl = PlayThrough(dst);
			CHECK(pl.loaded && pl.info.hasAlpha && pl.hasAlphaPlane && pl.decodeErrors == 0);
			CHECKF(pl.presented >= 9, "presented %u", pl.presented);
			CHECKF(pl.alphaInside > 200 && pl.alphaOutside < 60, "alpha inside %d outside %d", pl.alphaInside, pl.alphaOutside);
			CHECKF(pl.alphaMax > 200 && pl.alphaMin < 60, "alpha range %d..%d", pl.alphaMin, pl.alphaMax);
			// discarding alpha on request
			const std::string flat = dir + "/png_flat.webm";
			o.alpha = AlphaMode::Discard;
			CHECK(TranscodeToWebm(ffmpegExe, src.pngSeq, flat, o, Control()).Ok());
			CHECK(ProbeMedia(flat, mp).Ok() && !mp.hasAlpha);
		}

		g_context = "transcode prores 4444 (alpha)";
		if (src.haveProres && selected("prores"))
		{
			const std::string dst = dir + "/prores_alpha.webm";
			WebmTranscodeOptions o;
			o.cpuUsed = 4;
			o.crf = 20;
			PlanRunReport rep;
			const Result r = TranscodeToWebm(ffmpegExe, src.prores, dst, o, Control(), &rep);
			CHECKF(r.Ok(), "%s\n%s", r.message.c_str(), rep.log.c_str());
			const Played pl = PlayThrough(dst);
			CHECK(pl.loaded && pl.info.hasAlpha && pl.hasAlphaPlane && pl.decodeErrors == 0);
			CHECKF(pl.presented >= 9, "presented %u", pl.presented);
			CHECKF(pl.alphaInside > 200 && pl.alphaOutside < 60, "alpha inside %d outside %d", pl.alphaInside, pl.alphaOutside);
		}

		g_context = "variants";
		if (!g_mediaDir.empty() && selected("variant"))
		{
			WebmTranscodeOptions o;
			o.cpuUsed = 6;
			o.crf = 36;
			// 1080p with audio -> 360p
			{
				const std::string dst = dir + "/variant_360.webm";
				PlanRunReport rep;
				const Result r = MakeVariant(ffmpegExe, g_mediaDir + "/vp9_1080p30_opus.webm", dst, 360, o, Control(), &rep);
				CHECKF(r.Ok(), "%s\n%s", r.message.c_str(), rep.log.c_str());
				MediaProbe mp;
				CHECK(ProbeMedia(dst, mp).Ok());
				CHECKF(mp.width == 640 && mp.height == 360 && mp.videoFrameCount >= 299 && mp.videoFrameCount <= 301, "%dx%d %u frames", mp.width, mp.height, mp.videoFrameCount);
				CHECK(mp.audioTracks.size() == 1 && mp.audioTracks[0].codec == AudioCodec::Opus && !mp.hasAlpha);
				const Played pl = PlayThrough(dst);
				CHECK(pl.loaded && pl.decodeErrors == 0 && pl.presented >= 290);
			}
			// alpha WebM -> 180p keeps its alpha (decoded with libvpx)
			{
				const std::string dst = dir + "/variant_alpha_180.webm";
				PlanRunReport rep;
				const Result r = MakeVariant(ffmpegExe, g_mediaDir + "/vp9_alpha.webm", dst, 180, o, Control(), &rep);
				CHECKF(r.Ok(), "%s\n%s", r.message.c_str(), rep.log.c_str());
				MediaProbe mp;
				CHECK(ProbeMedia(dst, mp).Ok());
				CHECKF(mp.width == 320 && mp.height == 180 && mp.hasAlpha && mp.videoFrameCount == 150, "%dx%d alpha %d %u frames", mp.width, mp.height, mp.hasAlpha, mp.videoFrameCount);
				const Played pl = PlayThrough(dst);
				CHECK(pl.loaded && pl.info.hasAlpha && pl.hasAlphaPlane && pl.decodeErrors == 0 && pl.presented >= 140);
				// the source alpha is not constant: the variant must not have flattened it
				const Played srcPlay = PlayThrough(g_mediaDir + "/vp9_alpha.webm");
				CHECKF(pl.alphaMax - pl.alphaMin == srcPlay.alphaMax - srcPlay.alphaMin || (pl.alphaMax - pl.alphaMin > 100), "variant alpha %d..%d, source %d..%d", pl.alphaMin, pl.alphaMax, srcPlay.alphaMin, srcPlay.alphaMax);
			}
			// odd source size: the width follows and stays even
			if (FileExists(g_mediaDir + "/vp9_odd_321x181.webm"))
			{
				const std::string dst = dir + "/variant_odd_90.webm";
				const Result r = MakeVariant(ffmpegExe, g_mediaDir + "/vp9_odd_321x181.webm", dst, 90, o, Control());
				CHECK(r.Ok());
				MediaProbe mp;
				CHECK(ProbeMedia(dst, mp).Ok());
				CHECKF(mp.height == 90 && mp.width == 160, "%dx%d", mp.width, mp.height);
			}
			CHECK(ResultIs(MakeVariant(ffmpegExe, g_mediaDir + "/vp9_alpha.webm", dir + "/v0.webm", 0, o, Control()), Status::InvalidArgument));
		}

		g_context = "failures";
		if (selected("fail"))
		{
			// missing source
			const std::string dst = dir + "/fail1.webm";
			CHECK(!TranscodeToWebm(ffmpegExe, Out("no_such_source.mp4"), dst, WebmTranscodeOptions(), Control()).Ok());
			CHECK(!FileExists(dst) && !FileExists(dst + ".tmp"));
			// garbage source
			CHECK(ResultIs(TranscodeToWebm(ffmpegExe, Out("garbage.mp4"), dst, WebmTranscodeOptions(), Control()), Status::InvalidData));
			CHECK(!FileExists(dst) && !FileExists(dst + ".tmp"));
			// src == dst
			CHECK(ResultIs(TranscodeToWebm(ffmpegExe, src.mp4, src.mp4, WebmTranscodeOptions(), Control()), Status::InvalidArgument));
			// an existing destination survives a failed run
			if (src.haveMp4)
			{
				const std::string keep = dir + "/keep.webm";
				WriteText(keep, "ORIGINAL");
				WebmTranscodeOptions bad;
				bad.inputVideoDecoder = "no_such_decoder"; // ffmpeg refuses at start-up
				const Result r = TranscodeToWebm(ffmpegExe, src.mp4, keep, bad, Control());
				CHECK(!r.Ok());
				CHECKF(ReadText(keep) == "ORIGINAL" && !FileExists(keep + ".tmp"), "existing output was touched (%s)", r.message.c_str());
			}
			// an audio-less source with default options
			if (src.havePng)
			{
				WebmTranscodeOptions o;
				o.inputFrameRate = 10;
				o.cpuUsed = 8;
				CHECK(TranscodeToWebm(ffmpegExe, src.pngSeq, dir + "/noaudio.webm", o, Control()).Ok());
			}
		}

		g_context = "unicode and spaces in paths";
		if (src.haveMp4 && selected("unicode"))
		{
			const std::string folder = dir + "/\xE5\xBD\xB1\xE7\x89\x87 folder \xC3\xB1 (test)";
			fs::create_directories(fs::u8path(folder), ec);
			const std::string in = folder + "/in put \xE5\xBD\xB1.mp4";
			fs::copy_file(fs::u8path(src.mp4), fs::u8path(in), fs::copy_options::overwrite_existing, ec);
			const std::string dst = folder + "/\xE5\xBD\xB1\xE7\x89\x87 out.webm";
			WebmTranscodeOptions o;
			o.cpuUsed = 8;
			o.crf = 45;
			o.twoPass = true;
			PlanRunReport rep;
			const Result r = TranscodeToWebm(ffmpegExe, in, dst, o, Control(), &rep);
			CHECKF(r.Ok(), "%s\n%s", r.message.c_str(), rep.log.c_str());
			MediaProbe mp;
			CHECK(ProbeMedia(dst, mp).Ok() && mp.videoFrameCount == 60);
		}

		g_context = "cancel";
		if (!g_mediaDir.empty() && selected("cancel"))
		{
			for (const bool twoPass : {false, true})
			{
				const std::string dst = dir + (twoPass ? "/cancel_2pass.webm" : "/cancel.webm");
				WebmTranscodeOptions o;
				o.cpuUsed = 1; // slow enough to be cancelled in the middle
				o.twoPass = twoPass;
				std::atomic<bool> cancel{false};
				std::chrono::steady_clock::time_point cancelAt;
				double seen = 0.0;
				Control ctl;
				ctl.cancel = &cancel;
				ctl.progress = [&](double f)
				{
					seen = f;
					if (f > 0.02 && !cancel.load())
					{
						cancelAt = std::chrono::steady_clock::now();
						cancel = true;
					}
				};
				// safety net: cancel after 40 s even when no progress arrives
				std::atomic<bool> done{false};
				std::thread watchdog([&] { WaitFor([&] { return done.load(); }, 40000); if (!cancel.load()) { cancelAt = std::chrono::steady_clock::now(); cancel = true; } });
				PlanRunReport rep;
				const auto t0 = std::chrono::steady_clock::now();
				const Result r = TranscodeToWebm(ffmpegExe, g_mediaDir + "/vp9_1080p30_opus.webm", dst, o, ctl, &rep);
				const auto t1 = std::chrono::steady_clock::now();
				done = true;
				watchdog.join();
				Log("    cancel%s: progress %.3f, run %.2f s, cancel -> return %.2f s\n", twoPass ? " (two pass)" : "", seen, Since(t0), std::chrono::duration<double>(t1 - cancelAt).count());
				CHECK(ResultIs(r, Status::Cancelled) && rep.cancelled);
				CHECKF(std::chrono::duration<double>(t1 - cancelAt).count() < 3.0, "cancel took %.2f s", std::chrono::duration<double>(t1 - cancelAt).count());
				CHECK(!FileExists(dst) && !FileExists(dst + ".tmp") && !FileExists(dst + ".2pass-0.log"));
				// the output file was released (the process is really gone): a new file can take its place at once
				WriteText(dst + ".tmp", "x");
				CHECK(FileExists(dst + ".tmp"));
				std::error_code e2;
				CHECK(fs::remove(fs::u8path(dst + ".tmp"), e2));
			}
			// cancelled before it starts
			{
				std::atomic<bool> cancel{true};
				Control ctl;
				ctl.cancel = &cancel;
				const Result r = TranscodeToWebm(ffmpegExe, src.mp4, dir + "/cancel_early.webm", WebmTranscodeOptions(), ctl);
				CHECK(ResultIs(r, Status::Cancelled) && !FileExists(dir + "/cancel_early.webm.tmp"));
			}
		}

		g_context = "ffmpeg hang";
		if (selected("hang"))
		{
			// a "ffmpeg" that never produces anything: the plan runner kills it and cleans up
			TranscodePlan plan;
			plan.finalOutput = dir + "/hang.webm";
			plan.tempOutput = dir + "/hang.webm.tmp";
			FfmpegCommand c;
			c.args = {"--helper-sleep", "60000"};
			plan.commands.push_back(c);
			WriteText(plan.tempOutput, "partial");
			PlanRunOptions ro;
			ro.noProgressTimeoutMs = 600;
			PlanRunReport rep;
			const auto t0 = std::chrono::steady_clock::now();
			const Result r = RunTranscodePlan(CurrentExecutablePath(), plan, ro, Control(), &rep);
			CHECK(ResultIs(r, Status::IoError) && rep.timedOut);
			CHECKF(Since(t0) < 5.0, "took %.2f s", Since(t0));
			CHECK(!FileExists(plan.tempOutput) && !FileExists(plan.finalOutput));
		}
	}
}

int main(int argc, char** argv)
{
	for (int i = 1; i < argc; ++i)
		if (std::strncmp(argv[i], "--helper-", 9) == 0)
			return HelperMain(argv[i], argc, argv, i + 1);

	g_mediaDir =
#ifdef CEV_DEFAULT_MEDIA_DIR
		CEV_DEFAULT_MEDIA_DIR;
#else
		"test_media";
#endif
	std::string only;
	std::string fetchUrl, fetchSha;
	for (int i = 1; i < argc; ++i)
	{
		if (std::strcmp(argv[i], "--media") == 0 && i + 1 < argc)
			g_mediaDir = argv[++i];
		else if (std::strcmp(argv[i], "--ffmpeg") == 0 && i + 1 < argc)
			g_ffmpegArg = argv[++i];
		else if (std::strcmp(argv[i], "--only") == 0 && i + 1 < argc)
			only = argv[++i];
		else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc)
			g_outDir = argv[++i];
		else if (std::strcmp(argv[i], "--fetch-url") == 0 && i + 1 < argc)
			fetchUrl = argv[++i];
		else if (std::strcmp(argv[i], "--fetch-sha256") == 0 && i + 1 < argc)
			fetchSha = argv[++i];
		else if (std::strcmp(argv[i], "--keep") == 0)
			g_keep = true;
		else
		{
			std::printf("usage: cevffmpeg_selftest [--media <dir>] [--ffmpeg <ffmpeg.exe>] [--only <substring>] [--out <dir>] [--keep] [--fetch-url <url> --fetch-sha256 <hex>]\n");
			return 2;
		}
	}
	while (g_mediaDir.size() > 1 && (g_mediaDir.back() == '/' || g_mediaDir.back() == '\\'))
		g_mediaDir.pop_back();
	if (!FileExists(g_mediaDir + "/vp9_alpha.webm"))
	{
		std::printf("note: no test clips in %s (variant / cancel tests are skipped; pass --media <dir>)\n", g_mediaDir.c_str());
		g_mediaDir.clear();
	}
	if (g_outDir.empty())
	{
		std::error_code ec;
		g_outDir = (fs::temp_directory_path(ec) / "cevffmpeg_selftest").u8string();
	}
	{
		std::error_code ec;
		fs::remove_all(fs::u8path(g_outDir), ec);
		fs::create_directories(fs::u8path(g_outDir), ec);
	}

	Log("CEVPlayerFFmpeg selftest, output: %s\n", g_outDir.c_str());
	const auto t0 = std::chrono::steady_clock::now();
	auto sel = [&only](const char* name) { return only.empty() || std::string(name).find(only) != std::string::npos; };

	if (sel("sha256")) { Log("== sha256\n"); TestSha256(); }
	if (sel("quoting")) { Log("== quoting\n"); TestQuoting(); }
	if (sel("progress")) { Log("== progress parser\n"); TestProgressParser(); }
	if (sel("capabilit")) { Log("== capability parsing\n"); TestCapabilityParsing(); }
	if (sel("locator")) { Log("== locator order\n"); TestLocatorOrder(); }
	if (sel("builders")) { Log("== argument builders\n"); TestCommandBuilders(); }
	if (sel("probe")) { Log("== source probe parsing\n"); TestSourceProbeParsing(); }
	if (sel("runner")) { Log("== runner (helper processes)\n"); TestRunner(); }
	if (sel("installer")) { Log("== installer\n"); TestInstaller(); }
	if (!fetchUrl.empty()) { Log("== winhttp %s\n", fetchUrl.c_str()); TestWinHttp(fetchUrl, fetchSha); }

	// Part 2: a real ffmpeg.
	{
		g_context = "locate";
		std::string exe;
		if (!g_ffmpegArg.empty())
		{
			FfmpegInfo info;
			const Result r = ProbeFfmpeg(g_ffmpegArg, DefaultProbeExec(), info);
			CHECKF(r.Ok(), "%s", r.message.c_str());
			CHECK(info.report.sufficient);
			exe = g_ffmpegArg;
		}
		else
		{
			LocateOptions lo; // settings and downloaded copy empty: PATH
			const LocateResult lr = Locate(lo);
			Log("== locate on PATH: %s\n", lr.message.c_str());
			if (lr.found)
			{
				CHECK(lr.source == FfmpegSource::System && lr.info.report.sufficient && !lr.info.version.empty());
				CHECK(lr.info.caps.encoders.count("libvpx-vp9") == 1 && lr.info.caps.muxers.count("webm") == 1);
				Log("   %s; recommended missing: %s; import missing: %s\n", lr.info.version.c_str(),
					DescribeMissing(lr.info.report.missingRecommended).c_str(), DescribeMissing(lr.info.report.missingImport).c_str());
				exe = lr.info.path;
				// the configured path wins over PATH, an insufficient / bogus one is reported and skipped
				LocateOptions o2;
				o2.settingsPath = exe;
				o2.searchSystemPath = false;
				const LocateResult l2 = Locate(o2);
				CHECK(l2.found && l2.source == FfmpegSource::Configured);
				LocateOptions o3;
				o3.settingsPath = CurrentExecutablePath(); // an executable that is not ffmpeg
				o3.searchSystemPath = false;
				const LocateResult l3 = Locate(o3);
				CHECK(!l3.found && l3.rejected.size() == 1);
			}
			else
			{
				Log("SKIP real ffmpeg tests: no usable ffmpeg on PATH (use --ffmpeg <path>)\n");
				++g_skipped;
			}
		}
		static const char* const kPart1[] = {"sha256", "quoting", "progress", "capabilit", "locator", "builders", "probe", "runner", "installer"};
		bool part1Only = false;
		for (const char* n : kPart1)
			part1Only = part1Only || (!only.empty() && std::string(n).find(only) != std::string::npos);
		if (!exe.empty() && !part1Only)
			TestRealFfmpeg(exe, only == "real" ? std::string() : only);
	}

	if (!g_keep)
	{
		std::error_code ec;
		fs::remove_all(fs::u8path(g_outDir), ec);
	}
	Log("\n%d checks, %d failures, %d skipped, %.1f s\n", g_checks, g_failures, g_skipped, Since(t0));
	Log(g_failures == 0 ? "PASS\n" : "FAIL\n");
	return g_failures == 0 ? 0 : 1;
}
