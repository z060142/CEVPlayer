#include "StdAfx.h"
#include "Editor/ImportOptionsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QThread>
#include <QVBoxLayout>

#include <ThreadingUtils.h>

namespace CEVEditor
{
	CImportOptionsDialog::CImportOptionsDialog(const QString& sourceFile, const SImportOptions& initial, QWidget* pParent)
		: QDialog(pParent)
	{
		setWindowTitle(tr("Import video"));
		setMinimumWidth(460);
		QVBoxLayout* const pMain = new QVBoxLayout(this);
		pMain->addWidget(new QLabel(tr("Source: %1").arg(QFileInfo(sourceFile).fileName()), this));

		QFormLayout* const pForm = new QFormLayout();
		m_pName = new QLineEdit(QString::fromUtf8(initial.assetName.c_str()), this);
		m_pName->setToolTip(tr("Asset name: letters, digits, underscore and dash only. The files of the asset start with it."));
		pForm->addRow(tr("Asset name"), m_pName);

		m_pLanguage = new QLineEdit(QString::fromUtf8(initial.language.c_str()), this);
		m_pLanguage->setToolTip(tr("Name of the language of the audio track inside the video."));
		pForm->addRow(tr("Audio language"), m_pLanguage);

		m_pRoute = new QComboBox(this);
		m_pRoute->addItem(tr("embedded (the video plays its own audio)"), "embedded");
		m_pRoute->addItem(tr("mixed (external audio files per language)"), "mixed");
		m_pRoute->addItem(tr("cue (CryAudio trigger, needs a WAV / Ogg)"), "cue");
		m_pRoute->setCurrentIndex(std::max(0, m_pRoute->findData(QString::fromUtf8(initial.route.c_str()))));
		pForm->addRow(tr("Audio route"), m_pRoute);

		m_pTrigger = new QLineEdit(QString::fromUtf8(initial.trigger.c_str()), this);
		m_pTrigger->setPlaceholderText(tr("Play_<asset name>"));
		m_pTrigger->setToolTip(tr("Cue route: base of the ACE trigger names (Play_ / Pause_ / Resume_ are generated)."));
		pForm->addRow(tr("Trigger name"), m_pTrigger);
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

		QGroupBox* const pLater = new QGroupBox(tr("Compression, resolution and sub-videos (FFmpeg stage)"), this);
		QVBoxLayout* const pLaterLayout = new QVBoxLayout(pLater);
		pLaterLayout->addWidget(new QLabel(tr("Not available yet: WebM files are copied as they are."), pLater));
		pLater->setEnabled(false);
		pMain->addWidget(pLater);

		QDialogButtonBox* const pButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(pButtons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(pButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		pMain->addWidget(pButtons);
	}

	SImportOptions CImportOptionsDialog::Options() const
	{
		SImportOptions o;
		o.assetName = m_pName->text().trimmed().toUtf8().constData();
		o.language = m_pLanguage->text().trimmed().toUtf8().constData();
		o.route = m_pRoute->currentData().toString().toUtf8().constData();
		o.trigger = m_pTrigger->text().trimmed().toUtf8().constData();
		o.split = static_cast<ESplitAudio>(m_pSplit->currentData().toInt());
		o.wav = m_pWav->isChecked();
		o.ogg = m_pOgg->isChecked();
		o.oggQuality = static_cast<float>(m_pOggQuality->value());
		return o;
	}

	bool CImportOptionsDialog::Ask(const QString& sourceFile, SImportOptions& options)
	{
		auto run = [sourceFile](SImportOptions in) -> std::pair<bool, SImportOptions>
		{
			CImportOptionsDialog dialog(sourceFile, in, QApplication::activeWindow());
			if (dialog.exec() != QDialog::Accepted)
				return { false, in };
			return { true, dialog.Options() };
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
