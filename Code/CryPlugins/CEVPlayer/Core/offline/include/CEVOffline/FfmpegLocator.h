// FfmpegLocator: finds a usable ffmpeg executable and tells what it can do.
//
// Search order: 1. the path configured in the editor settings (a file, or a folder that holds ffmpeg.exe or
// bin/ffmpeg.exe), 2. our downloaded copy (same folder rules), 3. the system PATH. A candidate is accepted
// only when `-encoders` / `-decoders` / `-muxers` / `-demuxers` show the minimum feature set
// (MinimumRequirement()); an executable that is present but insufficient is treated as absent and the missing
// features are reported (LocateResult::rejected), the search goes on with the next candidate.
#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

#include "CEVOffline/Offline.h"

namespace cev
{
	namespace offline
	{
		namespace ffmpeg
		{
			// Names as printed by `ffmpeg -encoders` etc. (muxer / demuxer names are split at ',', so the demuxer
			// "mov,mp4,m4a,3gp,3g2,mj2" yields mov, mp4, m4a, ...).
			struct FfmpegCapabilities
			{
				std::set<std::string> encoders;
				std::set<std::string> decoders;
				std::set<std::string> muxers;
				std::set<std::string> demuxers;
			};

			// A list of required features, each written "<kind>:<name>" in the report (kind: encoder, decoder,
			// muxer, demuxer).
			struct CapabilityRequirement
			{
				std::vector<std::string> encoders;
				std::vector<std::string> decoders;
				std::vector<std::string> muxers;
				std::vector<std::string> demuxers;
			};

			// Needed for every transcode we run: encoders libvpx-vp9 and libopus, muxer webm.
			const CapabilityRequirement& MinimumRequirement();
			// Nice to have (audio side exports done by ffmpeg, not by our own tools): libvorbis, pcm_s16le, ogg, wav.
			const CapabilityRequirement& RecommendedRequirement();
			// Importing arbitrary video: demuxers mov/mp4, matroska, avi, image2; decoders h264, hevc, prores, vp8,
			// vp9, libvpx-vp9, libdav1d, png, aac, mp3, opus, vorbis, flac, pcm_s16le.
			const CapabilityRequirement& ImportRequirement();

			struct CapabilityReport
			{
				bool sufficient = false;                        // MinimumRequirement() met
				std::vector<std::string> missingRequired;       // "encoder:libvpx-vp9", ...
				std::vector<std::string> missingRecommended;
				std::vector<std::string> missingImport;
				bool importComplete() const { return missingImport.empty(); }
			};

			// Pure parsers (testable on canned text).
			std::set<std::string> ParseNameList(const std::string& listOutput, bool splitCommas);
			std::string ParseVersion(const std::string& versionOutput);        // "8.0-full_build-www.gyan.dev"
			std::string ParseConfiguration(const std::string& versionOutput);  // the "configuration:" line, may be empty
			CapabilityReport EvaluateCapabilities(const FfmpegCapabilities& caps);
			std::vector<std::string> MissingFrom(const CapabilityRequirement& req, const FfmpegCapabilities& caps);
			// "missing encoder:libvpx-vp9, muxer:webm" (empty when nothing is missing).
			std::string DescribeMissing(const std::vector<std::string>& missing);

			struct FfmpegInfo
			{
				std::string path;                // UTF-8, the executable
				std::string version;
				std::string configuration;
				bool gpl = false;                // configure line has --enable-gpl / --enable-version3 / --enable-nonfree
				bool nonfree = false;
				FfmpegCapabilities caps;
				CapabilityReport report;
			};

			enum class FfmpegSource { None, Configured, Downloaded, System };
			const char* FfmpegSourceName(FfmpegSource source);

			// Runs `exe args...` and returns its stdout (the default uses RunCapture; tests inject a fake).
			using ProbeExec = std::function<Result(const std::string& exe, const std::vector<std::string>& args, std::string& stdoutText)>;
			ProbeExec DefaultProbeExec(int timeoutMs = 20000);

			// Runs -version / -encoders / -decoders / -muxers / -demuxers and fills `out` (path is set by the caller
			// or here to `exe`). Fails when the file is not ffmpeg or does not run.
			Result ProbeFfmpeg(const std::string& exe, const ProbeExec& exec, FfmpegInfo& out);

			struct LocateOptions
			{
				std::string settingsPath;        // configured by the user, may be empty
				std::string downloadedDir;       // our downloaded copy (folder), may be empty
				bool searchSystemPath = true;
				// Overrides the PATH variable when pathOverrideSet is true (tests).
				bool pathOverrideSet = false;
				std::string pathOverride;
				ProbeExec exec;                  // empty: DefaultProbeExec()
			};

			struct LocateNote
			{
				FfmpegSource source = FfmpegSource::None;
				std::string path;
				std::string reason;              // why the candidate was not used
			};

			struct LocateResult
			{
				bool found = false;
				FfmpegSource source = FfmpegSource::None;
				FfmpegInfo info;                 // valid when found
				std::vector<LocateNote> rejected; // candidates that exist but are unusable / insufficient, in search order
				std::string message;             // one human readable line (found: version + source; else why not)
			};

			LocateResult Locate(const LocateOptions& options);

			// "<file>" -> itself when it exists; "<dir>" -> <dir>/ffmpeg.exe or <dir>/bin/ffmpeg.exe; else empty.
			std::string ResolveExecutable(const std::string& fileOrDir);
			// First ffmpeg(.exe) in the PATH-style list (';' on Windows, ':' elsewhere), empty when none.
			std::string FindOnPath(const std::string& pathList);
		}
	}
}
