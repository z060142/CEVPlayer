// Fullscreen output: the fullscreen players (and the development debug layers) as one retained layer list of the
// renderer's XUI2DL stage (docs/design/M11-xui2dl.md, EnginePatch/). There is no other drawing path: an engine
// without the patch has no fullscreen output. Main thread only.
#pragma once

#include <cstddef>

#include <CEVPlayer/IVideoPlayer.h>

namespace CEVPlayer
{
	namespace FullscreenOutput
	{
		enum class EBackend { None, XUI2DL };

		//! One fullscreen player.
		struct SLayer
		{
			int               textureId = 0;
			int               imageWidth = 0, imageHeight = 0;     //!< picture size (not the packed texture's size)
			EVideoFit         fit = EVideoFit::Letterbox;
			EVideoPixelLayout layout = EVideoPixelLayout::RGBA8;
			bool              hasAlpha = false;
			//! Packed layouts only (values as in the contract snapshot: planeY/U/V/A, colorMatrix, colorRange, chromaSiting).
			uint32 planeY[4] = {}, planeU[4] = {}, planeV[4] = {}, planeA[4] = {};
			uint32 colorMatrix = 0, colorRange = 0, chromaSiting = 0;
		};

		//! A development debug layer (viewport pixels). solid: a colour rectangle, else a premultiplied RGBA texture.
		struct SDebugLayer { int textureId = 0; float x = 0, y = 0, w = 0, h = 0; float color[4] = { 1, 1, 1, 1 }; bool solid = false; };

		//! Probes once and caches; None logs one error (once per run).
		EBackend Backend();
		//! The backend has XUI2DL_CAP_YUV.
		bool SupportsPlanar();
		//! Called once per frame (UpdateAfterRender). Turns the layers (first = bottom) into the backend's list; a list
		//! byte-identical to the one sent last is not sent again.
		void Present(const SLayer* pLayers, size_t count, const SDebugLayer* pDebug, size_t debugCount);
		//! Sends an empty list.
		void Shutdown();
		//! True while cev_probe_xui2dl drives the backend directly: Present sends nothing. Back to false: the next
		//! Present sends its list again.
		void SetProbeOwnsFrame(bool owns);
	}
}
