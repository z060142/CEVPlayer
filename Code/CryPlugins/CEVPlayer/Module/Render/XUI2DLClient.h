// Access to the renderer's XUI2DL stage (docs/design/M11-xui2dl.md): the "XUI2DL_GetApi" export of a patched
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
	}
}
