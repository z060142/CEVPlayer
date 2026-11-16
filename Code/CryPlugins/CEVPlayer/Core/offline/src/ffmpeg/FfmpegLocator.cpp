#include "CEVOffline/FfmpegLocator.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "CEVOffline/FfmpegRunner.h"
#include "FfmpegInternal.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace cev
{
	namespace offline
	{
		namespace ffmpeg
		{
			namespace
			{
				namespace fs = std::filesystem;

#if defined(_WIN32)
				const char* const kExeName = "ffmpeg.exe";
				const char kPathSep = ';';
#else
				const char* const kExeName = "ffmpeg";
				const char kPathSep = ':';
#endif

				bool IsFile(const fs::path& p)
				{
					std::error_code ec;
					return fs::is_regular_file(p, ec);
				}
				bool IsDir(const fs::path& p)
				{
					std::error_code ec;
					return fs::is_directory(p, ec);
				}

				std::string PathVariable()
				{
#if defined(_WIN32)
					const DWORD n = GetEnvironmentVariableW(L"PATH", nullptr, 0);
					if (n == 0)
						return std::string();
					std::wstring w(n, L'\0');
					const DWORD got = GetEnvironmentVariableW(L"PATH", &w[0], n);
					w.resize(got);
					return detail::WideToUtf8(w);
#else
					const char* p = std::getenv("PATH");
					return p != nullptr ? p : "";
#endif
				}

				std::vector<std::string> FindAllOnPath(const std::string& pathList)
				{
					std::vector<std::string> hits;
					size_t start = 0;
					while (start <= pathList.size())
					{
						size_t end = pathList.find(kPathSep, start);
						if (end == std::string::npos)
							end = pathList.size();
						std::string dir = detail::Trim(pathList.substr(start, end - start));
						start = end + 1;
						if (dir.size() >= 2 && dir.front() == '"' && dir.back() == '"')
							dir = dir.substr(1, dir.size() - 2);
						if (dir.empty())
							continue;
						const fs::path candidate = fs::u8path(dir) / kExeName;
						if (IsFile(candidate))
							hits.push_back(candidate.u8string());
					}
					return hits;
				}

				void AddNames(const std::string& lineNames, bool splitCommas, std::set<std::string>& out)
				{
					size_t start = 0;
					while (start <= lineNames.size())
					{
						size_t end = splitCommas ? lineNames.find(',', start) : std::string::npos;
						if (end == std::string::npos)
							end = lineNames.size();
						if (end > start)
							out.insert(lineNames.substr(start, end - start));
						start = end + 1;
					}
				}

				void Need(const char* kind, const std::vector<std::string>& wanted, const std::set<std::string>& have, std::vector<std::string>& missing)
				{
					for (const std::string& w : wanted)
						if (have.find(w) == have.end())
							missing.push_back(std::string(kind) + ":" + w);
				}

				bool Contains(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }
			}

			const CapabilityRequirement& MinimumRequirement()
			{
				static const CapabilityRequirement r = {{"libvpx-vp9", "libopus"}, {}, {"webm"}, {}};
				return r;
			}

			const CapabilityRequirement& RecommendedRequirement()
			{
				static const CapabilityRequirement r = {{"libvorbis", "pcm_s16le"}, {}, {"ogg", "wav"}, {}};
				return r;
			}

			const CapabilityRequirement& ImportRequirement()
			{
				static const CapabilityRequirement r = {
					{},
					{"h264", "hevc", "prores", "vp8", "vp9", "libvpx-vp9", "libdav1d", "png", "aac", "mp3", "opus", "vorbis", "flac", "pcm_s16le"},
					{},
					{"mov", "mp4", "matroska", "webm", "avi", "image2"}};
				return r;
			}

			std::set<std::string> ParseNameList(const std::string& listOutput, bool splitCommas)
			{
				std::set<std::string> names;
				bool inList = false;
				for (const std::string& raw : detail::SplitLines(listOutput))
				{
					const std::string line = detail::Trim(raw);
					if (!inList)
					{
						// The legend ends with a line of dashes; everything before it (and without it) is not a list.
						if (line.size() >= 3 && line.find_first_not_of('-') == std::string::npos)
							inList = true;
						continue;
					}
					if (line.empty())
						continue;
					const size_t flagEnd = line.find_first_of(" \t");
					if (flagEnd == std::string::npos)
						continue;
					const std::string flags = line.substr(0, flagEnd);
					if (flags.size() > 8 || flags.find_first_not_of("VASDEFTSBXI.") != std::string::npos)
						continue;
					const size_t nameBegin = line.find_first_not_of(" \t", flagEnd);
					if (nameBegin == std::string::npos)
						continue;
					size_t nameEnd = line.find_first_of(" \t", nameBegin);
					if (nameEnd == std::string::npos)
						nameEnd = line.size();
					AddNames(line.substr(nameBegin, nameEnd - nameBegin), splitCommas, names);
				}
				return names;
			}

			std::string ParseVersion(const std::string& versionOutput)
			{
				for (const std::string& raw : detail::SplitLines(versionOutput))
				{
					const std::string line = detail::Trim(raw);
					static const char kPrefix[] = "ffmpeg version ";
					if (detail::ToLower(line.substr(0, sizeof(kPrefix) - 1)) != kPrefix)
						continue;
					const size_t b = sizeof(kPrefix) - 1;
					size_t e = line.find_first_of(" \t", b);
					if (e == std::string::npos)
						e = line.size();
					return line.substr(b, e - b);
				}
				return std::string();
			}

			std::string ParseConfiguration(const std::string& versionOutput)
			{
				for (const std::string& raw : detail::SplitLines(versionOutput))
				{
					const std::string line = detail::Trim(raw);
					static const char kPrefix[] = "configuration:";
					if (line.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0)
						return detail::Trim(line.substr(sizeof(kPrefix) - 1));
				}
				return std::string();
			}

			std::vector<std::string> MissingFrom(const CapabilityRequirement& req, const FfmpegCapabilities& caps)
			{
				std::vector<std::string> missing;
				Need("encoder", req.encoders, caps.encoders, missing);
				Need("decoder", req.decoders, caps.decoders, missing);
				Need("muxer", req.muxers, caps.muxers, missing);
				Need("demuxer", req.demuxers, caps.demuxers, missing);
				return missing;
			}

			CapabilityReport EvaluateCapabilities(const FfmpegCapabilities& caps)
			{
				CapabilityReport r;
				r.missingRequired = MissingFrom(MinimumRequirement(), caps);
				r.missingRecommended = MissingFrom(RecommendedRequirement(), caps);
				r.missingImport = MissingFrom(ImportRequirement(), caps);
				r.sufficient = r.missingRequired.empty();
				return r;
			}

			std::string DescribeMissing(const std::vector<std::string>& missing)
			{
				if (missing.empty())
					return std::string();
				std::string s = "missing ";
				for (size_t i = 0; i < missing.size(); ++i)
				{
					if (i != 0)
						s += ", ";
					s += missing[i];
				}
				return s;
			}

			const char* FfmpegSourceName(FfmpegSource source)
			{
				switch (source)
				{
				case FfmpegSource::Configured: return "configured path";
				case FfmpegSource::Downloaded: return "downloaded copy";
				case FfmpegSource::System: return "system PATH";
				default: return "none";
				}
			}

			ProbeExec DefaultProbeExec(int timeoutMs)
			{
				return [timeoutMs](const std::string& exe, const std::vector<std::string>& args, std::string& text) -> Result
				{
					const RunResult r = RunCapture(exe, args, timeoutMs);
					text = r.stdoutText;
					if (!r.result.Ok())
					{
						Result res = r.result;
						if (!r.started)
							res.message = "cannot run " + exe + ": " + res.message;
						return res;
					}
					return Result();
				};
			}

			Result ProbeFfmpeg(const std::string& exe, const ProbeExec& exec, FfmpegInfo& out)
			{
				using detail::Res;
				out = FfmpegInfo();
				out.path = exe;
				std::string text;
				Result r = exec(exe, {"-hide_banner", "-version"}, text);
				if (!r.Ok())
					return r;
				out.version = ParseVersion(text);
				if (out.version.empty())
					return Res(Status::InvalidData, exe + " is not ffmpeg (no 'ffmpeg version' line in -version)");
				out.configuration = ParseConfiguration(text);
				out.gpl = Contains(out.configuration, "--enable-gpl");
				out.nonfree = Contains(out.configuration, "--enable-nonfree");

				struct Query
				{
					const char* arg;
					std::set<std::string>* target;
					bool commas;
				};
				const Query queries[] = {
					{"-encoders", &out.caps.encoders, false},
					{"-decoders", &out.caps.decoders, false},
					{"-muxers", &out.caps.muxers, true},
					{"-demuxers", &out.caps.demuxers, true}};
				for (const Query& q : queries)
				{
					text.clear();
					r = exec(exe, {"-hide_banner", q.arg}, text);
					if (!r.Ok())
						return r;
					*q.target = ParseNameList(text, q.commas);
					if (q.target->empty())
						return Res(Status::InvalidData, std::string("ffmpeg ") + q.arg + " printed no usable list");
				}
				out.report = EvaluateCapabilities(out.caps);
				return detail::Okay();
			}

			std::string ResolveExecutable(const std::string& fileOrDir)
			{
				if (fileOrDir.empty())
					return std::string();
				const fs::path p = fs::u8path(fileOrDir);
				if (IsFile(p))
					return p.u8string();
				if (IsDir(p))
				{
					const fs::path direct = p / kExeName;
					if (IsFile(direct))
						return direct.u8string();
					const fs::path bin = p / "bin" / kExeName;
					if (IsFile(bin))
						return bin.u8string();
				}
				return std::string();
			}

			std::string FindOnPath(const std::string& pathList)
			{
				const std::vector<std::string> hits = FindAllOnPath(pathList);
				return hits.empty() ? std::string() : hits.front();
			}

			LocateResult Locate(const LocateOptions& options)
			{
				LocateResult result;
				const ProbeExec exec = options.exec ? options.exec : DefaultProbeExec();
				std::set<std::string> tried; // lower case paths, an executable is probed once

				auto tryCandidate = [&](FfmpegSource source, const std::string& exe) -> bool
				{
					if (!tried.insert(detail::ToLower(exe)).second)
						return false;
					FfmpegInfo info;
					const Result r = ProbeFfmpeg(exe, exec, info);
					if (!r.Ok())
					{
						result.rejected.push_back({source, exe, r.message});
						return false;
					}
					if (!info.report.sufficient)
					{
						result.rejected.push_back({source, exe, "ffmpeg " + info.version + " is insufficient: " + DescribeMissing(info.report.missingRequired)});
						return false;
					}
					result.found = true;
					result.source = source;
					result.info = std::move(info);
					return true;
				};

				auto tryFileOrDir = [&](FfmpegSource source, const std::string& configured) -> bool
				{
					if (configured.empty())
						return false;
					const std::string exe = ResolveExecutable(configured);
					if (exe.empty())
					{
						result.rejected.push_back({source, configured, "no " + std::string(kExeName) + " there"});
						return false;
					}
					return tryCandidate(source, exe);
				};

				bool done = tryFileOrDir(FfmpegSource::Configured, options.settingsPath);
				if (!done)
					done = tryFileOrDir(FfmpegSource::Downloaded, options.downloadedDir);
				if (!done && options.searchSystemPath)
				{
					const std::string pathList = options.pathOverrideSet ? options.pathOverride : PathVariable();
					for (const std::string& exe : FindAllOnPath(pathList))
					{
						if (tryCandidate(FfmpegSource::System, exe))
						{
							done = true;
							break;
						}
					}
				}

				if (result.found)
				{
					result.message = "ffmpeg " + result.info.version + " (" + FfmpegSourceName(result.source) + "): " + result.info.path;
				}
				else if (result.rejected.empty())
				{
					result.message = "ffmpeg was not found (no configured path, no downloaded copy, nothing on PATH)";
				}
				else
				{
					result.message = "no usable ffmpeg: ";
					for (size_t i = 0; i < result.rejected.size(); ++i)
					{
						if (i != 0)
							result.message += "; ";
						result.message += result.rejected[i].path + " (" + result.rejected[i].reason + ")";
					}
				}
				return result;
			}
		}
	}
}
