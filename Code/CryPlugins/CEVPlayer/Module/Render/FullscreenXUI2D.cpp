#include "StdAfx.h"
#include "PluginEnv.h"
#include "FullscreenXUI2D.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include <CryCore/Platform/CryWindows.h> // GetModuleHandleA / GetProcAddress
#include <CryRenderer/IRenderer.h>
#include <CryRenderer/ITexture.h>
#include <CryRenderer/IXUIRenderer.h>

namespace CEVPlayer
{
	namespace FullscreenXUI2D
	{
		namespace
		{
			using namespace Cry::XUI;
			using FullscreenOutput::EQuadKind;
			using FullscreenOutput::SQuad;

			//! Below every CryXTUI surface (they use orders >= 0).
			constexpr int kSurfaceOrder = -(1 << 20);
			//! XUI2D does not hold the engine textures it wraps (the render thread looks the id up when it draws, and
			//! engine ids are reused): a texture stays referenced here for this many main-thread frames after the last
			//! submitted frame that used it.
			constexpr uint32 kTextureGraceFrames = 4;

			struct STexture
			{
				_smart_ptr<ITexture> pTexture;
				TTextureHandle       handle = 0;
				uint32               generation = 0;   // the resource generation `handle` was made under
				uint32               lastUsedFrame = 0;
			};

			int s_patched = -1;   // -1 not read yet
			std::unordered_map<int, STexture> s_textures;   // engine texture id ->
			uint32 s_surface = 0;
			int s_viewW = 0, s_viewH = 0;                   // the viewport given to the surface
			bool s_generationKnown = false;
			uint32 s_generation = 0;

			std::vector<SVertex> s_vertices;   // scratch of Present (kept across frames)
			std::vector<uint32> s_indices;
			std::vector<SDrawCmd> s_cmds;
			std::vector<float> s_params;

			const char* RendererModuleName(IRenderer* pRenderer)
			{
				switch (pRenderer->GetRenderType())
				{
				case ERenderType::Direct3D11: return "CryRenderD3D11";
				case ERenderType::Direct3D12: return "CryRenderD3D12";
				case ERenderType::Vulkan:     return "CryRenderVulkan";
				default:                      return nullptr;
				}
			}

			IXUIRenderer* Renderer()
			{
				return gEnv != nullptr && gEnv->pRenderer != nullptr ? gEnv->pRenderer->GetIXUIRenderer() : nullptr;
			}

			uint8 ToByte(float v)
			{
				const float c = std::min(1.f, std::max(0.f, v));
				return static_cast<uint8>(c * 255.f + 0.5f);
			}

			//! SVertex::rgba: premultiplied, bytes r, g, b, a (rgba & 0xFF is red).
			uint32 PackColor(const float (&c)[4])
			{
				return static_cast<uint32>(ToByte(c[0])) | (static_cast<uint32>(ToByte(c[1])) << 8) | (static_cast<uint32>(ToByte(c[2])) << 16) |
				       (static_cast<uint32>(ToByte(c[3])) << 24);
			}

			//! Plane `p` (x, y, w, h texels) sampled at luma position uv * (lumaW, lumaH), `step` luma pixels per plane texel
			//! (1, or 2 for chroma) and (offX, offY) plane texels of chroma siting added: (scaleU, scaleV, offU, offV) and the
			//! clamp rectangle (the plane inset by half a texel), texture coordinates.
			void PlaneMapping(const uint32 (&p)[4], double lumaW, double lumaH, double step, double offX, double offY, double texW, double texH,
			                  float (&uv)[4], float (&clampRect)[4])
			{
				uv[0] = static_cast<float>(lumaW / step / texW);
				uv[1] = static_cast<float>(lumaH / step / texH);
				uv[2] = static_cast<float>((p[0] + offX) / texW);
				uv[3] = static_cast<float>((p[1] + offY) / texH);
				clampRect[0] = static_cast<float>((p[0] + 0.5) / texW);
				clampRect[1] = static_cast<float>((p[1] + 0.5) / texH);
				clampRect[2] = static_cast<float>((p[0] + p[2] - 0.5) / texW);
				clampRect[3] = static_cast<float>((p[1] + p[3] - 0.5) / texH);
			}

			//! SPlanarParams of a packed quad over a texW x texH texture (XTFrame 23 §23.aj "drawing"; IXUIRenderer.h
			//! EShading::TexturePlanar / SPlanarParams). colorMatrix 0 BT.601 / 1 BT.709, colorRange 0 limited / 1 full,
			//! chromaSiting 0 left / 1 center / 2 top-left (the contract snapshot values).
			bool PlanarParams(const SQuad& q, int texW, int texH, SPlanarParams& out)
			{
				std::memset(&out, 0, sizeof(out));
				const bool alpha = q.kind == EQuadKind::Yuva420;
				if (texW <= 0 || texH <= 0 || q.planeY[2] == 0 || q.planeY[3] == 0 || q.planeU[2] == 0 || q.planeU[3] == 0 || q.planeV[2] == 0 || q.planeV[3] == 0 ||
				    (alpha && (q.planeA[2] == 0 || q.planeA[3] == 0)))
					return false;
				const double tw = texW, th = texH;
				// Chroma continuous coordinate of luma position (X, Y): Center (X/2, Y/2), Left (X/2 + 0.25, Y/2),
				// TopLeft (X/2 + 0.25, Y/2 + 0.25), in chroma texels.
				const double cx = q.chromaSiting == 1 ? 0.0 : 0.25;
				const double cy = q.chromaSiting == 2 ? 0.25 : 0.0;
				const double lumaW = q.planeY[2], lumaH = q.planeY[3];
				PlaneMapping(q.planeY, lumaW, lumaH, 1.0, 0.0, 0.0, tw, th, out.uv[0], out.clampRect[0]);
				PlaneMapping(q.planeU, lumaW, lumaH, 2.0, cx, cy, tw, th, out.uv[1], out.clampRect[1]);
				PlaneMapping(q.planeV, lumaW, lumaH, 2.0, cx, cy, tw, th, out.uv[2], out.clampRect[2]);
				if (alpha)
					PlaneMapping(q.planeA, lumaW, lumaH, 1.0, 0.0, 0.0, tw, th, out.uv[3], out.clampRect[3]);

				// Limited: Y' = (y - 16/255) * 255/219, C = (c - 128/255) * 255/224; Full: Y' = y, C = c - 128/255.
				// R = Y' + 2 (1 - Kr) Cr, B = Y' + 2 (1 - Kb) Cb, G = (Y' - Kr R - Kb B) / (1 - Kr - Kb).
				const bool bt709 = q.colorMatrix == 1;
				const double kr = bt709 ? 0.2126 : 0.299;
				const double kb = bt709 ? 0.0722 : 0.114;
				const double kg = 1.0 - kr - kb;
				const bool limited = q.colorRange == 0;
				const double ys = limited ? 255.0 / 219.0 : 1.0;
				const double yo = limited ? -16.0 / 219.0 : 0.0;
				const double cs = limited ? 255.0 / 224.0 : 1.0;
				const double co = limited ? -128.0 / 224.0 : -128.0 / 255.0;
				const double rv = 2.0 * (1.0 - kr);
				const double bu = 2.0 * (1.0 - kb);
				const double gv = 2.0 * kr * (1.0 - kr) / kg;
				const double gu = 2.0 * kb * (1.0 - kb) / kg;
				const double rows[3][4] = {
					{ ys, 0.0, rv * cs, yo + rv * co },
					{ ys, -gu * cs, -gv * cs, yo - (gu + gv) * co },
					{ ys, bu * cs, 0.0, yo + bu * co },
				};
				for (int r = 0; r < 3; ++r)
					for (int c = 0; c < 4; ++c)
						out.matrix[r][c] = static_cast<float>(rows[r][c]);
				out.hasAlpha = alpha ? 1.f : 0.f;
				out.premultiplied = 0.f;
				return true;
			}

			//! The entry of engine texture `id`, referenced and wrapped for `generation`; null when the id is unknown.
			STexture* Acquire(IXUIRenderer& renderer, int id, uint32 generation, uint32 frame)
			{
				auto it = s_textures.find(id);
				if (it == s_textures.end())
				{
					ITexture* const pTexture = gEnv->pRenderer->EF_GetTextureByID(id);
					if (pTexture == nullptr)
						return nullptr;
					it = s_textures.emplace(id, STexture()).first;
					it->second.pTexture = pTexture;
				}
				STexture& t = it->second;
				if (t.handle == 0 || t.generation != generation)
				{
					t.handle = renderer.WrapEngineTexture(id);
					t.generation = generation;
				}
				t.lastUsedFrame = frame;
				return t.handle != 0 ? &t : nullptr;
			}

			//! Lets go of the textures no submitted frame used for more than kTextureGraceFrames frames.
			void Age(IXUIRenderer* pRenderer, uint32 generation, uint32 frame)
			{
				for (auto it = s_textures.begin(); it != s_textures.end();)
				{
					if (frame - it->second.lastUsedFrame > kTextureGraceFrames)
					{
						if (pRenderer != nullptr && it->second.handle != 0 && it->second.generation == generation)
							pRenderer->DestroyTexture(it->second.handle);
						it = s_textures.erase(it); // releases the reference
					}
					else
						++it;
				}
			}

			void DestroySurface(IXUIRenderer* pRenderer, const char* why)
			{
				if (s_surface == 0)
					return;
				if (pRenderer != nullptr)
					pRenderer->DestroySurface(s_surface);
				LogAlways("fullscreen output: XUI2D surface %u destroyed (%s)", s_surface, why);
				s_surface = 0;
				s_viewW = s_viewH = 0;
			}
		}

		int PatchedBits()
		{
			if (s_patched >= 0)
				return s_patched;
			if (gEnv == nullptr || gEnv->pRenderer == nullptr || gEnv->IsDedicated())
				return 0; // no renderer yet: read again once there is one
			s_patched = 0;
#if CRY_PLATFORM_WINDOWS
			if (const char* moduleName = RendererModuleName(gEnv->pRenderer))
			{
				if (HMODULE hModule = ::GetModuleHandleA(moduleName))
				{
					using PFN_XUI2DPatched = int (*)();
					if (const PFN_XUI2DPatched pPatched = reinterpret_cast<PFN_XUI2DPatched>(::GetProcAddress(hModule, "XUI2D_Patched")))
						s_patched = pPatched();
				}
			}
#endif
			return s_patched;
		}

		bool Available()
		{
			return PatchedBits() != 0 && Renderer() != nullptr;
		}

		void Present(const std::vector<SQuad>& quads, int outputW, int outputH)
		{
			IXUIRenderer* const pRenderer = Renderer();
			const uint32 frame = gEnv->nMainFrameID;
			const uint32 generation = pRenderer != nullptr ? pRenderer->GetResourceGeneration() : 0;
			if (!s_generationKnown || generation != s_generation)
			{
				// Every texture handle and the surface belong to the old generation: dropped, made again on demand. The
				// texture references stay (their lifetime is about the render thread, not the handles).
				if (s_generationKnown)
					LogAlways("fullscreen output: XUI2D resource generation %u -> %u", s_generation, generation);
				s_generationKnown = true;
				s_generation = generation;
				DestroySurface(pRenderer, "resource generation changed");
				for (auto& kv : s_textures)
					kv.second.handle = 0;
			}
			if (pRenderer == nullptr || generation == 0)
			{
				Age(nullptr, generation, frame); // no XUI2D device resources: nothing to draw into
				return;
			}

			s_vertices.clear();
			s_indices.clear();
			s_cmds.clear();
			s_params.clear();
			for (const SQuad& q : quads)
			{
				if (q.w <= 0.f || q.h <= 0.f)
					continue;
				SDrawCmd cmd;
				std::memset(&cmd, 0, sizeof(cmd));
				cmd.sampler = ESampler::LinearClamp;
				cmd.blend = q.replace ? EBlend::Replace : EBlend::SrcOver;
				cmd.stencilOp = EStencilOp::None;
				cmd.program = EEffectProgram::None;
				cmd.kind = EDrawKind::Triangles;
				// clipX/Y/W/H stay 0: no scissor (XUI2DPass.cpp, bWantScissor = clipW > 0 && clipH > 0).
				uint32 rgba = 0xFFFFFFFFu;
				if (q.kind == EQuadKind::Solid)
				{
					cmd.shading = EShading::Solid;
					rgba = PackColor(q.color);
				}
				else
				{
					STexture* const pTex = Acquire(*pRenderer, q.textureId, generation, frame);
					if (pTex == nullptr)
						continue;
					cmd.texture = pTex->handle;
					if (q.kind == EQuadKind::RgbaPremult)
					{
						cmd.shading = EShading::Texture;
						rgba = PackColor(q.color);
					}
					else
					{
						SPlanarParams pp;
						if (!PlanarParams(q, pTex->pTexture->GetWidth(), pTex->pTexture->GetHeight(), pp))
							continue;
						cmd.shading = EShading::TexturePlanar;
						cmd.paramIndex = static_cast<uint32>(s_params.size());
						const float* const pFloats = reinterpret_cast<const float*>(&pp);
						s_params.insert(s_params.end(), pFloats, pFloats + sizeof(pp) / sizeof(float));
						rgba = PackColor(q.color);
					}
				}
				const float x0 = q.x, y0 = q.y, x1 = q.x + q.w, y1 = q.y + q.h;
				cmd.baseVertex = static_cast<uint32>(s_vertices.size());
				cmd.vertexCount = 4;
				cmd.firstIndex = static_cast<uint32>(s_indices.size());
				cmd.indexCount = 6;
				s_vertices.push_back(SVertex{ x0, y0, 1.f, 0.f, 0.f, rgba });
				s_vertices.push_back(SVertex{ x1, y0, 1.f, 1.f, 0.f, rgba });
				s_vertices.push_back(SVertex{ x1, y1, 1.f, 1.f, 1.f, rgba });
				s_vertices.push_back(SVertex{ x0, y1, 1.f, 0.f, 1.f, rgba });
				static const uint32 kIndices[6] = { 0, 1, 2, 0, 2, 3 };
				s_indices.insert(s_indices.end(), kIndices, kIndices + 6);
				s_cmds.push_back(cmd);
			}

			if (!s_cmds.empty())
			{
				if (s_surface == 0)
				{
					s_surface = pRenderer->CreateScreenSurface(SDisplayContextKey{}); // the display context active at the flush
					if (s_surface != 0)
					{
						pRenderer->SetSurfaceOrder(s_surface, kSurfaceOrder);
						LogAlways("fullscreen output: XUI2D surface %u created (order %d, generation %u)", s_surface, kSurfaceOrder, generation);
					}
				}
				if (s_surface != 0)
				{
					if (outputW != s_viewW || outputH != s_viewH)
					{
						pRenderer->SetSurfaceViewport(s_surface, 0, 0, outputW, outputH);
						s_viewW = outputW;
						s_viewH = outputH;
					}
					SPass pass;
					std::memset(&pass, 0, sizeof(pass));
					pass.target = kSurfaceTarget;
					pass.firstCmd = 0;
					pass.cmdCount = static_cast<uint32>(s_cmds.size());
					SFrame f = {};
					f.vertices = s_vertices.data();
					f.vertexCount = static_cast<uint32>(s_vertices.size());
					f.indices = s_indices.data();
					f.indexCount = static_cast<uint32>(s_indices.size());
					f.cmds = s_cmds.data();
					f.cmdCount = static_cast<uint32>(s_cmds.size());
					f.passes = &pass;
					f.passCount = 1;
					f.params = s_params.empty() ? nullptr : s_params.data();
					f.paramCount = static_cast<uint32>(s_params.size());
					f.targetWidth = outputW;
					f.targetHeight = outputH;
					pRenderer->Submit(s_surface, f);
				}
			}
			Age(pRenderer, generation, frame);
		}

		void Shutdown()
		{
			IXUIRenderer* const pRenderer = Renderer();
			const uint32 generation = pRenderer != nullptr ? pRenderer->GetResourceGeneration() : 0;
			DestroySurface(pRenderer, "shutdown");
			for (auto& kv : s_textures)
			{
				if (pRenderer != nullptr && kv.second.handle != 0 && kv.second.generation == generation)
					pRenderer->DestroyTexture(kv.second.handle);
			}
			s_textures.clear();
			s_generationKnown = false;
		}
	}
}
