#include "StdAfx.h"
#include "PluginEnv.h"
#include "XUI2DLClient.h"

#include <CryCore/Platform/CryWindows.h> // GetModuleHandleA / GetProcAddress
#include <CryRenderer/IRenderer.h>

#include <cstddef> // offsetof

namespace CEVPlayer
{
	namespace XUI2DLClient
	{
		namespace
		{
			bool s_probed = false;
			const XUI2DLApi* s_pApi = nullptr;
			// The table as the first renderers had it (up to GetStats); later members are checked by `size` (L3a).
			constexpr uint32_t kBaseSize = static_cast<uint32_t>(offsetof(XUI2DLApi, SetLoadtimeUpdate));

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
						if (pApi != nullptr && pApi->size >= kBaseSize && pApi->version == XUI2DL_API_VERSION)
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

		SetLoadtimeUpdateFn LoadtimeUpdateEntry()
		{
			const XUI2DLApi* const pApi = Get();
			if (pApi == nullptr || pApi->size < offsetof(XUI2DLApi, SetLoadtimeUpdate) + sizeof(pApi->SetLoadtimeUpdate)
				|| (pApi->caps & XUI2DL_CAP_LOADTIME) == 0)
				return nullptr;
			return pApi->SetLoadtimeUpdate;
		}
	}
}
