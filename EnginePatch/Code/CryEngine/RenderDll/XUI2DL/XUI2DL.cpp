// XUI2DL (CEVPlayer docs/design/M11-xui2dl.md sections 4 and 5). Derived from the XUI2D render stage of
// XTFrame for CRYENGINE (CE_XTUI), MIT; see NOTICE.md. The pieces taken from XUI2D name their origin.
#include "StdAfx.h"
#include "XUI2DL.h"

#include "../XRenderD3D9/DriverD3D.h"
#include "../Common/RenderDisplayContext.h"
#include "../Common/Textures/Texture.h"
#include "../Common/RendererResources.h"
#include "../Common/Shaders/Shader.h"
#include "../Common/CryNameR.h"
#include "../Common/DevBuffer.h"
#include "../XRenderD3D9/DeviceManager/DeviceObjectHelpers.h"
#include "../XRenderD3D9/GraphicsPipeline/Common/PrimitiveRenderPass.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>

namespace XUI2DL
{

namespace
{

//////////////////////////////////////////////////////////////////////////
// Retained list (main thread) and its snapshots

struct SHeldLayer
{
	XUI2DLLayer          desc;
	_smart_ptr<CTexture> pTexture;   //!< held while the snapshot lives; null = no usable texture (drawn as skipped)
};

//! Immutable once published. References: the main thread's current pointer and one per queued draw.
struct SSnapshot
{
	std::atomic<int> refs{ 1 };
	XUI2DLRect       viewport;
	uint32           count = 0;
	SHeldLayer       layers[XUI2DL_MAX_LAYERS];
};

void SnapshotAddRef(SSnapshot* p)
{
	p->refs.fetch_add(1, std::memory_order_relaxed);
}

//! Dropping the last reference releases the textures. On the main thread CBaseResource::Release only
//! schedules the deletion (CRenderer::ScheduleResourceForDelete); on a render thread it deletes at once.
void SnapshotRelease(SSnapshot* p)
{
	if (p && p->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
		delete p;
}

//! The free command of a queued draw. As XUI2D's RT_FreeArena (XUI2DRenderer.cpp:2505-2527): on the render
//! loading thread ~CTexture asserts, so the release waits there until the loading loop has stopped.
void RT_Free(SSnapshot* p)
{
	if (gRenDev->m_pRT->IsRenderLoadingThread())
	{
		gRenDev->ExecuteRenderThreadCommand([p]() { RT_Free(p); }, ERenderCommandFlags::RenderLoadingThread_defer);
		return;
	}
	SnapshotRelease(p);
}

CryCriticalSection s_currentLock;          //!< guards s_pCurrent (main thread swaps, render thread drops it on release)
SSnapshot*         s_pCurrent = nullptr;

//////////////////////////////////////////////////////////////////////////
// Statistics (XUI2DLStats)

std::atomic<bool>   s_deviceAvailable{ false };   //!< between RT_CreateResources and RT_ReleaseResources
std::atomic<bool>   s_resourcesReady{ false };
std::atomic<bool>   s_shaderReady{ false };
std::atomic<uint32> s_layersHeld{ 0 };
std::atomic<uint32> s_outputWidth{ 0 };
std::atomic<uint32> s_outputHeight{ 0 };
std::atomic<uint64> s_framesDrawn{ 0 };
std::atomic<uint64> s_drawsExecuted{ 0 };
std::atomic<uint64> s_drawsSkipped{ 0 };

//////////////////////////////////////////////////////////////////////////
// Render-thread resources (XUI2DResources.h / .cpp, reduced)

//! Per-pass constants, register b0 of XUI2DL.cfx. 80 bytes (XUI2D::SPassConstants).
struct SPassConstants
{
	Matrix44A matViewProj;
	Vec4      params;
};

//! 24 bytes, the layout of XUI2D::SVertex: POSITION (x*w, y*w, w), TEXCOORD, COLOR r,g,b,a bytes.
struct SVertex
{
	float  x, y, w;
	float  u, v;
	uint32 rgba;
};
static_assert(sizeof(SVertex) == 24, "SVertex must match s_vertexDecl");

// XUI2DResources.cpp:21-26.
const D3D11_INPUT_ELEMENT_DESC s_vertexDecl[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
};

const uint32 kVertexBytes = XUI2DL_MAX_LAYERS * 4 * sizeof(SVertex);
const uint32 kIndexBytes = XUI2DL_MAX_LAYERS * 6 * sizeof(uint32);
const uint32 kLogEvery = 300;

enum ETechnique { eTech_Solid, eTech_Texture, eTech_TextureStraight, eTech_Count };
const char* const kTechniqueNames[eTech_Count] = { "XUI2DLSolid", "XUI2DLTexture", "XUI2DLTextureStraight" };

struct SSlot
{
	CRenderPrimitive prim;
	uint64           boundKey = 0;   //!< identity of the bound texture (XUI2DResources.cpp:716-731)
	SSlot() : prim(CRenderPrimitive::eFlags_None) {}
};

struct SResources
{
	CShader*                              pShader = nullptr;
	CCryNameTSCRC                         techniques[eTech_Count];
	bool                                  techniqueFound[eTech_Count] = {};
	bool                                  techniquePending[eTech_Count] = {};
	bool                                  prewarmDone = false;
	InputLayoutHandle                     vertexFormat = InputLayoutHandle::Unspecified;
	buffer_handle_t                       vb = ~0u;
	buffer_handle_t                       ib = ~0u;
	CConstantBufferPtr                    pPassCB;
	SPassConstants                        lastPass;
	bool                                  bHasPass = false;
	std::unique_ptr<CPrimitiveRenderPass> pPass;
	std::unique_ptr<SSlot[]>              pSlots;
	// The open pass instance (XUI2DPass.cpp:714-734): one per scissor run.
	bool                                  open = false;
	bool                                  scissor = false;
	D3DRectangle                          rect = { 0, 0, 0, 0 };
	uint32                                executions = 0;
	uint64                                skippedSinceLog = 0;
};

SResources* s_pRes = nullptr;   //!< render thread only; created lazily by RT_EnsureResources

void RT_EnsureResources();

bool ShaderValid(const SResources& r)
{
	return r.pShader && !(r.pShader->m_Flags & EF_NOTFOUND);
}

//! XUI2DResources.cpp:492-502: true when every stage of the technique is compiled; a missing stage is requested.
bool TechniqueReady(CShader* pShader, const CCryNameTSCRC& technique)
{
#if CRY_RENDERER_VULKAN
	return true;
#else
	SDeviceObjectHelpers::THwShaderInfo info;
	return SDeviceObjectHelpers::GetShaderInstanceInfo(info, pShader, technique, 0, 0, MDV_NONE, nullptr, false) != EShaderStage_None;
#endif
}

//! The prewarm without the blocking wait (XUI2DResources.cpp:506-575): requested at creation, polled per execution.
void PrewarmPoll(SResources& r)
{
	if (r.prewarmDone)
		return;
	bool all = true;
	for (int t = 0; t < eTech_Count; ++t)
	{
		if (!r.techniqueFound[t])
			all = false;
		else if (r.techniquePending[t])
		{
			r.techniquePending[t] = !TechniqueReady(r.pShader, r.techniques[t]);
			all = all && !r.techniquePending[t];
		}
	}
	if (all)
	{
		r.prewarmDone = true;
		s_shaderReady.store(true, std::memory_order_release);
	}
}

//////////////////////////////////////////////////////////////////////////
// Render-thread drawing (the XUI2DPass.cpp path for one target-0 surface, reduced)

void FlushInstance(SResources& r)
{
	if (!r.open)
		return;
	if (r.pPass->GetPrimitiveCount() > 0)
		r.pPass->Execute();
	r.open = false;
}

void OpenInstance(SResources& r, CTexture* pColor, const SRenderViewport& vp, bool bScissor, const D3DRectangle& rect)
{
	FlushInstance(r);
	r.open = true;
	r.scissor = bScissor;
	r.rect = rect;
	CPrimitiveRenderPass& pass = *r.pPass;
	pass.SetFlags(CPrimitiveRenderPass::ePassFlags_None);
	pass.SetRenderTarget(0, pColor);
	pass.SetDepthTarget(nullptr);
	pass.SetViewport(vp);
	pass.SetTargetClearMask(CPrimitiveRenderPass::eClear_None);
	pass.SetScissor(bScissor, rect);
	pass.BeginAddingPrimitives();
}

uint32 PackColor(const float c[4])
{
	auto byteOf = [](float v) { return uint32(std::lround(std::min(std::max(v, 0.0f), 1.0f) * 255.0f)); };
	return byteOf(c[0]) | (byteOf(c[1]) << 8) | (byteOf(c[2]) << 16) | (byteOf(c[3]) << 24);
}

//! The display context's current back buffer and viewport (XUI2DPass.cpp:275-296 and 654-682).
CTexture* ResolveOutput(const SDisplayContextKey& key, SRenderViewport& viewport)
{
	CRenderDisplayContext* pDC = gcpRendD3D.FindDisplayContext(key);
	if (!pDC && key == SDisplayContextKey{})
		pDC = gcpRendD3D.GetActiveDisplayContext();
	if (!pDC)
		return nullptr;
	pDC->PostPresent();   // as CRenderAuxGeomD3D::PreparePass: refresh the swap-chain back-buffer proxy first
	viewport = pDC->GetViewport();
	return pDC->GetCurrentColorOutput();
}

struct SDrawItem
{
	uint32       layer;
	ETechnique   technique;
	bool         bScissor;
	D3DRectangle rect;
};

void RT_Draw(const SSnapshot& snap, const SDisplayContextKey& key)
{
	// Lazy creation (M11 section 5): the first non-empty snapshot to execute creates the resources.
	if (!s_pRes && snap.count > 0 && s_deviceAvailable.load(std::memory_order_acquire))
		RT_EnsureResources();
	SResources* const pRes = s_pRes;
	if (!pRes || !ShaderValid(*pRes) || !pRes->pPass || !pRes->pPassCB || pRes->vb == ~0u || pRes->ib == ~0u)
	{
		s_drawsSkipped.fetch_add(snap.count, std::memory_order_relaxed);
		return;
	}
	SResources& r = *pRes;
	PROFILE_LABEL_SCOPE("XUI2DL");
	PrewarmPoll(r);

	SRenderViewport outputVp;
	CTexture* const pColor = ResolveOutput(key, outputVp);
	if (!pColor)
	{
		s_drawsSkipped.fetch_add(snap.count, std::memory_order_relaxed);
		return;
	}
	const int targetW = pColor->GetWidth();
	const int targetH = pColor->GetHeight();
	s_outputWidth.store(uint32(std::max(targetW, 0)), std::memory_order_relaxed);
	s_outputHeight.store(uint32(std::max(targetH, 0)), std::memory_order_relaxed);

	SRenderViewport vp = outputVp;
	if (snap.viewport.w > 0.0f && snap.viewport.h > 0.0f)
		vp = SRenderViewport(int(std::lround(snap.viewport.x)), int(std::lround(snap.viewport.y)), int(std::lround(snap.viewport.w)), int(std::lround(snap.viewport.h)));
	if (vp.width <= 0 || vp.height <= 0)
	{
		s_drawsSkipped.fetch_add(snap.count, std::memory_order_relaxed);
		return;
	}

	// Geometry and draw list: one quad per drawable layer, positions in viewport pixels.
	SVertex   vertices[XUI2DL_MAX_LAYERS * 4];
	uint32    indices[XUI2DL_MAX_LAYERS * 6];
	SDrawItem items[XUI2DL_MAX_LAYERS];
	uint32    itemCount = 0;
	uint64    skipped = 0;
	const int clipX0 = std::max(vp.x, 0), clipY0 = std::max(vp.y, 0);
	const int clipX1 = std::min(vp.x + vp.width, targetW), clipY1 = std::min(vp.y + vp.height, targetH);
	for (uint32 i = 0; i < snap.count; ++i)
	{
		const SHeldLayer&  held = snap.layers[i];
		const XUI2DLLayer& l = held.desc;
		SDrawItem          item;
		item.layer = i;
		float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
		if (l.kind == XUI2DL_KIND_SOLID)
			item.technique = eTech_Solid;
		else if (l.kind == XUI2DL_KIND_RGBA_PREMULT || l.kind == XUI2DL_KIND_RGBA_STRAIGHT)
		{
			CTexture* const pTex = held.pTexture;
			if (!pTex || !pTex->GetDevTexture() || pTex->GetWidth() <= 0 || pTex->GetHeight() <= 0)
			{
				++skipped;   // texture gone
				continue;
			}
			item.technique = l.kind == XUI2DL_KIND_RGBA_PREMULT ? eTech_Texture : eTech_TextureStraight;
			if (l.src.w > 0.0f && l.src.h > 0.0f)
			{
				const float tw = float(pTex->GetWidth()), th = float(pTex->GetHeight());
				u0 = l.src.x / tw;
				v0 = l.src.y / th;   // row 0 = top row = v 0
				u1 = (l.src.x + l.src.w) / tw;
				v1 = (l.src.y + l.src.h) / th;
			}
		}
		else
		{
			++skipped;   // YUV kinds: not in L1 (caps without XUI2DL_CAP_YUV)
			continue;
		}
		// Scissor in viewport pixels -> target pixels, clamped to the viewport and the target (XUI2DPass.cpp:1360-1375).
		item.bScissor = l.scissor.w > 0.0f && l.scissor.h > 0.0f;
		item.rect = { 0, 0, 0, 0 };
		if (item.bScissor)
		{
			const float x0 = float(vp.x) + l.scissor.x, y0 = float(vp.y) + l.scissor.y;
			item.rect = { LONG(std::max(float(clipX0), std::floor(x0))), LONG(std::max(float(clipY0), std::floor(y0))),
				          LONG(std::min(float(clipX1), std::ceil(x0 + l.scissor.w))), LONG(std::min(float(clipY1), std::ceil(y0 + l.scissor.h))) };
			if (item.rect.right <= item.rect.left || item.rect.bottom <= item.rect.top)
			{
				++skipped;   // entirely outside
				continue;
			}
		}
		const uint32 rgba = PackColor(l.color);
		const float  x0 = l.dst.x, y0 = l.dst.y, x1 = l.dst.x + l.dst.w, y1 = l.dst.y + l.dst.h;
		SVertex* const pV = vertices + itemCount * 4;
		pV[0] = { x0, y0, 1.0f, u0, v0, rgba };
		pV[1] = { x1, y0, 1.0f, u1, v0, rgba };
		pV[2] = { x1, y1, 1.0f, u1, v1, rgba };
		pV[3] = { x0, y1, 1.0f, u0, v1, rgba };
		const uint32 base = itemCount * 4;
		const uint32 quad[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
		memcpy(indices + itemCount * 6, quad, sizeof(quad));
		items[itemCount++] = item;
	}

	uint64 executed = 0;
	if (itemCount > 0)
	{
		// One upload per execution, then drop the command list's cached stream bindings: the BU_DYNAMIC
		// upload moves the buffers (XUI2D CE-94, XUI2DPass.cpp:448-460).
		gRenDev->m_DevBufMan.UpdateBuffer(r.vb, vertices, itemCount * 4 * sizeof(SVertex));
		gRenDev->m_DevBufMan.UpdateBuffer(r.ib, indices, itemCount * 6 * sizeof(uint32));
		GetDeviceObjectFactory().GetCoreCommandList().Reset();

		// The ortho of the viewport window (origin top-left), reverse depth, uploaded transposed (XUI2DPass.cpp:684-696).
		SPassConstants pc;
		Matrix44A      ortho;
		mathMatrixOrthoOffCenterLHReverseDepth(&ortho, 0.0f, float(vp.width), float(vp.height), 0.0f, 0.0f, 1.0f);
		pc.matViewProj = ortho.GetTransposed();
		pc.params = Vec4(1.0f / float(vp.width), 1.0f / float(vp.height), float(targetW), float(targetH));
		if (!r.bHasPass || memcmp(&r.lastPass, &pc, sizeof(pc)) != 0)
		{
			r.pPassCB->UpdateBuffer(&pc, sizeof(pc));
			r.lastPass = pc;
			r.bHasPass = true;
		}

		r.open = false;
		for (uint32 n = 0; n < itemCount; ++n)
		{
			const SDrawItem&   item = items[n];
			const XUI2DLLayer& l = snap.layers[item.layer].desc;
			// Scissor is pass-level: a change opens another instance (XUI2DPass.cpp:1376-1381).
			if (!r.open || item.bScissor != r.scissor || (item.bScissor && (item.rect.left != r.rect.left || item.rect.top != r.rect.top ||
			                                                               item.rect.right != r.rect.right || item.rect.bottom != r.rect.bottom)))
				OpenInstance(r, pColor, vp, item.bScissor, item.rect);

			CTexture* pTex = snap.layers[item.layer].pTexture;
			uint64    key = 0;
			if (item.technique == eTech_Solid || !pTex)
				pTex = CRendererResources::s_ptexWhite;
			else
				key = (1ull << 40) | uint64(uint32(pTex->GetID()));

			SSlot&            slot = r.pSlots[n];
			CRenderPrimitive& prim = slot.prim;
			// Blend states of XUI2DPass.cpp:59-67 (premultiplied sources).
			const uint64 renderState = GS_NODEPTHTEST | (l.blend == XUI2DL_BLEND_SRCOVER ? uint64(GS_BLSRC_ONE | GS_BLDST_ONEMINUSSRCALPHA) : 0);
			prim.SetFlags(CRenderPrimitive::eFlags_None);
			prim.SetTechnique(r.pShader, r.techniques[item.technique], 0);
			prim.SetRenderState(renderState);
			prim.SetStencilState(0, 0, 0xFF, 0xFF);
			prim.SetCullMode(eCULL_None);
			prim.SetEnableDepthClip(true);
			prim.SetInlineConstantBuffer(eConstantBufferShaderSlot_PerDraw, r.pPassCB, EShaderStage_Vertex | EShaderStage_Pixel);
			if (slot.boundKey != key)
			{
				// A different texture may sit at the address bound last time: make the resource set see a change.
				prim.SetTexture(0, CRendererResources::s_ptexWhite, EDefaultResourceViews::Default, EShaderStage_Pixel);
				slot.boundKey = key;
			}
			prim.SetTexture(0, pTex, EDefaultResourceViews::Default, EShaderStage_Pixel);
			prim.SetSampler(0, SamplerStateHandle(EDefaultSamplerStates::LinearClamp), EShaderStage_Pixel);
			prim.SetPrimitiveType(CRenderPrimitive::ePrim_Custom);
			prim.SetCustomVertexStream(r.vb, r.vertexFormat, sizeof(SVertex));
			prim.SetCustomIndexStream(r.ib, RenderIndexType::Index32);
			prim.SetDrawInfo(eptTriangleList, 0, n * 6, 6);
			prim.Compile(*r.pPass);
			if (r.pPass->AddPrimitive(&prim))
				++executed;
			else
				++skipped;   // pipeline state not ready (async shader compile) or invalid
		}
		FlushInstance(r);
	}

	s_drawsExecuted.fetch_add(executed, std::memory_order_relaxed);
	s_drawsSkipped.fetch_add(skipped, std::memory_order_relaxed);
	if (executed > 0)
		s_framesDrawn.fetch_add(1, std::memory_order_relaxed);
	r.skippedSinceLog += skipped;
	if (++r.executions % kLogEvery == 0 && r.skippedSinceLog > 0)
	{
		CryLogAlways("[XUI2DL] %llu layer draw(s) skipped over the last %u executions (pipeline state pending, texture gone or unsupported kind; shader %s)",
		             (unsigned long long)r.skippedSinceLog, kLogEvery, r.prewarmDone ? "ready" : "compiling");
		r.skippedSinceLog = 0;
	}
}

//////////////////////////////////////////////////////////////////////////
// Interface

int SetFrame(const XUI2DLFrame* pFrame)
{
	if (!gRenDev || !gRenDev->m_pRT->IsMainThread())
		return 0;
	if (!pFrame || pFrame->size != sizeof(XUI2DLFrame) || pFrame->layerCount > XUI2DL_MAX_LAYERS || (pFrame->layerCount > 0 && !pFrame->pLayers))
		return 0;
	for (uint32 i = 0; i < pFrame->layerCount; ++i)
	{
		const XUI2DLLayer& l = pFrame->pLayers[i];
		if (l.size != sizeof(XUI2DLLayer) || l.kind > XUI2DL_KIND_YUVA420 || l.blend > XUI2DL_BLEND_SRCOVER)
			return 0;
	}

	SSnapshot* pNew = nullptr;
	if (pFrame->layerCount > 0)
	{
		pNew = new SSnapshot();
		pNew->viewport = pFrame->viewport;
		pNew->count = pFrame->layerCount;
		for (uint32 i = 0; i < pFrame->layerCount; ++i)
		{
			SHeldLayer& held = pNew->layers[i];
			held.desc = pFrame->pLayers[i];
			if (held.desc.kind == XUI2DL_KIND_SOLID)
				continue;
			// GetByID answers an unknown id with the NoTexture placeholder, never null: no texture then.
			// The caller owns the texture while it calls, so the reference taken here cannot race its deletion.
			CTexture* const pTex = CTexture::GetByID(held.desc.textureId);
			if (pTex && pTex != CRendererResources::s_ptexNoTexture)
				held.pTexture = pTex;
		}
	}

	SSnapshot* pOld = nullptr;
	{
		CryAutoCriticalSection lock(s_currentLock);
		pOld = s_pCurrent;
		s_pCurrent = pNew;
		s_layersHeld.store(pNew ? pNew->count : 0u, std::memory_order_relaxed);
	}
	SnapshotRelease(pOld);   // main thread: texture deletions are only scheduled
	return 1;
}

void GetStats(XUI2DLStats* pStats)
{
	if (!pStats || pStats->size == 0)
		return;
	XUI2DLStats s;
	memset(&s, 0, sizeof(s));
	s.resourcesReady = s_resourcesReady.load(std::memory_order_acquire) ? 1u : 0u;
	s.shaderReady = s_shaderReady.load(std::memory_order_acquire) ? 1u : 0u;
	s.layersHeld = s_layersHeld.load(std::memory_order_relaxed);
	s.outputWidth = s_outputWidth.load(std::memory_order_relaxed);
	s.outputHeight = s_outputHeight.load(std::memory_order_relaxed);
	s.framesDrawn = s_framesDrawn.load(std::memory_order_relaxed);
	s.drawsExecuted = s_drawsExecuted.load(std::memory_order_relaxed);
	s.drawsSkipped = s_drawsSkipped.load(std::memory_order_relaxed);
	const uint32 bytes = std::min<uint32>(pStats->size, uint32(sizeof(s)));
	s.size = bytes;
	memcpy(pStats, &s, bytes);
}

const XUI2DLApi s_api = { uint32(sizeof(XUI2DLApi)), XUI2DL_API_VERSION, XUI2DL_CAP_CORE, 0, &SetFrame, &GetStats };

//! Render thread, from the first RT_Draw of a non-empty snapshot. A technique still compiling is only
//! requested here; its draws are skipped and counted until it is ready.
void RT_EnsureResources()
{
	if (s_pRes)
		return;
	SResources* const pRes = new SResources();
	SResources&       r = *pRes;

	r.pShader = gRenDev->m_cEF.mfForName("XUI2DL", EF_SYSTEM);
	const bool bShader = ShaderValid(r);
	if (!bShader)
		CryLogAlways("[XUI2DL] shader 'XUI2DL' not found (%s); check Engine/Shaders/HWScripts/CryFX/XUI2DL.cfx", r.pShader ? "EF_NOTFOUND" : "null");
	uint32 found = 0, ready = 0;
	for (int t = 0; t < eTech_Count; ++t)
	{
		r.techniques[t] = CCryNameTSCRC(kTechniqueNames[t]);
		r.techniqueFound[t] = bShader && r.pShader->mfFindTechnique(r.techniques[t]) != nullptr;
		if (!r.techniqueFound[t])
		{
			if (bShader)
				CryLogAlways("[XUI2DL] shader 'XUI2DL' has no technique '%s'", kTechniqueNames[t]);
			continue;
		}
		++found;
		r.techniquePending[t] = true;   // PrewarmPoll requests the instances (asynchronous compile) and polls them
	}

	r.vertexFormat = CDeviceObjectFactory::CreateCustomVertexFormat(CRY_ARRAY_COUNT(s_vertexDecl), s_vertexDecl);
	r.vb = gRenDev->m_DevBufMan.Create(BBT_VERTEX_BUFFER, BU_DYNAMIC, kVertexBytes);
	r.ib = gRenDev->m_DevBufMan.Create(BBT_INDEX_BUFFER, BU_DYNAMIC, kIndexBytes);
	r.pPassCB = gRenDev->m_DevBufMan.CreateConstantBuffer(sizeof(SPassConstants));
	if (r.pPassCB)
		r.pPassCB->SetDebugName("XUI2DL pass CB");
	r.pPass.reset(new CPrimitiveRenderPass(true));
	r.pSlots.reset(new SSlot[XUI2DL_MAX_LAYERS]);
	s_pRes = pRes;

	if (bShader)
	{
		PrewarmPoll(r);
		for (int t = 0; t < eTech_Count; ++t)
			ready += (r.techniqueFound[t] && !r.techniquePending[t]) ? 1u : 0u;
	}
	const bool bBuffers = r.vb != ~0u && r.ib != ~0u && r.pPassCB;
	s_resourcesReady.store(bBuffers, std::memory_order_release);
	CryLogAlways("[XUI2DL] render resources created (shader %s, %u/%d technique(s), %u ready from the shader cache, buffers %s)",
	             bShader ? "ok" : "MISSING", found, int(eTech_Count), ready, bBuffers ? "ok" : "FAILED");
}

} // namespace

//////////////////////////////////////////////////////////////////////////
// Engine entry points

void RT_CreateResources()
{
	// Nothing is created here (M11 section 5, lazy resources): only the device is noted as available.
	s_deviceAvailable.store(true, std::memory_order_release);
}

void RT_ReleaseResources()
{
	s_deviceAvailable.store(false, std::memory_order_release);
	SSnapshot* pDropped = nullptr;
	{
		CryAutoCriticalSection lock(s_currentLock);
		pDropped = s_pCurrent;
		s_pCurrent = nullptr;
		s_layersHeld.store(0, std::memory_order_relaxed);
	}
	SnapshotRelease(pDropped);   // render thread (resource release): the textures go at once

	SResources* const pRes = s_pRes;
	if (!pRes)
		return;
	s_resourcesReady.store(false, std::memory_order_release);
	s_shaderReady.store(false, std::memory_order_release);
	s_pRes = nullptr;
	// Primitives and the pass first, then the buffers they reference, then the shader (XUI2DResources.cpp:607-628).
	pRes->pSlots.reset();
	pRes->pPass.reset();
	pRes->pPassCB = nullptr;
	if (pRes->vb != ~0u)
		gRenDev->m_DevBufMan.Destroy(pRes->vb);
	if (pRes->ib != ~0u)
		gRenDev->m_DevBufMan.Destroy(pRes->ib);
	SAFE_RELEASE(pRes->pShader);
	delete pRes;
	CryLogAlways("[XUI2DL] render resources released");
}

void EndFrame(const SDisplayContextKey& key)
{
	if (!s_deviceAvailable.load(std::memory_order_acquire))
		return;
	SSnapshot* pSnap = nullptr;
	{
		CryAutoCriticalSection lock(s_currentLock);
		pSnap = s_pCurrent;
		if (pSnap)
			SnapshotAddRef(pSnap);
	}
	if (!pSnap)
		return;
	// The draw is skipped while the loading loop owns the back buffer (L1; the loop is L3). Its lambda only
	// carries a raw pointer, so dropping it on the loading thread releases nothing; the free command always runs.
	const SDisplayContextKey keyCopy = key;
	gRenDev->ExecuteRenderThreadCommand([pSnap, keyCopy]() { RT_Draw(*pSnap, keyCopy); }, ERenderCommandFlags::SkipDuringLoading);
	gRenDev->ExecuteRenderThreadCommand([pSnap]() { RT_Free(pSnap); }, ERenderCommandFlags::None);
}

} // namespace XUI2DL

extern "C" __declspec(dllexport) const XUI2DLApi* XUI2DL_GetApi(uint32_t version)
{
	return version == XUI2DL_API_VERSION ? &XUI2DL::s_api : nullptr;
}
