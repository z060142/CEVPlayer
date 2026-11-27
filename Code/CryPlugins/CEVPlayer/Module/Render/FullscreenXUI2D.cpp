#include "StdAfx.h"
#include "PluginEnv.h"
#include "FullscreenXUI2D.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
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
			std::vector<int> s_cmdTextureIds;       // per s_cmds entry: the engine texture id, 0 for a solid quad
			std::vector<ITexture*> s_cmdTextures;   // per s_cmds entry: that engine texture (referenced by s_textures)
			std::vector<float> s_params;

			// L6: the frame the loading loop draws. Present (main thread) replaces it with every frame it composes, empty
			// or not; the main thread does not present while a level loads, so the loop draws the last frame composed
			// before the load. The texture handles in it were made on the main thread under `generation`; the engine
			// textures stay referenced by s_textures (Age does not run during the load).
			// L7b: when the XUI2D resource generation is no longer the one the drawn handles were made under (a level
			// unload while a level is loaded releases and recreates the XUI2D resources), the loop wraps the frozen engine
			// texture ids again and rebuilds the draws it submits (same geometry and parameters, new handles), once per
			// generation change; a draw whose engine texture is gone or whose wrap fails is left out for that generation.
			// The handles the loop wraps are handed to the main thread's table (s_textures) by the first Present after the
			// load (AdoptLoopHandles).
			struct SLoopHandle
			{
				int            textureId = 0;
				TTextureHandle handle = 0;
			};
			struct SFrozen
			{
				std::vector<SVertex> vertices;
				std::vector<uint32> indices;
				std::vector<SDrawCmd> cmds;        // as composed on the main thread (handles of `generation`)
				std::vector<int> textureIds;       // per cmds entry: engine texture id, 0 for a solid quad
				std::vector<ITexture*> textures;   // per cmds entry: the engine texture the id resolved to (identity check)
				std::vector<float> params;
				int width = 0, height = 0;   // SFrame::targetWidth / targetHeight
				uint32 generation = 0;
				uint32 mainFrame = 0;        // gEnv->nMainFrameID of the Present that composed it (log only)
				IXUILoadFrame* pFrame = nullptr;     // the surface's loading-loop entry (GetLoadFrame, main thread)
				IXUIRenderer* pRenderer = nullptr;   // the renderer the frame was composed for
				// Loop thread (L7b): the draws submitted, the generation of their handles, the handles the loop wrapped under
				// drawGeneration (handed to the main thread after the load), whether this load logged a left-out draw.
				std::vector<SDrawCmd> drawCmds;
				uint32 drawGeneration = 0;
				std::vector<SLoopHandle> loopHandles;
				bool skipLogged = false;
			};
			std::mutex s_frozenMutex;   // main thread writes in Present, the loop reads in LoadtimeUpdate (short copies)
			SFrozen s_frozen;
			IXUILoadFrame* s_pLoadFrame = nullptr;   // main thread: GetLoadFrame of s_surface

			// Loop-thread state of the current iteration and the counters of one loading loop (reset by EndLoopIfBegun).
			IXUILoadFrame* s_pLoopFrame = nullptr;   // loop thread: the entry submitted in this iteration's update
			std::atomic<bool> s_loopBegun{ false };  // a Begin succeeded since the last End
			std::atomic<uint32> s_loopIterations{ 0 }, s_loopEmpty{ 0 }, s_loopBegins{ 0 }, s_loopRefused{ 0 }, s_loopExecuted{ 0 }, s_loopExecuteFailed{ 0 };
			std::atomic<uint32> s_loopRebuilds{ 0 };   // L7b: generation changes the loop rebuilt the frozen frame for
			// L7b, main thread: the handles a finished loop wrapped (moved out of s_frozen by EndLoop) and their
			// generation; AdoptLoopHandles puts them into s_textures.
			std::vector<SLoopHandle> s_adoptHandles;
			uint32 s_adoptGeneration = 0;

			void ClearFrozen()
			{
				std::lock_guard<std::mutex> lock(s_frozenMutex);
				s_frozen.vertices.clear();
				s_frozen.indices.clear();
				s_frozen.cmds.clear();
				s_frozen.textureIds.clear();
				s_frozen.textures.clear();
				s_frozen.params.clear();
				s_frozen.pFrame = nullptr;
				s_frozen.drawCmds.clear();
				s_frozen.loopHandles.clear(); // a finished loop's handles were moved out by EndLoop before this
			}

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

			//! Main thread. End of the loading-loop entry when a loop began it since the last call (IXUILoadFrame::End
			//! "after the loading loop stopped"), with one log line of that loop's counters.
			void EndLoop(const char* why)
			{
				const uint32 iterations = s_loopIterations.exchange(0);
				const uint32 empty = s_loopEmpty.exchange(0);
				const uint32 begins = s_loopBegins.exchange(0);
				const uint32 refused = s_loopRefused.exchange(0);
				const uint32 executed = s_loopExecuted.exchange(0);
				const uint32 executeFailed = s_loopExecuteFailed.exchange(0);
				const uint32 rebuilds = s_loopRebuilds.exchange(0);
				const bool begun = s_loopBegun.exchange(false);
				{
					// L7b: the loop has stopped; the handles it wrapped go to the main thread's table (AdoptLoopHandles).
					std::lock_guard<std::mutex> lock(s_frozenMutex);
					if (!s_frozen.loopHandles.empty())
					{
						s_adoptHandles.insert(s_adoptHandles.end(), s_frozen.loopHandles.begin(), s_frozen.loopHandles.end());
						s_adoptGeneration = s_frozen.drawGeneration;
						s_frozen.loopHandles.clear();
					}
				}
				if (iterations == 0 && !begun)
					return;
				SLoadFrameStats stats;
				if (s_pLoadFrame != nullptr)
					s_pLoadFrame->GetStats(stats);
				if (begun && s_pLoadFrame != nullptr)
					s_pLoadFrame->End();
				uint32 frozenFrame = 0, frozenCmds = 0;
				{
					std::lock_guard<std::mutex> lock(s_frozenMutex);
					frozenFrame = s_frozen.mainFrame;
					frozenCmds = static_cast<uint32>(s_frozen.cmds.size());
				}
				LogAlways("fullscreen output: loading loop (XUI2D surface %u, %s): %u iteration(s), %u without a frame, %u begun, %u refused (last refusal %u), "
				          "%u executed, %u execute(s) refused, %u rebuild(s); End %s; frozen frame of main frame %u (%u draw(s)), main frame now %u; entry stats executed %u "
				          "draws %u skipped %u clears %u white %u dropped %u output %ux%u",
					s_surface, why, iterations, empty, begins, refused, stats.lastRefusal, executed, executeFailed, rebuilds, begun ? "called" : "not needed", frozenFrame, frozenCmds,
					gEnv->nMainFrameID, stats.executed, stats.draws, stats.drawsSkipped, stats.clears, stats.whiteFallbacks, stats.droppedFrames, stats.outputWidth,
					stats.outputHeight);
			}

			//! Main thread, after the generation check of Present (`adopt` true) or at Shutdown (false). L7b: a handle a
			//! finished loop wrapped becomes the handle of its s_textures entry when it was made under the current
			//! generation and the entry has no valid handle; the main thread is then the entry that uses it last
			//! (IXUILoadFrame: a handle is destroyed through the entry that last uses it) and Age / Shutdown destroy it.
			//! Any other handle of the current generation is destroyed here; a handle of another generation is invalid
			//! already (Destroy* is a no-op for it, IXUIRenderer::GetResourceGeneration).
			void AdoptLoopHandles(IXUIRenderer* pRenderer, uint32 generation, bool adopt)
			{
				if (s_adoptHandles.empty())
					return;
				uint32 adopted = 0, destroyed = 0;
				for (const SLoopHandle& h : s_adoptHandles)
				{
					if (s_adoptGeneration != generation || pRenderer == nullptr)
						continue;
					auto it = s_textures.find(h.textureId);
					if (adopt && it != s_textures.end() && (it->second.handle == 0 || it->second.generation != generation))
					{
						it->second.handle = h.handle;
						it->second.generation = generation;
						++adopted;
					}
					else
					{
						pRenderer->DestroyTexture(h.handle);
						++destroyed;
					}
				}
				const uint32 count = static_cast<uint32>(s_adoptHandles.size());
				LogAlways("fullscreen output: %u texture handle(s) wrapped in the loading loop (generation %u, now %u): %u adopted, %u destroyed, %u stale", count,
					s_adoptGeneration, generation, adopted, destroyed, count - adopted - destroyed);
				s_adoptHandles.clear();
			}

			void DestroySurface(IXUIRenderer* pRenderer, const char* why)
			{
				if (s_surface == 0)
					return;
				EndLoop(why); // the entry belongs to the surface: given back before it goes
				ClearFrozen();
				s_pLoadFrame = nullptr;
				if (pRenderer != nullptr)
					pRenderer->DestroySurface(s_surface);
				LogAlways("fullscreen output: XUI2D surface %u destroyed (%s)", s_surface, why);
				s_surface = 0;
				s_viewW = s_viewH = 0;
			}

#if CEV_HAVE_LOADTIME_CALLBACKS
			//! L6: the order-150 loadtime callback (between the texture sources at 100 and the UIs at 200, so the video
			//! lands under the XTFrame UI). XTFrame CE-117 (XUI2D_Patched bit 512): Begin + Submit in
			//! the update phase, Execute in the render phase; only the first screen-surface entry to execute in an
			//! iteration gets its ClearOutput, so asking for it every time clears only when nothing was drawn before.
			class CLoopDraw final : public ILoadtimeCallback
			{
			public:
				void LoadtimeUpdate(float) override
				{
					s_pLoopFrame = nullptr;
					s_loopIterations.fetch_add(1, std::memory_order_relaxed);
					std::lock_guard<std::mutex> lock(s_frozenMutex);
					if (s_frozen.pFrame == nullptr || s_frozen.cmds.empty())
					{
						s_loopEmpty.fetch_add(1, std::memory_order_relaxed);
						return; // nothing composed before the load: no Begin in this iteration
					}
					// L7b: GetResourceGeneration may be called from any thread; a changed value invalidates every handle.
					const uint32 generation = s_frozen.pRenderer != nullptr ? s_frozen.pRenderer->GetResourceGeneration() : s_frozen.drawGeneration;
					if (generation != s_frozen.drawGeneration)
						Rebuild(generation);
					if (s_frozen.drawCmds.empty())
					{
						s_loopEmpty.fetch_add(1, std::memory_order_relaxed);
						return; // every draw is left out under this generation: no Begin
					}
					if (!s_frozen.pFrame->Begin(s_frozen.drawGeneration))
					{
						s_loopRefused.fetch_add(1, std::memory_order_relaxed);
						return;
					}
					s_loopBegun.store(true, std::memory_order_relaxed);
					s_loopBegins.fetch_add(1, std::memory_order_relaxed);
					s_frozen.pFrame->ClearOutput(0.f, 0.f, 0.f, 1.f);
					SPass pass;
					std::memset(&pass, 0, sizeof(pass));
					pass.target = kSurfaceTarget;
					pass.firstCmd = 0;
					pass.cmdCount = static_cast<uint32>(s_frozen.drawCmds.size());
					SFrame f = {};
					f.vertices = s_frozen.vertices.data();
					f.vertexCount = static_cast<uint32>(s_frozen.vertices.size());
					f.indices = s_frozen.indices.data();
					f.indexCount = static_cast<uint32>(s_frozen.indices.size());
					f.cmds = s_frozen.drawCmds.data();
					f.cmdCount = static_cast<uint32>(s_frozen.drawCmds.size());
					f.passes = &pass;
					f.passCount = 1;
					f.params = s_frozen.params.empty() ? nullptr : s_frozen.params.data();
					f.paramCount = static_cast<uint32>(s_frozen.params.size());
					f.targetWidth = s_frozen.width;
					f.targetHeight = s_frozen.height;
					s_frozen.pFrame->Submit(f); // deep copy into the entry's arena
					s_pLoopFrame = s_frozen.pFrame;
				}

				bool LoadtimeRender() override
				{
					IXUILoadFrame* const pFrame = s_pLoopFrame;
					s_pLoopFrame = nullptr;
					if (pFrame == nullptr)
						return true; // nothing submitted in this iteration: no Execute
					if (pFrame->Execute())
						s_loopExecuted.fetch_add(1, std::memory_order_relaxed);
					else
						s_loopExecuteFailed.fetch_add(1, std::memory_order_relaxed);
					return true;
				}

			private:
				//! Loop thread, s_frozenMutex held. L7b: wraps the frozen engine texture ids again under `generation` (each id
				//! once) and rebuilds drawCmds from cmds with the new handles; geometry, parameters and order stay. A draw
				//! whose engine id no longer resolves to the texture the main thread used (the engine's id lookup,
				//! IRenderer::EF_GetTextureByID: XUI2D's WrapEngineTexture does not check the id) or whose wrap returns 0 is
				//! left out under this generation, with one log line per load.
				static void Rebuild(uint32 generation)
				{
					const uint32 previous = s_frozen.drawGeneration;
					s_frozen.drawCmds.clear();
					s_frozen.loopHandles.clear(); // made under another generation: invalid now (Destroy* would be a no-op)
					s_frozen.drawGeneration = generation;
					uint32 wrapped = 0, skipped = 0;
					for (size_t i = 0; i < s_frozen.cmds.size(); ++i)
					{
						SDrawCmd cmd = s_frozen.cmds[i];
						const int id = i < s_frozen.textureIds.size() ? s_frozen.textureIds[i] : 0;
						if (id > 0)
						{
							const char* why = nullptr;
							TTextureHandle handle = 0;
							for (const SLoopHandle& h : s_frozen.loopHandles)
							{
								if (h.textureId == id)
									handle = h.handle;
							}
							if (handle == 0)
							{
								ITexture* const pNow = gEnv->pRenderer != nullptr ? gEnv->pRenderer->EF_GetTextureByID(id) : nullptr;
								if (pNow == nullptr || pNow != s_frozen.textures[i])
									why = "engine texture id no longer valid";
								else if ((handle = s_frozen.pRenderer->WrapEngineTexture(id)) == 0)
									why = "WrapEngineTexture failed";
								else
								{
									s_frozen.loopHandles.push_back(SLoopHandle{ id, handle });
									++wrapped;
								}
							}
							if (why != nullptr)
							{
								++skipped;
								if (!s_frozen.skipLogged)
								{
									s_frozen.skipLogged = true;
									LogAlways("fullscreen output: loading loop: draw %u (engine texture %d) left out under XUI2D resource generation %u (%s)",
										static_cast<uint32>(i), id, generation, why);
								}
								continue;
							}
							cmd.texture = handle;
						}
						s_frozen.drawCmds.push_back(cmd);
					}
					s_loopRebuilds.fetch_add(1, std::memory_order_relaxed);
					LogAlways("fullscreen output: loading loop: XUI2D resource generation %u -> %u, frozen frame rebuilt (%u of %u draw(s), %u texture(s) wrapped again, "
					          "%u left out)",
						previous, generation, static_cast<uint32>(s_frozen.drawCmds.size()), static_cast<uint32>(s_frozen.cmds.size()), wrapped, skipped);
				}
			};
			CLoopDraw s_loopDraw;
#endif
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
			// L6: the first main-thread frame after a loading loop gives the surface back before submitting to it.
			EndLoop("loop ended");
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
			AdoptLoopHandles(pRenderer, generation, true); // L7b: the handles a finished loop wrapped (none without one)
			if (pRenderer == nullptr || generation == 0)
			{
				ClearFrozen();
				Age(nullptr, generation, frame); // no XUI2D device resources: nothing to draw into
				return;
			}

			s_vertices.clear();
			s_indices.clear();
			s_cmds.clear();
			s_cmdTextureIds.clear();
			s_cmdTextures.clear();
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
				int textureId = 0;
				ITexture* pEngineTexture = nullptr;
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
					textureId = q.textureId;
					pEngineTexture = pTex->pTexture.get();
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
				s_cmdTextureIds.push_back(textureId);
				s_cmdTextures.push_back(pEngineTexture);
			}

			if (!s_cmds.empty())
			{
				if (s_surface == 0)
				{
					s_surface = pRenderer->CreateScreenSurface(SDisplayContextKey{}); // the display context active at the flush
					if (s_surface != 0)
					{
						pRenderer->SetSurfaceOrder(s_surface, kSurfaceOrder);
						s_pLoadFrame = pRenderer->GetLoadFrame(s_surface); // L6: made here, before any load (main thread)
						LogAlways("fullscreen output: XUI2D surface %u created (order %d, generation %u, loading-loop entry %s)", s_surface, kSurfaceOrder, generation,
							s_pLoadFrame != nullptr ? "yes" : "none");
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

			// L6: this frame becomes the one a loading loop starting before the next Present draws (the same draws, the
			// same handles; empty when nothing was submitted).
			{
				std::lock_guard<std::mutex> lock(s_frozenMutex);
				const bool submitted = !s_cmds.empty() && s_surface != 0;
				s_frozen.vertices.assign(s_vertices.begin(), submitted ? s_vertices.end() : s_vertices.begin());
				s_frozen.indices.assign(s_indices.begin(), submitted ? s_indices.end() : s_indices.begin());
				s_frozen.cmds.assign(s_cmds.begin(), submitted ? s_cmds.end() : s_cmds.begin());
				s_frozen.params.assign(s_params.begin(), submitted ? s_params.end() : s_params.begin());
				s_frozen.textureIds.assign(s_cmdTextureIds.begin(), submitted ? s_cmdTextureIds.end() : s_cmdTextureIds.begin());
				s_frozen.textures.assign(s_cmdTextures.begin(), submitted ? s_cmdTextures.end() : s_cmdTextures.begin());
				s_frozen.width = outputW;
				s_frozen.height = outputH;
				s_frozen.generation = generation;
				s_frozen.mainFrame = frame;
				s_frozen.pFrame = submitted ? s_pLoadFrame : nullptr;
				s_frozen.pRenderer = pRenderer;
				// L7b: a loop starts from the main thread's handles (EndLoop moved any loop-made handles out already).
				s_frozen.drawCmds = s_frozen.cmds;
				s_frozen.drawGeneration = generation;
				s_frozen.loopHandles.clear();
				s_frozen.skipLogged = false;
			}
			// The textures of the frozen frame are among those just used: Age does not run while a level loads (no
			// Present), so none of them is released during the loop.
			Age(pRenderer, generation, frame);
		}

		ILoadtimeCallback* LoopCallback()
		{
#if CEV_HAVE_LOADTIME_CALLBACKS
			return &s_loopDraw;
#else
			return nullptr;
#endif
		}

		void Shutdown()
		{
			IXUIRenderer* const pRenderer = Renderer();
			const uint32 generation = pRenderer != nullptr ? pRenderer->GetResourceGeneration() : 0;
			DestroySurface(pRenderer, "shutdown");
			AdoptLoopHandles(pRenderer, generation, false); // L7b: loop handles not adopted yet are destroyed
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
