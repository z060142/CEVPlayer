// QViewport of the video preview (Character Tool style: own swap chain, SignalRender). It is 2D only: mouse and key
// events are not forwarded to the base class (no camera navigation, no cursor capture). Qt re-creates the native window
// of a widget when the docking system re-parents it; the swap chain of QViewport is bound to that window and is only
// created in the constructor, so the owner re-creates the whole viewport when it is told (signalNeedsRecreate): see
// CVideoPreviewPanel (docs/research/04 §4.7).
#pragma once

#include <QViewport.h>

#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>

namespace CEVEditor
{
	class CVideoViewport : public QViewport
	{
		Q_OBJECT
	public:
		explicit CVideoViewport(QWidget* pParent);
		~CVideoViewport() override;

		static int LiveCount() { return s_live; }
		static int CreatedCount() { return s_created; }
		static int DestroyedCount() { return s_destroyed; }

	signals:
		//! The native window changed (or the widget was re-parented): the swap chain is bound to a dead window.
		//! Emitted from a Qt event, the owner must delete this viewport later (never inside this signal / an idle update).
		void signalNeedsRecreate();

	protected:
		bool event(QEvent* pEvent) override;
		void mousePressEvent(QMouseEvent* pEvent) override { pEvent->ignore(); }
		void mouseReleaseEvent(QMouseEvent* pEvent) override { pEvent->ignore(); }
		void mouseMoveEvent(QMouseEvent* pEvent) override { pEvent->ignore(); }
		void wheelEvent(QWheelEvent* pEvent) override { pEvent->ignore(); }
		void keyPressEvent(QKeyEvent* pEvent) override { pEvent->ignore(); }
		void keyReleaseEvent(QKeyEvent* pEvent) override { pEvent->ignore(); }

	private:
		bool m_notified = false;
		static int s_live;
		static int s_created;
		static int s_destroyed;
	};
}
