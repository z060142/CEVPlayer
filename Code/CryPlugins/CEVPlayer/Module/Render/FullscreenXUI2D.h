// Fullscreen output, XUI2D backend (docs/design/M11-xui2dl.md section 8, L5): on an engine with XTFrame's XUI2D
// (CryRenderer/IXUIRenderer.h with EShading::TexturePlanar, CEV_HAVE_XUI2D) the fullscreen quads go to one more XUI2D
// screen surface, a second client next to the CryXTUI plug-in. XUI2D is not retained: every frame with quads is
// submitted, from UpdateBeforeRender, so it lands in the same frame batch as CryXTUI's surfaces and is drawn below
// them (surface order -(1 << 20)). Main thread only. Only compiled with CEV_HAVE_XUI2D.
#pragma once

#include <vector>

#include "Render/FullscreenOutput.h"

namespace CEVPlayer
{
	namespace FullscreenXUI2D
	{
		//! XUI2D_Patched() of the renderer module (0 when the export is missing). Read once and cached.
		int PatchedBits();
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
