#include "StdAfx.h"
#include "Editor/InfoPanel.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QMessageBox>
#include <QDoubleSpinBox>
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
#include <QUrl>
#include <QVBoxLayout>

#include "Asset/VideoImport.h"
#include "Editor/VideoEditor.h"
#include "FFmpeg/FfmpegService.h"
#include "Jobs/JobRunner.h"
#include "Offline/IOfflineOps.h"

namespace CEVEditor
{
	namespace fs = std::filesystem;

	namespace
	{
		QString Q(const std::string& s) { return QString::fromUtf8(s.c_str()); }
		std::string S(const QString& q) { return std::string(q.toUtf8().constData()); }

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
	}

	CVideoInfoPanel::CVideoInfoPanel(CVideoEditor* pEditor, QWidget* pParent)
		: QWidget(pParent)
		, m_pEditor(pEditor)
	{
		setWindowTitle(tr("Info"));
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

		// ---- FFmpeg
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
		QPushButton* const pLocate = new QPushButton(tr("Locate..."), pFfmpegBox);
		pLocate->setToolTip(tr("Choose ffmpeg.exe (stored in %LOCALAPPDATA%/CEVPlayer/editor.json). It needs libvpx-vp9, libopus and the webm muxer."));
		m_pFfmpegDownload = new QPushButton(tr("Download minimal build"), pFfmpegBox);
		QPushButton* const pRedetect = new QPushButton(tr("Re-detect"), pFfmpegBox);
		pFfmpegButtons->addWidget(pLocate);
		pFfmpegButtons->addWidget(m_pFfmpegDownload);
		pFfmpegButtons->addWidget(pRedetect);
		pFfmpegButtons->addStretch(1);
		pFfmpegLayout->addLayout(pFfmpegButtons);
		connect(pLocate, &QPushButton::clicked, this, [this]()
		{
			CFfmpegService* const pService = CFfmpegService::Get();
			if (pService == nullptr)
				return;
			const QString file = QFileDialog::getOpenFileName(this, tr("Choose ffmpeg.exe"), QString::fromUtf8(pService->ConfiguredPath().c_str()), tr("ffmpeg (ffmpeg.exe);;All files (*)"));
			if (!file.isEmpty())
				pService->SetPath(std::string(file.toUtf8().constData()));
		});
		connect(m_pFfmpegDownload, &QPushButton::clicked, this, [this]()
		{
			CFfmpegService* const pService = CFfmpegService::Get();
			if (pService == nullptr)
				return;
			std::string error;
			if (pService->StartDownload(error) == 0)
				QMessageBox::warning(this, tr("FFmpeg"), QString::fromUtf8(error.c_str()));
			else
				Log(tr("Downloading the minimal FFmpeg build (progress in the job list) ..."));
		});
		connect(pRedetect, &QPushButton::clicked, this, [this]()
		{
			if (CFfmpegService* const pService = CFfmpegService::Get())
				pService->StartDetect(true);
		});
		pMain->addWidget(pFfmpegBox);

		// ---- descriptor
		QGroupBox* const pDescBox = new QGroupBox(tr("Descriptor (.cevvideo)"), pContent);
		QVBoxLayout* const pDescLayout = new QVBoxLayout(pDescBox);
		QGridLayout* const pGrid = new QGridLayout();
		pGrid->addWidget(new QLabel(tr("Audio route"), pDescBox), 0, 0);
		m_pRoute = new QComboBox(pDescBox);
		m_pRoute->addItems({ "embedded", "mixed", "cue" });
		pGrid->addWidget(m_pRoute, 0, 1);
		pGrid->addWidget(new QLabel(tr("A/V offset (ms)"), pDescBox), 1, 0);
		m_pAvOffset = new QDoubleSpinBox(pDescBox);
		m_pAvOffset->setRange(-5000.0, 5000.0);
		m_pAvOffset->setDecimals(1);
		m_pAvOffset->setToolTip(tr("Positive: the picture is shown later than the sound by this much."));
		pGrid->addWidget(m_pAvOffset, 1, 1);
		pGrid->addWidget(new QLabel(tr("Default language"), pDescBox), 2, 0);
		m_pDefaultLanguage = new QComboBox(pDescBox);
		pGrid->addWidget(m_pDefaultLanguage, 2, 1);
		pDescLayout->addLayout(pGrid);

		pDescLayout->addWidget(new QLabel(tr("Languages"), pDescBox));
		m_pLanguages = new QTableWidget(0, 6, pDescBox);
		m_pLanguages->setHorizontalHeaderLabels({ tr("Name"), tr("Embedded"), tr("File"), tr("Trigger"), tr("Pause"), tr("Resume") });
		m_pLanguages->horizontalHeader()->setStretchLastSection(true);
		m_pLanguages->setMinimumHeight(90);
		pDescLayout->addWidget(m_pLanguages);
		QHBoxLayout* const pLangButtons = new QHBoxLayout();
		QPushButton* const pAddLang = new QPushButton(tr("Add language"), pDescBox);
		QPushButton* const pRemoveLang = new QPushButton(tr("Remove"), pDescBox);
		pLangButtons->addWidget(pAddLang);
		pLangButtons->addWidget(pRemoveLang);
		pLangButtons->addStretch(1);
		pDescLayout->addLayout(pLangButtons);
		connect(pAddLang, &QPushButton::clicked, this, [this]() { AddLanguageRow(QString(), false, QString(), QString(), QString(), QString()); });
		connect(pRemoveLang, &QPushButton::clicked, this, [this]()
		{
			const int row = m_pLanguages->currentRow();
			if (row >= 0)
				m_pLanguages->removeRow(row);
		});

		pDescLayout->addWidget(new QLabel(tr("Markers"), pDescBox));
		m_pMarkers = new QTableWidget(0, 2, pDescBox);
		m_pMarkers->setHorizontalHeaderLabels({ tr("Name"), tr("Time (s)") });
		m_pMarkers->horizontalHeader()->setStretchLastSection(true);
		m_pMarkers->setMinimumHeight(70);
		pDescLayout->addWidget(m_pMarkers);
		QHBoxLayout* const pMarkerButtons = new QHBoxLayout();
		QPushButton* const pAddMarker = new QPushButton(tr("Add marker"), pDescBox);
		QPushButton* const pRemoveMarker = new QPushButton(tr("Remove"), pDescBox);
		pMarkerButtons->addWidget(pAddMarker);
		pMarkerButtons->addWidget(pRemoveMarker);
		pMarkerButtons->addStretch(1);
		pDescLayout->addLayout(pMarkerButtons);
		connect(pAddMarker, &QPushButton::clicked, this, [this]() { AddMarkerRow(QString(), 0.0); });
		connect(pRemoveMarker, &QPushButton::clicked, this, [this]()
		{
			const int row = m_pMarkers->currentRow();
			if (row >= 0)
				m_pMarkers->removeRow(row);
		});

		QPushButton* const pApply = new QPushButton(tr("Apply to descriptor (then Save)"), pDescBox);
		connect(pApply, &QPushButton::clicked, this, &CVideoInfoPanel::OnApplyDescriptor);
		pDescLayout->addWidget(pApply);
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
		auto addButton = [&](const QString& text, const QString& tip, int row, int col, std::function<void()> fn)
		{
			QPushButton* const b = new QPushButton(text, m_pOpButtons);
			b->setToolTip(tip);
			connect(b, &QPushButton::clicked, this, [fn]() { fn(); });
			pOps->addWidget(b, row, col);
		};
		auto language = [this]() -> std::string
		{
			if (!m_pEditor)
				return "english";
			const SDescriptor& d = m_pEditor->State().descriptor;
			if (!d.defaultLanguage.empty())
				return d.defaultLanguage;
			return d.languages.empty() ? std::string("english") : d.languages.front().name;
		};
		addButton(tr("Split audio to WebM"), tr("Writes the audio track as <name>_<language>.webm (no re-encoding) and lists it as a language file."), 0, 0, [this, language]()
		{
			if (!m_pEditor) return;
			SAudioOps ops; ops.split = true; ops.language = language();
			m_pEditor->RunAudioOps(ops);
		});
		addButton(tr("Export WAV"), tr("48 kHz / 16 bit WAV whose first sample is video second 0, stored as an asset file."), 0, 1, [this, language]()
		{
			if (!m_pEditor) return;
			SAudioOps ops; ops.wav = true; ops.language = language();
			m_pEditor->RunAudioOps(ops);
		});
		addButton(tr("Export Ogg"), tr("Ogg Vorbis of the audio track, stored as an asset file."), 1, 0, [this, language]()
		{
			if (!m_pEditor) return;
			SAudioOps ops; ops.ogg = true; ops.language = language();
			m_pEditor->RunAudioOps(ops);
		});
		addButton(tr("Strip audio"), tr("Writes <name>_silent.webm (picture only) and makes the descriptor play it."), 1, 1, [this, language]()
		{
			if (!m_pEditor) return;
			SAudioOps ops; ops.strip = true; ops.language = language();
			m_pEditor->RunAudioOps(ops);
		});
		addButton(tr("Upgrade to cue route"), tr("Needs a WAV / Ogg of the asset: registers it with SDL_mixer / ACE and switches the descriptor to the cue route."), 2, 0, [this]()
		{
			if (m_pEditor)
				m_pEditor->UpgradeToCue();
		});
		addButton(tr("Reimport source"), tr("Copies the original file over the copy of the source again (hash and details are renewed)."), 2, 1, [this]()
		{
			if (!m_pEditor) return;
			std::string error;
			if (m_pEditor->ReimportSource(error))
				Log(m_pEditor->State().details.Get(DetailKey::kTranscoded) == "1" ? tr("Reimport started: the source is copied again and encoded on a job.") : tr("Reimported the source."));
			else
				Log(tr("Reimport failed: %1").arg(Q(error)));
		});
		addButton(tr("Generate variants..."), tr("FFmpeg: smaller copies of the video (<name>_<height>p.webm) listed in the descriptor <Variants>."), 3, 0, [this]()
		{
			if (m_pEditor)
				m_pEditor->GenerateVariantsDialog();
		});
		addButton(tr("Re-compress / resize..."), tr("FFmpeg: encodes the video again from the stored source copy with another quality / height; the asset keeps its GUID."), 3, 1, [this]()
		{
			if (m_pEditor)
				m_pEditor->RecompressDialog();
		});
		addButton(tr("Open folder"), tr("Opens the folder of the asset."), 4, 0, [this]()
		{
			if (m_pEditor && m_pEditor->HasAsset())
				QDesktopServices::openUrl(QUrl::fromLocalFile(Q(AssetsAbsolute(m_pEditor->State().folderRel))));
		});
		pOpsLayout->addWidget(m_pOpButtons);
		for (QPushButton* const b : m_pOpButtons->findChildren<QPushButton*>())
		{
			if (b->text() == tr("Generate variants..."))
				m_pBtnVariants = b;
			else if (b->text() == tr("Re-compress / resize..."))
				m_pBtnRecompress = b;
		}

		m_pAce = new QLabel(pOpsBox);
		m_pAce->setWordWrap(true);
		m_pAce->setTextInteractionFlags(Qt::TextSelectableByMouse);
		pOpsLayout->addWidget(m_pAce);

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
		pOpsLayout->addWidget(m_pLog);
		pMain->addWidget(pOpsBox);
		pMain->addStretch(1);
	}

	void CVideoInfoPanel::Log(const QString& line)
	{
		m_pLog->appendPlainText(line);
	}

	void CVideoInfoPanel::AddLanguageRow(const QString& name, bool embedded, const QString& file, const QString& trigger, const QString& pause, const QString& resume)
	{
		const int row = m_pLanguages->rowCount();
		m_pLanguages->insertRow(row);
		m_pLanguages->setItem(row, 0, new QTableWidgetItem(name));
		QTableWidgetItem* const check = new QTableWidgetItem();
		check->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
		check->setCheckState(embedded ? Qt::Checked : Qt::Unchecked);
		m_pLanguages->setItem(row, 1, check);
		m_pLanguages->setItem(row, 2, new QTableWidgetItem(file));
		m_pLanguages->setItem(row, 3, new QTableWidgetItem(trigger));
		m_pLanguages->setItem(row, 4, new QTableWidgetItem(pause));
		m_pLanguages->setItem(row, 5, new QTableWidgetItem(resume));
	}

	void CVideoInfoPanel::AddMarkerRow(const QString& name, double time)
	{
		const int row = m_pMarkers->rowCount();
		m_pMarkers->insertRow(row);
		m_pMarkers->setItem(row, 0, new QTableWidgetItem(name));
		m_pMarkers->setItem(row, 1, new QTableWidgetItem(QString::number(time, 'f', 3)));
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
			m_pLanguages->setRowCount(0);
			m_pMarkers->setRowCount(0);
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
			const bool stale = st.source.originalExists && !st.source.originalMatchesImport;
			m_pSource->setText(Q(st.source.text));
			m_pSource->setStyleSheet(stale || !st.source.copyExists || !st.source.copyMatchesImport ? "color: #e0a030;" : "color: #70c070;");
		}
		else
		{
			m_pSource->setText(tr("Checking the source file ..."));
			m_pSource->setStyleSheet(QString());
		}

		const std::string ace = st.details.Get(DetailKey::kAceLibrary);
		if (!ace.empty())
		{
			m_pAce->setText(tr("SDL_mixer / ACE: library %1, copies %2\nWwise / FMOD: this plug-in only writes the SDL_mixer registration. Manual steps: import the WAV / Ogg of this asset into your middleware project, create one start / pause / resume event per language, and enter the trigger names of the Languages table above.")
				.arg(Q(ace)).arg(Q(st.details.Get(DetailKey::kSdlFiles))));
		}
		else
		{
			m_pAce->setText(tr("Not registered with SDL_mixer / ACE yet (Upgrade to cue route does it once the asset has a WAV / Ogg)."));
		}

		FillFiles();
		FillVariants();
		// The tables hold what the user is typing: refill them only when the descriptor in the state was replaced
		// (asset opened / reloaded / merged), not on every state change (e.g. when a background check finishes).
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
				role = tr("video (shipped), copy of the source");
			else if (rel == st.videoRel)
				role = tr("video (shipped)");
			else if (rel == st.sourceCopyRel)
				role = tr("source copy - original file, not shipped");
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
		detail += tr("\nConfigured path: %1").arg(s.configuredPath.empty() ? tr("(none)") : Q(s.configuredPath));
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
		m_filling = true;
		const SDescriptor& d = m_pEditor->State().descriptor;
		m_pRoute->setCurrentText(Q(d.route));
		m_pAvOffset->setValue(d.avOffsetMs);
		m_pLanguages->setRowCount(0);
		m_pDefaultLanguage->clear();
		m_pDefaultLanguage->addItem(QString());
		for (const SDescriptor::SLanguage& l : d.languages)
		{
			AddLanguageRow(Q(l.name), l.embedded, Q(l.file), Q(l.trigger), Q(l.pauseTrigger), Q(l.resumeTrigger));
			m_pDefaultLanguage->addItem(Q(l.name));
		}
		m_pDefaultLanguage->setCurrentText(Q(d.defaultLanguage));
		m_pLanguages->resizeColumnsToContents();
		m_pMarkers->setRowCount(0);
		for (const SDescriptor::SMarker& m : d.markers)
			AddMarkerRow(Q(m.name), m.time);
		m_filling = false;
	}

	void CVideoInfoPanel::OnApplyDescriptor()
	{
		if (!m_pEditor || !m_pEditor->HasAsset() || m_filling)
			return;
		SDescriptor d = m_pEditor->State().descriptor; // keeps the variants and the root triggers
		d.route = S(m_pRoute->currentText());
		d.avOffsetMs = m_pAvOffset->value();
		d.languages.clear();
		for (int row = 0; row < m_pLanguages->rowCount(); ++row)
		{
			auto text = [&](int col) { QTableWidgetItem* const it = m_pLanguages->item(row, col); return it ? S(it->text().trimmed()) : std::string(); };
			SDescriptor::SLanguage l;
			l.name = text(0);
			if (l.name.empty())
				continue;
			QTableWidgetItem* const check = m_pLanguages->item(row, 1);
			l.embedded = check != nullptr && check->checkState() == Qt::Checked;
			l.file = text(2);
			l.trigger = text(3);
			l.pauseTrigger = text(4);
			l.resumeTrigger = text(5);
			d.languages.push_back(l);
		}
		d.defaultLanguage = S(m_pDefaultLanguage->currentText());
		if (d.FindLanguage(d.defaultLanguage) < 0)
			d.defaultLanguage.clear();
		d.markers.clear();
		for (int row = 0; row < m_pMarkers->rowCount(); ++row)
		{
			QTableWidgetItem* const name = m_pMarkers->item(row, 0);
			QTableWidgetItem* const time = m_pMarkers->item(row, 1);
			if (name == nullptr || time == nullptr || name->text().trimmed().isEmpty())
				continue;
			SDescriptor::SMarker m;
			m.name = S(name->text().trimmed());
			m.time = time->text().toDouble();
			d.markers.push_back(m);
		}
		std::stable_sort(d.markers.begin(), d.markers.end(), [](const SDescriptor::SMarker& a, const SDescriptor::SMarker& b) { return a.time < b.time; });
		m_pEditor->SetDescriptor(d);
		Log(tr("Descriptor changed in memory: Save (Ctrl+S) writes it."));
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
