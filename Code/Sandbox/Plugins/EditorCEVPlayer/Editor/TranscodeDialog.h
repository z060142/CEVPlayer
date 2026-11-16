// Widgets of the FFmpeg stage: the list of variant heights (shared by the import dialog) and the dialog of the operations
// "Generate variants..." and "Re-compress / resize..." on an existing asset. Main thread only.
#pragma once

#include <vector>

#include <QDialog>
#include <QWidget>

#include "Asset/VideoImport.h"

class QCheckBox;
class QLineEdit;
class QSpinBox;

namespace CEVEditor
{
	//! Check boxes for 2160 / 1440 / 1080 / 720 / 540 / 360 (and a line for other heights). Heights not below the source
	//! height are disabled once the source height is known (SetSourceHeight); unknown (0): all can be chosen and the job
	//! skips the ones that are not below the video.
	class CVariantPicker : public QWidget
	{
		Q_OBJECT
	public:
		explicit CVariantPicker(QWidget* pParent = nullptr);
		void SetSourceHeight(int height);
		void SetHeights(const std::vector<int>& heights);
		std::vector<int> Heights() const;

	private:
		std::vector<QCheckBox*> m_boxes;
		std::vector<int> m_boxHeights;
		QLineEdit* m_pOther = nullptr;
	};

	class CTranscodeDialog : public QDialog
	{
		Q_OBJECT
	public:
		enum class EMode { Variants, Recompress };
		//! `sourceHeight` = height of the asset's video (0 = unknown), `existing` = heights already in the descriptor (checked at start).
		CTranscodeDialog(EMode mode, const QString& assetName, int sourceHeight, const std::vector<int>& existing, int crf, int height, QWidget* pParent = nullptr);
		STranscodeOps Ops() const;

		//! False when cancelled. Fills `ops` (recompress / changeVariants / removeOtherVariants set by the mode).
		static bool Ask(EMode mode, const QString& assetName, int sourceHeight, const std::vector<int>& existing, int crf, int height, STranscodeOps& ops, QWidget* pParent = nullptr);

	private:
		EMode m_mode;
		QSpinBox* m_pCrf = nullptr;
		QSpinBox* m_pHeight = nullptr;
		CVariantPicker* m_pVariants = nullptr;
	};
}
