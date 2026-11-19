// Middle column of the video editor: the preview picture and the transport bar.
//
// The preview does not use the engine renderer: CPreviewSession decodes with CEVPlayerCore and plays the sound through
// Qt Multimedia, CVideoPreviewWidget paints the frames with QPainter. A QTimer (16 ms) drives the Core while the panel
// is visible; it stops while the panel is hidden (tab covered, dock collapsed, closed) and the playback pauses until the
// panel is shown again. While the main window is minimized or the editor is in game mode the playback is paused too.
// The picture is only repainted for a new frame, a seek while paused, or an option change.
#pragma once

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <IEditor.h>

#include "Editor/PreviewSession.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;
class QToolButton;

namespace CEVEditor
{
	class CVideoPreviewWidget;

	class CVideoPreviewPanel : public QWidget, public IAutoEditorNotifyListener
	{
		Q_OBJECT
	public:
		explicit CVideoPreviewPanel(QWidget* pParent = nullptr);
		~CVideoPreviewPanel() override;

		struct SSource
		{
			std::string descriptorRel;   //!< relative to the game root (Assets)
			std::string videoRel;        //!< main video, relative to the game root
			double avOffsetMs = 0.0;
			double fps = 0.0;            //!< 0 = unknown (the media's frame rate, else 30, is used for stepping)
		};

		//! Loads the asset into the preview. Empty descriptorRel = clear.
		void SetSource(const SSource& source);
		void Clear();
		//! Releases the media (the files can be replaced afterwards); call SetSource again to continue.
		void ReleasePlayer();
		void SetFps(double fps) { m_source.fps = fps; }

		// ---- self test (cev_editor_preview_test) ----
		CPreviewSession& Session() { return m_session; }
		CVideoPreviewWidget* PictureWidget() const { return m_pPicture; }
		bool TimerActive() const { return m_timer.isActive(); }
		void PlayPause() { OnPlayPause(); }
		//! Preview timers running in the process right now.
		static int ActiveTimers();

		// IEditorNotifyListener
		void OnEditorNotifyEvent(EEditorNotifyEvent event) override;

	protected:
		void showEvent(QShowEvent* pEvent) override;
		void hideEvent(QHideEvent* pEvent) override;

	private slots:
		void OnPlayPause();
		void OnStop();
		void OnStep(int direction);
		void OnSliderMoved(int value);
		void OnLoopToggled(bool on);
		void OnAudioModeChanged(int index);
		void OnVolumeChanged(int value);
		void OnTimer();

	private:
		void UpdateTransport();
		void UpdateTimer();
		void SetStatus(const QString& text);
		QString ReadyStatus() const;
		double Fps() const;
		QString Timecode(double seconds) const;
		void Reload();
		float Volume() const;

		CPreviewSession m_session;
		SSource m_source;
		bool m_hasSource = false;
		bool m_shutdown = false;
		bool m_inGameMode = false;
		bool m_minimized = false;
		bool m_sliderUpdating = false;
		bool m_loggedFirstFrame = false;
		uint64_t m_lastSerial = 0;
		QTimer m_timer;

		CVideoPreviewWidget* m_pPicture = nullptr;
		QLabel* m_pStatus = nullptr;
		QLabel* m_pTime = nullptr;
		QToolButton* m_pPlay = nullptr;
		QToolButton* m_pStop = nullptr;
		QToolButton* m_pStepBack = nullptr;
		QToolButton* m_pStepForward = nullptr;
		QCheckBox* m_pLoop = nullptr;
		QSlider* m_pSlider = nullptr;
		QSlider* m_pVolume = nullptr;
		QComboBox* m_pAudio = nullptr;
		QComboBox* m_pFit = nullptr;
		QComboBox* m_pBackground = nullptr;
	};
}
