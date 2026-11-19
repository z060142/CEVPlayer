// Right column of the video editor: stream information, file status (existence, source staleness), the descriptor (route, AV
// offset, audio tracks, markers) in the engine's property tree, the FFmpeg status and the operation buttons with the job list.
// What the engine already provides is used from there (E1): Reimport is the asset's own (CAsset::Reimport), the FFmpeg path
// is an editor preference, the folder is opened from the Asset Browser, results go to the notification center.
#pragma once

#include <memory>
#include <string>

#include <QPointer>
#include <QWidget>

#include "Asset/VideoDescriptorIO.h"

class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QPropertyTreeLegacy;
namespace yasli { struct Context; }

namespace CEVEditor
{
	class CVideoEditor;

	class CVideoInfoPanel : public QWidget
	{
		Q_OBJECT
	public:
		explicit CVideoInfoPanel(CVideoEditor* pEditor, QWidget* pParent = nullptr);
		~CVideoInfoPanel() override;

		//! Self test (cev_editor_info_test): `changed` goes through the JSON archive into the object the property tree edits
		//! (SDescriptor::Serialize, input) and then through the same handler as an edit in the tree. False + error on a failure.
		bool ApplyThroughSerializationForTest(const SDescriptor& changed, std::string& error);
		//! The descriptor the property tree shows / edits.
		const SDescriptor& EditedDescriptor() const { return m_edit; }

	public slots:
		void Refresh();

	private slots:
		void RefreshFfmpeg();
		void OnTreeChanged();
		void OnJobChanged(int id);
		void OnJobLog(int id, const QString& line);

	private:
		void BuildUi();
		void FillFiles();
		void FillVariants();
		void FillDescriptor();
		void RefreshJobs();
		void Log(const QString& line);

		QPointer<CVideoEditor> m_pEditor;

		QLabel* m_pTitle = nullptr;
		QLabel* m_pInfo = nullptr;
		QLabel* m_pSource = nullptr;
		QLabel* m_pOffline = nullptr;
		QTableWidget* m_pFiles = nullptr;
		QTableWidget* m_pVariants = nullptr;
		QLabel* m_pFfmpegState = nullptr;
		QLabel* m_pFfmpegDetail = nullptr;
		QPushButton* m_pFfmpegDownload = nullptr;
		QPushButton* m_pBtnVariants = nullptr;
		QPushButton* m_pBtnRecompress = nullptr;
		QPushButton* m_pBtnReimport = nullptr;
		QPropertyTreeLegacy* m_pTree = nullptr;
		SDescriptor m_edit;                         //!< what the property tree edits (copy of the editor's descriptor)
		SDescriptorSerializeContext m_context;      //!< folder of the descriptor, for the audio track file picker
		std::unique_ptr<yasli::Context> m_pArchiveContext;
		QLabel* m_pAce = nullptr;
		QListWidget* m_pJobs = nullptr;
		QPlainTextEdit* m_pLog = nullptr;
		QPushButton* m_pCancelJob = nullptr;
		QWidget* m_pOpButtons = nullptr;
		int m_filledRevision = -1;   //!< CVideoEditor::DescriptorRevision() the property tree was filled from
	};
}
