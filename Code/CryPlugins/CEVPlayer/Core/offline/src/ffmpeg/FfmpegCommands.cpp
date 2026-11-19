#include "CEVOffline/FfmpegCommands.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "FfmpegInternal.h"

namespace cev
{
	namespace offline
	{
		namespace ffmpeg
		{
			namespace
			{
				namespace fs = std::filesystem;

				// "a, b(c, d), e" -> {"a", "b(c, d)", "e"}
				std::vector<std::string> SplitTopLevel(const std::string& s)
				{
					std::vector<std::string> out;
					int depth = 0;
					std::string cur;
					for (size_t i = 0; i < s.size(); ++i)
					{
						const char c = s[i];
						if (c == '(' || c == '[')
							++depth;
						else if ((c == ')' || c == ']') && depth > 0)
							--depth;
						if (c == ',' && depth == 0)
						{
							out.push_back(detail::Trim(cur));
							cur.clear();
						}
						else
						{
							cur.push_back(c);
						}
					}
					out.push_back(detail::Trim(cur));
					return out;
				}

				bool ParseSize(const std::string& field, int& w, int& h)
				{
					int a = 0, b = 0;
					if (std::sscanf(field.c_str(), "%dx%d", &a, &b) == 2 && a > 0 && b > 0)
					{
						w = a;
						h = b;
						return true;
					}
					return false;
				}

				double ParseDuration(const std::string& line)
				{
					const size_t p = line.find("Duration:");
					if (p == std::string::npos)
						return 0.0;
					int h = 0, m = 0;
					double s = 0.0;
					if (std::sscanf(line.c_str() + p + 9, " %d:%d:%lf", &h, &m, &s) == 3)
						return h * 3600.0 + m * 60.0 + s;
					return 0.0; // N/A
				}

				std::string TempFor(const std::string& dst) { return dst + ".tmp"; }

#if defined(_WIN32)
				const char* const kNullSink = "NUL";
#else
				const char* const kNullSink = "/dev/null";
#endif
				void Push(std::vector<std::string>& v, const char* a) { v.emplace_back(a); }
				void Push(std::vector<std::string>& v, const char* a, const std::string& b) { v.emplace_back(a); v.push_back(b); }

				void RemoveFile(const std::string& utf8)
				{
					if (utf8.empty())
						return;
					std::error_code ec;
					fs::remove(fs::u8path(utf8), ec);
				}

				bool SamePath(const std::string& a, const std::string& b)
				{
					std::error_code ec;
					const fs::path pa = fs::weakly_canonical(fs::u8path(a), ec);
					const fs::path pb = fs::weakly_canonical(fs::u8path(b), ec);
					return detail::ToLower(pa.u8string()) == detail::ToLower(pb.u8string());
				}
			}

			bool PixelFormatHasAlpha(const std::string& fmt)
			{
				static const char* const kAlpha[] = {"yuva", "rgba", "bgra", "argb", "abgr", "gbrap", "ya8", "ya16", "ayuv"};
				for (const char* k : kAlpha)
				{
					const size_t n = std::char_traits<char>::length(k);
					if (fmt.compare(0, n, k) == 0)
						return true;
				}
				return false;
			}

			SourceProbe ParseSourceProbe(const std::string& text)
			{
				SourceProbe p;
				bool inFirstVideo = false;
				for (const std::string& raw : detail::SplitLines(text))
				{
					if (p.durationSeconds == 0.0 && raw.find("Duration:") != std::string::npos)
						p.durationSeconds = ParseDuration(raw);

					const size_t sp = raw.find("Stream #");
					if (sp != std::string::npos)
					{
						inFirstVideo = false;
						const size_t vp = raw.find(": Video:", sp);
						const size_t ap = raw.find(": Audio:", sp);
						if (ap != std::string::npos)
						{
							p.hasAudio = true;
							++p.audioStreams;
							p.ok = true;
						}
						else if (vp != std::string::npos && !p.hasVideo && raw.find("attached pic") == std::string::npos)
						{
							p.hasVideo = true;
							p.ok = true;
							inFirstVideo = true;
							const std::vector<std::string> fields = SplitTopLevel(raw.substr(vp + 8));
							if (!fields.empty())
							{
								const size_t e = fields[0].find_first_of(" (");
								p.videoCodec = fields[0].substr(0, e);
							}
							bool gotSize = false;
							for (size_t i = 1; i < fields.size(); ++i)
							{
								const std::string& f = fields[i];
								if (!gotSize && ParseSize(f, p.width, p.height))
								{
									gotSize = true;
								}
								else if (i == 1 && !gotSize)
								{
									p.pixelFormat = f.substr(0, f.find('('));
								}
								else if (f.size() > 4 && f.compare(f.size() - 4, 4, " fps") == 0)
								{
									p.frameRate = std::strtod(f.c_str(), nullptr);
								}
								else if (p.frameRate == 0.0 && f.size() > 4 && f.compare(f.size() - 4, 4, " tbr") == 0)
								{
									p.frameRate = std::strtod(f.c_str(), nullptr);
								}
							}
							p.hasAlpha = PixelFormatHasAlpha(p.pixelFormat);
						}
						continue;
					}
					if (inFirstVideo)
					{
						const std::string line = detail::Trim(raw);
						if (line.compare(0, 10, "alpha_mode") == 0)
						{
							const size_t colon = line.find(':');
							if (colon != std::string::npos && std::atoi(line.c_str() + colon + 1) == 1)
								p.hasAlpha = true;
						}
					}
				}
				return p;
			}

			Result ProbeSource(const std::string& exe, const std::string& src, SourceProbe& out, int timeoutMs,
				const std::vector<std::string>& extraInputArgs)
			{
				using detail::Res;
				out = SourceProbe();
				std::vector<std::string> args = {"-hide_banner"};
				args.insert(args.end(), extraInputArgs.begin(), extraInputArgs.end());
				Push(args, "-i", src);
				const RunResult r = RunCapture(exe, args, timeoutMs); // exits with 1 ("At least one output file must be specified")
				if (!r.started || r.timedOut || r.cancelled)
					return r.result;
				out = ParseSourceProbe(r.stderrLog);
				if (!out.ok)
				{
					std::string last;
					const std::vector<std::string> lines = detail::SplitLines(r.stderrLog);
					for (size_t i = lines.size(); i-- > 0;)
					{
						last = detail::Trim(lines[i]);
						if (!last.empty())
							break;
					}
					return Res(Status::InvalidData, "ffmpeg cannot read " + src + (last.empty() ? std::string() : ": " + last));
				}
				return detail::Okay();
			}

			// -------------------------------------------------------------------------------------------------------
			// Builders
			// -------------------------------------------------------------------------------------------------------
			TranscodePlan BuildTranscodeToWebm(const std::string& src, const std::string& dst, const WebmTranscodeOptions& o)
			{
				TranscodePlan plan;
				plan.finalOutput = dst;
				plan.tempOutput = TempFor(dst);
				if (src.empty() || dst.empty())
					return plan;

				const bool alpha = o.alpha == AlphaMode::Preserve || (o.alpha == AlphaMode::Auto && o.sourceHasAlpha);
				plan.alphaOutput = alpha;
				const int crf = std::max(0, std::min(63, o.crf));
				const bool haveAudio = o.sourceHasAudio && o.keepAudio;

				// Input side.
				std::vector<std::string> common;
				Push(common, "-hide_banner");
				Push(common, "-y");
				Push(common, "-loglevel", o.ffmpegLogLevel <= 0 ? "error" : (o.ffmpegLogLevel == 1 ? "warning" : "info"));
				Push(common, "-progress", "pipe:1");
				Push(common, "-nostats");
				if (o.inputFrameRate > 0.0)
				{
					char fr[32];
					std::snprintf(fr, sizeof(fr), "%g", o.inputFrameRate);
					Push(common, "-framerate", fr);
				}
				if (o.inputStartNumber >= 0)
					Push(common, "-start_number", std::to_string(o.inputStartNumber));
				if (!o.inputVideoDecoder.empty())
					Push(common, "-c:v", o.inputVideoDecoder);
				Push(common, "-i", src);

				// Video chain: only the scale and format filters (the minimal build has nothing else).
				std::string scale;
				if (o.width > 0 || o.height > 0)
					scale = "scale=" + (o.width > 0 ? std::to_string(o.width) : std::string("-2")) + ":" + (o.height > 0 ? std::to_string(o.height) : std::string("-2")) + ":flags=lanczos";
				if (!o.colorMatrix.empty())
					scale += (scale.empty() ? "scale=" : ":") + std::string("out_color_matrix=") + o.colorMatrix;
				std::string chain = scale;
				if (!chain.empty())
					chain += ",";
				chain += alpha ? "format=yuva420p" : "format=yuv420p";

				std::vector<std::string> video;
				Push(video, "-map", "0:v:0");
				Push(video, "-vf", chain);
				Push(video, "-c:v", "libvpx-vp9");
				Push(video, "-b:v", o.videoBitrateKbps > 0 ? std::to_string(o.videoBitrateKbps) + "k" : std::string("0"));
				Push(video, "-crf", std::to_string(crf));
				Push(video, "-deadline", "good");
				Push(video, "-cpu-used", std::to_string(std::max(0, std::min(8, o.cpuUsed))));
				Push(video, "-row-mt", "1");
				// M7: a keyframe every 2 s bounds the catch-up when a hidden player becomes visible again
				Push(video, "-g", std::to_string(KeyframeIntervalFrames(o.sourceFrameRate > 0.0 ? o.sourceFrameRate : o.inputFrameRate)));
				if (alpha)
					Push(video, "-auto-alt-ref", "0"); // libvpx-vp9 cannot use alt-ref frames together with alpha
				if (o.threads > 0)
					Push(video, "-threads", std::to_string(o.threads));
				if (o.colorMatrix == "bt709")
				{
					Push(video, "-colorspace", "bt709");
					Push(video, "-color_primaries", "bt709");
					Push(video, "-color_trc", "bt709");
				}

				std::vector<std::string> audio;
				if (haveAudio)
				{
					Push(audio, "-map", "0:a:0?");
					Push(audio, "-c:a", "libopus");
					Push(audio, "-b:a", std::to_string(std::max(6, o.audioBitrateKbps)) + "k");
				}
				else
				{
					Push(audio, "-an");
				}

				if (o.twoPass)
				{
					plan.passLogPrefix = o.passLogPrefix.empty() ? dst + ".2pass" : o.passLogPrefix;
					FfmpegCommand p1;
					p1.label = "pass 1 of 2";
					p1.weight = 0.45;
					p1.args = common;
					p1.args.insert(p1.args.end(), video.begin(), video.end());
					Push(p1.args, "-pass", "1");
					Push(p1.args, "-passlogfile", plan.passLogPrefix);
					Push(p1.args, "-an");
					Push(p1.args, "-f", "webm");
					p1.args.push_back(kNullSink); // webm muxer into the null device: no null muxer needed
					plan.commands.push_back(std::move(p1));

					FfmpegCommand p2;
					p2.label = "pass 2 of 2";
					p2.weight = 0.55;
					p2.args = common;
					p2.args.insert(p2.args.end(), video.begin(), video.end());
					Push(p2.args, "-pass", "2");
					Push(p2.args, "-passlogfile", plan.passLogPrefix);
					p2.args.insert(p2.args.end(), audio.begin(), audio.end());
					Push(p2.args, "-f", "webm");
					p2.args.push_back(plan.tempOutput);
					plan.commands.push_back(std::move(p2));
				}
				else
				{
					FfmpegCommand c;
					c.label = "encode";
					c.weight = 1.0;
					c.args = common;
					c.args.insert(c.args.end(), video.begin(), video.end());
					c.args.insert(c.args.end(), audio.begin(), audio.end());
					Push(c.args, "-f", "webm");
					c.args.push_back(plan.tempOutput);
					plan.commands.push_back(std::move(c));
				}
				return plan;
			}

			int KeyframeIntervalFrames(double frameRate)
			{
				if (!(frameRate > 0.0) || frameRate > 1000.0)
					return 60; // unknown (also NaN / nonsense)
				return std::max(1, static_cast<int>(std::lround(frameRate * 2.0)));
			}

			TranscodePlan BuildMakeVariant(const std::string& src, const std::string& dst, int height, const WebmTranscodeOptions& options)
			{
				WebmTranscodeOptions o = options;
				o.width = 0;
				o.height = height;
				if (o.inputVideoDecoder.empty() && o.sourceHasAlpha && o.alpha != AlphaMode::Discard)
					o.inputVideoDecoder = "libvpx-vp9"; // the native vp9 decoder drops the WebM alpha channel
				if (height <= 0)
					return TranscodePlan();
				return BuildTranscodeToWebm(src, dst, o);
			}

			// -------------------------------------------------------------------------------------------------------
			// Runner
			// -------------------------------------------------------------------------------------------------------
			Result RunTranscodePlan(const std::string& exe, const TranscodePlan& plan, const PlanRunOptions& ro,
				const Control& ctl, PlanRunReport* report)
			{
				using detail::Res;
				PlanRunReport local;
				PlanRunReport& rep = report != nullptr ? *report : local;
				rep = PlanRunReport();
				if (plan.Empty())
					return Res(Status::InvalidArgument, "nothing to run (empty plan: check the source / destination / height)");
				if (SamePath(plan.finalOutput, plan.tempOutput))
					return Res(Status::InvalidArgument, "bad plan");

				auto cleanup = [&]
				{
					RemoveFile(plan.tempOutput);
					if (!plan.passLogPrefix.empty())
						RemoveFile(plan.passLogPrefix + "-0.log");
				};
				RemoveFile(plan.tempOutput); // leftover of an earlier failed run

				double totalWeight = 0.0;
				for (const FfmpegCommand& c : plan.commands)
					totalWeight += c.weight;
				if (!(totalWeight > 0.0))
					totalWeight = 1.0;

				double done = 0.0;
				double lastReported = -1.0;
				const auto t0 = std::chrono::steady_clock::now();
				for (size_t i = 0; i < plan.commands.size(); ++i)
				{
					const FfmpegCommand& c = plan.commands[i];
					if (ctl.cancel != nullptr && ctl.cancel->load(std::memory_order_relaxed))
					{
						cleanup();
						rep.cancelled = true;
						return Res(Status::Cancelled, "cancelled");
					}
					RunOptions run;
					run.exe = exe;
					run.args = c.args;
					run.parseProgress = true;
					run.durationSeconds = ro.durationSeconds;
					run.cancel = ctl.cancel;
					run.noProgressTimeoutMs = ro.noProgressTimeoutMs;
					run.totalTimeoutMs = ro.totalTimeoutMs;
					run.cancelGraceMs = ro.cancelGraceMs;
					const double base = done / totalWeight;
					const double share = c.weight / totalWeight;
					run.progress = [&](double f)
					{
						if (!ctl.progress)
							return;
						double v = base + share * f;
						if (v >= 1.0)
							v = 0.999; // 1.0 only after the output is in place
						if (v > lastReported)
						{
							lastReported = v;
							ctl.progress(v);
						}
					};
					const RunResult rr = RunProcess(run);
					rep.exitCode = rr.exitCode;
					rep.log += "=== " + c.label + " (exit " + std::to_string(rr.exitCode) + ") ===\n" + rr.stderrLog;
					if (!rep.log.empty() && rep.log.back() != '\n')
						rep.log.push_back('\n');
					if (!rr.result.Ok())
					{
						cleanup();
						rep.cancelled = rr.cancelled;
						rep.timedOut = rr.timedOut;
						rep.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
						return rr.result;
					}
					done += c.weight;
				}

				std::error_code ec;
				fs::rename(fs::u8path(plan.tempOutput), fs::u8path(plan.finalOutput), ec);
				if (ec)
				{
					cleanup();
					return Res(Status::IoError, "cannot move the encoded file into place: " + ec.message());
				}
				if (!plan.passLogPrefix.empty())
					RemoveFile(plan.passLogPrefix + "-0.log");
				rep.elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
				if (ctl.progress && lastReported < 1.0)
					ctl.progress(1.0);
				return detail::Okay();
			}

			namespace
			{
				Result PrepareAndRun(const std::string& exe, const std::string& src, const std::string& dst, int variantHeight,
					WebmTranscodeOptions options, const Control& ctl, PlanRunReport* report)
				{
					using detail::Res;
					if (SamePath(src, dst))
						return Res(Status::InvalidArgument, "source and destination are the same file");
					std::vector<std::string> inputArgs;
					if (options.inputFrameRate > 0.0)
					{
						char fr[32];
						std::snprintf(fr, sizeof(fr), "%g", options.inputFrameRate);
						inputArgs = {"-framerate", fr};
					}
					if (options.inputStartNumber >= 0)
					{
						inputArgs.push_back("-start_number");
						inputArgs.push_back(std::to_string(options.inputStartNumber));
					}
					SourceProbe probe;
					Result r = ProbeSource(exe, src, probe, 30000, inputArgs);
					if (!r.Ok())
						return r;
					if (!probe.hasVideo)
						return Res(Status::Unsupported, src + " has no video stream");
					options.sourceHasAlpha = probe.hasAlpha;
					options.sourceHasAudio = probe.hasAudio;
					if (options.inputFrameRate <= 0.0)
						options.sourceFrameRate = probe.frameRate; // a PNG sequence runs at inputFrameRate
					if (probe.hasAlpha && options.inputVideoDecoder.empty() && (probe.videoCodec == "vp9" || probe.videoCodec == "vp8"))
						options.inputVideoDecoder = probe.videoCodec == "vp9" ? "libvpx-vp9" : "libvpx"; // WebM alpha rides in BlockAdditions
					const TranscodePlan plan = variantHeight > 0 ? BuildMakeVariant(src, dst, variantHeight, options) : BuildTranscodeToWebm(src, dst, options);
					PlanRunOptions ro;
					ro.durationSeconds = probe.durationSeconds;
					return RunTranscodePlan(exe, plan, ro, ctl, report);
				}
			}

			Result TranscodeToWebm(const std::string& exe, const std::string& src, const std::string& dst,
				const WebmTranscodeOptions& options, const Control& ctl, PlanRunReport* report)
			{
				return PrepareAndRun(exe, src, dst, 0, options, ctl, report);
			}

			Result MakeVariant(const std::string& exe, const std::string& src, const std::string& dst, int height,
				const WebmTranscodeOptions& options, const Control& ctl, PlanRunReport* report)
			{
				if (height <= 0)
					return detail::Res(Status::InvalidArgument, "variant height must be positive");
				return PrepareAndRun(exe, src, dst, height, options, ctl, report);
			}
		}
	}
}
