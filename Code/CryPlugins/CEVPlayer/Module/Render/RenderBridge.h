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
		//! M10: the renderer offers loading-loop callbacks (IRenderer::AddLoadtimeCallback, engine patch 17). Always
		//! false without CEV_HAVE_LOADTIME_CALLBACKS; otherwise read once from the renderer module's export
		//! `extern "C" int XUI2D_Patched()` (bit 7 = 128) and cached.
		bool LoadtimeAvailable();
		//! 1 = stock renderer (CPU conversion, aux fullscreen), 2 = patched renderer found. M1: always 1.
		int Stage();

		//! Largest texture side the renderer accepts (0 = unknown / no limit).
		int MaxTextureSize();

		//! Texture formats of the plug-in: RGBA8 (premultiplied picture) and R8 (packed planar layouts, M8).
		enum class EFormat : uint8_t { Rgba8, R8 };

		//! Texture (RGBA8 or R8) registered under `name` (must start with "$" to be reachable from materials / Flash).
		//! UploadToVideoMemory(nullptr, ..., FT_NOMIPS): allocates the device texture, which waits for the render
		//! thread, so call this once per video and never in a hot path. Returns the texture with ONE reference owned by
		//! the caller (give it back with ReleaseTexture) or nullptr, then `*pFailure` tells why (never null).
		//! If a texture of that name exists already it is reused (the engine recreates its device texture at the new
		//! size and format); render targets, non 2D textures and anything that is neither RGBA8 nor R8 are refused, and
		//! the result is verified (size, one mip, format) before it is returned.
		ITexture* CreateTexture(const char* name, int width, int height, EFormat format, const char** pFailure);
		//! Uploads one tightly packed picture (RGBA8: 4 bytes, R8: 1 byte per texel) of exactly the texture's size and
		//! format. Call only when the picture
		//! changed. Refuses (false) when the texture does not match the buffer: UpdateTextureInVideoMemory copies
		//! TextureDataSize(w, h, mips) bytes from the buffer and takes the texture by id (a stale id would write into
		//! the renderer's "no texture").
		bool UpdateTexture(ITexture* pTexture, const uint8_t* data, int width, int height, EFormat format);
		//! Drops the caller's reference. Never goes through the texture id (RemoveTexture(id) of a stale id releases the
		//! renderer's "no texture"). Note: a texture that was drawn through the aux geometry stays alive in the aux
		//! texture cache (its name stays registered); fullscreen players therefore use reusable generated names.
		void ReleaseTexture(ITexture* pTexture);

		//! Makes the texture slot of the (sub) material sample `pTexture`. The previous texture is returned in
		//! `*ppPrevious` with its reference now owned by the caller (give it back to RestoreMaterialTexture).
		//! The new texture gets one reference owned by the material slot.
		bool SetMaterialTexture(IMaterial* pMaterial, int subMaterial, int textureSlot, ITexture* pTexture, ITexture** ppPrevious);
		//! Undoes SetMaterialTexture. `pOurs` is the texture that was bound, `pPrevious` the one it replaced
		//! (consumed). If somebody else replaced the slot meanwhile nothing is restored.
		void RestoreMaterialTexture(IMaterial* pMaterial, int subMaterial, int textureSlot, ITexture* pOurs, ITexture* pPrevious);
	}
}
