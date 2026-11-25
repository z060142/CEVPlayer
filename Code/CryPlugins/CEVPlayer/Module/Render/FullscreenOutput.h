// Fullscreen output: the fullscreen players (and the development debug layers) drawn by a 2D stage of the renderer
// (docs/design/M11-xui2dl.md). Two backends: an engine with XTFrame's XUI2D (IXUIRenderer, CEV_HAVE_XUI2D) gets the
// layers as frames of one more XUI2D screen surface, below the XTFrame UI (Render/FullscreenXUI2D.*); any other engine
// needs the XUI2DL patch (EnginePatch/) and gets one retained layer list. There is no other drawing path: an engine
// with neither has no fullscreen output. Main thread only.
#pragma once

#include <cstddef>
#include <vector>

#include <CEVPlayer/IVideoPlayer.h>

namespace CEVPlayer
{
	namespace FullscreenOutput
	{
		enum class EBackend { None, XUI2DL, XUI2D };
		//! The plug-in update step a Present call comes from. XUI2D takes the layers in BeforeRender (same frame batch
		//! as CryXTUI's surfaces, ordered below them), XUI2DL in AfterRender.
		enum class EStep { BeforeRender, AfterRender };

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

		//! One backend-neutral quad (the layers turned into output pixels, origin top-left; first = bottom).
		enum class EQuadKind { Solid, RgbaPremult, Yuv420, Yuva420 };
		struct SQuad
		{
			EQuadKind kind = EQuadKind::Solid;
			bool      replace = false;              //!< blend: replace (letterbox bars, solid debug layers), else source-over
			float     x = 0, y = 0, w = 0, h = 0;   //!< destination rectangle, output pixels
			int       textureId = 0;                //!< engine texture id (not Solid)
			float     color[4] = { 1, 1, 1, 1 };    //!< premultiplied multiplier; Solid: the colour itself
			//! Yuv420 / Yuva420 only: the picture size and the plane description (texels of the packed texture).
			uint32 imageWidth = 0, imageHeight = 0;
			uint32 planeY[4] = {}, planeU[4] = {}, planeV[4] = {}, planeA[4] = {};
			uint32 colorMatrix = 0, colorRange = 0, chromaSiting = 0;
		};

		//! Probes once and caches (XUI2D, else XUI2DL, else None; one log line); None logs one error (once per run).
		EBackend Backend();
		//! "xui2d", "xui2dl" or "none".
		const char* BackendName();
		//! XUI2D: XUI2D_Patched has the planar bit (256); XUI2DL: the backend has XUI2DL_CAP_YUV.
		bool SupportsPlanar();
		//! The step the current backend takes its layers in (a Present from the other step does nothing).
		bool PresentsAt(EStep step);
		//! Called once per frame from each step. Turns the layers (first = bottom) into the backend's quads. XUI2DL: a
		//! list byte-identical to the one sent last is not sent again. XUI2D: a frame every frame that has quads.
		void Present(EStep step, const SLayer* pLayers, size_t count, const SDebugLayer* pDebug, size_t debugCount);
		//! XUI2DL: sends an empty list. XUI2D: destroys the surface and the texture handles.
		void Shutdown();
		//! True while cev_probe_xui2dl drives the XUI2DL backend directly: Present sends nothing. Back to false: the
		//! next Present sends its list again.
		void SetProbeOwnsFrame(bool owns);

		//! The quads of the layers for an output of screenW x screenH: per player its letterbox bars then its picture,
		//! then the debug layers. At most maxQuads: past it the debug layers go first, then the topmost players
		//! (`dropped` set). Shared by both backends.
		void BuildQuads(const SLayer* pLayers, size_t count, const SDebugLayer* pDebug, size_t debugCount, float screenW, float screenH,
		                size_t maxQuads, std::vector<SQuad>& out, bool& dropped);
	}
}
