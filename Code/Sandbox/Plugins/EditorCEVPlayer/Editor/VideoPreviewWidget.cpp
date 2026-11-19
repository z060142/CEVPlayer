#include "StdAfx.h"
#include "Editor/VideoPreviewWidget.h"

#include <algorithm>
#include <cmath>

#include <QImage>
#include <QPainter>

#include "Editor/PreviewSession.h"

namespace CEVEditor
{
	namespace
	{
		constexpr int kCheckerCell = 16;
		constexpr int kMargin = 8;
	}

	CVideoPreviewWidget::CVideoPreviewWidget(QWidget* pParent)
		: QWidget(pParent)
	{
		setAttribute(Qt::WA_OpaquePaintEvent); // every pixel is painted: no parent background repaint behind it
		setMinimumSize(160, 90);
		m_checker = QPixmap(2 * kCheckerCell, 2 * kCheckerCell);
		QPainter p(&m_checker);
		p.fillRect(0, 0, 2 * kCheckerCell, 2 * kCheckerCell, QColor(204, 204, 204));
		p.fillRect(kCheckerCell, 0, kCheckerCell, kCheckerCell, QColor(150, 150, 150));
		p.fillRect(0, kCheckerCell, kCheckerCell, kCheckerCell, QColor(150, 150, 150));
	}

	void CVideoPreviewWidget::SetBackground(EBackground background)
	{
		if (background == m_background)
			return;
		m_background = background;
		update();
	}

	void CVideoPreviewWidget::SetFit(bool fit)
	{
		if (fit == m_fit)
			return;
		m_fit = fit;
		update();
	}

	void CVideoPreviewWidget::SetSmooth(bool smooth)
	{
		if (smooth == m_smooth)
			return;
		m_smooth = smooth;
		update();
	}

	void CVideoPreviewWidget::paintEvent(QPaintEvent*)
	{
		++m_paints;
		QPainter painter(this);
		const QRect all = rect();
		painter.fillRect(all, QColor(24, 24, 24));

		cev::FrameView frame;
		const bool haveFrame = m_pSession != nullptr && m_pSession->GetFrame(frame) && frame.width > 0 && frame.height > 0;
		const cev::MediaInfo* const pInfo = m_pSession != nullptr && m_pSession->IsReady() ? &m_pSession->Info() : nullptr;
		double vw = haveFrame ? frame.width : (pInfo != nullptr && pInfo->width > 0 ? pInfo->width : 16);
		double vh = haveFrame ? frame.height : (pInfo != nullptr && pInfo->height > 0 ? pInfo->height : 9);

		// destination rectangle: fit with a margin, or 1:1 centred while it fits (a larger video stays fitted)
		const double W = all.width();
		const double H = all.height();
		double scale = std::min((W - 2.0 * kMargin) / vw, (H - 2.0 * kMargin) / vh);
		if (!m_fit && haveFrame && vw <= W && vh <= H)
			scale = 1.0;
		scale = std::max(scale, 0.01);
		const double dw = std::floor(vw * scale);
		const double dh = std::floor(vh * scale);
		const QRectF dst(std::floor(0.5 * (W - dw)), std::floor(0.5 * (H - dh)), dw, dh);

		switch (m_background)
		{
		case EBackground::Checker:
			painter.setBrushOrigin(dst.topLeft());
			painter.fillRect(dst, QBrush(m_checker));
			break;
		case EBackground::Black:
			painter.fillRect(dst, Qt::black);
			break;
		case EBackground::White:
			painter.fillRect(dst, Qt::white);
			break;
		}

		if (!haveFrame)
		{
			m_paintedSerial = 0;
			return;
		}
		// no copy: a view of the Core's premultiplied RGBA buffer for the duration of this paint (see the header)
		const QImage image(frame.rgba, frame.width, frame.height, frame.rgbaStride, QImage::Format_RGBA8888_Premultiplied);
		painter.setRenderHint(QPainter::SmoothPixmapTransform, m_smooth);
		painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
		painter.drawImage(dst, image, QRectF(0, 0, frame.width, frame.height));
		m_paintedSerial = frame.serial;
	}
}
