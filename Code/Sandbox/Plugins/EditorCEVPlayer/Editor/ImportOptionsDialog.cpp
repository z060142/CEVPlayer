#include "StdAfx.h"
#include "Editor/ImportOptionsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QThread>
#include <QVBoxLayout>

#include <Controls/QuestionDialog.h>
#include <ThreadingUtils.h>

#include "Editor/TranscodeDialog.h"
#include "FFmpeg/FfmpegService.h"

namespace CEVEditor
{
	CImportOptionsDialog::CImportOptionsDialog(const QString& sourceFile, const SImportOptions& initial, ESourceKind kind, bool batch, QWidget* pParent)
		: QDialog(pParent)
		, m_kind(kind)
		, m_batch(batch)
	{
		setWindowTitle(batch ? tr("Import videos") : (kind == ESourceKind::PngSequence ? tr("Import PNG sequence") : tr("Import video")));
		setMinimumWidth(480);
		QVBoxLayout* const pMain = new QVBoxLayout(this);
		pMain->addWidget(new QLabel(batch ? tr("Source: %1").arg(sourceFile) : tr("Source: %1").arg(QFileInfo(sourceFile).fileName()), this));

		QFormLayout* const pForm = new QFormLayout();
		m_pName = new QLineEdit(QString::fromUtf8(initial.assetName.c_str()), this);
		m_pName->setToolTip(tr("Asset name: letters, digits, underscore and dash only. The files of the asset start with it."));
		if (batch)
		{
			m_pName->setEnabled(false);
			m_pName->setPlaceholderText(tr("(the name of each file)"));
		}
		pForm->addRow(tr("Asset name"), m_pName);

		m_pAudioTrack = new QLineEdit(QString::fromUtf8(initial.audioTrack.c_str()), this);
		m_pAudioTrack->setToolTip(tr("Name of the audio track of the sound inside the video (1 to 63 of A-Z a-z 0-9 _ . -). Scripts choose tracks by this name; a language tag can be added later in the info panel."));
		pForm->addRow(tr("Audio track name"), m_pAudioTrack);

		m_pRoute = new QComboBox(this);
		m_pRoute->addItem(tr("embedded (the video plays its own audio)"), "embedded");
		m_pRoute->addItem(tr("mixed (external audio files per audio track)"), "mixed");
		m_pRoute->addItem(tr("cue (CryAudio trigger, needs a WAV / Ogg)"), "cue");
		m_pRoute->setCurrentIndex(std::max(0, m_pRoute->findData(QString::fromUtf8(initial.route.c_str()))));
		pForm->addRow(tr("Audio route"), m_pRoute);

		m_pTrigger = new QLineEdit(QString::fromUtf8(initial.trigger.c_str()), this);
		m_pTrigger->setPlaceholderText(tr("Play_<asset name>"));
		m_pTrigger->setToolTip(tr("Cue route: base of the ACE trigger names (Play_ / Pause_ / Resume_ are generated)."));
		pForm->addRow(tr("Trigger name"), m_pTrigger);
		if (batch)
		{
			m_pOverwrite = new QCheckBox(tr("Replace assets that already exist (otherwise those files are skipped)"), this);
			pForm->addRow(m_pOverwrite);
		}
		pMain->addLayout(pForm);

		QGroupBox* const pAudio = new QGroupBox(tr("Audio files (needs the offline processing library)"), this);
		QFormLayout* const pAudioForm = new QFormLayout(pAudio);
		m_pSplit = new QComboBox(pAudio);
		m_pSplit->addItem(tr("Do not split (audio stays inside the video)"), static_cast<int>(ESplitAudio::None));
		m_pSplit->addItem(tr("Split to an external WebM, keep the embedded track"), static_cast<int>(ESplitAudio::KeepEmbedded));
		m_pSplit->addItem(tr("Split to an external WebM, play a picture-only video"), static_cast<int>(ESplitAudio::StripEmbedded));
		m_pSplit->setCurrentIndex(std::max(0, m_pSplit->findData(static_cast<int>(initial.split))));
		pAudioForm->addRow(tr("Split audio"), m_pSplit);
		m_pWav = new QCheckBox(tr("Export a WAV (48 kHz, 16 bit)"), pAudio);
		m_pWav->setChecked(initial.wav);
		pAudioForm->addRow(m_pWav);
		m_pOgg = new QCheckBox(tr("Export an Ogg Vorbis"), pAudio);
		m_pOgg->setChecked(initial.ogg);
		pAudioForm->addRow(m_pOgg);
		m_pOggQuality = new QDoubleSpinBox(pAudio);
		m_pOggQuality->setRange(-0.1, 1.0);
		m_pOggQuality->setSingleStep(0.05);
		m_pOggQuality->setValue(static_cast<double>(initial.oggQuality));
		pAudioForm->addRow(tr("Ogg quality"), m_pOggQuality);
		pMain->addWidget(pAudio);

		// ---- FFmpeg stage
		m_pFfmpegBox = new QGroupBox(tr("Compression, resolution and sub-videos (FFmpeg)"), this);
		QVBoxLayout* const pFfLayout = new QVBoxLayout(m_pFfmpegBox);
		QHBoxLayout* const pStatusRow = new QHBoxLayout();
		m_pFfmpegStatus = new QLabel(m_pFfmpegBox);
		m_pFfmpegStatus->setWordWrap(true);
		pStatusRow->addWidget(m_pFfmpegStatus, 1);
		m_pFfmpegSetup = new QPushButton(tr("Set up FFmpeg..."), m_pFfmpegBox);
		pStatusRow->addWidget(m_pFfmpegSetup);
		pFfLayout->addLayout(pStatusRow);
		QFormLayout* const pFfForm = new QFormLayout();
		if (kind == ESourceKind::Webm)
		{
			m_pRecompress = new QCheckBox(tr("Re-encode the WebM (compression / resolution). Off: the file is copied as it is."), m_pFfmpegBox);
			m_pRecompress->setChecked(initial.recompress);
			pFfLayout->addWidget(m_pRecompress);
		}
		else
		{
			pFfLayout->addWidget(new QLabel(kind == ESourceKind::PngSequence
				? tr("The PNG files are encoded to <name>.webm (VP9; alpha is kept when the PNGs have it).")
				: tr("The video is copied into the asset folder as <name>_source.<ext> and encoded to <name>.webm (VP9 + Opus; alpha is kept when the source has it)."), m_pFfmpegBox));
		}
		m_pCrf = new QSpinBox(m_pFfmpegBox);
		m_pCrf->setRange(0, 63);
		m_pCrf->setValue(initial.crf);
		m_pCrf->setToolTip(tr("libvpx-vp9 constant quality: 0 = best / biggest, 63 = worst / smallest. 30-34 is a good range for games."));
		pFfForm->addRow(tr("Quality (CRF)"), m_pCrf);
		m_pHeight = new QSpinBox(m_pFfmpegBox);
		m_pHeight->setRange(0, 8640);
		m_pHeight->setSpecialValueText(tr("keep source size"));
		m_pHeight->setSuffix(tr(" px"));
		m_pHeight->setValue(initial.height);
		m_pHeight->setToolTip(tr("Output height of the main video (the width follows, kept even)."));
		pFfForm->addRow(tr("Output height"), m_pHeight);
		if (kind == ESourceKind::PngSequence)
		{
			m_pFps = new QDoubleSpinBox(m_pFfmpegBox);
			m_pFps->setRange(1.0, 240.0);
			m_pFps->setDecimals(3);
			m_pFps->setValue(initial.fps);
			pFfForm->addRow(tr("Frame rate"), m_pFps);
		}
		pFfLayout->addLayout(pFfForm);
		pFfLayout->addWidget(new QLabel(tr("Variants (smaller copies for low resolutions; heights not below the video are skipped):"), m_pFfmpegBox));
		m_pVariants = new CVariantPicker(m_pFfmpegBox);
		m_pVariants->SetHeights(initial.variants);
		pFfLayout->addWidget(m_pVariants);
		pMain->addWidget(m_pFfmpegBox);

		QDialogButtonBox* const pButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(pButtons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(pButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		pMain->addWidget(pButtons);

		connect(m_pFfmpegSetup, &QPushButton::clicked, this, [this]()
		{
			if (CFfmpegService* const pService = CFfmpegService::Get())
				pService->EnsureUsableModal(this, tr("Set up the FFmpeg that imports with compression, other formats and variants use."));
			UpdateFfmpegUi();
		});
		if (m_pRecompress != nullptr)
			connect(m_pRecompress, &QCheckBox::toggled, this, &CImportOptionsDialog::UpdateFfmpegUi);
		if (CFfmpegService* const pService = CFfmpegService::Get())
		{
			connect(pService, &CFfmpegService::Changed, this, &CImportOptionsDialog::UpdateFfmpegUi);
			pService->EnsureDetected();
		}
		UpdateFfmpegUi();
	}

	void CImportOptionsDialog::UpdateFfmpegUi()
	{
		CFfmpegService* const pService = CFfmpegService::Get();
		const SFfmpegStatus s = pService != nullptr ? pService->Status() : SFfmpegStatus();
		const bool needed = m_kind != ESourceKind::Webm;
		if (s.Ready())
		{
			m_pFfmpegStatus->setText(tr("FFmpeg: %1 (%2)").arg(QString::fromUtf8(s.version.c_str()), QString::fromUtf8(s.source.c_str())));
			m_pFfmpegStatus->setStyleSheet("color: #70c070;");
		}
		else if (s.Pending())
		{
			m_pFfmpegStatus->setText(tr("FFmpeg: looking for it ..."));
			m_pFfmpegStatus->setStyleSheet(QString());
		}
		else
		{
			m_pFfmpegStatus->setText(needed ? tr("FFmpeg is needed for this file and was not found. OK asks how to get it.") : tr("FFmpeg was not found: re-encoding and variants are not available."));
			m_pFfmpegStatus->setStyleSheet("color: #e0a030;");
		}
		m_pFfmpegSetup->setVisible(!s.Ready() && !s.Pending());
		// WebM files import without FFmpeg: its controls only work when there is one. Other sources always need it (OK sets it up).
		const bool usable = needed || s.Ready();
		const bool encodeControls = usable && (needed || (m_pRecompress != nullptr && m_pRecompress->isChecked()));
		if (m_pRecompress != nullptr)
			m_pRecompress->setEnabled(s.Ready());
		m_pCrf->setEnabled(encodeControls);
		m_pHeight->setEnabled(encodeControls);
		if (m_pFps != nullptr)
			m_pFps->setEnabled(usable);
		m_pVariants->setEnabled(usable);
	}

	SImportOptions CImportOptionsDialog::Options() const
	{
		SImportOptions o;
		o.assetName = m_pName->text().trimmed().toUtf8().constData();
		o.audioTrack = m_pAudioTrack->text().trimmed().toUtf8().constData();
		o.route = m_pRoute->currentData().toString().toUtf8().constData();
		o.trigger = m_pTrigger->text().trimmed().toUtf8().constData();
		o.split = static_cast<ESplitAudio>(m_pSplit->currentData().toInt());
		o.wav = m_pWav->isChecked();
		o.ogg = m_pOgg->isChecked();
		o.oggQuality = static_cast<float>(m_pOggQuality->value());
		o.recompress = m_pRecompress != nullptr && m_pRecompress->isEnabled() && m_pRecompress->isChecked();
		o.crf = m_pCrf->value();
		o.height = m_pHeight->isEnabled() ? m_pHeight->value() : 0;
		if (m_pVariants->isEnabled())
			o.variants = m_pVariants->Heights();
		if (m_pFps != nullptr)
			o.fps = m_pFps->value();
		o.overwrite = m_pOverwrite != nullptr && m_pOverwrite->isChecked();
		return o;
	}

	bool CImportOptionsDialog::Ask(const QString& sourceFile, SImportOptions& options, const std::string& folderRel, const std::string& alreadyConfirmedName, ESourceKind kind, bool batch)
	{
		auto run = [sourceFile, folderRel, alreadyConfirmedName, kind, batch](SImportOptions in) -> std::pair<bool, SImportOptions>
		{
			for (;;)
			{
				CImportOptionsDialog dialog(sourceFile, in, kind, batch, QApplication::activeWindow());
				if (dialog.exec() != QDialog::Accepted)
					return { false, in };
				SImportOptions chosen = dialog.Options();
				if (!batch)
				{
					// The name can be changed in the dialog after the engine checked the original one: look again, and never
					// replace an existing asset without the user's explicit yes.
					const SImportPlan plan = PlanImport(std::string(sourceFile.toUtf8().constData()), folderRel, chosen);
					if (!plan.ok)
					{
						CQuestionDialog::SWarning(tr("Import video"), QString::fromUtf8(plan.error.c_str()), QDialogButtonBox::Ok, QDialogButtonBox::Ok, QApplication::activeWindow());
						in = chosen;
						continue;
					}
					chosen.overwrite = false;
					if (plan.exists && plan.assetName != alreadyConfirmedName)
					{
						const QDialogButtonBox::StandardButton answer = CQuestionDialog::SWarning(tr("Import video"),
							QString::fromUtf8(plan.detail.c_str()) + tr("\n\nReplace it?"), QDialogButtonBox::Yes | QDialogButtonBox::No, QDialogButtonBox::No, QApplication::activeWindow());
						if (answer != QDialogButtonBox::Yes)
						{
							in = chosen; // back to the dialog: pick another name
							continue;
						}
					}
					chosen.overwrite = plan.exists;
				}
				// compression, other formats, variants: FFmpeg has to be there (asks: download / choose ffmpeg.exe)
				if (chosen.NeedsFfmpeg(kind))
				{
					CFfmpegService* const pService = CFfmpegService::Get();
					const QString why = kind == ESourceKind::Webm ? tr("Re-encoding a WebM and generating variants need FFmpeg.")
						: tr("Importing this kind of file needs FFmpeg (it converts it to WebM).");
					if (pService == nullptr || !pService->EnsureUsableModal(QApplication::activeWindow(), why))
					{
						in = chosen; // back to the dialog: change the options (WebM without compression needs no FFmpeg)
						continue;
					}
				}
				return { true, chosen };
			}
		};

		std::pair<bool, SImportOptions> result;
		if (QThread::currentThread() == QCoreApplication::instance()->thread())
			result = run(options); // already on the main thread: waiting for a post to ourselves would never return
		else
			result = ThreadingUtils::PostOnMainThread(run, options).get();
		if (result.first)
			options = result.second;
		return result.first;
	}
}
