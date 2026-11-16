#include "StdAfx.h"
#include "Jobs/JobRunner.h"

#include <QMetaObject>
#include <QPointer>
#include <QTimer>

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
		for (auto& job : jobs)
		{
			if (job->thread.joinable())
				job->thread.join();
		}
	}

	int CJobRunner::Submit(const QString& title, TWork work, TDone onDone)
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
			pJob = job.get();
			m_jobs.push_back(std::move(job));
		}

		QPointer<CJobRunner> self(this);
		pJob->thread = std::thread([self, this, id, pJob, work = std::move(work), onDone = std::move(onDone)]()
		{
			SOpContext ctx;
			ctx.cancel = &pJob->cancel;
			ctx.progress = [self, this, id, pJob](float p)
			{
				pJob->progress = p;
				if (m_shuttingDown)
					return;
				QMetaObject::invokeMethod(this, [this, id]() { Q_EMIT JobChanged(id); }, Qt::QueuedConnection);
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
				result = work(ctx);
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
			if (m_shuttingDown)
				return;
			QMetaObject::invokeMethod(this, [this, id, result, onDone]() { Finish(id, result, onDone); }, Qt::QueuedConnection);
		});
		Q_EMIT JobChanged(id);
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
		Q_EMIT JobChanged(id);
		if (onDone)
			onDone(result);
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
