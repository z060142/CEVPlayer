// Middle column of the video editor: the preview viewport and the transport bar.
//
// Rendering: the viewport's render callback draws a checkerboard and the video texture of the player. The texture is
// premultiplied RGBA; the aux geometry has no premultiplied blend mode, so it is drawn twice (docs/research/04 §4.6):
//   pass 1  colour (0,0,0,1), alpha blended   ->  dst = dst * (1 - a)
//   pass 2  colour (1,1,1,1), additive        ->  dst = dst + rgb (already multiplied by a)
// Lifetime: the viewport is created after the panel is shown (so that it already sits in its final window), and is
// re-created when Qt re-creates its native window (CVideoViewport::signalNeedsRecreate), always from a Qt event, never
// inside the idle notification or the render callback.
#pragma once

#include <QPointer>
#include <QWidget>

#include <IEditor.h>

#include "Editor/PreviewSession.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QSlider;
class QToolButton;
struct SRenderContext;
struct ITexture;

namespace CEVEditor
{
	class CVideoViewport;

	//! True between eNotify_OnMainFrameInitialized and eNotify_OnQuit. A pane restored from the saved layout is created
	//! before the main frame is complete: its viewport (swap chain) is only created once this is true.
	bool IsEditorReady();
	void SetEditorReady(bool ready);

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
			double fps = 0.0;            //!< 0 = unknown (30 is assumed for stepping)
		};

		//! Loads the asset into the preview player (creates it when needed). Empty descriptorRel = clear.
		void SetSource(const SSource& source);
		void Clear();
		//! Destroys the player (the file can be replaced afterwards); call SetSource again to continue.
		void ReleasePlayer();
		void SetFps(double fps) { m_source.fps = fps; }

		// IEditorNotifyListener
		void OnEditorNotifyEvent(EEditorNotifyEvent event) override;

	protected:
		void showEvent(QShowEvent* pEvent) override;

	private slots:
		void OnPlayPause();
		void OnStop();
		void OnStep(int direction);
		void OnSliderMoved(int value);
		void OnLoopToggled(bool on);
		void OnAudioModeChanged(int index);
		void OnViewportNeedsRecreate();

	private:
		void CreateViewport();
		void ReleaseCheckerOnce();
		void DestroyViewport();
		void Tick();
		void Render(const SRenderContext& rc);
		void UpdateTransport();
		void SetStatus(const QString& text);
		double Fps() const { return m_source.fps > 1.0 ? m_source.fps : 30.0; }
		QString Timecode(double seconds) const;
		void Reload();

		CPreviewSession m_session;
		SSource m_source;
		bool m_hasSource = false;
		QPointer<CVideoViewport> m_pViewport;
		QWidget* m_pViewportHost = nullptr;
		bool m_recreatePending = false;
		bool m_createScheduled = false;
		bool m_shutdown = false;
		bool m_checkerHeld = false;
		bool m_waitingForMainFrame = false;
		bool m_sliderUpdating = false;
		bool m_loggedFirstDraw = false;
		bool m_fit = true;
		uint32_t m_lastFrameSerial = 0;

		QLabel* m_pStatus = nullptr;
		QLabel* m_pTime = nullptr;
		QToolButton* m_pPlay = nullptr;
		QToolButton* m_pStop = nullptr;
		QToolButton* m_pStepBack = nullptr;
		QToolButton* m_pStepForward = nullptr;
		QCheckBox* m_pLoop = nullptr;
		QSlider* m_pSlider = nullptr;
		QComboBox* m_pAudio = nullptr;
		QComboBox* m_pFit = nullptr;
	};
}
