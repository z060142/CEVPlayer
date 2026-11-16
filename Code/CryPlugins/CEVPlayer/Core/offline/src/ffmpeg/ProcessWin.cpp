// FfmpegRunner: RunProcess on Windows (CreateProcessW + job object + pipes + reader threads).
#include "CEVOffline/FfmpegRunner.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

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
#if defined(_WIN32)
			namespace detail
			{
				std::wstring Utf8ToWide(const std::string& utf8, bool* ok)
				{
					if (ok != nullptr)
						*ok = true;
					if (utf8.empty())
						return std::wstring();
					const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
					if (n <= 0)
					{
						if (ok != nullptr)
							*ok = false;
						return std::wstring();
					}
					std::wstring w(static_cast<size_t>(n), L'\0');
					MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), &w[0], n);
					return w;
				}

				std::string WideToUtf8(const std::wstring& wide)
				{
					if (wide.empty())
						return std::string();
					const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
					if (n <= 0)
						return std::string();
					std::string s(static_cast<size_t>(n), '\0');
					WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &s[0], n, nullptr, nullptr);
					return s;
				}
			}

			namespace
			{
				using Clock = std::chrono::steady_clock;

				int64_t NowMs()
				{
					return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
				}

				// One pipe, parent end + child end. parentReads: parent holds the read end (child stdout / stderr).
				bool MakePipe(bool parentReads, HANDLE& parentEnd, HANDLE& childEnd)
				{
					SECURITY_ATTRIBUTES sa = {};
					sa.nLength = sizeof(sa);
					sa.bInheritHandle = TRUE;
					HANDLE r = nullptr;
					HANDLE w = nullptr;
					if (!CreatePipe(&r, &w, &sa, 64 * 1024))
						return false;
					parentEnd = parentReads ? r : w;
					childEnd = parentReads ? w : r;
					SetHandleInformation(parentEnd, HANDLE_FLAG_INHERIT, 0);
					return true;
				}

				void CloseIf(HANDLE& h)
				{
					if (h != nullptr && h != INVALID_HANDLE_VALUE)
						CloseHandle(h);
					h = nullptr;
				}

				// Drains one pipe on its own thread. Never calls user code.
				class PipeReader
				{
				public:
					PipeReader(HANDLE h, size_t cap, std::atomic<int64_t>* activity, ProgressParser* parser)
						: m_handle(h), m_capture(cap), m_activity(activity), m_parser(parser)
					{
						m_thread = std::thread([this] { Loop(); });
					}
					~PipeReader() { Join(); }
					PipeReader(const PipeReader&) = delete;
					PipeReader& operator=(const PipeReader&) = delete;

					bool Done() const { return m_done.load(std::memory_order_acquire); }

					// Waits up to waitMs for EOF, then cancels the blocking read and joins.
					void Join(int waitMs = 0)
					{
						if (!m_thread.joinable())
							return;
						const int64_t end = NowMs() + waitMs;
						while (!Done() && NowMs() < end)
							Sleep(5);
						if (!Done())
							CancelSynchronousIo(m_thread.native_handle());
						m_thread.join();
					}

					std::string TakeText()
					{
						std::lock_guard<std::mutex> lock(m_mutex);
						return m_capture.Take();
					}

				private:
					void Loop()
					{
						char buf[16384];
						for (;;)
						{
							DWORD got = 0;
							if (!ReadFile(m_handle, buf, sizeof(buf), &got, nullptr) || got == 0)
								break;
							m_activity->store(NowMs(), std::memory_order_relaxed);
							if (m_parser != nullptr)
							{
								m_parser->Feed(buf, got);
							}
							else
							{
								std::lock_guard<std::mutex> lock(m_mutex);
								m_capture.Append(buf, got);
							}
						}
						m_done.store(true, std::memory_order_release);
					}

					HANDLE m_handle;
					std::mutex m_mutex;
					detail::TextCapture m_capture;
					std::atomic<int64_t>* m_activity;
					ProgressParser* m_parser;
					std::atomic<bool> m_done{false};
					std::thread m_thread;
				};

				struct ProgressState
				{
					std::atomic<double> outTime{-1.0};
					std::atomic<double> speed{0.0};
					std::atomic<bool> ended{false};
				};

				void KillTree(HANDLE job, HANDLE process)
				{
					if (job != nullptr)
						TerminateJobObject(job, 1);
					else if (process != nullptr)
						TerminateProcess(process, 1);
				}
			}

			RunResult RunProcess(const RunOptions& opt)
			{
				using detail::Res;
				RunResult out;
				const auto t0 = Clock::now();
				auto finish = [&](Status status, const std::string& message) -> RunResult
				{
					out.result = Res(status, message);
					out.elapsedSeconds = std::chrono::duration<double>(Clock::now() - t0).count();
					return out;
				};

				if (opt.exe.empty())
					return finish(Status::InvalidArgument, "no executable given");
				bool okExe = true;
				bool okDir = true;
				const std::wstring wexe = detail::Utf8ToWide(opt.exe, &okExe);
				const std::wstring wdir = detail::Utf8ToWide(opt.workingDir, &okDir);
				std::wstring wcmd = detail::Utf8ToWide(BuildWindowsCommandLine(opt.exe, opt.args), &okExe);
				if (!okExe || !okDir)
					return finish(Status::InvalidArgument, "executable, arguments or working directory are not valid UTF-8");
				if (wcmd.size() > 32000)
					return finish(Status::InvalidArgument, "command line is too long");
				wcmd.push_back(L'\0');

				// ---- pipes
				HANDLE inParent = nullptr, inChild = nullptr;
				HANDLE outParent = nullptr, outChild = nullptr;
				HANDLE errParent = nullptr, errChild = nullptr;
				if (!MakePipe(false, inParent, inChild) || !MakePipe(true, outParent, outChild) || !MakePipe(true, errParent, errChild))
				{
					CloseIf(inParent); CloseIf(inChild); CloseIf(outParent); CloseIf(outChild); CloseIf(errParent); CloseIf(errChild);
					return finish(Status::IoError, "CreatePipe failed");
				}

				// ---- job (kills the process tree; falls back to TerminateProcess when no job can be used)
				HANDLE job = CreateJobObjectW(nullptr, nullptr);
				if (job != nullptr)
				{
					JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
					info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
					if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)))
						CloseIf(job);
				}

				// ---- start: only the three pipe ends are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST)
				STARTUPINFOEXW si = {};
				si.StartupInfo.cb = sizeof(si);
				si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
				si.StartupInfo.hStdInput = inChild;
				si.StartupInfo.hStdOutput = outChild;
				si.StartupInfo.hStdError = errChild;
				HANDLE inherit[3] = {inChild, outChild, errChild};
				SIZE_T attrSize = 0;
				InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
				std::unique_ptr<char[]> attrBuf(new (std::nothrow) char[attrSize]);
				bool attrOk = false;
				DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
				if (attrBuf != nullptr)
				{
					si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.get());
					if (InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize))
					{
						attrOk = UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr) != FALSE;
						if (!attrOk)
							DeleteProcThreadAttributeList(si.lpAttributeList);
					}
				}
				if (attrOk)
					flags |= EXTENDED_STARTUPINFO_PRESENT;
				else
					si.StartupInfo.cb = sizeof(STARTUPINFOW);

				PROCESS_INFORMATION pi = {};
				const BOOL created = CreateProcessW(
					opt.exe.find_first_of("\\/") != std::string::npos ? wexe.c_str() : nullptr,
					&wcmd[0], nullptr, nullptr, TRUE, flags, nullptr, wdir.empty() ? nullptr : wdir.c_str(),
					reinterpret_cast<LPSTARTUPINFOW>(&si), &pi);
				const DWORD createError = created ? 0 : GetLastError();
				if (attrOk)
					DeleteProcThreadAttributeList(si.lpAttributeList);
				CloseIf(inChild);
				CloseIf(outChild);
				CloseIf(errChild);
				if (!created)
				{
					CloseIf(inParent); CloseIf(outParent); CloseIf(errParent); CloseIf(job);
					return finish(Status::IoError, "cannot start " + opt.exe + " (CreateProcess error " + std::to_string(createError) + ")");
				}
				out.started = true;
				if (job != nullptr && !AssignProcessToJobObject(job, pi.hProcess))
					CloseIf(job); // already in a job that forbids nesting: plain TerminateProcess then
				ResumeThread(pi.hThread);
				CloseIf(pi.hThread);

				// ---- readers
				std::atomic<int64_t> activity{NowMs()};
				ProgressState ps;
				std::unique_ptr<ProgressParser> parser;
				if (opt.parseProgress)
				{
					parser.reset(new ProgressParser([&ps](const ProgressBlock& b)
					{
						if (b.hasTime)
							ps.outTime.store(b.outTimeSeconds);
						ps.speed.store(b.speed);
						if (b.ended)
							ps.ended.store(true);
					}));
				}
				PipeReader outReader(outParent, opt.maxStdoutBytes, &activity, parser.get());
				PipeReader errReader(errParent, opt.maxStderrBytes, &activity, nullptr);

				// ---- supervise
				bool cancelRequested = false;
				int64_t cancelAt = 0;
				double lastFraction = -1.0;
				auto reportProgress = [&](double f)
				{
					if (!opt.progress || f <= lastFraction)
						return;
					lastFraction = f;
					opt.progress(f);
				};
				const int64_t startMs = NowMs();
				for (;;)
				{
					if (WaitForSingleObject(pi.hProcess, 10) == WAIT_OBJECT_0)
						break;
					const int64_t now = NowMs();
					const double t = ps.outTime.load();
					if (t >= 0.0)
						reportProgress(ProgressFraction(t, opt.durationSeconds));

					if (opt.cancel != nullptr && opt.cancel->load(std::memory_order_relaxed) && !cancelRequested)
					{
						cancelRequested = true;
						cancelAt = now;
						out.cancelled = true;
						DWORD written = 0;
						WriteFile(inParent, "q", 1, &written, nullptr); // ffmpeg: quit gracefully
					}
					if (cancelRequested && now - cancelAt >= opt.cancelGraceMs)
					{
						out.killed = true;
						KillTree(job, pi.hProcess);
						break;
					}
					if (!cancelRequested)
					{
						if (opt.totalTimeoutMs > 0 && now - startMs >= opt.totalTimeoutMs)
						{
							out.timedOut = true;
							out.killed = true;
							KillTree(job, pi.hProcess);
							break;
						}
						if (opt.noProgressTimeoutMs > 0 && now - activity.load(std::memory_order_relaxed) >= opt.noProgressTimeoutMs)
						{
							out.timedOut = true;
							out.killed = true;
							KillTree(job, pi.hProcess);
							break;
						}
					}
				}
				WaitForSingleObject(pi.hProcess, 5000); // a killed process is gone almost immediately
				DWORD code = 1;
				if (!GetExitCodeProcess(pi.hProcess, &code))
					code = 1;
				out.exitCode = static_cast<int>(code);

				// Anything the process left behind dies with the job; that also closes the pipes for the readers.
				if (job != nullptr)
				{
					TerminateJobObject(job, 1);
					CloseIf(job);
				}
				CloseIf(inParent);
				outReader.Join(2000);
				errReader.Join(2000);
				CloseIf(outParent);
				CloseIf(errParent);
				CloseIf(pi.hProcess);

				out.stdoutText = opt.parseProgress ? std::string() : outReader.TakeText();
				out.stderrLog = errReader.TakeText();
				out.lastOutTimeSeconds = ps.outTime.load();
				out.lastSpeed = ps.speed.load();

				if (out.cancelled)
					return finish(Status::Cancelled, "cancelled");
				if (out.timedOut)
					return finish(Status::IoError, opt.totalTimeoutMs > 0 && NowMs() - startMs >= opt.totalTimeoutMs
						? "the process exceeded the time limit and was terminated"
						: "the process produced no output for too long and was terminated (hung)");
				if (out.exitCode != 0)
				{
					std::string lastLine;
					const std::vector<std::string> lines = detail::SplitLines(out.stderrLog);
					for (size_t i = lines.size(); i-- > 0;)
					{
						lastLine = detail::Trim(lines[i]);
						if (!lastLine.empty())
							break;
					}
					return finish(Status::InvalidData, "the process exited with code " + std::to_string(out.exitCode) + (lastLine.empty() ? std::string() : ": " + lastLine));
				}
				reportProgress(1.0);
				return finish(Status::Ok, std::string());
			}

			std::string CurrentExecutablePath()
			{
				wchar_t buf[MAX_PATH * 4] = {};
				const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
				if (n == 0 || n >= sizeof(buf) / sizeof(buf[0]))
					return std::string();
				return detail::WideToUtf8(std::wstring(buf, n));
			}
#else
			RunResult RunProcess(const RunOptions&)
			{
				RunResult r;
				r.result = detail::Res(Status::Unsupported, "FfmpegRunner is only implemented for Windows");
				return r;
			}

			std::string CurrentExecutablePath() { return std::string(); }
#endif

			RunResult RunCapture(const std::string& exe, const std::vector<std::string>& args, int timeoutMs)
			{
				RunOptions o;
				o.exe = exe;
				o.args = args;
				o.noProgressTimeoutMs = timeoutMs;
				o.totalTimeoutMs = timeoutMs;
				return RunProcess(o);
			}
		}
	}
}
