// The picture of the video preview: a plain QWidget painted with QPainter (no engine viewport, no swap chain).
//
// paintEvent() fills the widget, draws the background of the video rectangle (checkerboard / black / white) and the
// current frame of the session. The frame is wrapped into a QImage over the Core's buffer without copying it
// (QImage::Format_RGBA8888_Premultiplied is exactly the Core's premultiplied RGBA8): cev::PlayerCore keeps the frame
// it returned from GetFrame() valid until the next Update() that returns true, and Update() runs on this same (main)
// thread from the panel's timer, never during a paint, so the buffer cannot be recycled while it is drawn. The QImage
// does not outlive paintEvent().
//
// The widget never repaints by itself: the owner calls update() for a new frame (serial changed), a seek while paused
// and option changes; Qt adds resize / expose repaints. Scaling is fast while playing and smooth while paused.
#pragma once

#include <cstdint>

#include <QPixmap>
#include <QWidget>

namespace CEVEditor
{
	class CPreviewSession;

	class CVideoPreviewWidget final : public QWidget
	{
	public:
		enum class EBackground { Checker, Black, White };

		explicit CVideoPreviewWidget(QWidget* pParent = nullptr);

		void SetSession(const CPreviewSession* pSession) { m_pSession = pSession; }
		void SetBackground(EBackground background);
		void SetFit(bool fit);
		//! Smooth scaling (paused) or fast scaling (playing).
		void SetSmooth(bool smooth);

		//! paintEvent() calls of this widget so far.
		uint64_t PaintCount() const { return m_paints; }
		//! Serial of the frame drawn by the last paint (0 = none).
		uint64_t PaintedSerial() const { return m_paintedSerial; }

	protected:
		void paintEvent(QPaintEvent* pEvent) override;

	private:
		const CPreviewSession* m_pSession = nullptr;
		EBackground m_background = EBackground::Checker;
		bool m_fit = true;
		bool m_smooth = true;
		QPixmap m_checker;
		uint64_t m_paints = 0;
		uint64_t m_paintedSerial = 0;
	};
}
