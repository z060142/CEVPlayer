#include "StdAfx.h"
#include "PluginEnv.h"
#include "FullscreenOutput.h"

#include <cstring>

#include <CryRenderer/IRenderer.h>

#include "Render/XUI2DLClient.h"

namespace CEVPlayer
{
	namespace FullscreenOutput
	{
		namespace
		{
			bool s_backendKnown = false;
			EBackend s_backend = EBackend::None;
			const XUI2DLApi* s_pApi = nullptr;

			XUI2DLLayer s_sent[XUI2DL_MAX_LAYERS];   // the list sent last (s_sentCount layers)
			uint32_t s_sentCount = 0;
			bool s_forceSend = false;                // the next Present sends even an identical list
			bool s_probeOwnsFrame = false;
			bool s_overflowWarned = false;
			uint64_t s_setFrameCalls = 0;

			XUI2DLLayer s_build[XUI2DL_MAX_LAYERS];
			uint32_t s_buildCount = 0;

			XUI2DLLayer Blank(uint32_t kind, uint32_t blend, float x, float y, float w, float h)
			{
				XUI2DLLayer l;
				std::memset(&l, 0, sizeof(l)); // byte comparison of whole lists: no stale bytes anywhere
				l.size = sizeof(l);
				l.kind = kind;
				l.blend = blend;
				l.dst = XUI2DLRect{ x, y, w, h };
				l.color[0] = l.color[1] = l.color[2] = l.color[3] = 1.f;
				return l;
			}

			XUI2DLLayer Solid(float x, float y, float w, float h, float r, float g, float b, float a)
			{
				XUI2DLLayer l = Blank(XUI2DL_KIND_SOLID, XUI2DL_BLEND_REPLACE, x, y, w, h);
				l.color[0] = r;
				l.color[1] = g;
				l.color[2] = b;
				l.color[3] = a;
				return l;
			}

			XUI2DLPlane Plane(const uint32 (&p)[4]) { return XUI2DLPlane{ p[0], p[1], p[2], p[3] }; }

			//! The layers of one player (letterbox bars, then the picture) into pOut; returns how many (0..5).
			uint32_t PlayerLayers(const SLayer& layer, float screenW, float screenH, XUI2DLLayer* pOut)
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
						const XUI2DLRect bars[4] = {
							{ 0.f, 0.f, screenW, y },
							{ 0.f, y + h, screenW, screenH - y - h },
							{ 0.f, y, x, h },
							{ x + w, y, screenW - x - w, h },
						};
						for (const XUI2DLRect& r : bars)
						{
							if (r.w > 0.f && r.h > 0.f)
								pOut[n++] = Solid(r.x, r.y, r.w, r.h, 0.f, 0.f, 0.f, 1.f);
						}
					}
				}

				uint32_t kind = XUI2DL_KIND_RGBA_PREMULT;
				if (layer.layout == EVideoPixelLayout::PackedYuv420)
					kind = XUI2DL_KIND_YUV420;
				else if (layer.layout == EVideoPixelLayout::PackedYuva420)
					kind = XUI2DL_KIND_YUVA420;
				XUI2DLLayer& v = pOut[n++];
				v = Blank(kind, XUI2DL_BLEND_SRCOVER, x, y, w, h);
				v.textureId = layer.textureId;
				if (kind != XUI2DL_KIND_RGBA_PREMULT)
				{
					v.imageWidth = static_cast<uint32_t>(layer.imageWidth);
					v.imageHeight = static_cast<uint32_t>(layer.imageHeight);
					v.planeY = Plane(layer.planeY);
					v.planeU = Plane(layer.planeU);
					v.planeV = Plane(layer.planeV);
					v.planeA = Plane(layer.planeA);
					v.colorMatrix = layer.colorMatrix;
					v.colorRange = layer.colorRange;
					v.chromaSiting = layer.chromaSiting;
				}
				return n;
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
		}

		EBackend Backend()
		{
			if (s_backendKnown)
				return s_backend;
			if (gEnv == nullptr || gEnv->pRenderer == nullptr || gEnv->IsDedicated())
				return EBackend::None; // no renderer yet: decided once there is one
			s_backendKnown = true;
			s_pApi = XUI2DLClient::Get();
			s_backend = s_pApi != nullptr ? EBackend::XUI2DL : EBackend::None;
			if (s_backend == EBackend::None)
				LogError("fullscreen output is unavailable: the renderer has no XUI2DL (apply EnginePatch/apply_xui2dl.py and rebuild the renderer)");
			return s_backend;
		}

		bool SupportsPlanar()
		{
			return Backend() == EBackend::XUI2DL && (s_pApi->caps & XUI2DL_CAP_YUV) != 0;
		}

		void Present(const SLayer* pLayers, size_t count, const SDebugLayer* pDebug, size_t debugCount)
		{
			if (Backend() != EBackend::XUI2DL || s_probeOwnsFrame)
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

			// Players first (bottom), then the debug layers; past XUI2DL_MAX_LAYERS the debug layers go first, then the
			// topmost players.
			XUI2DLLayer players[XUI2DL_MAX_LAYERS];
			uint32_t playerCount = 0;
			bool dropped = false;
			s_buildCount = 0;
			for (size_t i = 0; pLayers != nullptr && i < count; ++i)
			{
				XUI2DLLayer one[5];
				const uint32_t n = PlayerLayers(pLayers[i], screenW, screenH, one);
				if (n == 0)
					continue;
				if (playerCount + n > XUI2DL_MAX_LAYERS)
				{
					dropped = true;
					break; // this player and every one above it
				}
				std::memcpy(players + playerCount, one, sizeof(XUI2DLLayer) * n);
				playerCount += n;
			}
			std::memcpy(s_build, players, sizeof(XUI2DLLayer) * playerCount);
			s_buildCount = playerCount;
			for (size_t i = 0; pDebug != nullptr && i < debugCount; ++i)
			{
				const SDebugLayer& d = pDebug[i];
				if (d.w <= 0.f || d.h <= 0.f || (!d.solid && d.textureId <= 0))
					continue;
				if (s_buildCount >= XUI2DL_MAX_LAYERS)
				{
					dropped = true;
					break;
				}
				XUI2DLLayer l = d.solid ? Blank(XUI2DL_KIND_SOLID, XUI2DL_BLEND_REPLACE, d.x, d.y, d.w, d.h)
				                        : Blank(XUI2DL_KIND_RGBA_PREMULT, XUI2DL_BLEND_SRCOVER, d.x, d.y, d.w, d.h);
				if (!d.solid)
					l.textureId = d.textureId;
				for (int c = 0; c < 4; ++c)
					l.color[c] = d.color[c];
				s_build[s_buildCount++] = l;
			}
			// Debug layers fill only what the players left: when players were dropped, no debug layer can be in.
			if (dropped && !s_overflowWarned)
			{
				s_overflowWarned = true;
				LogWarning("fullscreen output: more than %d layers; debug layers and then the topmost players were dropped", static_cast<int>(XUI2DL_MAX_LAYERS));
			}

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

		void Shutdown()
		{
			if (!s_backendKnown || s_backend != EBackend::XUI2DL)
				return;
			Send(nullptr, 0);
		}

		void SetProbeOwnsFrame(bool owns)
		{
			if (s_probeOwnsFrame && !owns)
				s_forceSend = true;
			s_probeOwnsFrame = owns;
		}
	}
}
