// Options of one video import (shown by the importer and by the editor window's drop handler; main thread only).
#pragma once

#include <QDialog>

#include "Asset/VideoImport.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace CEVEditor
{
	class CVariantPicker;

	class CImportOptionsDialog : public QDialog
	{
		Q_OBJECT
	public:
		//! `kind` decides which FFmpeg controls exist (re-encode box for WebM, frame rate for a PNG sequence). `batch`: the options
		//! apply to several files (the name comes from each file, existing assets are skipped unless "replace" is checked).
		CImportOptionsDialog(const QString& sourceFile, const SImportOptions& initial, ESourceKind kind, bool batch, QWidget* pParent = nullptr);
		SImportOptions Options() const;

		//! Shows the dialog on the main thread and returns false when the user cancelled. Safe to call from any thread:
		//! on a background thread (the engine runs importers there) the dialog is posted to the main thread and this
		//! call waits for its result; on the main thread it simply runs. NEVER wait for the main thread while the main
		//! thread waits for you ("PostOnMainThread().get() deadlock").
		//! `folderRel` = destination folder. After OK the target is checked again (the user may have renamed the asset):
		//! when an asset of that name exists the user is asked and may refuse (the dialog comes back); a yes sets
		//! options.overwrite. `alreadyConfirmedName` = name the engine's own import dialog already asked about.
		//! When the chosen options need FFmpeg and none is usable, the "download / choose ffmpeg.exe" flow runs before this returns true.
		//! `batch`: `sourceFile` is a label ("3 files"), no name / overwrite check.
		static bool Ask(const QString& sourceFile, SImportOptions& options, const std::string& folderRel, const std::string& alreadyConfirmedName = std::string(),
			ESourceKind kind = ESourceKind::Webm, bool batch = false);

	private slots:
		void UpdateFfmpegUi();

	private:
		ESourceKind m_kind;
		bool m_batch;
		QLineEdit* m_pName = nullptr;
		QLineEdit* m_pAudioTrack = nullptr;
		QComboBox* m_pRoute = nullptr;
		QLineEdit* m_pTrigger = nullptr;
		QComboBox* m_pSplit = nullptr;
		QCheckBox* m_pWav = nullptr;
		QCheckBox* m_pOgg = nullptr;
		QDoubleSpinBox* m_pOggQuality = nullptr;
		QCheckBox* m_pOverwrite = nullptr;
		// FFmpeg stage
		QGroupBox* m_pFfmpegBox = nullptr;
		QLabel* m_pFfmpegStatus = nullptr;
		QPushButton* m_pFfmpegSetup = nullptr;
		QCheckBox* m_pRecompress = nullptr;
		QSpinBox* m_pCrf = nullptr;
		QSpinBox* m_pHeight = nullptr;
		CVariantPicker* m_pVariants = nullptr;
		QDoubleSpinBox* m_pFps = nullptr;
	};
}
