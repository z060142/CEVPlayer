// Small job system for the long operations (split audio, WAV, Ogg, source verification, ...): one std::thread per job,
// independent of the engine's single background queue (docs/research/04 §5). Jobs only do plain C++ (Core / file I/O /
// IOfflineOps); their completion callback runs on the main thread (a Qt queued call into this object). The runner is
// owned by the plug-in and joins its threads on destruction (cancelling them first); no thread is ever detached.
//
// Thread rules: Submit / Cancel* / CancelAndWait are main thread calls. A job function must not touch Qt, the engine
// (gEnv) or the asset manager; it reports through the SOpContext (progress / log are marshalled to the main thread).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <QObject>
#include <QString>

#include "Offline/IOfflineOps.h"

namespace CEVEditor
{
	class CJobRunner : public QObject
	{
		Q_OBJECT
	public:
		enum class EState { Running, Succeeded, Failed, Cancelled };

		struct SJobInfo
		{
			int id = 0;
			QString title;
			EState state = EState::Running;
			float progress = 0.f;
			QString message;   //!< result text
		};

		//! Created by the plug-in; null before / after its lifetime.
		static CJobRunner* Get();

		CJobRunner();
		~CJobRunner() override;

		using TWork = std::function<SOpResult(const SOpContext&)>;
		using TDone = std::function<void(const SOpResult&)>;

		//! Starts `work` on a new thread. `onDone` (optional) runs on the main thread once the job has finished.
		//! `owner` (optional, e.g. the GUID of an asset) lets the asset operations that move or delete files wait for
		//! the jobs that have those files open (CancelAndWait). Returns the job id.
		int Submit(const QString& title, TWork work, TDone onDone = TDone(), const std::string& owner = std::string());
		void Cancel(int id);
		void CancelAll();
		//! True while a job of `owner` has not finished its work function (its files may still be open).
		bool HasRunningFor(const std::string& owner) const;
		//! Cancels the jobs of `owner` and waits (at most `timeoutMs`) until their work functions have returned.
		//! Returns false when one of them is still running afterwards.
		bool CancelAndWait(const std::string& owner, int timeoutMs);
		//! Waits (at most `timeoutMs`) until every job's work function has returned (no file is open, no child process runs).
		//! Used at quit after CancelAll: the process may end right after the quit notification, so the cancelled jobs must have
		//! removed their partial files by then. Main thread.
		bool WaitIdle(int timeoutMs) const;

		std::vector<SJobInfo> Jobs() const;
		int RunningCount() const;

	signals:
		void JobChanged(int id);                      // state or progress changed (main thread)
		void JobLog(int id, const QString& line);     // main thread

	private:
		struct SJob
		{
			SJobInfo info;
			std::atomic<bool> cancel{ false };
			std::atomic<float> progress{ 0.f };
			std::thread thread;
			std::string owner;
			std::atomic<bool> threadDone{ false };   //!< the work function returned (no file is open any more)
			std::atomic<bool> changePending{ false };//!< a JobChanged notification for this job is queued already
			std::atomic<long long> lastNotifyMs{ 0 };//!< steady clock time of the last queued notification
			bool finished = false;                   //!< the completion ran on the main thread
		};

		void Finish(int id, const SOpResult& result, const TDone& onDone);

		mutable std::mutex m_mutex;
		std::vector<std::unique_ptr<SJob>> m_jobs;
		int m_nextId = 1;
		std::atomic<bool> m_shuttingDown{ false };
	};
}
