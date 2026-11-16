// Fullscreen output, stage 1: renderer aux 2D images on the top layer. This is the ONLY place that touches the
// aux geometry interfaces (IRenderAuxImage / IRenderAuxText), so that the stage 2 pass can replace it as a unit.
//
// Limits of the aux path (known, documented in the design): the aux blend is straight alpha while the video
// texture is premultiplied (alpha videos get dark fringes); aux is not drawn while a level is loading.
#pragma once

#include <cstddef>

#include <CEVPlayer/IVideoPlayer.h>

namespace CEVPlayer
{
	namespace FullscreenOutput
	{
		struct SLayer
		{
			int textureId = 0;
			int textureWidth = 0;
			int textureHeight = 0;
			EVideoFit fit = EVideoFit::Letterbox;
		};

		//! Queues the layers for this frame, first = bottom. Call once per frame after the scene was rendered
		//! (the plug-in's UpdateAfterRender step), only when renderer calls are allowed.
		void Draw(const SLayer* pLayers, size_t count);

		//! Debug text block in the top left corner (cev_stats).
		void DrawTextLines(const char* const* ppLines, size_t count);
	}
}
