#include "StdAfx.h"
#include "PluginEnv.h"
#include "FullscreenOutput.h"

#include <CryRenderer/IRenderer.h>
#include <CryRenderer/IRenderAuxGeom.h>

namespace CEVPlayer
{
	namespace FullscreenOutput
	{
		namespace
		{
			struct SRect
			{
				float x = 0.f;
				float y = 0.f;
				float w = 0.f;
				float h = 0.f;
			};

			void FillRect(const SRect& r, float red, float green, float blue)
			{
				if (r.w > 0.f && r.h > 0.f)
					IRenderAuxImage::Draw2dImage(r.x, r.y, r.w, r.h, -1, 0.f, 0.f, 1.f, 1.f, 0.f, red, green, blue, 1.f);
			}
		}

		void Draw(const SLayer* pLayers, size_t count)
		{
			if (gEnv == nullptr || gEnv->pRenderer == nullptr || pLayers == nullptr)
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

			for (size_t i = 0; i < count; ++i)
			{
				const SLayer& layer = pLayers[i];
				if (layer.textureId <= 0 || layer.textureWidth <= 0 || layer.textureHeight <= 0)
					continue;

				SRect dst;
				dst.w = screenW;
				dst.h = screenH;
				if (layer.fit != EVideoFit::Fill)
				{
					const float videoAspect = static_cast<float>(layer.textureWidth) / static_cast<float>(layer.textureHeight);
					const float screenAspect = screenW / screenH;
					if (videoAspect > screenAspect)
					{
						dst.w = screenW;
						dst.h = screenW / videoAspect;
					}
					else
					{
						dst.h = screenH;
						dst.w = screenH * videoAspect;
					}
					dst.x = 0.5f * (screenW - dst.w);
					dst.y = 0.5f * (screenH - dst.h);

					if (layer.fit == EVideoFit::Letterbox)
					{
						FillRect(SRect{ 0.f, 0.f, screenW, dst.y }, 0.f, 0.f, 0.f);
						FillRect(SRect{ 0.f, dst.y + dst.h, screenW, screenH - dst.y - dst.h }, 0.f, 0.f, 0.f);
						FillRect(SRect{ 0.f, dst.y, dst.x, dst.h }, 0.f, 0.f, 0.f);
						FillRect(SRect{ dst.x + dst.w, dst.y, screenW - dst.x - dst.w, dst.h }, 0.f, 0.f, 0.f);
					}
				}

				// The aux quad is flipped vertically: t0 = 1, t1 = 0 shows a top-down texture upright.
				IRenderAuxImage::Draw2dImage(dst.x, dst.y, dst.w, dst.h, layer.textureId, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f);
			}
		}

		void DrawTextLines(const char* const* ppLines, size_t count)
		{
			if (gEnv == nullptr || gEnv->pRenderer == nullptr || ppLines == nullptr)
				return;
			const ColorF white(1.f, 1.f, 1.f, 1.f);
			float y = 10.f;
			for (size_t i = 0; i < count; ++i)
			{
				IRenderAuxText::Draw2dLabel(10.f, y, 1.3f, white, false, "%s", ppLines[i]);
				y += 16.f;
			}
		}
	}
}
