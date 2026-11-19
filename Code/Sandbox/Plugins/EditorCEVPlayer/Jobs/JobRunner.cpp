#include "StdAfx.h"
#include "Jobs/JobRunner.h"

#include <algorithm>
#include <chrono>

#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <IEditor.h>
#include <Notifications/NotificationCenter.h>

namespace CEVEditor
{
	namespace
	{
		CJobRunner* g_pRunner = nullptr;
	}

	CJobRunner* CJobRunner::Get()
	{
		return g_pRunner;
	}

	CJobRunner::CJobRunner()
	{
		g_pRunner = this;
	}

	CJobRunner::~CJobRunner()
	{
		g_pRunner = nullptr;
		m_shuttingDown = true;
		std::vector<std::unique_ptr<SJob>> jobs;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			for (auto& job : m_jobs)
				job->cancel = true;
			jobs.swap(m_jobs);
		}
		// every thread is joined here, none outlives the plug-in (the work functions poll the cancel flag)
		for (auto& job : jobs)
		{
			if (job->thread.joinable())
				job->thread.join();
		}
		// the notifications belong to the editor's notification center: only touch them while it is there
		if (GetIEditor() == nullptr || GetIEditor()->GetNotificationCenter() == nullptr)
		{
			for (auto& n : m_notifications)
				n.second.release();
		}
		m_notifications.clear();
	}

	void CJobRunner::UpdateNotification(int id)
	{
		auto it = m_notifications.find(id);
		if (it == m_notifications.end())
			return;
		float progress = 0.f;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			for (auto& job : m_jobs)
			{
				if (job->info.id == id)
					progress = job->progress;
			}
		}
		it->second->SetProgress(std::min(progress, 0.99f)); // 1.0 marks it complete: only Finish does that
	}

	int CJobRunner::Submit(const QString& title, TWork work, TDone onDone, const std::string& owner, bool notify)
	{
		int id = 0;
		SJob* pJob = nullptr;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			// forget finished jobs of the last time the list was long (keeps the panel's list short)
			while (m_jobs.size() > 40)
			{
				auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [](const std::unique_ptr<SJob>& j) { return j->finished; });
				if (it == m_jobs.end())
					break;
				if ((*it)->thread.joinable())
					(*it)->thread.join();
				m_jobs.erase(it);
			}
			auto job = std::make_unique<SJob>();
			id = m_nextId++;
			job->info.id = id;
			job->info.title = title;
			job->owner = owner;
			pJob = job.get();
			m_jobs.push_back(std::move(job));
		}

		pJob->thread = std::thread([this, id, pJob, work = std::move(work), onDone = std::move(onDone)]()
		{
			SOpContext ctx;
			ctx.cancel = &pJob->cancel;
			ctx.progress = [this, id, pJob](float p)
			{
				pJob->progress = p;
				if (m_shuttingDown)
					return;
				// The offline library reports progress for every block it processes (tens of thousands of calls for a long
				// clip): one queued notification per job at a time, or the main thread drowns in them.
				const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
				if (nowMs - pJob->lastNotifyMs.load() < 100) // at most ten updates per second per job
					return;
				if (pJob->changePending.exchange(true))
					return;
				pJob->lastNotifyMs = nowMs;
				QMetaObject::invokeMethod(this, [this, id]()
				{
					{
						std::lock_guard<std::mutex> lock(m_mutex);
						for (auto& job : m_jobs)
						{
							if (job->info.id == id)
								job->changePending = false;
						}
					}
					UpdateNotification(id);
					Q_EMIT JobChanged(id);
				}, Qt::QueuedConnection);
			};
			ctx.log = [this, id](const std::string& line)
			{
				if (m_shuttingDown)
					return;
				const QString text = QString::fromUtf8(line.c_str());
				QMetaObject::invokeMethod(this, [this, id, text]() { Q_EMIT JobLog(id, text); }, Qt::QueuedConnection);
			};
			SOpResult result;
			try
			{
				if (!m_shuttingDown)
				{
					result = work(ctx);
				}
				else
				{
					result.status = EOpStatus::Cancelled;
					result.message = "cancelled (shutting down)";
				}
			}
			catch (const std::exception& e)
			{
				result.status = EOpStatus::Failed;
				result.message = std::string("exception: ") + e.what();
			}
			catch (...)
			{
				result.status = EOpStatus::Failed;
				result.message = "unknown exception";
			}
			pJob->threadDone = true; // from here on the job holds no file any more
			if (m_shuttingDown)
				return;
			QMetaObject::invokeMethod(this, [this, id, result, onDone]() { Finish(id, result, onDone); }, Qt::QueuedConnection);
		});
		if (notify && GetIEditor() != nullptr && GetIEditor()->GetNotificationCenter() != nullptr)
			m_notifications[id] = std::make_unique<CProgressNotification>(title, tr("running"), true);
		// queued: Submit may run inside a signal handler of a panel that listens to JobChanged
		QMetaObject::invokeMethod(this, [this, id]() { Q_EMIT JobChanged(id); }, Qt::QueuedConnection);
		return id;
	}

	void CJobRunner::Finish(int id, const SOpResult& result, const TDone& onDone)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			for (auto& job : m_jobs)
			{
				if (job->info.id != id)
					continue;
				job->finished = true;
				job->progress = 1.f;
				job->info.message = QString::fromUtf8(result.message.c_str());
				job->info.state = result.status == EOpStatus::Ok ? EState::Succeeded
					: result.status == EOpStatus::Cancelled ? EState::Cancelled : EState::Failed;
			}
		}
		if (!result.message.empty())
			Q_EMIT JobLog(id, QString::fromUtf8(result.message.c_str()));
		auto it = m_notifications.find(id);
		if (it != m_notifications.end())
		{
			// the first line of the result: the full text stays in the job log of the info panel
			QString text = QString::fromUtf8(result.message.c_str()).section('\n', 0, 0);
			const QString state = result.status == EOpStatus::Ok ? tr("done") : result.status == EOpStatus::Cancelled ? tr("cancelled") : tr("FAILED");
			it->second->SetMessage(text.isEmpty() ? state : state + ": " + text);
			m_notifications.erase(it); // the destructor sets the progress to 1 (complete)
		}
		Q_EMIT JobChanged(id);
		if (onDone)
			onDone(result);
	}

	bool CJobRunner::HasRunningFor(const std::string& owner) const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (const auto& job : m_jobs)
		{
			if (job->owner == owner && !job->threadDone)
				return true;
		}
		return false;
	}

	bool CJobRunner::CancelAndWait(const std::string& owner, int timeoutMs)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			for (auto& job : m_jobs)
			{
				if (job->owner == owner)
					job->cancel = true;
			}
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (HasRunningFor(owner))
		{
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		// The completions of the finished jobs are queued calls: run them now, so that their results (files they made,
		// state of the asset) are applied before the caller renames / moves / deletes the files.
		if (QThread::currentThread() == QCoreApplication::instance()->thread())
			QCoreApplication::sendPostedEvents(this, 0);
		return true;
	}

	bool CJobRunner::WaitIdle(int timeoutMs) const
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		for (;;)
		{
			bool busy = false;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				for (const auto& job : m_jobs)
					busy = busy || !job->threadDone;
			}
			if (!busy)
				return true;
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}

	void CJobRunner::Cancel(int id)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (auto& job : m_jobs)
		{
			if (job->info.id == id)
				job->cancel = true;
		}
	}

	void CJobRunner::CancelAll()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (auto& job : m_jobs)
			job->cancel = true;
	}

	std::vector<CJobRunner::SJobInfo> CJobRunner::Jobs() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::vector<SJobInfo> out;
		for (const auto& job : m_jobs)
		{
			SJobInfo info = job->info;
			info.progress = job->progress;
			out.push_back(info);
		}
		return out;
	}

	int CJobRunner::RunningCount() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		int n = 0;
		for (const auto& job : m_jobs)
			n += job->finished ? 0 : 1;
		return n;
	}
}
