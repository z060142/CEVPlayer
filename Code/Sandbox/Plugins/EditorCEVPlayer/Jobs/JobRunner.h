// Small job system for the long operations (split audio, WAV, Ogg, source verification, ...): one std::thread per job,
// independent of the engine's single background queue (docs/research/04 §5). Jobs only do plain C++ (Core / file I/O /
// IOfflineOps); their completion callback runs on the main thread (a Qt queued call into this object). The runner is
// owned by the plug-in and joins its threads on destruction (cancelling them first).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
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
		//! Callable from any thread. Returns the job id.
		int Submit(const QString& title, TWork work, TDone onDone = TDone());
		void Cancel(int id);
		void CancelAll();

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
			bool finished = false;
		};

		void Finish(int id, const SOpResult& result, const TDone& onDone);

		mutable std::mutex m_mutex;
		std::vector<std::unique_ptr<SJob>> m_jobs;
		int m_nextId = 1;
		std::atomic<bool> m_shuttingDown{ false };
	};
}
