#include "StdAfx.h"
#include "Editor/VideoViewport.h"

#include <QViewportSettings.h>

#include <QEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>

namespace CEVEditor
{
	int CVideoViewport::s_live = 0;
	int CVideoViewport::s_created = 0;
	int CVideoViewport::s_destroyed = 0;

	CVideoViewport::CVideoViewport(QWidget* pParent)
		: QViewport(gEnv, []()
		{
			// the pipeline the Character Tool's preview uses (BlendSpacePreview.cpp)
			IRenderer::SGraphicsPipelineDescription desc;
			desc.type = EGraphicsPipelineType::Minimum;
			desc.shaderFlags = SHDF_SECONDARY_VIEWPORT | SHDF_ALLOWHDR | SHDF_FORWARD_MINIMAL;
			return desc;
		}(), pParent)
	{
		SViewportSettings settings = GetSettings();
		settings.grid.showGrid = false;
		settings.grid.origin = false;
		settings.camera.showViewportOrientation = false;
		settings.rendering.fps = false;
		settings.background.useGradient = false;
		settings.background.topColor = ColorB(24, 24, 24, 255);
		SetSettings(settings);
		++s_live;
		++s_created;
		CryLogAlways("[CEVEditor] viewport created (live %d, created %d, destroyed %d)", s_live, s_created, s_destroyed);
	}

	CVideoViewport::~CVideoViewport()
	{
		--s_live;
		++s_destroyed;
		CryLogAlways("[CEVEditor] viewport destroyed (live %d, created %d, destroyed %d)", s_live, s_created, s_destroyed);
	}

	bool CVideoViewport::event(QEvent* pEvent)
	{
		const bool handled = QViewport::event(pEvent);
		if (!m_notified && (pEvent->type() == QEvent::WinIdChange || pEvent->type() == QEvent::ParentChange))
		{
			m_notified = true; // once: the owner replaces this viewport
			Q_EMIT signalNeedsRecreate();
		}
		return handled;
	}
}
