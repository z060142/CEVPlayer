#include "StdAfx.h"
#include "Asset/ImportQueue.h"

#include <memory>

#include <QPointer>
#include <QTimer>

#include <CrySystem/ISystem.h>
#include <IEditor.h>
#include <Notifications/NotificationCenter.h>

#include "FFmpeg/FfmpegService.h"
#include "Jobs/JobRunner.h"

namespace CEVEditor
{
	namespace
	{
		CImportQueue* g_pQueue = nullptr;

		void Notify(bool warning, const QString& title, const QString& text)
		{
			if (GetIEditor() == nullptr || GetIEditor()->GetNotificationCenter() == nullptr)
				return;
			if (warning)
				GetIEditor()->GetNotificationCenter()->ShowWarning(title, text);
			else
				GetIEditor()->GetNotificationCenter()->ShowInfo(title, text);
		}

		// The new CAsset of an import is owned by the result until FinishImport merged it; a job whose completion never runs
		// (shutdown) must not leak it.
		std::shared_ptr<SImportResult> MakeResult()
		{
			return std::shared_ptr<SImportResult>(new SImportResult(), [](SImportResult* p)
			{
				delete p->pAsset;
				delete p;
			});
		}
	}

	CImportQueue* CImportQueue::Get()
	{
		return g_pQueue;
	}

	CImportQueue::CImportQueue()
	{
		g_pQueue = this;
	}

	CImportQueue::~CImportQueue()
	{
		g_pQueue = nullptr;
	}

	int CImportQueue::BeginBatch(const QString& title)
	{
		const int id = m_nextBatch++;
		m_batches[id].title = title;
		return id;
	}

	void CImportQueue::Enqueue(int batch, const std::string& source, const std::string& folder, const SImportOptions& options)
	{
		if (m_stopped)
			return;
		auto it = m_batches.find(batch);
		if (it == m_batches.end())
			return;
		++it->second.total;
		SEntry e;
		e.batch = batch;
		e.source = source;
		e.folder = folder;
		e.options = options;
		m_entries.push_back(e);
		Q_EMIT Changed();
		Pump();
	}

	void CImportQueue::EndBatch(int batch)
	{
		auto it = m_batches.find(batch);
		if (it == m_batches.end())
			return;
		it->second.closed = true;
		MaybeSummarize(batch);
	}

	void CImportQueue::CancelAll(bool permanent)
	{
		if (permanent)
			m_stopped = true;
		std::deque<SEntry> dropped;
		dropped.swap(m_entries);
		if (m_running && m_runningJob != 0 && CJobRunner::Get() != nullptr)
			CJobRunner::Get()->Cancel(m_runningJob);
		for (const SEntry& e : dropped)
			Done(e, EEnd::Cancelled, "cancelled before it started", false);
		Q_EMIT Changed();
	}

	void CImportQueue::Pump()
	{
		if (m_running || m_entries.empty() || m_stopped)
			return;
		const SEntry entry = m_entries.front();
		m_entries.pop_front();
		m_running = true;
		Start(entry);
	}

	void CImportQueue::Start(const SEntry& entry)
	{
		CJobRunner* const pRunner = CJobRunner::Get();
		if (pRunner == nullptr)
		{
			Done(entry, EEnd::Failed, "the job runner is not available");
			return;
		}
		const SImportPlan plan = PlanImport(entry.source, entry.folder, entry.options);
		if (!plan.ok)
		{
			Done(entry, EEnd::Failed, plan.error);
			return;
		}
		if (plan.exists && !entry.options.overwrite)
		{
			Done(entry, EEnd::Failed, "skipped, " + plan.detail);
			return;
		}
		if (!plan.note.empty())
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] %s: %s", entry.source.c_str(), plan.note.c_str());
		if (entry.options.NeedsFfmpeg(plan.kind))
		{
			CFfmpegService* const pService = CFfmpegService::Get();
			if (pService == nullptr)
			{
				Done(entry, EEnd::Failed, "the FFmpeg service is not available");
				return;
			}
			const SFfmpegStatus status = pService->Status();
			if (!status.Pending() && !status.Ready())
			{
				Done(entry, EEnd::Failed, pService->UnavailableMessage());
				return;
			}
			if (status.state == SFfmpegStatus::EState::Unknown)
				pService->EnsureDetected();
		}
		std::string prepareError;
		if (plan.exists && !PrepareOverwrite(plan, prepareError))
		{
			Done(entry, EEnd::Failed, prepareError);
			return;
		}
		auto result = MakeResult();
		const std::string source = entry.source;
		const SImportOptions options = entry.options;
		const QString title = QObject::tr("Import %1").arg(QString::fromUtf8(plan.assetName.c_str()));
		m_runningJob = pRunner->Submit(title, [source, plan, options, result](const SOpContext& ctx)
		{
			*result = ImportVideoFile(source, plan, options, ctx.cancel, &ctx);
			SOpResult r;
			r.status = result->ok ? EOpStatus::Ok : (ctx.Cancelled() ? EOpStatus::Cancelled : EOpStatus::Failed);
			r.message = result->error;
			return r;
		}, [this, entry, result](const SOpResult& r)
		{
			m_runningJob = 0;
			if (r.Ok())
			{
				FinishImport(*result, entry.options); // main thread: merges the asset, starts the audio operations
				SImportResult merged = *result;
				result->pAsset = nullptr; // the asset manager owns it now
				Done(entry, EEnd::Ok, merged.metadataRel);
			}
			else
			{
				delete result->pAsset; // never merged
				result->pAsset = nullptr;
				Done(entry, r.status == EOpStatus::Cancelled ? EEnd::Cancelled : EEnd::Failed, r.message);
			}
		}, plan.guid.IsNull() ? std::string() : AssetOwnerKey(plan.guid));
	}

	void CImportQueue::Done(const SEntry& entry, EEnd end, const std::string& text, bool wasRunning)
	{
		if (wasRunning)
			m_running = false;
		SBatch& b = m_batches[entry.batch];
		++b.finished;
		std::string line;
		switch (end)
		{
		case EEnd::Ok:
			++b.ok;
			line = "ok       " + entry.source + " -> " + text;
			CryLogAlways("[CEVEditor] import ok: %s -> %s", entry.source.c_str(), text.c_str());
			break;
		case EEnd::Cancelled:
			++b.cancelled;
			line = "cancelled " + entry.source;
			CryLogAlways("[CEVEditor] import cancelled: %s", entry.source.c_str());
			break;
		case EEnd::Failed:
			++b.failed;
			line = "FAILED   " + entry.source + ": " + text;
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] import failed: %s: %s", entry.source.c_str(), text.c_str());
			break;
		}
		b.lines.push_back(line);
		MaybeSummarize(entry.batch);
		Q_EMIT Changed();
		// the next file starts from the event loop: Done may be called from inside Start (a refused file) or a completion
		if (!m_pumpQueued && !m_entries.empty() && !m_stopped)
		{
			m_pumpQueued = true;
			QTimer::singleShot(0, this, [this]()
			{
				m_pumpQueued = false;
				Pump();
			});
		}
	}

	void CImportQueue::MaybeSummarize(int batch)
	{
		auto it = m_batches.find(batch);
		if (it == m_batches.end())
			return;
		const SBatch& b = it->second;
		if (!b.closed || b.finished < b.total)
			return;
		if (b.total != 1) // a single file already logged its own line
		{
			std::string text = "batch '" + std::string(b.title.toUtf8().constData()) + "' finished: " + std::to_string(b.total) + " file(s): ok " + std::to_string(b.ok) +
				", failed " + std::to_string(b.failed) + ", cancelled " + std::to_string(b.cancelled);
			for (const std::string& l : b.lines)
				text += "\n  " + l;
			CryLogAlways("[CEVEditor] %s", text.c_str());
			Notify(b.failed > 0, QObject::tr("Video import"), QObject::tr("%1: %2 of %3 file(s) imported, %4 failed, %5 cancelled").arg(b.title).arg(b.ok).arg(b.total).arg(b.failed).arg(b.cancelled));
		}
		m_batches.erase(it);
	}
}
