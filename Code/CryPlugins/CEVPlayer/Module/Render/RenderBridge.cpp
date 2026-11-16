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

		int CreateTexture(const char* name, int width, int height)
		{
			if (!Available() || width <= 0 || height <= 0)
				return 0;
			const int id = static_cast<int>(gEnv->pRenderer->UploadToVideoMemory(nullptr, width, height, eTF_R8G8B8A8, eTF_R8G8B8A8, 0, false, FILTER_BILINEAR, 0, name, FT_NOMIPS));
			LogDebug("texture <%s> id <%d> %dx%d created", name, id, width, height);
			return id;
		}

		bool UpdateTexture(int textureId, const uint8_t* rgba, int width, int height)
		{
			if (!Available() || textureId <= 0 || rgba == nullptr)
				return false;
			gEnv->pRenderer->UpdateTextureInVideoMemory(static_cast<uint32>(textureId), const_cast<unsigned char*>(rgba), 0, 0, width, height, eTF_R8G8B8A8);
			return true;
		}

		ITexture* GetTexture(int textureId)
		{
			if (!Available() || textureId <= 0)
				return nullptr;
			return gEnv->pRenderer->EF_GetTextureByID(textureId);
		}

		void RemoveTexture(int textureId)
		{
			if (!Available() || textureId <= 0)
				return;
			LogDebug("texture id <%d> removed", textureId);
			gEnv->pRenderer->RemoveTexture(static_cast<unsigned int>(textureId));
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
