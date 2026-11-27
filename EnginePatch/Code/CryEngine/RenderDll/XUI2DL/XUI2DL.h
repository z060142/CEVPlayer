// Portions derived from XTFrame for CRYENGINE (XUI2D), Copyright (c) 2026 XTFrame contributors, MIT license.
// XUI2DL: the minimal 2D stage CEVPlayer draws fullscreen video with.
// Derived from the XUI2D render stage of XTFrame for CRYENGINE (CE_XTUI), MIT; see NOTICE.md.
//
// One retained list of up to XUI2DL_MAX_LAYERS layers, replaced by XUI2DLApi::SetFrame on the main
// thread and drawn onto the active output by every EndFrame (after Scaleform, before XUI2D and aux).
#pragma once

#include "XUI2DLApi.h"

struct SDisplayContextKey;
struct ILoadtimeCallback;

namespace XUI2DL
{

//! Render thread, end of CD3D9Renderer::RT_CreateRenderResources: shader, vertex format, buffers,
//! primitives; requests the shader instances (prewarm, never waited for).
void RT_CreateResources();
//! Render thread, the FRR_SYSTEM_RESOURCES branch of CD3D9Renderer::RT_ReleaseRenderResources (also every level
//! unload): releases the device objects. The retained list stays (it is the caller's state).
void RT_ReleaseResources();
//! Render thread, the FRR_TEXTURES branch of CD3D9Renderer::RT_ReleaseRenderResources (renderer shutdown), before
//! CTexture::ShutDown: drops the retained list and with it the texture references.
void RT_ReleaseRetained();
//! Main thread, CD3D9Renderer::EndFrame: queues the draw of the retained list (nothing when it is empty).
void EndFrame(const SDisplayContextKey& key);

// The loading-loop driver (M11 section 7; engines without CE_XTUI patch 17). These hooks are inserted only
// where apply_xui2dl.py found no patch 17; it then also defines XUI2DL_LOADTIME_HOOKS=1 for the renderer.

//! Render thread, the loading loop of SRenderThread::Process, right after the StartLoadtimeFlashPlayback
//! callback's LoadtimeUpdate (outside m_rdldLock): calls the update registered with SetLoadtimeUpdate.
void RT_LoadtimeUpdate(float dtSeconds);
//! Render thread, the loading loop of SRenderThread::Process, inside m_rdldLock, after the StartLoadtimeFlashPlayback
//! callback's LoadtimeRender and before RT_PresentFast: draws the retained list (clearing the back buffer to opaque
//! black first unless a flash callback drew). Nothing when the list is empty or the resources / shader are not ready.
void RT_LoadtimeRender();
//! Main thread, system event LEVEL_LOAD_START_LOADINGSCREEN: starts the loading loop when an update is
//! registered and no loop runs (not in the editor, only with a render thread, not for asynchronous loads).
//! With a non-empty retained list it first has the resources created, the shader prewarmed and every layer's primitive
//! compiled (at most 2 s); on timeout that loop draws nothing.
void LoadtimeAutoStart();
//! Main thread, system events LEVEL_LOAD_END / LEVEL_LOAD_ERROR / FAST_SHUTDOWN: stops the loop
//! LoadtimeAutoStart started, unless a StartLoadtimeFlashPlayback caller joined it (that caller stops it).
void LoadtimeAutoRelease();
//! Main thread, CD3D9Renderer::StartLoadtimeFlashPlayback before it starts a loop: when the loop already
//! runs (started by LoadtimeAutoStart), installs `pCallback` into it and returns true (the caller returns).
bool LoadtimeJoin(ILoadtimeCallback* pCallback);
//! Main thread, start of CD3D9Renderer::StopLoadtimeFlashPlayback: makes the stock stop path also stop a
//! loop that runs without a StartLoadtimeFlashPlayback callback.
void LoadtimeBeforeStop();

} // namespace XUI2DL

//! The interface table (XUI2DLApi.h); nullptr when `version` is not XUI2DL_API_VERSION.
extern "C" __declspec(dllexport) const XUI2DLApi* XUI2DL_GetApi(uint32_t version);
