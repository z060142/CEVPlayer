// The editor's view of the external FFmpeg (docs/design/M5-ffmpeg.md, "Editor side"): which ffmpeg is used, what it can do,
// the download of our minimal build, and thin wrappers around the CEVPlayerFFmpeg transcode calls.
//
// Threading: Status() / WaitForExe() are safe on any thread. Everything else (SetPath, StartDetect, StartDownload, the modal
// helpers, the signals) is main thread only. Detection and download run on CJobRunner jobs; the job threads never touch Qt or the
// engine. ffmpeg itself only ever runs as a child process (CEVPlayerFFmpeg, no libav* linkage).
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <QObject>
#include <QString>

#include "Offline/IOfflineOps.h"

class QWidget;

namespace CEVEditor
{
	struct SFfmpegStatus
	{
		enum class EState
		{
			Unknown,    //!< detection has not been started yet
			Detecting,  //!< a detection job is running
			Ready,      //!< a usable ffmpeg was found (exe, version, ...)
			Absent,     //!< nothing usable (message / rejected tell why)
		};
		EState state = EState::Unknown;
		std::string exe;                         //!< UTF-8 path of the executable (Ready)
		std::string version;
		std::string source;                      //!< "configured" | "downloaded" | "system"
		std::string configuration;
		bool gpl = false;
		bool nonfree = false;
		std::vector<std::string> missingRequired;     //!< "encoder:libvpx-vp9" (never non-empty when Ready)
		std::vector<std::string> missingRecommended;
		std::vector<std::string> missingImport;
		std::vector<std::string> rejected;            //!< "<source> <path>: <reason>" candidates that exist but cannot be used
		std::string message;                          //!< one line: what was found / why not
		std::string configuredPath;                   //!< the path of the editor setting ("" = none)

		bool Ready() const { return state == EState::Ready; }
		bool Pending() const { return state == EState::Unknown || state == EState::Detecting; }
	};

	//! Multi line text of the status (log / console / tooltip).
	std::string DescribeStatus(const SFfmpegStatus& status);

	//! The options of one encode as the editor uses them (CEVPlayerFFmpeg::WebmTranscodeOptions without the header).
	struct SEncodeOptions
	{
		int crf = 32;                 //!< libvpx-vp9 constant quality 0 (best) .. 63
		int width = 0;                //!< 0 = keep; only one of width / height: the other follows
		int height = 0;
		double inputFps = 0.0;        //!< PNG sequence frame rate
		int inputStartNumber = -1;    //!< PNG sequence first number (-1 = ffmpeg's own guess)
	};

	//! Job thread: ffmpeg `src` -> VP9 / Opus WebM `dst` (written as dst.tmp and renamed; a failed / cancelled run leaves
	//! nothing behind and does not touch an existing dst). `ctx.progress` receives 0..1.
	SOpResult FfmpegTranscode(const std::string& exe, const std::string& src, const std::string& dst, const SEncodeOptions& options, const SOpContext& ctx);
	//! Job thread: same, forced to `height` (width follows, even). A WebM source with alpha keeps it.
	SOpResult FfmpegMakeVariant(const std::string& exe, const std::string& src, const std::string& dst, int height, const SEncodeOptions& options, const SOpContext& ctx);
	//! `ctx` with its progress mapped to [from, to] (for steps of a longer job).
	SOpContext SubContext(const SOpContext& ctx, float from, float to);

	class CFfmpegService : public QObject
	{
		Q_OBJECT
	public:
		static CFfmpegService* Get();   //!< null before / after the plug-in's lifetime

		CFfmpegService();
		~CFfmpegService() override;

		// ---- any thread ----
		SFfmpegStatus Status() const;
		//! Blocks (polling `cancel`) while the detection is pending, then gives the executable. False + error when there is none.
		bool WaitForExe(const std::atomic<bool>* cancel, std::string& exe, std::string& error);
		//! One sentence for refusals ("FFmpeg is not available: ... set it with ...").
		std::string UnavailableMessage() const;

		// ---- main thread ----
		//! Starts the detection when it has not run yet (lazy, at first use / when the main frame is up).
		void EnsureDetected();
		//! Runs the detection on a job (again). A running detection is repeated once more afterwards.
		void StartDetect(bool force);
		//! Persists the configured path ("" clears it) and detects again.
		void SetPath(const std::string& path);
		std::string ConfiguredPath() const;

		//! Reason why downloading our minimal build is disabled, "" when it is possible.
		static std::string DownloadDisabledReason();
		//! Starts the download job of the pinned minimal build (progress / cancel through the job list). 0 + error when it cannot.
		int StartDownload(std::string& error);
		//! Test / offline install: the "download" is a local zip (LocalFileFetcher), verified against `sha256` exactly like the
		//! real one, installed into <install root>/<version>. The folder is then searched like the downloaded copy.
		int StartInstallFromZip(const std::string& zipPath, const std::string& sha256, const std::string& version, std::string& error);
		bool DownloadRunning() const { return m_downloadJob != 0; }
		void CancelDownload();

		//! The interactive flow of "an operation needs FFmpeg": when none is usable, asks (download the minimal build, or choose
		//! ffmpeg.exe) and returns true once a usable one exists. Modal; never use it from a script / console path.
		bool EnsureUsableModal(QWidget* pParent, const QString& why);
		//! File dialog + SetPath + waits (modal) for the detection. True when the chosen file is usable (else the reason is shown).
		bool ChooseExecutableModal(QWidget* pParent);

	signals:
		void Changed();                                   //!< the status changed (main thread)
		void DownloadFinished(bool ok, const QString& message);

	private:
		int SubmitInstall(bool fromLocalZip, const std::string& zipOrUrl, const std::string& sha256, const std::string& version, std::string& error);
		void WaitDetectModal(QWidget* pParent);

		mutable std::mutex m_mutex;
		std::condition_variable m_cv;
		SFfmpegStatus m_status;
		bool m_detectAgain = false;
		int m_downloadJob = 0;
		std::string m_downloadedDirOverride;   //!< a test install: searched as the downloaded copy
	};
}
