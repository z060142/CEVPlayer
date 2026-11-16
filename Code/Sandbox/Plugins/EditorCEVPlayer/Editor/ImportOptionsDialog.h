// Options of one video import (shown by the importer and by the editor window's drop handler; main thread only).
#pragma once

#include <QDialog>

#include "Asset/VideoImport.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;

namespace CEVEditor
{
	class CImportOptionsDialog : public QDialog
	{
		Q_OBJECT
	public:
		CImportOptionsDialog(const QString& sourceFile, const SImportOptions& initial, QWidget* pParent = nullptr);
		SImportOptions Options() const;

		//! Shows the dialog on the main thread and returns false when the user cancelled. Safe to call from any thread:
		//! on a background thread (the engine runs importers there) the dialog is posted to the main thread and this
		//! call waits for its result; on the main thread it simply runs. NEVER wait for the main thread while the main
		//! thread waits for you (docs/research/04 §2.6, "PostOnMainThread().get() deadlock").
		//! `folderRel` = destination folder. After OK the target is checked again (the user may have renamed the asset):
		//! when an asset of that name exists the user is asked and may refuse (the dialog comes back); a yes sets
		//! options.overwrite. `alreadyConfirmedName` = name the engine's own import dialog already asked about.
		static bool Ask(const QString& sourceFile, SImportOptions& options, const std::string& folderRel, const std::string& alreadyConfirmedName = std::string());

	private:
		QLineEdit* m_pName = nullptr;
		QLineEdit* m_pLanguage = nullptr;
		QComboBox* m_pRoute = nullptr;
		QLineEdit* m_pTrigger = nullptr;
		QComboBox* m_pSplit = nullptr;
		QCheckBox* m_pWav = nullptr;
		QCheckBox* m_pOgg = nullptr;
		QDoubleSpinBox* m_pOggQuality = nullptr;
	};
}
