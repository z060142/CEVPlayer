// CEVPlayerFFmpeg - runs the external ffmpeg executable (CRYENGINE independent, editor side only, never linked
// by the runtime plugin; libav* is never linked, ffmpeg is always a separate process).
//
// FfmpegRunner: spawn a process (Windows: CreateProcessW inside a kill-on-close job object, no console window,
// stdin / stdout / stderr pipes, UTF-8 arguments with CommandLineToArgvW compatible quoting), read the
// `-progress pipe:1` key=value stream, enforce timeouts, cancel (send 'q', then kill the whole process tree).
//
// Rules (same as Offline.h): nothing throws, failures are Result{status, message}. The Control progress callback
// and the cancel flag are used on the calling thread only (reader threads never call user code).
// Windows only: the parsing helpers (quoting, progress parser) are portable, RunProcess() reports Unsupported
// elsewhere.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "CEVOffline/Offline.h"

namespace cev
{
	namespace offline
	{
		namespace ffmpeg
		{
			// ---- command line ------------------------------------------------------------------------------------
			// One argument quoted so that CommandLineToArgvW / the MSVC and MinGW CRT parse it back unchanged
			// (backslashes before a quote are doubled, an embedded quote becomes \", trailing backslashes are doubled).
			std::string QuoteWindowsArg(const std::string& utf8Arg);
			// "<exe>" arg1 arg2 ... (UTF-8). The executable is always quoted.
			std::string BuildWindowsCommandLine(const std::string& exe, const std::vector<std::string>& args);

			// ---- `-progress` stream ------------------------------------------------------------------------------
			// One block of the key=value stream (ends with a `progress=continue|end` line).
			struct ProgressBlock
			{
				bool hasTime = false;            // out_time_us / out_time_ms carried a usable (non-negative) time
				double outTimeSeconds = 0.0;
				int64_t frame = -1;
				double fps = 0.0;
				double speed = 0.0;              // "1.5x" -> 1.5, 0 when N/A
				int64_t totalSize = -1;
				bool ended = false;              // progress=end
			};

			// Incremental parser; feed it arbitrary chunks. A block is delivered when its progress= line arrives.
			class ProgressParser
			{
			public:
				using BlockCallback = std::function<void(const ProgressBlock&)>;
				explicit ProgressParser(BlockCallback cb) : m_cb(std::move(cb)) {}
				void Feed(const char* data, size_t size);
				void Feed(const std::string& s) { Feed(s.data(), s.size()); }

			private:
				void Line(const std::string& line);
				BlockCallback m_cb;
				std::string m_line;
				ProgressBlock m_block;
			};

			// outTime / duration clamped to [0, 1); 0 when the duration is unknown (<= 0). 1.0 is reserved for the end.
			double ProgressFraction(double outTimeSeconds, double durationSeconds);

			// ---- process -----------------------------------------------------------------------------------------
			struct RunOptions
			{
				std::string exe;                       // UTF-8 path of the executable
				std::vector<std::string> args;         // UTF-8, without the executable
				std::string workingDir;                // empty: inherit
				// Progress: when parseProgress is set stdout is parsed as a `-progress pipe:1` stream (and not kept in
				// RunResult::stdoutText); the fraction is outTime / durationSeconds (0 duration: no fractions).
				bool parseProgress = false;
				double durationSeconds = 0.0;
				const std::atomic<bool>* cancel = nullptr;
				std::function<void(double)> progress;  // monotonic 0..1, calling thread
				// Hang detection: no byte on stdout / stderr for this long -> kill (0 = disabled).
				int noProgressTimeoutMs = 60000;
				// Wall clock limit for the whole run (0 = none).
				int totalTimeoutMs = 0;
				// After a cancel request: write 'q' to stdin, kill the process tree when it is still alive after this.
				int cancelGraceMs = 500;
				// Caps for the captured text (the middle is dropped, a marker is inserted).
				size_t maxStdoutBytes = 4u << 20;
				size_t maxStderrBytes = 4u << 20;
			};

			struct RunResult
			{
				Result result;                         // Ok only when the process exited with code 0
				bool started = false;
				int exitCode = -1;
				bool cancelled = false;
				bool timedOut = false;
				bool killed = false;                   // the runner had to terminate the process
				double lastOutTimeSeconds = -1.0;      // last progress block that carried a time
				double lastSpeed = 0.0;
				std::string stdoutText;                // empty with parseProgress
				std::string stderrLog;                 // full stderr (capped by maxStderrBytes)
				double elapsedSeconds = 0.0;
			};

			// Runs the process to completion; never blocks forever (see the timeouts). The returned status:
			// Ok (exit code 0) / Cancelled / IoError (cannot start, timeout) / InvalidData (non-zero exit code).
			RunResult RunProcess(const RunOptions& options);

			// Convenience: run a short query (no progress), stdout and stderr captured.
			RunResult RunCapture(const std::string& exe, const std::vector<std::string>& args, int timeoutMs = 20000);

			// Full path of the running executable (UTF-8), empty when unknown. Used by the selftest to spawn itself.
			std::string CurrentExecutablePath();
		}
	}
}
