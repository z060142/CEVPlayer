// Right column of the video editor: stream information, file status (existence, source staleness), the descriptor
// (route, AV offset, languages, markers) and the operation buttons with the job list.
#pragma once

#include <QPointer>
#include <QWidget>

class QDoubleSpinBox;
class QComboBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

namespace CEVEditor
{
	class CVideoEditor;

	class CVideoInfoPanel : public QWidget
	{
		Q_OBJECT
	public:
		explicit CVideoInfoPanel(CVideoEditor* pEditor, QWidget* pParent = nullptr);

	public slots:
		void Refresh();

	private slots:
		void OnApplyDescriptor();
		void OnJobChanged(int id);
		void OnJobLog(int id, const QString& line);

	private:
		void BuildUi();
		void FillFiles();
		void FillDescriptor();
		void RefreshJobs();
		void Log(const QString& line);
		void AddLanguageRow(const QString& name, bool embedded, const QString& file, const QString& trigger, const QString& pause, const QString& resume);
		void AddMarkerRow(const QString& name, double time);

		QPointer<CVideoEditor> m_pEditor;

		QLabel* m_pTitle = nullptr;
		QLabel* m_pInfo = nullptr;
		QLabel* m_pSource = nullptr;
		QLabel* m_pOffline = nullptr;
		QTableWidget* m_pFiles = nullptr;
		QDoubleSpinBox* m_pAvOffset = nullptr;
		QComboBox* m_pRoute = nullptr;
		QComboBox* m_pDefaultLanguage = nullptr;
		QTableWidget* m_pLanguages = nullptr;
		QTableWidget* m_pMarkers = nullptr;
		QLabel* m_pAce = nullptr;
		QListWidget* m_pJobs = nullptr;
		QPlainTextEdit* m_pLog = nullptr;
		QPushButton* m_pCancelJob = nullptr;
		QWidget* m_pOpButtons = nullptr;
		bool m_filling = false;
	};
}
