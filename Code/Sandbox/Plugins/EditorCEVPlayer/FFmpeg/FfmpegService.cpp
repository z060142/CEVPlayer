#include "StdAfx.h"
#include "FFmpeg/FfmpegService.h"

#include <algorithm>
#include <chrono>

#include <QApplication>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>

#include <Controls/QuestionDialog.h>
#include <FileDialogs/SystemFileDialog.h>

#include <CrySystem/ISystem.h>

#include "CEVOffline/FfmpegCommands.h"
#include "CEVOffline/FfmpegInstall.h"
#include "CEVOffline/FfmpegLocator.h"

#include "Jobs/JobRunner.h"
#include "Settings/EditorSettings.h"

namespace CEVEditor
{
	namespace ff = cev::offline::ffmpeg;

	namespace
	{
		CFfmpegService* g_pService = nullptr;

		//! The engine's question dialog with the three answers of "FFmpeg needed" (it only knows standard buttons: their texts
		//! are replaced).
		class CFfmpegNeededDialog : public CQuestionDialog
		{
		public:
			CFfmpegNeededDialog(const QString& text, bool canDownload, QWidget* pParent)
				: CQuestionDialog(pParent)
			{
				const QDialogButtonBox::StandardButtons buttons = (canDownload ? QDialogButtonBox::Yes : QDialogButtonBox::NoButton) | QDialogButtonBox::Open | QDialogButtonBox::Cancel;
				SetupQuestion(QObject::tr("FFmpeg needed"), text, buttons, canDownload ? QDialogButtonBox::Yes : QDialogButtonBox::Open);
				if (QPushButton* const pDownload = m_buttons->button(QDialogButtonBox::Yes))
					pDownload->setText(QObject::tr("Download minimal build"));
				if (QPushButton* const pChoose = m_buttons->button(QDialogButtonBox::Open))
					pChoose->setText(QObject::tr("Choose ffmpeg.exe..."));
			}
		};

		std::string Join(const std::vector<std::string>& items)
		{
			std::string out;
			for (const std::string& s : items)
				out += (out.empty() ? "" : ", ") + s;
			return out.empty() ? std::string("none") : out;
		}

		std::string Tail(const std::string& text, size_t maxChars)
		{
			if (text.size() <= maxChars)
				return text;
			const size_t cut = text.find('\n', text.size() - maxChars);
			return "..." + text.substr(cut == std::string::npos ? text.size() - maxChars : cut);
		}

		std::string Trimmed(std::string s)
		{
			auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"'; };
			while (!s.empty() && isSpace(static_cast<unsigned char>(s.front())))
				s.erase(s.begin());
			while (!s.empty() && isSpace(static_cast<unsigned char>(s.back())))
				s.pop_back();
			return s;
		}

		SOpResult ToOpResult(const cev::offline::Result& r, const std::string& log)
		{
			SOpResult out;
			out.message = r.message;
			if (r.Ok())
			{
				out.status = EOpStatus::Ok;
				return out;
			}
			out.status = r.status == cev::Status::Cancelled ? EOpStatus::Cancelled : EOpStatus::Failed;
			if (out.status == EOpStatus::Failed && !log.empty())
				out.message += "\nffmpeg output (end):\n" + Tail(log, 1500);
			return out;
		}

		cev::offline::Control ToControl(const SOpContext& ctx)
		{
			cev::offline::Control c;
			c.cancel = ctx.cancel;
			if (ctx.progress)
			{
				const auto progress = ctx.progress;
				c.progress = [progress](double f) { progress(static_cast<float>(f)); };
			}
			return c;
		}

		ff::WebmTranscodeOptions ToLibOptions(const SEncodeOptions& o)
		{
			ff::WebmTranscodeOptions w;
			w.crf = o.crf;
			w.width = o.width;
			w.height = o.height;
			w.inputFrameRate = o.inputFps;
			w.inputStartNumber = o.inputStartNumber;
			w.alpha = ff::AlphaMode::Auto; // yuva420p when the source has alpha (ProRes 4444, PNG with alpha, alpha WebM)
			return w;
		}

		SFfmpegStatus ToStatus(const ff::LocateResult& r, const std::string& configured)
		{
			SFfmpegStatus s;
			s.configuredPath = configured;
			s.message = r.message;
			for (const ff::LocateNote& n : r.rejected)
				s.rejected.push_back(std::string(ff::FfmpegSourceName(n.source)) + " " + n.path + ": " + n.reason);
			if (!r.found)
			{
				s.state = SFfmpegStatus::EState::Absent;
				return s;
			}
			s.state = SFfmpegStatus::EState::Ready;
			s.exe = r.info.path;
			s.version = r.info.version;
			s.configuration = r.info.configuration;
			s.gpl = r.info.gpl;
			s.nonfree = r.info.nonfree;
			s.source = ff::FfmpegSourceName(r.source);
			s.missingRequired = r.info.report.missingRequired;
			s.missingRecommended = r.info.report.missingRecommended;
			s.missingImport = r.info.report.missingImport;
			return s;
		}
	}

	std::string DescribeStatus(const SFfmpegStatus& s)
	{
		std::string out;
		switch (s.state)
		{
		case SFfmpegStatus::EState::Unknown:
			out = "ffmpeg status: unknown (not detected yet)";
			break;
		case SFfmpegStatus::EState::Detecting:
			out = "ffmpeg status: detecting";
			break;
		case SFfmpegStatus::EState::Ready:
			out = "ffmpeg status: ready, path " + s.exe + ", version " + s.version + ", source " + s.source +
				"; missing recommended: " + Join(s.missingRecommended) + "; missing for import: " + Join(s.missingImport);
			break;
		case SFfmpegStatus::EState::Absent:
			out = "ffmpeg status: absent (insufficient or not found): " + s.message;
			break;
		}
		out += "; configured path: " + (s.configuredPath.empty() ? std::string("(none)") : s.configuredPath);
		for (const std::string& r : s.rejected)
			out += "\n  rejected: " + r;
		return out;
	}

	SOpContext SubContext(const SOpContext& ctx, float from, float to)
	{
		SOpContext sub = ctx;
		if (ctx.progress)
		{
			const auto progress = ctx.progress;
			sub.progress = [progress, from, to](float p) { progress(from + (to - from) * std::min(1.f, std::max(0.f, p))); };
		}
		return sub;
	}

	SOpResult FfmpegTranscode(const std::string& exe, const std::string& src, const std::string& dst, const SEncodeOptions& options, const SOpContext& ctx)
	{
		ff::PlanRunReport report;
		const cev::offline::Result r = ff::TranscodeToWebm(exe, src, dst, ToLibOptions(options), ToControl(ctx), &report);
		return ToOpResult(r, report.log);
	}

	SOpResult FfmpegMakeVariant(const std::string& exe, const std::string& src, const std::string& dst, int height, const SEncodeOptions& options, const SOpContext& ctx)
	{
		ff::PlanRunReport report;
		const cev::offline::Result r = ff::MakeVariant(exe, src, dst, height, ToLibOptions(options), ToControl(ctx), &report);
		return ToOpResult(r, report.log);
	}

	// ---------------------------------------------------------------------------------------------------------------
	CFfmpegService* CFfmpegService::Get()
	{
		return g_pService;
	}

	CFfmpegService::CFfmpegService()
	{
		g_pService = this;
		m_status.configuredPath = FfmpegPathSetting(); // the preferences are loaded later: ReloadConfiguredPath follows
		gCevEditorPreferences.signalSettingsChanged.Connect(this, &CFfmpegService::ReloadConfiguredPath); // "reset to default"
	}

	CFfmpegService::~CFfmpegService()
	{
		gCevEditorPreferences.signalSettingsChanged.DisconnectObject(this);
		g_pService = nullptr;
		m_cv.notify_all();
	}

	SFfmpegStatus CFfmpegService::Status() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_status;
	}

	std::string CFfmpegService::ConfiguredPath() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_status.configuredPath;
	}

	std::string CFfmpegService::UnavailableMessage() const
	{
		const SFfmpegStatus s = Status();
		return "FFmpeg is not available (" + (s.message.empty() ? std::string("not detected") : s.message) +
			"). Set ffmpeg.exe in Edit > Preferences > CEVPlayer / FFmpeg, or use the console command cev_editor_ffmpeg_path <path>.";
	}

	bool CFfmpegService::WaitForExe(const std::atomic<bool>* cancel, std::string& exe, std::string& error)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
		std::unique_lock<std::mutex> lock(m_mutex);
		while (m_status.Pending())
		{
			if (cancel != nullptr && cancel->load())
			{
				error = "cancelled";
				return false;
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				error = "FFmpeg detection did not finish";
				return false;
			}
			m_cv.wait_for(lock, std::chrono::milliseconds(100));
		}
		if (!m_status.Ready())
		{
			lock.unlock();
			error = UnavailableMessage();
			return false;
		}
		exe = m_status.exe;
		return true;
	}

	void CFfmpegService::EnsureDetected()
	{
		StartDetect(false);
	}

	void CFfmpegService::StartDetect(bool force)
	{
		ff::LocateOptions options;
		std::string configured;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_status.state == SFfmpegStatus::EState::Detecting)
			{
				m_detectAgain = true;
				return;
			}
			if (!force && m_status.state != SFfmpegStatus::EState::Unknown)
				return;
			m_status.state = SFfmpegStatus::EState::Detecting;
			configured = m_status.configuredPath;
			options.settingsPath = configured;
			// tests: CEV_EDITOR_FFMPEG_NO_PATH=1 hides the system PATH (an ffmpeg on it would be found after a bad configured path)
			options.searchSystemPath = qEnvironmentVariableIsEmpty("CEV_EDITOR_FFMPEG_NO_PATH");
			if (!m_downloadedDirOverride.empty())
				options.downloadedDir = m_downloadedDirOverride;
			else if (ff::FfmpegDownloadEnabled())
				options.downloadedDir = ff::FfmpegInstallDir(ff::DefaultFfmpegInstallRoot(), ff::kFfmpegMinimalVersion);
		}
		Q_EMIT Changed();
		CJobRunner* const pRunner = CJobRunner::Get();
		if (pRunner == nullptr)
		{
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_status.state = SFfmpegStatus::EState::Absent;
				m_status.message = "the job runner is not available";
			}
			m_cv.notify_all();
			Q_EMIT Changed();
			return;
		}
		pRunner->Submit(QObject::tr("Detect FFmpeg"), [this, options, configured](const SOpContext&)
		{
			const ff::LocateResult found = ff::Locate(options); // runs `ffmpeg -version / -encoders / ...`: a few hundred ms
			SFfmpegStatus status = ToStatus(found, configured);
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				status.configuredPath = m_status.configuredPath; // may have changed while the job ran (a repeat follows)
				m_status = status;
			}
			m_cv.notify_all();
			SOpResult r;
			r.status = EOpStatus::Ok;
			r.message = found.found ? std::string() : found.message;
			return r;
		}, [this](const SOpResult&)
		{
			bool again = false;
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				again = m_detectAgain;
				m_detectAgain = false;
			}
			if (again)
			{
				StartDetect(true);
				return;
			}
			const SFfmpegStatus s = Status();
			CryLogAlways("[CEVEditor] %s", DescribeStatus(s).c_str());
			Q_EMIT Changed();
		}, "ffmpeg", false); // automatic and quick: no notification
	}

	void CFfmpegService::SetPath(const std::string& path)
	{
		SetFfmpegPathSetting(Trimmed(path)); // the preference page; its change notification (if any) ends up in ReloadConfiguredPath
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_status.configuredPath = FfmpegPathSetting();
		}
		StartDetect(true);
	}

	void CFfmpegService::ReloadConfiguredPath()
	{
		const std::string value = FfmpegPathSetting();
		bool detect = false;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_status.configuredPath == value)
				return;
			m_status.configuredPath = value;
			detect = m_status.state != SFfmpegStatus::EState::Unknown; // not detected yet: the first detection uses it
		}
		CryLogAlways("[CEVEditor] FFmpeg path preference: %s", value.empty() ? "(none)" : value.c_str());
		if (detect)
			StartDetect(true);
		else
			Q_EMIT Changed();
	}

	void OnFfmpegPreferenceChanged()
	{
		if (CFfmpegService* const pService = CFfmpegService::Get())
		{
			// from inside the Preferences window's serialization: later, from the event loop
			QTimer::singleShot(0, pService, [pService]() { pService->ReloadConfiguredPath(); });
		}
	}

	// ---------------------------------------------------------------------------------------------------------------
	std::string CFfmpegService::DownloadDisabledReason()
	{
		return ff::FfmpegDownloadDisabledReason();
	}

	int CFfmpegService::StartDownload(std::string& error)
	{
		const std::string reason = DownloadDisabledReason();
		if (!reason.empty())
		{
			error = reason;
			return 0;
		}
		ff::FfmpegPackage pkg = ff::PinnedFfmpegPackage();
		// tests: CEV_EDITOR_FFMPEG_DOWNLOAD_URL replaces the URL (e.g. an unreachable host to see the connection failure message)
		const QByteArray urlOverride = qgetenv("CEV_EDITOR_FFMPEG_DOWNLOAD_URL");
		if (!urlOverride.isEmpty())
		{
			pkg.url = urlOverride.constData();
			CryLogAlways("[CEVEditor] ffmpeg download: URL replaced by CEV_EDITOR_FFMPEG_DOWNLOAD_URL: %s", pkg.url.c_str());
		}
		return SubmitInstall(false, pkg.url, pkg.sha256, pkg.version, error);
	}

	int CFfmpegService::StartInstallFromZip(const std::string& zipPath, const std::string& sha256, const std::string& version, std::string& error)
	{
		return SubmitInstall(true, zipPath, sha256, version, error);
	}

	int CFfmpegService::SubmitInstall(bool fromLocalZip, const std::string& zipOrUrl, const std::string& sha256, const std::string& version, std::string& error)
	{
		CJobRunner* const pRunner = CJobRunner::Get();
		if (pRunner == nullptr)
		{
			error = "the job runner is not available";
			return 0;
		}
		if (m_downloadJob != 0)
		{
			error = "a download is already running";
			return 0;
		}
		const std::string root = ff::DefaultFfmpegInstallRoot();
		if (root.empty())
		{
			error = "the install folder (%LOCALAPPDATA%) is unknown";
			return 0;
		}
		const std::string installDir = ff::FfmpegInstallDir(root, version);
		if (fromLocalZip)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_downloadedDirOverride = installDir;
		}
		const QString title = fromLocalZip ? QObject::tr("Install FFmpeg from a local zip") : QObject::tr("Download FFmpeg (minimal build %1)").arg(QString::fromUtf8(version.c_str()));
		m_downloadJob = pRunner->Submit(title, [=](const SOpContext& ctx)
		{
			ff::FfmpegPackage pkg;
			pkg.version = version;
			pkg.sha256 = sha256;
			pkg.url = zipOrUrl;
			std::string installed;
			cev::offline::Result r;
			if (fromLocalZip)
			{
				ff::LocalFileFetcher fetcher;
				r = ff::DownloadAndInstallFfmpeg(pkg, fetcher, installDir, &installed, ToControl(ctx));
			}
			else
			{
				ff::WinHttpFetcher fetcher;
				r = ff::DownloadAndInstallFfmpeg(pkg, fetcher, installDir, &installed, ToControl(ctx));
			}
			SOpResult out = ToOpResult(r, std::string());
			if (r.Ok())
				out.message = "installed " + installed;
			return out;
		}, [this](const SOpResult& r)
		{
			m_downloadJob = 0;
			const QString text = QString::fromUtf8(r.message.c_str());
			if (r.Ok())
				CryLogAlways("[CEVEditor] ffmpeg download / install ok: %s", r.message.c_str());
			else
				CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] ffmpeg download / install %s: %s", r.status == EOpStatus::Cancelled ? "cancelled" : "FAILED", r.message.c_str());
			Q_EMIT DownloadFinished(r.Ok(), text);
			StartDetect(true); // the installed copy is checked like any other candidate
		}, "ffmpeg-download");
		return m_downloadJob;
	}

	void CFfmpegService::CancelDownload()
	{
		if (m_downloadJob != 0 && CJobRunner::Get() != nullptr)
			CJobRunner::Get()->Cancel(m_downloadJob);
	}

	// ---------------------------------------------------------------------------------------------------------------
	void CFfmpegService::WaitDetectModal(QWidget* pParent)
	{
		if (!Status().Pending())
			return;
		QProgressDialog dialog(tr("Looking for FFmpeg ..."), tr("Cancel"), 0, 0, pParent);
		dialog.setWindowModality(Qt::WindowModal);
		dialog.setMinimumDuration(300);
		QEventLoop loop;
		connect(this, &CFfmpegService::Changed, &loop, [this, &loop]() { if (!Status().Pending()) loop.quit(); });
		connect(&dialog, &QProgressDialog::canceled, &loop, &QEventLoop::quit);
		QTimer::singleShot(60000, &loop, &QEventLoop::quit);
		if (Status().Pending())
			loop.exec();
	}

	bool CFfmpegService::ChooseExecutableModal(QWidget* pParent)
	{
		CSystemFileDialog::RunParams params;
		params.title = tr("Choose ffmpeg.exe");
		params.initialFile = QString::fromUtf8(ConfiguredPath().c_str());
		params.extensionFilters << CExtensionFilter(tr("ffmpeg"), "exe");
		const QString file = CSystemFileDialog::RunImportFile(params, pParent);
		if (file.isEmpty())
			return false;
		SetPath(std::string(file.toUtf8().constData()));
		WaitDetectModal(pParent);
		const SFfmpegStatus s = Status();
		if (s.Ready())
			return true;
		QString text = tr("This file cannot be used:\n%1").arg(QString::fromUtf8(s.message.c_str()));
		for (const std::string& r : s.rejected)
			text += QString("\n") + QString::fromUtf8(r.c_str());
		CQuestionDialog::SWarning(tr("FFmpeg"), text, QDialogButtonBox::Ok, QDialogButtonBox::Ok, pParent);
		return false;
	}

	bool CFfmpegService::EnsureUsableModal(QWidget* pParent, const QString& why)
	{
		StartDetect(false);
		WaitDetectModal(pParent);
		for (;;)
		{
			const SFfmpegStatus s = Status();
			if (s.Ready())
				return true;
			const std::string disabledReason = DownloadDisabledReason();
			QString text = why + tr("\n\nFFmpeg was not found or is not sufficient: %1").arg(QString::fromUtf8(s.message.c_str()));
			if (disabledReason.empty())
			{
				text += tr("\n\nDownload our minimal build (LGPL, version %1, from the GitHub releases of %2; the SHA-256 of the download is verified before anything is installed), or choose an ffmpeg.exe yourself.")
					.arg(QString::fromUtf8(ff::kFfmpegMinimalVersion), QString::fromUtf8(ff::kFfmpegMinimalRepo));
			}
			else
			{
				text += tr("\n\nThe download is not available: %1\nChoose an ffmpeg.exe (it needs libvpx-vp9, libopus and the webm muxer).").arg(QString::fromUtf8(disabledReason.c_str()));
			}
			CFfmpegNeededDialog box(text, disabledReason.empty(), pParent);
			const QDialogButtonBox::StandardButton answer = box.Execute();
			if (answer == QDialogButtonBox::Open)
			{
				if (ChooseExecutableModal(pParent))
					return true;
			}
			else if (answer == QDialogButtonBox::Yes && disabledReason.empty())
			{
				std::string error;
				const int jobId = StartDownload(error);
				if (jobId == 0)
				{
					CQuestionDialog::SWarning(tr("FFmpeg"), QString::fromUtf8(error.c_str()), QDialogButtonBox::Ok, QDialogButtonBox::Ok, pParent);
					continue;
				}
				QProgressDialog dialog(tr("Downloading FFmpeg ..."), tr("Cancel"), 0, 100, pParent);
				dialog.setWindowModality(Qt::WindowModal);
				dialog.setAutoClose(false);
				dialog.setAutoReset(false);
				dialog.setMinimumDuration(0);
				QEventLoop loop;
				bool ok = false;
				QString message;
				connect(this, &CFfmpegService::DownloadFinished, &loop, [&](bool o, const QString& m) { ok = o; message = m; loop.quit(); });
				if (CJobRunner* const pRunner = CJobRunner::Get())
				{
					connect(pRunner, &CJobRunner::JobChanged, &dialog, [&dialog, pRunner, jobId](int id)
					{
						if (id != jobId)
							return;
						for (const CJobRunner::SJobInfo& job : pRunner->Jobs())
						{
							if (job.id == jobId)
								dialog.setValue(static_cast<int>(job.progress * 100.f));
						}
					});
				}
				connect(&dialog, &QProgressDialog::canceled, this, [this]() { CancelDownload(); });
				dialog.show();
				loop.exec();
				dialog.close();
				if (!ok)
				{
					CQuestionDialog::SWarning(tr("FFmpeg download"), tr("The download was not installed:\n%1").arg(message), QDialogButtonBox::Ok, QDialogButtonBox::Ok, pParent);
					continue;
				}
				WaitDetectModal(pParent);
				if (Status().Ready())
					return true;
				continue;
			}
			else
			{
				return false; // Cancel
			}
		}
	}
}
