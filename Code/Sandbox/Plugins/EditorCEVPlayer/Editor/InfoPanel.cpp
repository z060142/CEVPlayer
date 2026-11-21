#include "StdAfx.h"
#include "Editor/InfoPanel.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>

#include <AssetSystem/Asset.h>
#include <CrySerialization/IArchiveHost.h>
#include <CrySystem/ISystem.h>
#include <IEditor.h>
#include <Notifications/NotificationCenter.h>
#include <Serialization/QPropertyTreeLegacy/QPropertyTreeLegacy.h>

#include "Asset/VideoImport.h"
#include "Editor/VideoEditor.h"
#include "FFmpeg/FfmpegService.h"
#include "Jobs/JobRunner.h"
#include "Offline/IOfflineOps.h"
#include "Settings/EditorSettings.h"

namespace CEVEditor
{
	namespace fs = std::filesystem;

	namespace
	{
		QString Q(const std::string& s) { return QString::fromUtf8(s.c_str()); }

		QString SizeText(unsigned long long bytes)
		{
			if (bytes >= (1ull << 20))
				return QString::number(static_cast<double>(bytes) / (1 << 20), 'f', 1) + " MB";
			if (bytes >= 1024)
				return QString::number(static_cast<double>(bytes) / 1024.0, 'f', 1) + " KB";
			return QString::number(bytes) + " B";
		}

		bool HasExtension(const std::string& path, const char* ext)
		{
			const size_t dot = path.find_last_of('.');
			if (dot == std::string::npos)
				return false;
			return _stricmp(path.c_str() + dot + 1, ext) == 0;
		}

		void Notify(bool warning, const QString& title, const QString& text)
		{
			if (GetIEditor() == nullptr || GetIEditor()->GetNotificationCenter() == nullptr)
				return;
			if (warning)
				GetIEditor()->GetNotificationCenter()->ShowWarning(title, text);
			else
				GetIEditor()->GetNotificationCenter()->ShowInfo(title, text);
		}
	}

	CVideoInfoPanel::CVideoInfoPanel(CVideoEditor* pEditor, QWidget* pParent)
		: QWidget(pParent)
		, m_pEditor(pEditor)
	{
		setWindowTitle(tr("Info"));
		m_pArchiveContext.reset(new yasli::Context(&m_context));
		BuildUi();
		if (CJobRunner* const pRunner = CJobRunner::Get())
		{
			connect(pRunner, &CJobRunner::JobChanged, this, &CVideoInfoPanel::OnJobChanged);
			connect(pRunner, &CJobRunner::JobLog, this, &CVideoInfoPanel::OnJobLog);
		}
		if (m_pEditor)
			connect(m_pEditor.data(), &CVideoEditor::StateChanged, this, &CVideoInfoPanel::Refresh);
		if (CFfmpegService* const pService = CFfmpegService::Get())
		{
			connect(pService, &CFfmpegService::Changed, this, &CVideoInfoPanel::RefreshFfmpeg);
			pService->EnsureDetected(); // lazy: the first panel (or the main frame) starts the detection, the result is cached
		}
		Refresh();
		RefreshFfmpeg();
		RefreshJobs();
	}

	CVideoInfoPanel::~CVideoInfoPanel()
	{
		if (m_pTree != nullptr)
			m_pTree->detach(); // the tree holds a pointer to m_edit
	}

	void CVideoInfoPanel::BuildUi()
	{
		QVBoxLayout* const pOuter = new QVBoxLayout(this);
		pOuter->setContentsMargins(0, 0, 0, 0);
		QScrollArea* const pScroll = new QScrollArea(this);
		pScroll->setWidgetResizable(true);
		pOuter->addWidget(pScroll);
		QWidget* const pContent = new QWidget(pScroll);
		pScroll->setWidget(pContent);
		QVBoxLayout* const pMain = new QVBoxLayout(pContent);

		m_pTitle = new QLabel(pContent);
		QFont titleFont = m_pTitle->font();
		titleFont.setBold(true);
		m_pTitle->setFont(titleFont);
		m_pTitle->setWordWrap(true);
		pMain->addWidget(m_pTitle);

		m_pInfo = new QLabel(pContent);
		m_pInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
		pMain->addWidget(m_pInfo);

		m_pSource = new QLabel(pContent);
		m_pSource->setWordWrap(true);
		pMain->addWidget(m_pSource);

		// ---- files
		QGroupBox* const pFilesBox = new QGroupBox(tr("Files"), pContent);
		QVBoxLayout* const pFilesLayout = new QVBoxLayout(pFilesBox);
		m_pFiles = new QTableWidget(0, 3, pFilesBox);
		m_pFiles->setHorizontalHeaderLabels({ tr("File"), tr("Role"), tr("Status") });
		m_pFiles->horizontalHeader()->setStretchLastSection(true);
		m_pFiles->setEditTriggers(QAbstractItemView::NoEditTriggers);
		m_pFiles->setSelectionMode(QAbstractItemView::NoSelection);
		m_pFiles->setMinimumHeight(110);
		pFilesLayout->addWidget(m_pFiles);
		pMain->addWidget(pFilesBox);

		// ---- variants
		QGroupBox* const pVariantsBox = new QGroupBox(tr("Variants (descriptor <Variants>)"), pContent);
		QVBoxLayout* const pVariantsLayout = new QVBoxLayout(pVariantsBox);
		m_pVariants = new QTableWidget(0, 4, pVariantsBox);
		m_pVariants->setHorizontalHeaderLabels({ tr("File"), tr("Size"), tr("Status"), tr("Disk") });
		m_pVariants->horizontalHeader()->setStretchLastSection(true);
		m_pVariants->setEditTriggers(QAbstractItemView::NoEditTriggers);
		m_pVariants->setSelectionMode(QAbstractItemView::NoSelection);
		m_pVariants->setMinimumHeight(70);
		m_pVariants->setToolTip(tr("The runtime plays the smallest variant that is not below the output size (rule smallestNotBelowOutput). A variant is stale when it is older than the main video: generate it again."));
		pVariantsLayout->addWidget(m_pVariants);
		pMain->addWidget(pVariantsBox);

		// ---- FFmpeg (the path is an editor preference: Edit > Preferences > CEVPlayer / FFmpeg)
		QGroupBox* const pFfmpegBox = new QGroupBox(tr("FFmpeg"), pContent);
		QVBoxLayout* const pFfmpegLayout = new QVBoxLayout(pFfmpegBox);
		m_pFfmpegState = new QLabel(pFfmpegBox);
		m_pFfmpegState->setWordWrap(true);
		pFfmpegLayout->addWidget(m_pFfmpegState);
		m_pFfmpegDetail = new QLabel(pFfmpegBox);
		m_pFfmpegDetail->setWordWrap(true);
		m_pFfmpegDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
		pFfmpegLayout->addWidget(m_pFfmpegDetail);
		QHBoxLayout* const pFfmpegButtons = new QHBoxLayout();
		QPushButton* const pPreferences = new QPushButton(tr("Open preferences"), pFfmpegBox);
		pPreferences->setToolTip(tr("The FFmpeg path is set in Edit > Preferences > %1. It needs libvpx-vp9, libopus and the webm muxer.").arg(QString::fromUtf8(kPreferencesPagePath)));
		m_pFfmpegDownload = new QPushButton(tr("Download minimal build"), pFfmpegBox);
		QPushButton* const pRedetect = new QPushButton(tr("Re-detect"), pFfmpegBox);
		pFfmpegButtons->addWidget(pPreferences);
		pFfmpegButtons->addWidget(m_pFfmpegDownload);
		pFfmpegButtons->addWidget(pRedetect);
		pFfmpegButtons->addStretch(1);
		pFfmpegLayout->addLayout(pFfmpegButtons);
		connect(pPreferences, &QPushButton::clicked, this, []() { OpenEditorPreferences(); });
		connect(m_pFfmpegDownload, &QPushButton::clicked, this, [this]()
		{
			CFfmpegService* const pService = CFfmpegService::Get();
			if (pService == nullptr)
				return;
			std::string error;
			if (pService->StartDownload(error) == 0)
				Notify(true, tr("FFmpeg"), QString::fromUtf8(error.c_str()));
			else
				CryLogAlways("[CEVEditor] downloading the minimal FFmpeg build (progress in the notification center and the job list)");
		});
		connect(pRedetect, &QPushButton::clicked, this, [this]()
		{
			if (CFfmpegService* const pService = CFfmpegService::Get())
				pService->StartDetect(true);
		});
		pMain->addWidget(pFfmpegBox);

		// ---- descriptor: the engine's property tree on SDescriptor::Serialize; every change goes to the editor at once
		QGroupBox* const pDescBox = new QGroupBox(tr("Descriptor (.cevvideo)"), pContent);
		QVBoxLayout* const pDescLayout = new QVBoxLayout(pDescBox);
		m_pTree = new QPropertyTreeLegacy(pDescBox);
		m_pTree->setSizeToContent(true);
		m_pTree->setExpandLevels(2);
		m_pTree->setArchiveContext(m_pArchiveContext.get());
		connect(m_pTree, &QPropertyTreeLegacy::signalChanged, this, &CVideoInfoPanel::OnTreeChanged);
		pDescLayout->addWidget(m_pTree);
		QLabel* const pDescHint = new QLabel(tr("Changes mark the asset as modified; Save (Ctrl+S) writes the descriptor."), pDescBox);
		pDescHint->setWordWrap(true);
		pDescLayout->addWidget(pDescHint);
		pMain->addWidget(pDescBox);

		// ---- operations
		QGroupBox* const pOpsBox = new QGroupBox(tr("Operations"), pContent);
		QVBoxLayout* const pOpsLayout = new QVBoxLayout(pOpsBox);
		m_pOffline = new QLabel(pOpsBox);
		m_pOffline->setWordWrap(true);
		pOpsLayout->addWidget(m_pOffline);
		m_pOpButtons = new QWidget(pOpsBox);
		QGridLayout* const pOps = new QGridLayout(m_pOpButtons);
		pOps->setContentsMargins(0, 0, 0, 0);
		auto addButton = [&](const QString& text, const QString& tip, int row, int col, std::function<void()> fn) -> QPushButton*
		{
			QPushButton* const b = new QPushButton(text, m_pOpButtons);
			b->setToolTip(tip);
			connect(b, &QPushButton::clicked, this, [fn]() { fn(); });
			pOps->addWidget(b, row, col);
			return b;
		};
		// the produced files are named after (and listed as) the default audio track, else the first one, else "main"
		auto trackName = [this]() -> std::string
		{
			if (!m_pEditor)
				return "main";
			const SDescriptor& d = m_pEditor->State().descriptor;
			if (!d.defaultTrack.empty())
				return d.defaultTrack;
			return d.audioTracks.empty() ? std::string("main") : d.audioTracks.front().name;
		};
		addButton(tr("Split audio to WebM"), tr("Writes the embedded audio as <name>_<track>.webm (no re-encoding) and makes that audio track play the file."), 0, 0, [this, trackName]()
		{
			if (!m_pEditor) return;
			SAudioOps ops; ops.split = true; ops.audioTrack = trackName();
			m_pEditor->RunAudioOps(ops);
		});
		addButton(tr("Export WAV"), tr("48 kHz / 16 bit WAV whose first sample is video second 0, stored as an asset file."), 0, 1, [this, trackName]()
		{
			if (!m_pEditor) return;
			SAudioOps ops; ops.wav = true; ops.audioTrack = trackName();
			m_pEditor->RunAudioOps(ops);
		});
		addButton(tr("Export Ogg"), tr("Ogg Vorbis of the audio track, stored as an asset file."), 1, 0, [this, trackName]()
		{
			if (!m_pEditor) return;
			SAudioOps ops; ops.ogg = true; ops.audioTrack = trackName();
			m_pEditor->RunAudioOps(ops);
		});
		addButton(tr("Strip audio"), tr("Writes <name>_silent.webm (picture only) and makes the descriptor play it."), 1, 1, [this, trackName]()
		{
			if (!m_pEditor) return;
			SAudioOps ops; ops.strip = true; ops.audioTrack = trackName();
			m_pEditor->RunAudioOps(ops);
		});
		addButton(tr("Upgrade to cue route"), tr("Needs a WAV / Ogg of the asset: registers it with SDL_mixer / ACE and switches the descriptor to the cue route."), 2, 0, [this]()
		{
			if (m_pEditor)
				m_pEditor->UpgradeToCue();
		});
		m_pBtnReimport = addButton(tr("Reimport"), tr("The asset's Reimport (the same as in the Asset Browser): the stored import settings are used again; a changed original file is copied over the source file first."), 2, 1, [this]()
		{
			if (m_pEditor && !m_pEditor->Reimport())
				Notify(true, tr("Video asset"), tr("This asset cannot be reimported (no source file registered, or read-only)."));
		});
		m_pBtnVariants = addButton(tr("Generate variants..."), tr("FFmpeg: smaller copies of the video (<name>_<height>p.webm) listed in the descriptor <Variants>."), 3, 0, [this]()
		{
			if (m_pEditor)
				m_pEditor->GenerateVariantsDialog();
		});
		m_pBtnRecompress = addButton(tr("Re-compress / resize..."), tr("FFmpeg: encodes the video again from the stored source copy with another quality / height; the asset keeps its GUID."), 3, 1, [this]()
		{
			if (m_pEditor)
				m_pEditor->RecompressDialog();
		});
		pOpsLayout->addWidget(m_pOpButtons);

		m_pAce = new QLabel(pOpsBox);
		m_pAce->setWordWrap(true);
		m_pAce->setTextInteractionFlags(Qt::TextSelectableByMouse);
		pOpsLayout->addWidget(m_pAce);

		// progress and results of the jobs are also in the notification center; the list and the full log of each job stay here
		pOpsLayout->addWidget(new QLabel(tr("Jobs"), pOpsBox));
		m_pJobs = new QListWidget(pOpsBox);
		m_pJobs->setMinimumHeight(60);
		pOpsLayout->addWidget(m_pJobs);
		m_pCancelJob = new QPushButton(tr("Cancel selected job"), pOpsBox);
		connect(m_pCancelJob, &QPushButton::clicked, this, [this]()
		{
			QListWidgetItem* const item = m_pJobs->currentItem();
			if (item != nullptr && CJobRunner::Get() != nullptr)
				CJobRunner::Get()->Cancel(item->data(Qt::UserRole).toInt());
		});
		pOpsLayout->addWidget(m_pCancelJob);
		m_pLog = new QPlainTextEdit(pOpsBox);
		m_pLog->setReadOnly(true);
		m_pLog->setMaximumBlockCount(2000);
		m_pLog->setMinimumHeight(90);
		m_pLog->setToolTip(tr("Full output of the jobs (FFmpeg / audio operations)."));
		pOpsLayout->addWidget(m_pLog);
		pMain->addWidget(pOpsBox);
		pMain->addStretch(1);
	}

	void CVideoInfoPanel::Log(const QString& line)
	{
		m_pLog->appendPlainText(line);
	}

	void CVideoInfoPanel::Refresh()
	{
		const bool has = m_pEditor && m_pEditor->HasAsset();
		m_pOpButtons->setEnabled(has);
		IOfflineOps& offline = GetOfflineOps();
		m_pOffline->setText(offline.IsAvailable()
			? tr("Offline library: %1").arg(QString::fromUtf8(offline.Name()))
			: tr("Offline library: %1. Split / WAV / Ogg / strip report an error until it is wired in.").arg(QString::fromUtf8(offline.Name())));

		if (!has)
		{
			m_filledRevision = -1;
			m_pTitle->setText(tr("No video asset opened. Double click one in the asset browser, or drop a video file (webm, mp4, mov, mkv, avi) or a folder of PNG files here to import it."));
			m_pInfo->clear();
			m_pSource->clear();
			m_pAce->clear();
			m_pFiles->setRowCount(0);
			m_pVariants->setRowCount(0);
			m_pTree->detach();
			m_pTree->setEnabled(false);
			return;
		}
		const CVideoEditor::SState& st = m_pEditor->State();
		m_pTitle->setText(Q(st.assetName));

		QString info;
		if (st.mediaKnown && st.media.valid)
		{
			const SMediaInfo& m = st.media;
			info += tr("Video: %1, %2 x %3, %4 fps\n").arg(Q(m.videoCodec)).arg(m.width).arg(m.height).arg(m.fps, 0, 'f', 3);
			info += tr("Duration: %1 s\n").arg(m.durationSec, 0, 'f', 3);
			info += tr("Alpha: %1\n").arg(m.hasAlpha ? tr("yes") : tr("no"));
			if (m.maxKeyframeIntervalSec > 0.0)
			{
				info += tr("Max keyframe interval: %1 s\n").arg(m.maxKeyframeIntervalSec, 0, 'f', 2);
				if (m.maxKeyframeIntervalSec > 5.0)
					info += tr("Keyframes are sparse: showing the video again after it was hidden in a UI may stutter. Recompress it (adds a keyframe every 2 s).\n");
			}
			if (m.audioTracks.empty())
				info += tr("Audio: none\n");
			for (size_t i = 0; i < m.audioTracks.size(); ++i)
			{
				const SAudioTrackInfo& a = m.audioTracks[i];
				info += tr("Audio %1: %2, %3 Hz, %4 ch, start %5 s, codec delay %6 s\n").arg(static_cast<int>(i)).arg(Q(a.codec)).arg(a.sampleRate).arg(a.channels)
					.arg(a.startOffsetSec, 0, 'f', 4).arg(a.codecDelaySec, 0, 'f', 4);
			}
		}
		else
		{
			// from the Details written at import time until the file has been read
			info += tr("Video: %1, %2 x %3, %4 fps (from the import details)\n").arg(Q(st.details.Get(DetailKey::kCodec)))
				.arg(st.details.GetInt(DetailKey::kWidth)).arg(st.details.GetInt(DetailKey::kHeight)).arg(st.details.GetDouble(DetailKey::kFps), 0, 'f', 3);
			info += tr("Duration: %1 s\n").arg(st.details.GetDouble(DetailKey::kDuration), 0, 'f', 3);
			info += tr("Audio: %1\n").arg(Q(st.details.Get(DetailKey::kAudioTracks)));
		}
		info += tr("Route: %1").arg(Q(st.descriptor.route));
		m_pInfo->setText(info);

		if (st.source.checked)
		{
			m_pSource->setText(Q(st.source.text));
			m_pSource->setStyleSheet(st.source.reimportNeeded || !st.source.copyExists ? "color: #e0a030;" : "color: #70c070;");
		}
		else
		{
			m_pSource->setText(st.source.confirming ? Q(st.source.text) : tr("Checking the source file ..."));
			m_pSource->setStyleSheet(QString());
		}

		CAsset* const pAsset = m_pEditor->GetAssetBeingEdited();
		const bool canReimport = pAsset != nullptr && pAsset->HasSourceFile() && !pAsset->IsImmutable();
		m_pBtnReimport->setEnabled(canReimport);
		m_pBtnReimport->setText(st.source.reimportNeeded ? tr("Reimport (source changed)") : tr("Reimport"));

		const std::string ace = st.details.Get(DetailKey::kAceLibrary);
		if (!ace.empty())
		{
			m_pAce->setText(tr("SDL_mixer / ACE: library %1, copies %2\nWwise / FMOD: this plug-in only writes the SDL_mixer registration. Manual steps: import the WAV / Ogg of this asset into your middleware project, create one start / pause / resume event per audio track, and pick the triggers in the Audio tracks of the descriptor above (Source: Trigger).")
				.arg(Q(ace)).arg(Q(st.details.Get(DetailKey::kSdlFiles))));
		}
		else
		{
			m_pAce->setText(tr("Not registered with SDL_mixer / ACE yet (Upgrade to cue route does it once the asset has a WAV / Ogg)."));
		}

		FillFiles();
		FillVariants();
		// The tree holds what the user is editing: refill it only when the descriptor in the state was replaced (asset opened /
		// reloaded / merged), not on every state change (e.g. when a background check finishes).
		if (m_filledRevision != m_pEditor->DescriptorRevision())
		{
			m_filledRevision = m_pEditor->DescriptorRevision();
			FillDescriptor();
		}
	}

	void CVideoInfoPanel::FillFiles()
	{
		const CVideoEditor::SState& st = m_pEditor->State();
		struct SRow { std::string rel; QString role; };
		std::vector<SRow> rows;
		for (const std::string& rel : st.files)
		{
			QString role = tr("file");
			if (rel == st.descriptorRel)
				role = tr("descriptor (shipped)");
			else if (rel == st.videoRel && rel == st.sourceCopyRel)
				role = tr("video (shipped), source file of the asset");
			else if (rel == st.videoRel)
				role = tr("video (shipped)");
			else if (rel == st.sourceCopyRel)
				role = tr("source file - original file, not shipped");
			else if (HasExtension(rel, "wav") || HasExtension(rel, "ogg"))
				role = tr("sound file (the engine also lists it as a Sound asset)");
			else if (std::any_of(st.descriptor.variants.begin(), st.descriptor.variants.end(), [&](const SDescriptor::SVariant& v) { return _stricmp((st.folderRel + v.file).c_str(), rel.c_str()) == 0; }))
				role = tr("variant (shipped)");
			else if (HasExtension(rel, "mp4") || HasExtension(rel, "mov") || HasExtension(rel, "mkv") || HasExtension(rel, "avi"))
				role = tr("source copy - original file, not shipped");
			else if (HasExtension(rel, "webm"))
				role = tr("WebM (audio track)");
			rows.push_back({ rel, role });
		}
		for (const std::string& rel : SplitList(st.details.Get(DetailKey::kSdlFiles), ';'))
			rows.push_back({ rel, tr("SDL_mixer copy (outside the asset folder)") });
		if (!st.details.Get(DetailKey::kAceLibrary).empty())
			rows.push_back({ st.details.Get(DetailKey::kAceLibrary), tr("ACE library (outside the asset folder)") });

		m_pFiles->setRowCount(static_cast<int>(rows.size()));
		for (int i = 0; i < static_cast<int>(rows.size()); ++i)
		{
			const SRow& r = rows[static_cast<size_t>(i)];
			std::error_code ec;
			const fs::path path = fs::u8path(AssetsAbsolute(r.rel));
			const bool exists = fs::is_regular_file(path, ec);
			QTableWidgetItem* const status = new QTableWidgetItem(exists ? SizeText(static_cast<unsigned long long>(fs::file_size(path, ec))) : tr("MISSING"));
			if (!exists)
				status->setForeground(QColor(230, 90, 90));
			m_pFiles->setItem(i, 0, new QTableWidgetItem(Q(r.rel)));
			m_pFiles->setItem(i, 1, new QTableWidgetItem(r.role));
			m_pFiles->setItem(i, 2, status);
		}
		m_pFiles->resizeColumnsToContents();
	}

	void CVideoInfoPanel::FillVariants()
	{
		const CVideoEditor::SState& st = m_pEditor->State();
		const SDescriptor& d = st.descriptor;
		m_pVariants->setRowCount(static_cast<int>(d.variants.size()));
		// the encode master: <name>.webm (a variant older than it was made from an older picture)
		std::error_code ec;
		fs::path masterPath = fs::u8path(AssetsAbsolute(st.folderRel + st.assetName + ".webm"));
		if (!fs::is_regular_file(masterPath, ec))
			masterPath = fs::u8path(AssetsAbsolute(st.videoRel));
		const bool masterKnown = fs::is_regular_file(masterPath, ec);
		const fs::file_time_type masterTime = masterKnown ? fs::last_write_time(masterPath, ec) : fs::file_time_type();
		for (int i = 0; i < static_cast<int>(d.variants.size()); ++i)
		{
			const SDescriptor::SVariant& v = d.variants[static_cast<size_t>(i)];
			const fs::path path = fs::u8path(AssetsAbsolute(st.folderRel + v.file));
			const bool exists = fs::is_regular_file(path, ec);
			bool stale = false;
			if (exists && masterKnown)
			{
				const fs::file_time_type t = fs::last_write_time(path, ec);
				stale = !ec && t < masterTime - std::chrono::seconds(2);
			}
			QTableWidgetItem* const status = new QTableWidgetItem(!exists ? tr("MISSING") : stale ? tr("stale: older than the main video") : tr("up to date"));
			if (!exists)
				status->setForeground(QColor(230, 90, 90));
			else if (stale)
				status->setForeground(QColor(224, 160, 48));
			m_pVariants->setItem(i, 0, new QTableWidgetItem(Q(v.file)));
			m_pVariants->setItem(i, 1, new QTableWidgetItem(QString("%1 x %2").arg(v.width).arg(v.height)));
			m_pVariants->setItem(i, 2, status);
			m_pVariants->setItem(i, 3, new QTableWidgetItem(exists ? SizeText(static_cast<unsigned long long>(fs::file_size(path, ec))) : QString()));
		}
		m_pVariants->resizeColumnsToContents();
	}

	void CVideoInfoPanel::RefreshFfmpeg()
	{
		CFfmpegService* const pService = CFfmpegService::Get();
		if (pService == nullptr)
		{
			m_pFfmpegState->setText(tr("FFmpeg service not available."));
			return;
		}
		const SFfmpegStatus s = pService->Status();
		QString state;
		QString detail;
		switch (s.state)
		{
		case SFfmpegStatus::EState::Unknown:
		case SFfmpegStatus::EState::Detecting:
			state = tr("Looking for FFmpeg ...");
			m_pFfmpegState->setStyleSheet(QString());
			break;
		case SFfmpegStatus::EState::Ready:
			state = tr("FFmpeg ready: %1 (%2)").arg(Q(s.version), Q(s.source));
			m_pFfmpegState->setStyleSheet("color: #70c070;");
			detail = tr("Path: %1").arg(Q(s.exe));
			{
				auto joined = [](const std::vector<std::string>& v) { QString t; for (const std::string& i : v) t += (t.isEmpty() ? QString() : QString(", ")) + Q(i); return t; };
				detail += tr("\nMissing (recommended): %1").arg(s.missingRecommended.empty() ? tr("nothing") : joined(s.missingRecommended));
				detail += tr("\nMissing for import: %1").arg(s.missingImport.empty() ? tr("nothing") : joined(s.missingImport));
			}
			if (s.gpl || s.nonfree)
				detail += tr("\nThis build is GPL / nonfree licensed (it is only run as a separate program).");
			break;
		case SFfmpegStatus::EState::Absent:
			state = tr("FFmpeg not available: %1").arg(Q(s.message));
			m_pFfmpegState->setStyleSheet("color: #e0a030;");
			detail = tr("Only the operations that need FFmpeg are disabled (import of mp4 / mov / mkv / avi / PNG sequences, re-encode, variants); WebM import, audio split, WAV and Ogg work without it.");
			break;
		}
		for (const std::string& r : s.rejected)
			detail += "\n" + tr("Not usable: %1").arg(Q(r));
		detail += tr("\nConfigured path (Preferences > %1): %2").arg(QString::fromUtf8(kPreferencesPagePath), s.configuredPath.empty() ? tr("(none)") : Q(s.configuredPath));
		const std::string disabled = CFfmpegService::DownloadDisabledReason();
		detail += disabled.empty() ? tr("\nDownload: our minimal build is available.") : tr("\nDownload: %1").arg(Q(disabled));
		m_pFfmpegState->setText(state);
		m_pFfmpegDetail->setText(detail);
		m_pFfmpegDownload->setEnabled(disabled.empty() && !pService->DownloadRunning());
		m_pFfmpegDownload->setToolTip(disabled.empty() ? tr("Downloads the pinned minimal LGPL build; the SHA-256 is verified before it is installed.") : Q(disabled));
		const bool has = m_pEditor && m_pEditor->HasAsset();
		for (QPushButton* const b : { m_pBtnVariants, m_pBtnRecompress })
		{
			if (b == nullptr)
				continue;
			b->setEnabled(has && !s.Pending());
			if (has && !s.Pending() && !s.Ready())
				b->setToolTip(tr("FFmpeg is not available: clicking asks for it. ") + Q(s.message));
		}
	}

	void CVideoInfoPanel::FillDescriptor()
	{
		const CVideoEditor::SState& st = m_pEditor->State();
		m_edit = st.descriptor;
		m_context.folderRel = st.folderRel;
		m_pTree->setEnabled(true);
		if (!m_pTree->attached())
			m_pTree->attach(yasli::Serializer(m_edit));
		else
			m_pTree->revert();
	}

	void CVideoInfoPanel::OnTreeChanged()
	{
		if (!m_pEditor || !m_pEditor->HasAsset())
			return;
		SDescriptor d = m_pEditor->State().descriptor; // keeps the video, the variants and the root triggers
		d.route = m_edit.route;
		d.avOffsetMs = m_edit.avOffsetMs;
		d.audioTracks.clear();
		for (const SDescriptor::SAudioTrack& a : m_edit.audioTracks)
		{
			if (!a.name.empty())
				d.audioTracks.push_back(a);
		}
		d.followGameLanguage = m_edit.followGameLanguage;
		d.defaultTrack = m_edit.defaultTrack;
		if (d.FindAudioTrack(d.defaultTrack) < 0)
			d.defaultTrack.clear();
		d.markers = m_edit.markers;
		std::stable_sort(d.markers.begin(), d.markers.end(), [](const SDescriptor::SMarker& a, const SDescriptor::SMarker& b) { return a.time < b.time; });
		m_pEditor->SetDescriptor(d); // marks the asset modified; Save writes the file
		if (!DescriptorsEqual(d, m_edit))
		{
			m_edit = d; // markers sorted, unnamed track rows dropped, default track checked
			m_pTree->revert();
		}
	}

	bool CVideoInfoPanel::ApplyThroughSerializationForTest(const SDescriptor& changed, std::string& error)
	{
		if (!m_pEditor || !m_pEditor->HasAsset() || gEnv == nullptr || gEnv->pSystem == nullptr || gEnv->pSystem->GetArchiveHost() == nullptr)
		{
			error = "no asset / no archive host";
			return false;
		}
		Serialization::IArchiveHost* const pHost = gEnv->pSystem->GetArchiveHost();
		SDescriptor source = changed;
		DynArray<char> buffer;
		if (!pHost->SaveJsonBuffer(buffer, Serialization::SStruct(source)))
		{
			error = "SaveJsonBuffer failed";
			return false;
		}
		if (!pHost->LoadJsonBuffer(Serialization::SStruct(m_edit), buffer.data(), buffer.size()))
		{
			error = "LoadJsonBuffer failed";
			return false;
		}
		OnTreeChanged();
		m_pTree->revert();
		return true;
	}

	void CVideoInfoPanel::RefreshJobs()
	{
		CJobRunner* const pRunner = CJobRunner::Get();
		if (pRunner == nullptr)
			return;
		const int selected = m_pJobs->currentItem() != nullptr ? m_pJobs->currentItem()->data(Qt::UserRole).toInt() : 0;
		m_pJobs->clear();
		for (const CJobRunner::SJobInfo& job : pRunner->Jobs())
		{
			const char* state = job.state == CJobRunner::EState::Running ? "running" : job.state == CJobRunner::EState::Succeeded ? "done"
				: job.state == CJobRunner::EState::Cancelled ? "cancelled" : "FAILED";
			QListWidgetItem* const item = new QListWidgetItem(QString("#%1 %2 - %3 %4%").arg(job.id).arg(job.title).arg(state).arg(static_cast<int>(job.progress * 100.f)));
			item->setData(Qt::UserRole, job.id);
			m_pJobs->addItem(item);
			if (job.id == selected)
				m_pJobs->setCurrentItem(item);
		}
	}

	void CVideoInfoPanel::OnJobChanged(int)
	{
		RefreshJobs();
	}

	void CVideoInfoPanel::OnJobLog(int id, const QString& line)
	{
		Log(QString("#%1: %2").arg(id).arg(line));
	}
}
