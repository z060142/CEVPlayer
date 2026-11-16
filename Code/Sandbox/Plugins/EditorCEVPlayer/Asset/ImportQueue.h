// The import queue: every import that needs FFmpeg (and every import of the editor window / batch commands) runs through it,
// one file at a time (one ffmpeg process at a time), each on a CJobRunner job. Main thread only; the asset manager is
// touched on the main thread (PlanImport before, FinishImport after the job), the job does file work and ffmpeg.
#pragma once

#include <deque>
#include <map>
#include <string>
#include <vector>

#include <QObject>
#include <QString>

#include "Asset/VideoImport.h"

namespace CEVEditor
{
	class CImportQueue : public QObject
	{
		Q_OBJECT
	public:
		static CImportQueue* Get();   //!< null before / after the plug-in's lifetime

		CImportQueue();
		~CImportQueue() override;

		//! A group of files whose results are summed up (one log block / notification) when the last one is done.
		int BeginBatch(const QString& title);
		//! Queues a file (or a folder of PNG files). The plan, the overwrite check and the job happen when its turn comes:
		//! `options.overwrite` decides what happens to an existing asset of that name (false: that file is skipped).
		void Enqueue(int batch, const std::string& source, const std::string& folder, const SImportOptions& options);
		//! No more files will be added to the batch (its summary comes when everything queued is done).
		void EndBatch(int batch);
		//! Drops everything not started yet and cancels the running job. `permanent`: also refuse new files (shutdown).
		void CancelAll(bool permanent = false);

		bool Busy() const { return m_running || !m_entries.empty(); }
		int Pending() const { return static_cast<int>(m_entries.size()); }
		int RunningJob() const { return m_runningJob; }

	signals:
		void Changed();

	private:
		struct SEntry
		{
			int batch = 0;
			std::string source;
			std::string folder;
			SImportOptions options;
		};
		struct SBatch
		{
			QString title;
			int total = 0;
			int finished = 0;
			int ok = 0;
			int failed = 0;
			int cancelled = 0;
			bool closed = false;
			std::vector<std::string> lines;
		};
		enum class EEnd { Ok, Failed, Cancelled };

		void Pump();
		void Start(const SEntry& entry);
		void Done(const SEntry& entry, EEnd end, const std::string& text, bool wasRunning = true);
		void MaybeSummarize(int batch);

		std::deque<SEntry> m_entries;
		std::map<int, SBatch> m_batches;
		int m_nextBatch = 1;
		bool m_running = false;
		bool m_stopped = false;
		bool m_pumpQueued = false;
		int m_runningJob = 0;
	};
}
