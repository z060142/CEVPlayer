#include "StdAfx.h"
#include "PluginEnv.h"
#include "FullscreenOutput.h"

#include <cstdint>
#include <cstring>

#include <CryRenderer/IRenderer.h>

#include "Render/RenderBridge.h"
#include "Render/XUI2DLClient.h"
#if CEV_HAVE_XUI2D
#include "Render/FullscreenXUI2D.h"
#endif

namespace CEVPlayer
{
	namespace FullscreenOutput
	{
		namespace
		{
			bool s_backendKnown = false;
			EBackend s_backend = EBackend::None;
			const XUI2DLApi* s_pApi = nullptr;
			bool s_planarXui2d = false;              // XUI2D: XUI2D_Patched has the planar bit

			XUI2DLLayer s_sent[XUI2DL_MAX_LAYERS];   // the list sent last (s_sentCount layers)
			uint32_t s_sentCount = 0;
			bool s_forceSend = false;                // the next Present sends even an identical list
			bool s_probeOwnsFrame = false;
			bool s_overflowWarned = false;
			uint64_t s_setFrameCalls = 0;

			XUI2DLLayer s_build[XUI2DL_MAX_LAYERS];
			uint32_t s_buildCount = 0;
			std::vector<SQuad> s_quads;              // scratch of Present (kept across frames)

			void Copy4(uint32 (&dst)[4], const uint32 (&src)[4]) { std::memcpy(dst, src, sizeof(dst)); }

			SQuad Quad(EQuadKind kind, bool replace, float x, float y, float w, float h)
			{
				SQuad q;
				q.kind = kind;
				q.replace = replace;
				q.x = x;
				q.y = y;
				q.w = w;
				q.h = h;
				return q;
			}

			//! The quads of one player (letterbox bars, then the picture) into pOut; returns how many (0..5).
			uint32_t PlayerQuads(const SLayer& layer, float screenW, float screenH, SQuad* pOut)
			{
				if (layer.textureId <= 0 || layer.imageWidth <= 0 || layer.imageHeight <= 0)
					return 0;
				uint32_t n = 0;
				float x = 0.f, y = 0.f, w = screenW, h = screenH;
				if (layer.fit != EVideoFit::Fill)
				{
					const float videoAspect = static_cast<float>(layer.imageWidth) / static_cast<float>(layer.imageHeight);
					const float screenAspect = screenW / screenH;
					if (videoAspect > screenAspect)
					{
						w = screenW;
						h = screenW / videoAspect;
					}
					else
					{
						h = screenH;
						w = screenH * videoAspect;
					}
					x = 0.5f * (screenW - w);
					y = 0.5f * (screenH - h);

					if (layer.fit == EVideoFit::Letterbox)
					{
						const float bars[4][4] = {
							{ 0.f, 0.f, screenW, y },
							{ 0.f, y + h, screenW, screenH - y - h },
							{ 0.f, y, x, h },
							{ x + w, y, screenW - x - w, h },
						};
						for (const auto& r : bars)
						{
							if (r[2] > 0.f && r[3] > 0.f)
							{
								SQuad bar = Quad(EQuadKind::Solid, true, r[0], r[1], r[2], r[3]);
								bar.color[0] = bar.color[1] = bar.color[2] = 0.f;
								bar.color[3] = 1.f;
								pOut[n++] = bar;
							}
						}
					}
				}

				EQuadKind kind = EQuadKind::RgbaPremult;
				if (layer.layout == EVideoPixelLayout::PackedYuv420)
					kind = EQuadKind::Yuv420;
				else if (layer.layout == EVideoPixelLayout::PackedYuva420)
					kind = EQuadKind::Yuva420;
				SQuad& v = pOut[n++];
				v = Quad(kind, false, x, y, w, h);
				v.textureId = layer.textureId;
				if (kind != EQuadKind::RgbaPremult)
				{
					v.imageWidth = static_cast<uint32>(layer.imageWidth);
					v.imageHeight = static_cast<uint32>(layer.imageHeight);
					Copy4(v.planeY, layer.planeY);
					Copy4(v.planeU, layer.planeU);
					Copy4(v.planeV, layer.planeV);
					Copy4(v.planeA, layer.planeA);
					v.colorMatrix = layer.colorMatrix;
					v.colorRange = layer.colorRange;
					v.chromaSiting = layer.chromaSiting;
				}
				return n;
			}

			XUI2DLLayer ToXui2dl(const SQuad& q)
			{
				XUI2DLLayer l;
				std::memset(&l, 0, sizeof(l)); // byte comparison of whole lists: no stale bytes anywhere
				l.size = sizeof(l);
				switch (q.kind)
				{
				case EQuadKind::Solid:   l.kind = XUI2DL_KIND_SOLID; break;
				case EQuadKind::Yuv420:  l.kind = XUI2DL_KIND_YUV420; break;
				case EQuadKind::Yuva420: l.kind = XUI2DL_KIND_YUVA420; break;
				default:                 l.kind = XUI2DL_KIND_RGBA_PREMULT; break;
				}
				l.blend = q.replace ? XUI2DL_BLEND_REPLACE : XUI2DL_BLEND_SRCOVER;
				l.dst = XUI2DLRect{ q.x, q.y, q.w, q.h };
				for (int c = 0; c < 4; ++c)
					l.color[c] = q.color[c];
				if (q.kind != EQuadKind::Solid)
					l.textureId = q.textureId;
				if (q.kind == EQuadKind::Yuv420 || q.kind == EQuadKind::Yuva420)
				{
					l.imageWidth = q.imageWidth;
					l.imageHeight = q.imageHeight;
					l.planeY = XUI2DLPlane{ q.planeY[0], q.planeY[1], q.planeY[2], q.planeY[3] };
					l.planeU = XUI2DLPlane{ q.planeU[0], q.planeU[1], q.planeU[2], q.planeU[3] };
					l.planeV = XUI2DLPlane{ q.planeV[0], q.planeV[1], q.planeV[2], q.planeV[3] };
					l.planeA = XUI2DLPlane{ q.planeA[0], q.planeA[1], q.planeA[2], q.planeA[3] };
					l.colorMatrix = q.colorMatrix;
					l.colorRange = q.colorRange;
					l.chromaSiting = q.chromaSiting;
				}
				return l;
			}

			void Send(const XUI2DLLayer* pLayers, uint32_t count)
			{
				XUI2DLFrame f;
				std::memset(&f, 0, sizeof(f));
				f.size = sizeof(f);
				f.layerCount = count;
				f.pLayers = count > 0 ? pLayers : nullptr;
				f.viewport = XUI2DLRect{ 0.f, 0.f, 0.f, 0.f }; // the whole output
				const int accepted = s_pApi->SetFrame(&f);
				++s_setFrameCalls;
				if (mEnv != nullptr && mEnv->m_cevLog != 0)
					LogAlways("fullscreen output: SetFrame #%llu, %u layer(s) -> %d", static_cast<unsigned long long>(s_setFrameCalls), count, accepted);
				if (accepted == 0)
					LogWarning("fullscreen output: XUI2DL refused a list of %u layer(s)", count);
				if (count > 0)
					std::memcpy(s_sent, pLayers, sizeof(XUI2DLLayer) * count);
				s_sentCount = count;
				s_forceSend = false;
			}

			void PresentXui2dl(float screenW, float screenH, const SLayer* pLayers, size_t count, const SDebugLayer* pDebug, size_t debugCount)
			{
				bool dropped = false;
				BuildQuads(pLayers, count, pDebug, debugCount, screenW, screenH, XUI2DL_MAX_LAYERS, s_quads, dropped);
				if (dropped && !s_overflowWarned)
				{
					s_overflowWarned = true;
					LogWarning("fullscreen output: more than %d layers; debug layers and then the topmost players were dropped", static_cast<int>(XUI2DL_MAX_LAYERS));
				}
				s_buildCount = 0;
				for (const SQuad& q : s_quads)
					s_build[s_buildCount++] = ToXui2dl(q);

				if (s_buildCount == 0)
				{
					if (s_sentCount != 0 || s_forceSend)
						Send(nullptr, 0);
					return;
				}
				if (!s_forceSend && s_buildCount == s_sentCount && std::memcmp(s_build, s_sent, sizeof(XUI2DLLayer) * s_buildCount) == 0)
					return;
				Send(s_build, s_buildCount);
			}
		}

		void BuildQuads(const SLayer* pLayers, size_t count, const SDebugLayer* pDebug, size_t debugCount, float screenW, float screenH,
		                size_t maxQuads, std::vector<SQuad>& out, bool& dropped)
		{
			// Players first (bottom), then the debug layers; past maxQuads the debug layers go first, then the topmost
			// players.
			out.clear();
			dropped = false;
			for (size_t i = 0; pLayers != nullptr && i < count; ++i)
			{
				SQuad one[5];
				const uint32_t n = PlayerQuads(pLayers[i], screenW, screenH, one);
				if (n == 0)
					continue;
				if (out.size() + n > maxQuads)
				{
					dropped = true;
					break; // this player and every one above it
				}
				out.insert(out.end(), one, one + n);
			}
			for (size_t i = 0; pDebug != nullptr && i < debugCount; ++i)
			{
				const SDebugLayer& d = pDebug[i];
				if (d.w <= 0.f || d.h <= 0.f || (!d.solid && d.textureId <= 0))
					continue;
				if (out.size() >= maxQuads)
				{
					dropped = true;
					return;
				}
				SQuad q = d.solid ? Quad(EQuadKind::Solid, true, d.x, d.y, d.w, d.h) : Quad(EQuadKind::RgbaPremult, false, d.x, d.y, d.w, d.h);
				if (!d.solid)
					q.textureId = d.textureId;
				for (int c = 0; c < 4; ++c)
					q.color[c] = d.color[c];
				out.push_back(q);
			}
		}

		EBackend Backend()
		{
			if (s_backendKnown)
				return s_backend;
			if (gEnv == nullptr || gEnv->pRenderer == nullptr || gEnv->IsDedicated())
				return EBackend::None; // no renderer yet: decided once there is one
			s_backendKnown = true;
#if CEV_HAVE_XUI2D
			// XTFrame's XUI2D first: on such an engine XUI2DL is never touched.
			if (FullscreenXUI2D::Available())
			{
				s_backend = EBackend::XUI2D;
				s_planarXui2d = (FullscreenXUI2D::PatchedBits() & 256) != 0;
				LogAlways("fullscreen output: backend xui2d (XUI2D_Patched 0x%x, planar %d, load clear once (512) %d)", static_cast<unsigned>(FullscreenXUI2D::PatchedBits()),
					s_planarXui2d ? 1 : 0, (FullscreenXUI2D::PatchedBits() & 512) != 0 ? 1 : 0);
				return s_backend;
			}
#endif
			s_pApi = XUI2DLClient::Get();
			s_backend = s_pApi != nullptr ? EBackend::XUI2DL : EBackend::None;
			if (s_backend == EBackend::XUI2DL)
				LogAlways("fullscreen output: backend xui2dl (caps 0x%x)", s_pApi->caps);
			else
				LogError("fullscreen output is unavailable: the renderer has no XUI2DL (apply EnginePatch/apply_xui2dl.py and rebuild the renderer)");
			return s_backend;
		}

		const char* BackendName()
		{
			switch (Backend())
			{
			case EBackend::XUI2D:  return "xui2d";
			case EBackend::XUI2DL: return "xui2dl";
			default:               return "none";
			}
		}

		bool SupportsPlanar()
		{
			switch (Backend())
			{
			case EBackend::XUI2D:  return s_planarXui2d;
			case EBackend::XUI2DL: return (s_pApi->caps & XUI2DL_CAP_YUV) != 0;
			default:               return false;
			}
		}

		bool LoopDrawAvailable()
		{
#if CEV_HAVE_XUI2D && CEV_HAVE_LOADTIME_CALLBACKS
			// XUI2D_Patched bit 9 (512): within one loading-loop iteration only the first screen-surface entry to execute
			// has its ClearOutput applied (XTFrame CE-117, patch 19; kXui2DPatched_LoadClearOnce in the renderer's
			// RenderDll/XUI2D/XUI2DRenderer.h, IXUIRenderer.h IXUILoadFrame::ClearOutput). Without it the UI's clear wipes
			// a video drawn before it.
			return Backend() == EBackend::XUI2D && (FullscreenXUI2D::PatchedBits() & 512) != 0 && mEnv->m_cevDebugNoLoopDraw == 0
				&& RenderBridge::LoadtimeSource() == RenderBridge::ELoadtimeSource::Patch17;
#else
			return false;
#endif
		}

		ILoadtimeCallback* LoopDrawCallback()
		{
#if CEV_HAVE_XUI2D
			return FullscreenXUI2D::LoopCallback();
#else
			return nullptr;
#endif
		}

		bool PresentsAt(EStep step)
		{
			switch (Backend())
			{
			case EBackend::XUI2D:  return step == EStep::BeforeRender;
			case EBackend::XUI2DL: return step == EStep::AfterRender;
			default:               return false;
			}
		}

		void Present(EStep step, const SLayer* pLayers, size_t count, const SDebugLayer* pDebug, size_t debugCount)
		{
			if (!PresentsAt(step))
				return;
			if (s_backend == EBackend::XUI2DL && s_probeOwnsFrame)
				return;

			// Overlay size is what UI drawing uses; fall back to the main resolution.
			float screenW = static_cast<float>(gEnv->pRenderer->GetOverlayWidth());
			float screenH = static_cast<float>(gEnv->pRenderer->GetOverlayHeight());
			if (screenW <= 0.f || screenH <= 0.f)
			{
				screenW = static_cast<float>(gEnv->pRenderer->GetWidth());
				screenH = static_cast<float>(gEnv->pRenderer->GetHeight());
			}
			if (screenW <= 0.f || screenH <= 0.f)
				return;

			if (s_backend == EBackend::XUI2DL)
			{
				PresentXui2dl(screenW, screenH, pLayers, count, pDebug, debugCount);
				return;
			}
#if CEV_HAVE_XUI2D
			bool dropped = false;
			BuildQuads(pLayers, count, pDebug, debugCount, screenW, screenH, SIZE_MAX, s_quads, dropped);
			FullscreenXUI2D::Present(s_quads, static_cast<int>(screenW), static_cast<int>(screenH));
#endif
		}

		void Shutdown()
		{
			if (!s_backendKnown)
				return;
			if (s_backend == EBackend::XUI2DL)
				Send(nullptr, 0);
#if CEV_HAVE_XUI2D
			else if (s_backend == EBackend::XUI2D)
				FullscreenXUI2D::Shutdown();
#endif
		}

		void SetProbeOwnsFrame(bool owns)
		{
			if (s_probeOwnsFrame && !owns)
				s_forceSend = true;
			s_probeOwnsFrame = owns;
		}
	}
}
