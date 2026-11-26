// Portions derived from XTFrame for CRYENGINE (XUI2D), Copyright (c) 2026 XTFrame contributors, MIT license.
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
#include "../XRenderD3D9/GraphicsPipeline/Common/UtilityPasses.h"
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
std::atomic<uint64> s_loopFramesDrawn{ 0 };   //!< loading-loop iterations that drew at least one layer
// The loading-loop prewarm (LoadtimePrewarm / RT_PrepareForLoop).
std::atomic<bool>   s_loopReady{ false };       //!< every layer of the current snapshot compiled its primitive
std::atomic<bool>   s_loopNoDraw{ false };      //!< the prewarm timed out: this loading loop does not draw
std::atomic<bool>   s_loopFailLogged{ false };  //!< a failed draw in this loading loop was logged once

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

//! Per-draw constants of XUI2DLPlanar, register b1 (XTFrame 23-host-integration.md section 23.aj, "PlanarParams").
//! Planes in the order Y, U, V, A. 192 bytes.
struct SPlanarConstants
{
	Vec4 uv[4];          //!< picture uv -> texture uv: scale.xy, offset.zw (chroma siting folded into U and V)
	Vec4 clampRect[4];   //!< the plane rectangle inset by half a texel, texture uv (x0, y0, x1, y1)
	Vec4 matrix[3];      //!< rows R, G, B: cY, cU, cV, offset
	Vec4 flags;          //!< x = 1 with an alpha plane
};
static_assert(sizeof(SPlanarConstants) == 192, "SPlanarConstants must match CBXUI2DLPlanar");

//! CE-111 of XUI2D (XUI2DPass.cpp:42-48): every draw on a slot binds b1 with this same stage mask.
const EShaderStage kPlanarCBStages = EShaderStage_Pixel;

enum ETechnique { eTech_Solid, eTech_Texture, eTech_TextureStraight, eTech_Planar, eTech_Count };
const char* const kTechniqueNames[eTech_Count] = { "XUI2DLSolid", "XUI2DLTexture", "XUI2DLTextureStraight", "XUI2DLPlanar" };

struct SSlot
{
	CRenderPrimitive   prim;
	uint64             boundKey = 0;   //!< identity of the bound texture (XUI2DResources.cpp:716-731)
	CConstantBufferPtr pPlanarCB;      //!< b1, bound on every draw of the slot; read only by XUI2DLPlanar
	SPlanarConstants   lastPlanar;
	bool               bHasPlanar = false;
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
	bool                                  bSlotCBs = false;   //!< every slot's b1 buffer exists
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
	uint32           layer;
	ETechnique       technique;
	bool             bScissor;
	D3DRectangle     rect;
	SPlanarConstants planar;   //!< eTech_Planar only
};

//! The plane lies inside the texture and is not empty.
bool PlaneInside(const XUI2DLPlane& p, uint32 texW, uint32 texH)
{
	return p.w > 0 && p.h > 0 && p.x < texW && p.y < texH && p.w <= texW - p.x && p.h <= texH - p.y;
}

//! Section 23.aj, "繪製": a plane sampled at luma position uv * (lumaW, lumaH), `step` luma pixels per plane texel
//! (1, or 2 for chroma), plus `offX`/`offY` plane texels of chroma siting; the clamp is the rectangle inset by half a texel.
void PlaneMapping(const XUI2DLPlane& p, double lumaW, double lumaH, double step, double offX, double offY, double texW, double texH,
                  Vec4& uv, Vec4& clampRect)
{
	uv = Vec4(float(lumaW / step / texW), float(lumaH / step / texH), float((p.x + offX) / texW), float((p.y + offY) / texH));
	clampRect = Vec4(float((p.x + 0.5) / texW), float((p.y + 0.5) / texH), float((p.x + p.w - 0.5) / texW), float((p.y + p.h - 0.5) / texH));
}

//! The constants of a YUV layer from its description and the texture's actual size; false = invalid description
//! (a plane outside the texture or empty, picture size 0, an enum out of range, a texture that is not R8).
bool PlanarConstants(const XUI2DLLayer& l, const CTexture& tex, SPlanarConstants& out)
{
	const bool alpha = l.kind == XUI2DL_KIND_YUVA420;
	const int  tw = tex.GetWidth(), th = tex.GetHeight();
	if (tex.GetDstFormat() != eTF_R8 || tw <= 0 || th <= 0 || l.imageWidth == 0 || l.imageHeight == 0 ||
	    l.colorMatrix > 1 || l.colorRange > 1 || l.chromaSiting > 2)
		return false;
	if (!PlaneInside(l.planeY, uint32(tw), uint32(th)) || !PlaneInside(l.planeU, uint32(tw), uint32(th)) ||
	    !PlaneInside(l.planeV, uint32(tw), uint32(th)) || (alpha && !PlaneInside(l.planeA, uint32(tw), uint32(th))))
		return false;

	memset(&out, 0, sizeof(out));
	// Chroma siting (0 left, 1 center, 2 top-left): Xc = X/2 (+0.25), Yc = Y/2 (+0.25) in plane texels.
	const double cx = l.chromaSiting == 1 ? 0.0 : 0.25;
	const double cy = l.chromaSiting == 2 ? 0.25 : 0.0;
	const double lumaW = l.planeY.w, lumaH = l.planeY.h;
	PlaneMapping(l.planeY, lumaW, lumaH, 1.0, 0.0, 0.0, tw, th, out.uv[0], out.clampRect[0]);
	PlaneMapping(l.planeU, lumaW, lumaH, 2.0, cx, cy, tw, th, out.uv[1], out.clampRect[1]);
	PlaneMapping(l.planeV, lumaW, lumaH, 2.0, cx, cy, tw, th, out.uv[2], out.clampRect[2]);
	if (alpha)
		PlaneMapping(l.planeA, lumaW, lumaH, 1.0, 0.0, 0.0, tw, th, out.uv[3], out.clampRect[3]);

	// Limited: Y' = (y - 16/255) * 255/219, C = (c - 128/255) * 255/224; full: Y' = y, C = c - 128/255.
	// R = Y' + 2(1-Kr) Cr, B = Y' + 2(1-Kb) Cb, G = (Y' - Kr R - Kb B) / (1-Kr-Kb).
	const double kr = l.colorMatrix == 1 ? 0.2126 : 0.299;
	const double kb = l.colorMatrix == 1 ? 0.0722 : 0.114;
	const double kg = 1.0 - kr - kb;
	const bool   limited = l.colorRange == 0;
	const double ys = limited ? 255.0 / 219.0 : 1.0;
	const double yo = limited ? -16.0 / 219.0 : 0.0;
	const double cs = limited ? 255.0 / 224.0 : 1.0;
	const double co = limited ? -128.0 / 224.0 : -128.0 / 255.0;
	const double rv = 2.0 * (1.0 - kr), bu = 2.0 * (1.0 - kb);
	const double gv = rv * kr / kg, gu = bu * kb / kg;
	out.matrix[0] = Vec4(float(ys), 0.0f, float(rv * cs), float(yo + rv * co));
	out.matrix[1] = Vec4(float(ys), float(-gu * cs), float(-gv * cs), float(yo - (gu + gv) * co));
	out.matrix[2] = Vec4(float(ys), float(bu * cs), 0.0f, float(yo + bu * co));
	out.flags = Vec4(alpha ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
	return true;
}

bool ResourcesUsable(const SResources& r)
{
	return ShaderValid(r) && r.pPass && r.pPassCB && r.bSlotCBs && r.vb != ~0u && r.ib != ~0u;
}

//! Sets up the primitive of `slot` for one layer and compiles it against the open pass instance. Shared by the draw
//! (RT_DrawLayers) and the loading-loop prewarm (RT_CompileSnapshot), so both make the same pipeline state request.
//! `pTex` is the texture to bind (s_ptexWhite for solid layers), `key` its identity (0 for the white texture).
//! Returns true when the primitive compiled (CRenderPrimitive::Compile left nothing dirty).
bool PreparePrimitive(SResources& r, SSlot& slot, ETechnique technique, const XUI2DLLayer& l, CTexture* pTex, uint64 key, uint32 n)
{
	CRenderPrimitive& prim = slot.prim;
	// Blend states of XUI2DPass.cpp:59-67 (premultiplied sources).
	const uint64 renderState = GS_NODEPTHTEST | (l.blend == XUI2DL_BLEND_SRCOVER ? uint64(GS_BLSRC_ONE | GS_BLDST_ONEMINUSSRCALPHA) : 0);
	prim.SetFlags(CRenderPrimitive::eFlags_None);
	prim.SetTechnique(r.pShader, r.techniques[technique], 0);
	prim.SetRenderState(renderState);
	prim.SetStencilState(0, 0, 0xFF, 0xFF);
	prim.SetCullMode(eCULL_None);
	prim.SetEnableDepthClip(true);
	prim.SetInlineConstantBuffer(eConstantBufferShaderSlot_PerDraw, r.pPassCB, EShaderStage_Vertex | EShaderStage_Pixel);
	prim.SetInlineConstantBuffer(eConstantBufferShaderSlot_PerMaterial, slot.pPlanarCB, kPlanarCBStages);
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
	return prim.Compile(*r.pPass) == CRenderPrimitive::eDirty_None;
}

//! Draws the snapshot onto the output of `key`; returns the number of layer draws executed. Shared by the main
//! path (RT_Draw) and the loading loop (RT_LoadtimeRender); `bClearFirst` clears the output to opaque black first.
uint64 RT_DrawLayers(SResources& r, const SSnapshot& snap, const SDisplayContextKey& key, bool bClearFirst)
{
	PROFILE_LABEL_SCOPE("XUI2DL");
	SRenderViewport outputVp;
	CTexture* const pColor = ResolveOutput(key, outputVp);
	if (!pColor)
	{
		s_drawsSkipped.fetch_add(snap.count, std::memory_order_relaxed);
		return 0;
	}
	// The loading loop does not clear the back buffer (XUI2D's ClearNow, XUI2DRenderer.cpp:675-697).
	if (bClearFirst && pColor->GetDevTexture())
		CClearSurfacePass::Execute(pColor, ColorF(0.0f, 0.0f, 0.0f, 1.0f));
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
		return 0;
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
			// YUV kinds: the quad's uv is picture uv; `src` is in picture pixels.
			CTexture* const pTex = held.pTexture;
			if (!pTex || !pTex->GetDevTexture() || !PlanarConstants(l, *pTex, item.planar))
			{
				++skipped;   // texture gone or invalid description
				continue;
			}
			item.technique = eTech_Planar;
			if (l.src.w > 0.0f && l.src.h > 0.0f)
			{
				const float iw = float(l.imageWidth), ih = float(l.imageHeight);
				u0 = l.src.x / iw;
				v0 = l.src.y / ih;
				u1 = (l.src.x + l.src.w) / iw;
				v1 = (l.src.y + l.src.h) / ih;
			}
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
			if (item.technique == eTech_Planar && (!slot.bHasPlanar || memcmp(&slot.lastPlanar, &item.planar, sizeof(item.planar)) != 0))
			{
				slot.pPlanarCB->UpdateBuffer(&item.planar, sizeof(item.planar));
				slot.lastPlanar = item.planar;
				slot.bHasPlanar = true;
			}
			PreparePrimitive(r, slot, item.technique, l, pTex, key, n);
			if (r.pPass->AddPrimitive(&prim))
				++executed;
			else
				++skipped;   // pipeline state not ready (async shader compile) or invalid
		}
		FlushInstance(r);
	}

	s_drawsExecuted.fetch_add(executed, std::memory_order_relaxed);
	s_drawsSkipped.fetch_add(skipped, std::memory_order_relaxed);
	r.skippedSinceLog += skipped;
	if (++r.executions % kLogEvery == 0 && r.skippedSinceLog > 0)
	{
		CryLogAlways("[XUI2DL] %llu layer draw(s) skipped over the last %u executions (pipeline state pending, texture gone or unsupported kind; shader %s)",
		             (unsigned long long)r.skippedSinceLog, kLogEvery, r.prewarmDone ? "ready" : "compiling");
		r.skippedSinceLog = 0;
	}
	return executed;
}

//! Render thread, the loading-loop prewarm: for every layer of `snap` that RT_DrawLayers would draw, sets up and compiles
//! its slot's primitive against the active output with the same call the draw uses (PreparePrimitive), without drawing.
//! True when every compile succeeded (the pipeline states the loop needs are valid).
bool RT_CompileSnapshot(SResources& r, const SSnapshot& snap)
{
	SRenderViewport outputVp;
	CTexture* const pColor = ResolveOutput(SDisplayContextKey{}, outputVp);
	if (!pColor || outputVp.width <= 0 || outputVp.height <= 0)
		return false;
	const D3DRectangle noRect = { 0, 0, 0, 0 };
	OpenInstance(r, pColor, outputVp, false, noRect);
	bool   all = true;
	uint32 n = 0;
	for (uint32 i = 0; i < snap.count; ++i)
	{
		const SHeldLayer&  held = snap.layers[i];
		const XUI2DLLayer& l = held.desc;
		CTexture*          pTex = held.pTexture;
		ETechnique         technique = eTech_Solid;
		if (l.kind == XUI2DL_KIND_SOLID)
			pTex = nullptr;
		else if (!pTex || !pTex->GetDevTexture())
			continue;   // texture gone: RT_DrawLayers skips this layer too
		else if (l.kind == XUI2DL_KIND_RGBA_PREMULT)
			technique = eTech_Texture;
		else if (l.kind == XUI2DL_KIND_RGBA_STRAIGHT)
			technique = eTech_TextureStraight;
		else
			technique = eTech_Planar;
		const uint64 key = pTex ? ((1ull << 40) | uint64(uint32(pTex->GetID()))) : 0;
		all = PreparePrimitive(r, r.pSlots[n], technique, l, pTex ? pTex : CRendererResources::s_ptexWhite, key, n) && all;
		++n;
	}
	r.open = false;   // nothing was added; the next OpenInstance begins the pass again
	return all;
}

void RT_Draw(const SSnapshot& snap, const SDisplayContextKey& key)
{
	// Lazy creation (M11 section 5): the first non-empty snapshot to execute creates the resources.
	if (!s_pRes && snap.count > 0 && s_deviceAvailable.load(std::memory_order_acquire))
		RT_EnsureResources();
	SResources* const pRes = s_pRes;
	if (!pRes || !ResourcesUsable(*pRes))
	{
		s_drawsSkipped.fetch_add(snap.count, std::memory_order_relaxed);
		return;
	}
	PrewarmPoll(*pRes);
	if (RT_DrawLayers(*pRes, snap, key, false) > 0)
		s_framesDrawn.fetch_add(1, std::memory_order_relaxed);
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
	s.loopFramesDrawn = s_loopFramesDrawn.load(std::memory_order_relaxed);
	const uint32 bytes = std::min<uint32>(pStats->size, uint32(sizeof(s)));
	s.size = bytes;
	memcpy(pStats, &s, bytes);
}

//////////////////////////////////////////////////////////////////////////
// Loading-loop driver (M11 section 7). Derived from CE_XTUI patch 17 (CE-90) of XTFrame for CRYENGINE:
// the registry of RenderThread.cpp (LoadtimeCallbacks) reduced to one registrant, and LoadtimeAutoStart /
// LoadtimeAutoRelease of D3DRenderThread.cpp. The dispatch holds s_loadtimeLock while it calls the update and
// SetLoadtimeUpdate takes it too, so unregistering returns only when the update is not running.

#ifndef XUI2DL_LOADTIME_HOOKS
	#define XUI2DL_LOADTIME_HOOKS 0   // apply_xui2dl.py defines 1 when it inserted the loading-loop hooks
#endif

typedef void (* TLoadtimeUpdate)(void* pUser, float dtSeconds);

CryCriticalSection    s_loadtimeLock;
TLoadtimeUpdate       s_pfnLoadtimeUpdate = nullptr;   //!< guarded by s_loadtimeLock
void*                 s_pLoadtimeUser = nullptr;
std::atomic<bool>     s_loadtimeRegistered{ false };
std::atomic<threadID> s_loadtimeDispatchThread{ 0 };
bool                  s_loadtimeAutoOwned = false;      //!< main thread only

bool OnLoadtimeDispatchThread()
{
	const threadID t = s_loadtimeDispatchThread.load(std::memory_order_acquire);
	return t != 0 && t == CryGetCurrentThreadId();
}

//! The stand-in StopLoadtimeFlashPlayback finds while it stops a loop that has no flash callback (LoadtimeBeforeStop).
struct SStopMarker final : public ILoadtimeCallback
{
	void LoadtimeUpdate(float) override {}
	bool LoadtimeRender() override { return true; }
};
SStopMarker s_stopMarker;

int SetLoadtimeUpdate(TLoadtimeUpdate pfnUpdate, void* pUser)
{
	if (!XUI2DL_LOADTIME_HOOKS || !gRenDev || !gRenDev->m_pRT)
		return 0;
	if (OnLoadtimeDispatchThread())
	{
		CryLogAlways("[XUI2DL] SetLoadtimeUpdate called from inside the loading-loop update; refused");
		return 0;
	}
	if (!gRenDev->m_pRT->IsMainThread())
		return 0;
	CryAutoCriticalSection lock(s_loadtimeLock);
	s_pfnLoadtimeUpdate = pfnUpdate;
	s_pLoadtimeUser = pfnUpdate ? pUser : nullptr;
	s_loadtimeRegistered.store(pfnUpdate != nullptr, std::memory_order_release);
	return 1;
}

const XUI2DLApi s_api = { uint32(sizeof(XUI2DLApi)), XUI2DL_API_VERSION,
	                      XUI2DL_CAP_CORE | XUI2DL_CAP_YUV | (XUI2DL_LOADTIME_HOOKS ? uint32(XUI2DL_CAP_LOADTIME) : 0u), 0,
	                      &SetFrame, &GetStats, &SetLoadtimeUpdate };

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
	// A constant buffer has no device buffer until its first upload, and every draw binds b1: upload zeros now.
	bool bSlotCBs = true;
	for (uint32 i = 0; i < XUI2DL_MAX_LAYERS; ++i)
	{
		SSlot& slot = r.pSlots[i];
		slot.pPlanarCB = gRenDev->m_DevBufMan.CreateConstantBuffer(sizeof(SPlanarConstants));
		memset(&slot.lastPlanar, 0, sizeof(slot.lastPlanar));
		slot.bHasPlanar = slot.pPlanarCB && slot.pPlanarCB->UpdateBuffer(&slot.lastPlanar, sizeof(slot.lastPlanar));
		bSlotCBs = bSlotCBs && slot.bHasPlanar;
	}
	r.bSlotCBs = bSlotCBs;
	s_pRes = pRes;

	if (bShader)
	{
		PrewarmPoll(r);
		for (int t = 0; t < eTech_Count; ++t)
			ready += (r.techniqueFound[t] && !r.techniquePending[t]) ? 1u : 0u;
	}
	const bool bBuffers = r.vb != ~0u && r.ib != ~0u && r.pPassCB && r.bSlotCBs;
	s_resourcesReady.store(bBuffers, std::memory_order_release);
	CryLogAlways("[XUI2DL] render resources created (shader %s, %u/%d technique(s), %u ready from the shader cache, buffers %s)",
	             bShader ? "ok" : "MISSING", found, int(eTech_Count), ready, bBuffers ? "ok" : "FAILED");
}

//! Render thread, before the loading loop starts (LoadtimePrewarm): creates the resources when missing, polls the
//! shader prewarm and, once the shader instances are ready, compiles the primitives of the current snapshot
//! (RT_CompileSnapshot). A pipeline state created while its shaders were still compiling stays invalid in the device
//! object factory until CDeviceObjectFactory::UpdatePipelineStates re-initialises it; the engine calls that only in
//! RT_BeginFrame, which does not run inside the loading loop, so a failed compile calls it here and compiles again.
//! Sets s_loopReady. The loop itself never creates resources or requests shader instances.
void RT_PrepareForLoop()
{
	s_loopReady.store(false, std::memory_order_release);
	if (!s_deviceAvailable.load(std::memory_order_acquire))
		return;
	if (!s_pRes)
		RT_EnsureResources();   // ends with a PrewarmPoll
	else if (ShaderValid(*s_pRes))
		PrewarmPoll(*s_pRes);
	SResources* const pRes = s_pRes;
	if (!pRes || !ResourcesUsable(*pRes) || !s_resourcesReady.load(std::memory_order_acquire) || !pRes->prewarmDone)
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
	bool ok = RT_CompileSnapshot(*pRes, *pSnap);
	if (!ok)
	{
		GetDeviceObjectFactory().UpdatePipelineStates();
		ok = RT_CompileSnapshot(*pRes, *pSnap);
		CryLogAlways("[XUI2DL] loading-loop prewarm: primitives %s after re-initialising the pending pipeline states", ok ? "compiled" : "still not compiled");
	}
	RT_Free(pSnap);   // the render thread, not the loading thread: the loop has not started
	s_loopReady.store(ok, std::memory_order_release);
}

bool ReadyForLoop()
{
	return s_resourcesReady.load(std::memory_order_acquire) && s_shaderReady.load(std::memory_order_acquire);
}

//! Main thread, LoadtimeAutoStart before it enters the loop: with a non-empty retained list, have the resources
//! created, the shader prewarmed and every layer's primitive compiled on the render thread (RT_PrepareForLoop), polling
//! every 10 ms for at most 2 s; on timeout this loading loop does not draw (s_loopNoDraw).
void LoadtimePrewarm()
{
	s_loopNoDraw.store(false, std::memory_order_release);
	s_loopFailLogged.store(false, std::memory_order_release);
	bool bHaveLayers = false;
	{
		CryAutoCriticalSection lock(s_currentLock);
		bHaveLayers = s_pCurrent != nullptr;
	}
	if (!bHaveLayers || !s_deviceAvailable.load(std::memory_order_acquire))
		return;
	const CTimeValue start = gEnv->pTimer->GetAsyncTime();
	for (;;)
	{
		gRenDev->ExecuteRenderThreadCommand([]() { RT_PrepareForLoop(); }, ERenderCommandFlags::None);
		gcpRendD3D->FlushRTCommands(true, true, true);
		const float waited = (gEnv->pTimer->GetAsyncTime() - start).GetSeconds();
		if (s_loopReady.load(std::memory_order_acquire))
		{
			CryLogAlways("[XUI2DL] render resources ready for the loading loop after %.3f s", waited);
			return;
		}
		if (waited >= 2.0f)
		{
			CryLogAlways("[XUI2DL] render resources or shader not ready 2 s before the loading loop; the loop will not draw the retained layers");
			s_loopNoDraw.store(true, std::memory_order_release);
			return;
		}
		CrySleep(10);
	}
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
	// Only the render resources: the retained snapshot is the caller's state and survives a level unload
	// (FRR_SYSTEM_RESOURCES); RT_ReleaseRetained drops it when the renderer shuts down.
	s_deviceAvailable.store(false, std::memory_order_release);
	s_loopReady.store(false, std::memory_order_release);

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

void RT_ReleaseRetained()
{
	SSnapshot* pDropped = nullptr;
	{
		CryAutoCriticalSection lock(s_currentLock);
		pDropped = s_pCurrent;
		s_pCurrent = nullptr;
		s_layersHeld.store(0, std::memory_order_relaxed);
	}
	if (!pDropped)
		return;
	SnapshotRelease(pDropped);   // render thread, before CTexture::ShutDown: the textures go at once
	CryLogAlways("[XUI2DL] retained layers dropped (renderer shutdown)");
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

//////////////////////////////////////////////////////////////////////////
// Loading-loop entry points (patch 17: LoadtimeCallbacks::DispatchUpdate, CD3D9Renderer::LoadtimeAutoStart /
// LoadtimeAutoRelease and its changes to StartLoadtimeFlashPlayback / StopLoadtimeFlashPlayback)

void RT_LoadtimeUpdate(float dtSeconds)
{
	if (!s_loadtimeRegistered.load(std::memory_order_acquire))
		return;   // nothing registered: no lock taken (a registration racing this iteration is seen by the next one)
	CryAutoCriticalSection lock(s_loadtimeLock);
	if (!s_pfnLoadtimeUpdate)
		return;
	s_loadtimeDispatchThread.store(CryGetCurrentThreadId(), std::memory_order_release);
	s_pfnLoadtimeUpdate(s_pLoadtimeUser, dtSeconds);
	s_loadtimeDispatchThread.store(0, std::memory_order_release);
}

void RT_LoadtimeRender()
{
	// The prewarm timed out: this loop does not draw (and counts nothing).
	if (s_loopNoDraw.load(std::memory_order_acquire))
		return;
	// Nothing to draw, or not prepared: never create resources or compile shaders inside the loop (L0 survey C.2).
	if (!s_deviceAvailable.load(std::memory_order_acquire) || !ReadyForLoop())
		return;
	SResources* const pRes = s_pRes;
	if (!pRes || !ResourcesUsable(*pRes))
		return;
	SSnapshot* pSnap = nullptr;
	{
		CryAutoCriticalSection lock(s_currentLock);   // the lock SetFrame swaps the pointer under
		pSnap = s_pCurrent;
		if (pSnap)
			SnapshotAddRef(pSnap);
	}
	if (!pSnap)
		return;
	// The loop does not clear the back buffer: clear it unless a StartLoadtimeFlashPlayback callback drew this
	// iteration (no callback, or only LoadtimeBeforeStop's stand-in); over a flash callback the layers go on top.
	const ILoadtimeCallback* const pCallback = gRenDev->m_pRT->m_pLoadtimeCallback;
	const bool bClear = !pCallback || pCallback == &s_stopMarker;
	// The display context's own key is not known here: the default key resolves to the active display context.
	const uint64 skippedBefore = s_drawsSkipped.load(std::memory_order_relaxed);
	if (RT_DrawLayers(*pRes, *pSnap, SDisplayContextKey{}, bClear) > 0)
		s_loopFramesDrawn.fetch_add(1, std::memory_order_relaxed);
	const uint64 skipped = s_drawsSkipped.load(std::memory_order_relaxed) - skippedBefore;
	if (skipped > 0 && !s_loopFailLogged.exchange(true, std::memory_order_acq_rel))
		CryLogAlways("[XUI2DL] a loading-loop draw failed after the prewarm (%llu layer draw(s) skipped); further failures in this loop are only counted in drawsSkipped",
		             (unsigned long long)skipped);
	RT_Free(pSnap);   // never drops the last reference on the render loading thread
}

void LoadtimeAutoStart()
{
	// The conditions of CFlashUI::StartRenderThread (minus gfx_loadtimethread, which is FlashUI's own
	// switch): never in the editor, only with a render thread, not for asynchronous level loads.
	s_loopNoDraw.store(false, std::memory_order_release);   // a flag left by a loop someone else stopped
	if (gEnv->IsEditor() || !s_loadtimeRegistered.load(std::memory_order_acquire) || s_loadtimeAutoOwned)
		return;
	SRenderThread* const pRT = gRenDev->m_pRT;
	if (pRT->m_eVideoThreadMode != SRenderThread::eVTM_Disabled)
		return;   // someone (CFlashUI) already runs the loop: the registered update is called in it
	ICVar* const pMT = gEnv->pConsole->GetCVar("r_MultiThreaded");
	ICVar* const pAsyncLoad = gEnv->pConsole->GetCVar("g_asynclevelload");
	if (!pMT || pMT->GetIVal() <= 0 || !pRT->IsMultithreaded() || (pAsyncLoad && pAsyncLoad->GetIVal() > 0))
		return;

	LoadtimePrewarm();   // before LockReadIO: creating the resources reads the shader
	s_loadtimeAutoOwned = true;
	gEnv->pCryPak->LockReadIO(true);
	gcpRendD3D->FlushRTCommands(true, true, true);
	pRT->RC_StartVideoThread();
	while (pRT->m_eVideoThreadMode != SRenderThread::eVTM_Active)
	{
		pRT->FlushAndWait();
		CrySleep(0);
	}
	CryLogAlways("[XUI2DL] loading loop started for the registered loading-loop update");
}

void LoadtimeAutoRelease()
{
	if (!s_loadtimeAutoOwned)
		return;
	s_loadtimeAutoOwned = false;
	// A StartLoadtimeFlashPlayback caller that joined the loop stops it itself (and unlocks the IO it
	// locked); otherwise the loop is ours to stop (unless something stopped it already).
	if (gRenDev->m_pRT->m_pLoadtimeCallback)
		return;
	gcpRendD3D->StopLoadtimeFlashPlayback();
	gEnv->pCryPak->LockReadIO(false);
	s_loopNoDraw.store(false, std::memory_order_release);   // the loop has stopped
	CryLogAlways("[XUI2DL] loading loop for the registered loading-loop update released");
}

bool LoadtimeJoin(ILoadtimeCallback* pCallback)
{
	// The renderer already runs the loop for the registered update (it started it at
	// LEVEL_LOAD_START_LOADINGSCREEN before this caller's listener ran): join it. Only a null -> callback
	// change happens while the loop runs, so the loop sees either no flash callback or this one.
	SRenderThread* const pRT = gRenDev->m_pRT;
	if (pRT->m_eVideoThreadMode == SRenderThread::eVTM_Disabled)
		return false;
	pRT->m_pLoadtimeCallback = pCallback;
	CryLogAlways("[XUI2DL] a StartLoadtimeFlashPlayback callback joined the running loading loop");
	return true;
}

void LoadtimeBeforeStop()
{
	// Patch 17 stops the loop also when only registered callbacks are in it: callers such as
	// CMatMan::ForceDelayedMaterialDeletion and CSystem's quit path rely on this call leaving the render thread
	// out of the loading loop. The stock stop path runs only with a flash callback, so a no-op stand-in is
	// installed for it; that path clears it once the loop has stopped. With no loop running nothing changes.
	SRenderThread* const pRT = gRenDev->m_pRT;
	if (!pRT->m_pLoadtimeCallback && pRT->m_eVideoThreadMode != SRenderThread::eVTM_Disabled)
		pRT->m_pLoadtimeCallback = &s_stopMarker;
}

} // namespace XUI2DL

extern "C" __declspec(dllexport) const XUI2DLApi* XUI2DL_GetApi(uint32_t version)
{
	return version == XUI2DL_API_VERSION ? &XUI2DL::s_api : nullptr;
}
