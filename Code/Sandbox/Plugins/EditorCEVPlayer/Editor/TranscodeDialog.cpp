#include "StdAfx.h"
#include "Editor/TranscodeDialog.h"

#include <algorithm>
#include <functional>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QVBoxLayout>

namespace CEVEditor
{
	CVariantPicker::CVariantPicker(QWidget* pParent)
		: QWidget(pParent)
	{
		QVBoxLayout* const pMain = new QVBoxLayout(this);
		pMain->setContentsMargins(0, 0, 0, 0);
		QGridLayout* const pGrid = new QGridLayout();
		int i = 0;
		for (int h : StandardVariantHeights())
		{
			QCheckBox* const pBox = new QCheckBox(tr("%1p").arg(h), this);
			pBox->setToolTip(tr("Writes <name>_%1p.webm and lists it in the descriptor <Variants>. Skipped when the video is not taller than this.").arg(h));
			pGrid->addWidget(pBox, i / 3, i % 3);
			m_boxes.push_back(pBox);
			m_boxHeights.push_back(h);
			++i;
		}
		pMain->addLayout(pGrid);
		m_pOther = new QLineEdit(this);
		m_pOther->setPlaceholderText(tr("other heights, comma separated (e.g. 480,240)"));
		pMain->addWidget(m_pOther);
	}

	void CVariantPicker::SetSourceHeight(int height)
	{
		for (size_t i = 0; i < m_boxes.size(); ++i)
		{
			const bool ok = height <= 0 || m_boxHeights[i] < height;
			m_boxes[i]->setEnabled(ok);
			if (!ok)
				m_boxes[i]->setChecked(false);
		}
	}

	void CVariantPicker::SetHeights(const std::vector<int>& heights)
	{
		QStringList other;
		for (size_t i = 0; i < m_boxes.size(); ++i)
			m_boxes[i]->setChecked(std::find(heights.begin(), heights.end(), m_boxHeights[i]) != heights.end());
		for (int h : heights)
		{
			if (std::find(m_boxHeights.begin(), m_boxHeights.end(), h) == m_boxHeights.end())
				other << QString::number(h);
		}
		m_pOther->setText(other.join(","));
	}

	std::vector<int> CVariantPicker::Heights() const
	{
		std::vector<int> out;
		for (size_t i = 0; i < m_boxes.size(); ++i)
		{
			if (m_boxes[i]->isEnabled() && m_boxes[i]->isChecked())
				out.push_back(m_boxHeights[i]);
		}
		std::vector<int> extra;
		std::string error;
		if (ParseHeightList(std::string(m_pOther->text().toUtf8().constData()), extra, error))
		{
			for (int h : extra)
			{
				if (std::find(out.begin(), out.end(), h) == out.end())
					out.push_back(h);
			}
		}
		std::sort(out.begin(), out.end(), std::greater<int>());
		return out;
	}

	// ---------------------------------------------------------------------------------------------------------------
	CTranscodeDialog::CTranscodeDialog(EMode mode, const QString& assetName, int sourceHeight, const std::vector<int>& existing, int crf, int height, QWidget* pParent)
		: QDialog(pParent)
		, m_mode(mode)
	{
		setWindowTitle(mode == EMode::Variants ? tr("Generate variants") : tr("Re-compress / resize"));
		setMinimumWidth(420);
		QVBoxLayout* const pMain = new QVBoxLayout(this);
		QString intro = mode == EMode::Variants
			? tr("Asset %1: smaller copies of the video for lower resolutions (variants). The runtime picks the smallest one that is not below the output size.")
			: tr("Asset %1: re-encodes the video from the stored source copy (or from the current video when the asset has none) and replaces it; the asset keeps its GUID.");
		if (sourceHeight > 0)
			intro += tr("\nVideo height: %1 px.").arg(sourceHeight);
		QLabel* const pIntro = new QLabel(intro.arg(assetName), this);
		pIntro->setWordWrap(true);
		pMain->addWidget(pIntro);

		QFormLayout* const pForm = new QFormLayout();
		m_pCrf = new QSpinBox(this);
		m_pCrf->setRange(0, 63);
		m_pCrf->setValue(crf);
		m_pCrf->setToolTip(tr("libvpx-vp9 constant quality: 0 = best / biggest, 63 = worst / smallest. 30-34 is a good range for games."));
		m_pHeight = new QSpinBox(this);
		m_pHeight->setRange(0, 8640);
		m_pHeight->setSpecialValueText(tr("keep source size"));
		m_pHeight->setSuffix(tr(" px"));
		m_pHeight->setValue(height);
		m_pHeight->setToolTip(tr("Output height (the width follows, kept even)."));
		pForm->addRow(tr("Quality (CRF)"), m_pCrf);
		pForm->addRow(tr("Output height"), m_pHeight);
		if (mode == EMode::Variants)
		{
			// the variants are encoded with the quality of this dialog; the height of the main video does not change
			m_pHeight->setEnabled(false);
			m_pHeight->setValue(0);
		}
		pMain->addLayout(pForm);

		pMain->addWidget(new QLabel(mode == EMode::Variants ? tr("Variants to generate (the others stay as they are):") : tr("Variants to have afterwards (the unchecked ones are removed):"), this));
		m_pVariants = new CVariantPicker(this);
		m_pVariants->SetSourceHeight(sourceHeight);
		m_pVariants->SetHeights(existing);
		pMain->addWidget(m_pVariants);

		QDialogButtonBox* const pButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(pButtons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(pButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		pMain->addWidget(pButtons);
	}

	STranscodeOps CTranscodeDialog::Ops() const
	{
		STranscodeOps o;
		o.crf = m_pCrf->value();
		o.variants = m_pVariants->Heights();
		o.changeVariants = true;
		if (m_mode == EMode::Recompress)
		{
			o.recompress = true;
			o.height = m_pHeight->value();
			o.removeOtherVariants = true;
		}
		return o;
	}

	bool CTranscodeDialog::Ask(EMode mode, const QString& assetName, int sourceHeight, const std::vector<int>& existing, int crf, int height, STranscodeOps& ops, QWidget* pParent)
	{
		CTranscodeDialog dialog(mode, assetName, sourceHeight, existing, crf, height, pParent);
		if (dialog.exec() != QDialog::Accepted)
			return false;
		ops = dialog.Ops();
		if (mode == EMode::Variants && ops.variants.empty())
			return false; // nothing chosen
		return true;
	}
}
