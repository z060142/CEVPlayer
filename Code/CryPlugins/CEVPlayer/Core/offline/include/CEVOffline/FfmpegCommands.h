// FfmpegCommands: argument builders (pure, testable without running anything), a stderr probe of a source file and
// the plan runner that executes the builders' output through FfmpegRunner.
//
// Only features of the minimal FFmpeg build are used: libvpx-vp9, libopus, the webm muxer and the scale / format
// filters (see docs/design/M5-ffmpeg.md); two pass encoding writes its first pass to a webm muxer on NUL, so no
// "null" muxer is required.
#pragma once

#include <string>
#include <vector>

#include "CEVOffline/FfmpegRunner.h"
#include "CEVOffline/Offline.h"

namespace cev
{
	namespace offline
	{
		namespace ffmpeg
		{
			// ---- source probe (parses `ffmpeg -hide_banner -i <src>` stderr) -----------------------------------------
			struct SourceProbe
			{
				bool ok = false;                 // at least one stream was recognised
				double durationSeconds = 0.0;    // 0 = unknown (N/A, image sequences without a rate)
				bool hasVideo = false;
				bool hasAudio = false;
				int width = 0;
				int height = 0;
				double frameRate = 0.0;
				std::string videoCodec;          // "prores", "h264", "vp9", "png", ...
				std::string pixelFormat;         // "yuva444p12le", "rgba", ...
				bool hasAlpha = false;           // alpha pixel format or the WebM alpha_mode=1 stream tag
				int audioStreams = 0;
			};

			// Pure parser (canned text in the selftest).
			SourceProbe ParseSourceProbe(const std::string& ffmpegStderr);
			// True for pixel formats that carry alpha (yuva*, rgba, bgra, argb, abgr, gbrap*, ya8, ya16*, pal8 is not).
			bool PixelFormatHasAlpha(const std::string& pixelFormat);
			// Runs ffmpeg -i. A source ffmpeg cannot read gives a failed Result (message = last stderr line).
			// extraInputArgs go before -i (e.g. {"-framerate", "24"} for a PNG sequence).
			Result ProbeSource(const std::string& exe, const std::string& src, SourceProbe& out, int timeoutMs = 30000,
				const std::vector<std::string>& extraInputArgs = std::vector<std::string>());

			// ---- builders ----------------------------------------------------------------------------------------
			enum class AlphaMode
			{
				Auto,      // keep alpha when WebmTranscodeOptions::sourceHasAlpha (TranscodeToWebm() fills it from the probe)
				Preserve,  // always yuva420p
				Discard    // always yuv420p
			};

			struct WebmTranscodeOptions
			{
				int crf = 32;                    // libvpx-vp9 constant quality, 0 (best) .. 63
				int videoBitrateKbps = 0;        // 0: pure constant quality (-b:v 0); > 0: constrained quality (-b:v N k)
				int width = 0;                   // 0 = keep. Only one of width / height given: the other follows, kept even (-2)
				int height = 0;
				AlphaMode alpha = AlphaMode::Auto;
				bool sourceHasAlpha = false;
				bool sourceHasAudio = true;      // false: no audio mapping at all
				bool keepAudio = true;           // false: -an
				int audioBitrateKbps = 96;       // libopus
				int threads = 0;                 // 0 = ffmpeg default
				bool twoPass = false;
				int cpuUsed = 2;                 // libvpx -cpu-used (0 slowest .. 8 fastest); 4..8 is realtime-ish
				double inputFrameRate = 0.0;     // PNG sequence: -framerate N before -i (0: ffmpeg default of 25)
				std::string inputVideoDecoder;   // forced input decoder, e.g. "libvpx-vp9" (needed to decode WebM alpha)
				// Output colour: RGB / unknown sources are converted with this matrix by the scale filter and the stream is
				// tagged accordingly ("bt709"; empty = leave everything to ffmpeg's defaults).
				std::string colorMatrix = "bt709";
				std::string passLogPrefix;       // two pass: default <dst>.2pass
				int ffmpegLogLevel = 1;          // 0 = error, 1 = warning, 2 = info
			};

			// What the planner knows about the work, for the progress weights and the cleanup.
			struct FfmpegCommand
			{
				std::vector<std::string> args;   // without the executable
				double weight = 1.0;             // share of the whole job (progress)
				std::string label;               // "pass 1 of 2", "encode"
			};

			struct TranscodePlan
			{
				std::vector<FfmpegCommand> commands;       // run in order
				std::string finalOutput;                   // dst
				std::string tempOutput;                    // dst + ".tmp": what the last command writes (renamed on success)
				std::string passLogPrefix;                 // empty without two pass: <prefix>-0.log is removed afterwards
				bool alphaOutput = false;                  // the output carries alpha (yuva420p)
				bool Empty() const { return commands.empty(); }
			};

			// TranscodeToWebm: VP9 (+ Opus) in WebM. Alpha: yuva420p + -auto-alt-ref 0 (libvpx-vp9 alpha requirement).
			TranscodePlan BuildTranscodeToWebm(const std::string& src, const std::string& dst, const WebmTranscodeOptions& options);
			// MakeVariant: the same encode forced to `height` (width follows, even); a WebM variant source with alpha
			// is decoded with libvpx (inputVideoDecoder is set to "libvpx-vp9" when options.sourceHasAlpha).
			TranscodePlan BuildMakeVariant(const std::string& src, const std::string& dst, int height, const WebmTranscodeOptions& options);

			// ---- runner ------------------------------------------------------------------------------------------
			struct PlanRunOptions
			{
				double durationSeconds = 0.0;    // of the output (progress fractions); 0 = no fractions until the end
				int noProgressTimeoutMs = 60000;
				int totalTimeoutMs = 0;
				int cancelGraceMs = 500;
			};

			struct PlanRunReport
			{
				int exitCode = -1;               // of the last command that ran
				std::string log;                 // stderr of every command, with headers
				bool cancelled = false;
				bool timedOut = false;
				double elapsedSeconds = 0.0;
			};

			// Runs the commands; progress is spread by the weights. Success: tempOutput renamed over finalOutput.
			// Failure / cancel / timeout: tempOutput and the pass logs are removed, finalOutput is not touched.
			Result RunTranscodePlan(const std::string& exe, const TranscodePlan& plan, const PlanRunOptions& runOptions,
				const Control& ctl, PlanRunReport* report = nullptr);

			// Probe the source, fill sourceHasAlpha / sourceHasAudio / duration, build and run.
			Result TranscodeToWebm(const std::string& exe, const std::string& src, const std::string& dst,
				const WebmTranscodeOptions& options, const Control& ctl, PlanRunReport* report = nullptr);
			Result MakeVariant(const std::string& exe, const std::string& src, const std::string& dst, int height,
				const WebmTranscodeOptions& options, const Control& ctl, PlanRunReport* report = nullptr);
		}
	}
}
