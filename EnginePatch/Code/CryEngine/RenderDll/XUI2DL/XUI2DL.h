// XUI2DL: the minimal 2D stage CEVPlayer draws fullscreen video with (docs/design/M11-xui2dl.md).
// Derived from the XUI2D render stage of XTFrame for CRYENGINE (CE_XTUI), MIT; see NOTICE.md.
//
// One retained list of up to XUI2DL_MAX_LAYERS layers, replaced by XUI2DLApi::SetFrame on the main
// thread and drawn onto the active output by every EndFrame (after Scaleform, before XUI2D and aux).
#pragma once

#include "XUI2DLApi.h"

struct SDisplayContextKey;

namespace XUI2DL
{

//! Render thread, end of CD3D9Renderer::RT_CreateRenderResources: shader, vertex format, buffers,
//! primitives; requests the shader instances (prewarm, never waited for).
void RT_CreateResources();
//! Render thread, the FRR_SYSTEM_RESOURCES branch of CD3D9Renderer::RT_ReleaseRenderResources:
//! releases the device objects and drops the retained list.
void RT_ReleaseResources();
//! Main thread, CD3D9Renderer::EndFrame: queues the draw of the retained list (nothing when it is empty).
void EndFrame(const SDisplayContextKey& key);

} // namespace XUI2DL

//! The interface table (XUI2DLApi.h); nullptr when `version` is not XUI2DL_API_VERSION.
extern "C" __declspec(dllexport) const XUI2DLApi* XUI2DL_GetApi(uint32_t version);
