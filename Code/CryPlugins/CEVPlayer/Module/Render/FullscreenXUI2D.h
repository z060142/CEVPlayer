// Fullscreen output, XUI2D backend (docs/design/M11-xui2dl.md section 8, L5): on an engine with XTFrame's XUI2D
// (CryRenderer/IXUIRenderer.h with EShading::TexturePlanar, CEV_HAVE_XUI2D) the fullscreen quads go to one more XUI2D
// screen surface, a second client next to the CryXTUI plug-in. XUI2D is not retained: every frame with quads is
// submitted, from UpdateBeforeRender, so it lands in the same frame batch as CryXTUI's surfaces and is drawn below
// them (surface order -(1 << 20)). Main thread only, except the loading-loop callback (L6, LoopCallback). Only
// compiled with CEV_HAVE_XUI2D.
#pragma once

#include <vector>

#include "Render/FullscreenOutput.h"

struct ILoadtimeCallback;

namespace CEVPlayer
{
	namespace FullscreenXUI2D
	{
		//! XUI2D_Patched() of the renderer module (0 when the export is missing). Read once and cached.
		int PatchedBits();
		//! L6: the loading-loop drawing of the frozen frame (null without CEV_HAVE_LOADTIME_CALLBACKS): the order-150
		//! loadtime callback. LoadtimeUpdate: Begin + ClearOutput (opaque black) + Submit of the last frame the main
		//! thread composed before the load, on the surface's IXUILoadFrame (no Begin when that frame was empty);
		//! LoadtimeRender: Execute when this iteration submitted. Registered / removed by the player manager (main
		//! thread, never under its tick lock). The object always exists, so it can be removed after the conditions
		//! changed. The main thread gives the surface back (IXUILoadFrame::End) at the start of the first Present after a
		//! loop began its entry: the main thread presents only after the load, when the loop has stopped. L7b: when the
		//! XUI2D resource generation changes during the loop, LoadtimeUpdate wraps the frozen engine texture ids again
		//! and submits under the new generation; the main thread adopts those handles after the load.
		ILoadtimeCallback* LoopCallback();
		//! XUI2D_Patched is non-zero and IRenderer::GetIXUIRenderer() returns an object (checked in that order: the
		//! IRenderer slot only exists on a patched renderer).
		bool Available();
		//! One frame of `quads` for an output of outputW x outputH pixels. No quads: nothing is submitted (the surface
		//! draws nothing that frame); the texture references still age.
		void Present(const std::vector<FullscreenOutput::SQuad>& quads, int outputW, int outputH);
		//! Destroys the surface and the texture handles and lets go of every texture reference.
		void Shutdown();
	}
}
