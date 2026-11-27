// Access to the renderer's XUI2DL stage: the "XUI2DL_GetApi" export of a patched
// renderer DLL (EnginePatch/apply_xui2dl.py). Main thread.
#pragma once

#include "XUI2DLApi.h"

namespace CEVPlayer
{
	namespace XUI2DLClient
	{
		//! The interface table of the renderer module, or nullptr (stock renderer, no renderer yet, other version).
		//! Looked up once a renderer exists, then cached; the result is logged once.
		const XUI2DLApi* Get();

		using SetLoadtimeUpdateFn = int (*)(void (*pfnUpdate)(void* pUser, float dtSeconds), void* pUser);
		//! L3a: XUI2DLApi::SetLoadtimeUpdate when the renderer's table has that member (`size`) and its caps have
		//! XUI2DL_CAP_LOADTIME; nullptr otherwise (no XUI2DL, an older renderer, or one built over CE_XTUI patch 17).
		SetLoadtimeUpdateFn LoadtimeUpdateEntry();
	}
}
