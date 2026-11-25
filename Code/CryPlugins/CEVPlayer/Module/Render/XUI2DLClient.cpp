#include "StdAfx.h"
#include "PluginEnv.h"
#include "XUI2DLClient.h"

#include <CryCore/Platform/CryWindows.h> // GetModuleHandleA / GetProcAddress
#include <CryRenderer/IRenderer.h>

namespace CEVPlayer
{
	namespace XUI2DLClient
	{
		namespace
		{
			bool s_probed = false;
			const XUI2DLApi* s_pApi = nullptr;

			const char* RendererModuleName(IRenderer* pRenderer)
			{
				switch (pRenderer->GetRenderType())
				{
				case ERenderType::Direct3D11: return "CryRenderD3D11";
				case ERenderType::Direct3D12: return "CryRenderD3D12";
				case ERenderType::Vulkan:     return "CryRenderVulkan";
				default:                      return nullptr;
				}
			}
		}

		const XUI2DLApi* Get()
		{
			if (s_probed)
				return s_pApi;
			if (gEnv == nullptr || gEnv->pRenderer == nullptr || gEnv->IsDedicated())
				return nullptr; // no renderer yet: not cached, probed again once there is one
			s_probed = true;
#if CRY_PLATFORM_WINDOWS
			if (const char* moduleName = RendererModuleName(gEnv->pRenderer))
			{
				if (HMODULE hModule = ::GetModuleHandleA(moduleName))
				{
					using PFN_GetApi = const XUI2DLApi* (*)(uint32_t);
					if (const PFN_GetApi pGetApi = reinterpret_cast<PFN_GetApi>(::GetProcAddress(hModule, "XUI2DL_GetApi")))
					{
						const XUI2DLApi* const pApi = pGetApi(XUI2DL_API_VERSION);
						if (pApi != nullptr && pApi->size >= sizeof(XUI2DLApi) && pApi->version == XUI2DL_API_VERSION)
							s_pApi = pApi;
					}
				}
			}
#endif
			if (s_pApi != nullptr)
				LogAlways("renderer probe: XUI2DL_GetApi found (version %u, caps 0x%x)", s_pApi->version, s_pApi->caps);
			else
				LogAlways("renderer probe: XUI2DL_GetApi not found or version %d refused (no XUI2DL stage)", XUI2DL_API_VERSION);
			return s_pApi;
		}
	}
}
