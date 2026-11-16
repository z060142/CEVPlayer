// Renderer access of the plug-in, stage 1 (no engine patch): texture creation / upload / release, material slot
// binding and the renderer capability probe. Every function here is main-thread only. All aux 2D drawing lives in
// Render/FullscreenOutput.cpp, not here.
#pragma once

#include <cstdint>

struct ITexture;
struct IMaterial;

namespace CEVPlayer
{
	namespace RenderBridge
	{
		//! A renderer exists and it is a real one (not a dedicated server).
		bool Available();

		//! Probes the renderer module for the optional "CEVPlayer_Patched" export once and logs the result once
		//! ("stage 1" without it). Cheap after the first call.
		void ProbeOnce();
		//! 1 = stock renderer (CPU conversion, aux fullscreen), 2 = patched renderer found. M1: always 1.
		int Stage();

		//! RGBA8 texture registered under `name` (use a "$" prefix to make it reachable from materials / Flash).
		//! UploadToVideoMemory(nullptr, ..., FT_NOMIPS): allocates the device texture, which waits for the render
		//! thread, so call this once per video and never in a hot path. Returns the texture id or 0.
		int CreateTexture(const char* name, int width, int height);
		//! Uploads one tightly packed RGBA8 picture of exactly the texture's size. Call only when the picture
		//! changed.
		bool UpdateTexture(int textureId, const uint8_t* rgba, int width, int height);
		ITexture* GetTexture(int textureId);
		void RemoveTexture(int textureId);

		//! Makes the texture slot of the (sub) material sample `pTexture`. The previous texture is returned in
		//! `*ppPrevious` with its reference now owned by the caller (give it back to RestoreMaterialTexture).
		//! The new texture gets one reference owned by the material slot.
		bool SetMaterialTexture(IMaterial* pMaterial, int subMaterial, int textureSlot, ITexture* pTexture, ITexture** ppPrevious);
		//! Undoes SetMaterialTexture. `pOurs` is the texture that was bound, `pPrevious` the one it replaced
		//! (consumed). If somebody else replaced the slot meanwhile nothing is restored.
		void RestoreMaterialTexture(IMaterial* pMaterial, int subMaterial, int textureSlot, ITexture* pOurs, ITexture* pPrevious);
	}
}
