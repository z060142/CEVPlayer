#include "StdAfx.h"
#include "PluginEnv.h"
#include "RenderBridge.h"

#include <CryCore/Platform/CryWindows.h> // GetModuleHandleA / GetProcAddress
#include <CryRenderer/IRenderer.h>
#include <CryRenderer/IShader.h>
#include <Cry3DEngine/IMaterial.h>

namespace CEVPlayer
{
	namespace RenderBridge
	{
		namespace
		{
			IRenderer* s_probedRenderer = nullptr;
			int s_stage = 1;

			const char* RendererModuleName(IRenderer* pRenderer)
			{
				// GetRenderType is a stock vtable slot, safe on every renderer module.
				switch (pRenderer->GetRenderType())
				{
				case ERenderType::Direct3D11: return "CryRenderD3D11";
				case ERenderType::Direct3D12: return "CryRenderD3D12";
				case ERenderType::Vulkan:     return "CryRenderVulkan";
				default:                      return nullptr;
				}
			}

			SShaderItem* GetShaderItem(IMaterial* pMaterial, int subMaterial, IMaterial** ppTarget)
			{
				IMaterial* pTarget = pMaterial;
				if (pTarget != nullptr && subMaterial >= 0)
					pTarget = pTarget->GetSubMtl(subMaterial);
				if (pTarget == nullptr)
					return nullptr;
				*ppTarget = pTarget;
				return &pTarget->GetShaderItem();
			}

			SEfResTexture* GetSlot(SShaderItem* pItem, int textureSlot)
			{
				if (pItem == nullptr || pItem->m_pShaderResources == nullptr || textureSlot < 0 || textureSlot >= EFTT_MAX)
					return nullptr;
				return pItem->m_pShaderResources->GetTexture(textureSlot);
			}
		}

		bool Available()
		{
			return gEnv != nullptr && gEnv->pRenderer != nullptr && !gEnv->IsDedicated();
		}

		void ProbeOnce()
		{
			if (!Available() || gEnv->pRenderer == s_probedRenderer)
				return;
			IRenderer* const pRenderer = gEnv->pRenderer;
			s_probedRenderer = pRenderer;
			s_stage = 1;

			bool patched = false;
#if CRY_PLATFORM_WINDOWS
			if (const char* moduleName = RendererModuleName(pRenderer))
			{
				// The renderer DLL is loaded by CrySystem before any plug-in.
				if (HMODULE hModule = ::GetModuleHandleA(moduleName))
				{
					using PFN_Patched = int (*)();
					const PFN_Patched pPatched = reinterpret_cast<PFN_Patched>(::GetProcAddress(hModule, "CEVPlayer_Patched"));
					if (pPatched != nullptr && pPatched() != 0)
					{
						patched = true;
						s_stage = 2;
					}
				}
			}
#endif
			if (patched)
				LogAlways("renderer probe: CEVPlayer_Patched found, stage 2 available (M1 still renders with stage 1)");
			else
				LogAlways("renderer probe: CEVPlayer_Patched not found, stage 1 (CPU conversion, aux fullscreen)");
		}

		int Stage()
		{
			return s_stage;
		}

		int MaxTextureSize()
		{
			return Available() ? gEnv->pRenderer->GetMaxTextureSize() : 0;
		}

		ITexture* CreateTexture(const char* name, int width, int height, const char** pFailure)
		{
			*pFailure = "renderer not available";
			if (!Available() || name == nullptr || name[0] == '\0' || width <= 0 || height <= 0)
				return nullptr;

			const int maxSize = gEnv->pRenderer->GetMaxTextureSize();
			if (maxSize > 0 && (width > maxSize || height > maxSize))
			{
				*pFailure = "video is larger than the renderer's maximum texture size";
				return nullptr;
			}

			// A same-named texture is reused by the renderer. Only plain RGBA8 2D textures are acceptable (the
			// engine recreates the device texture at our size); never take over a render target / other type.
			if (ITexture* const pExisting = gEnv->pRenderer->EF_GetTextureByName(name))
			{
				if (pExisting->GetTextureType() != eTT_2D || (pExisting->GetFlags() & (FT_USAGE_RENDERTARGET | FT_USAGE_DEPTHSTENCIL | FT_USAGE_UNORDERED_ACCESS)) != 0
					|| (pExisting->IsTextureLoaded() && pExisting->GetTextureDstFormat() != eTF_R8G8B8A8))
				{
					*pFailure = "a texture with this name exists and is not a plain RGBA8 2D texture";
					return nullptr;
				}
			}

			const int id = static_cast<int>(gEnv->pRenderer->UploadToVideoMemory(nullptr, width, height, eTF_R8G8B8A8, eTF_R8G8B8A8, 0, false, FILTER_BILINEAR, 0, name, FT_NOMIPS));
			ITexture* const pTexture = id > 0 ? gEnv->pRenderer->EF_GetTextureByID(id) : nullptr;
			if (pTexture == nullptr)
			{
				*pFailure = "the renderer could not create the texture";
				return nullptr;
			}
			// The creation reference is ours. Verify what we got: the renderer substitutes its "no texture" when the
			// device texture could not be created.
			if (pTexture->GetTextureID() != id || pTexture->GetWidth() != width || pTexture->GetHeight() != height || pTexture->GetNumMips() != 1
				|| pTexture->GetTextureType() != eTT_2D || pTexture->GetTextureDstFormat() != eTF_R8G8B8A8)
			{
				*pFailure = "the created texture does not match the requested size / format";
				pTexture->Release();
				return nullptr;
			}
			LogDebug("texture <%s> id <%d> %dx%d created", name, id, width, height);
			*pFailure = "";
			return pTexture;
		}

		bool UpdateTexture(ITexture* pTexture, const uint8_t* rgba, int width, int height)
		{
			if (!Available() || pTexture == nullptr || rgba == nullptr)
				return false;
			const int id = pTexture->GetTextureID();
			if (id <= 0 || pTexture->GetWidth() != width || pTexture->GetHeight() != height || pTexture->GetNumMips() != 1
				|| pTexture->GetTextureDstFormat() != eTF_R8G8B8A8 || gEnv->pRenderer->EF_GetTextureByID(id) != pTexture)
				return false;
			gEnv->pRenderer->UpdateTextureInVideoMemory(static_cast<uint32>(id), const_cast<unsigned char*>(rgba), 0, 0, width, height, eTF_R8G8B8A8);
			return true;
		}

		void ReleaseTexture(ITexture* pTexture)
		{
			if (pTexture == nullptr)
				return;
			LogDebug("texture <%s> id <%d> released", pTexture->GetName(), pTexture->GetTextureID());
			pTexture->Release();
		}

		bool SetMaterialTexture(IMaterial* pMaterial, int subMaterial, int textureSlot, ITexture* pTexture, ITexture** ppPrevious)
		{
			*ppPrevious = nullptr;
			IMaterial* pTarget = nullptr;
			SShaderItem* pItem = GetShaderItem(pMaterial, subMaterial, &pTarget);
			SEfResTexture* pSlot = GetSlot(pItem, textureSlot);
			if (pSlot == nullptr || pTexture == nullptr || !Available())
				return false;

			pTexture->AddRef();                       // owned by the slot from now on
			*ppPrevious = pSlot->m_Sampler.m_pITex;   // the slot's reference moves to the caller
			pSlot->m_Sampler.m_pITex = pTexture;
			gEnv->pRenderer->ForceUpdateShaderItem(pItem, pTarget);
			return true;
		}

		void RestoreMaterialTexture(IMaterial* pMaterial, int subMaterial, int textureSlot, ITexture* pOurs, ITexture* pPrevious)
		{
			IMaterial* pTarget = nullptr;
			SShaderItem* pItem = GetShaderItem(pMaterial, subMaterial, &pTarget);
			SEfResTexture* pSlot = GetSlot(pItem, textureSlot);
			if (pSlot != nullptr && pSlot->m_Sampler.m_pITex == pOurs)
			{
				pSlot->m_Sampler.m_pITex = pPrevious; // previous reference goes back to the slot
				pPrevious = nullptr;
				if (pOurs != nullptr)
					pOurs->Release();                 // the reference SetMaterialTexture added
				if (Available())
					gEnv->pRenderer->ForceUpdateShaderItem(pItem, pTarget);
			}
			if (pPrevious != nullptr)
				pPrevious->Release();                  // slot was replaced or the material is gone: drop what we held
		}
	}
}
