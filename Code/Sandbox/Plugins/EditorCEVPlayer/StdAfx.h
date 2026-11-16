// EditorCEVPlayer: precompiled header.
#pragma once

#include <CryCore/Platform/platform.h>

#ifdef CRY_PLATFORM_WINDOWS
// MFC like the other Sandbox plug-ins: the EditorCommon headers of QViewport (Objects/BaseObject.h, Viewport.h, ...) use
// CObject, CPoint and CRect. _AFXDLL comes from CryEditorPlugin. CryAtlMfc.h includes <windows.h> itself.
#define CRY_USE_MFC
#include <CryCore/Platform/CryAtlMfc.h>
#endif

#include "EditorCommon.h"
