#include "StdAfx.h"
#include "PluginEnv.h"
#include "Console.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <CrySystem/ConsoleRegistration.h>
#include <CrySystem/File/ICryPak.h>
#include <CryCore/Platform/CryWindows.h> // GetModuleHandleA / GetProcAddress / GetLocalTime (M10a probe)
#include <CryRenderer/IRenderer.h>
#include <CryRenderer/IRenderAuxGeom.h>
#include <CryRenderer/ITexture.h>

#include "Bindings/ApiTable.h"
#include "Bindings/ApiTest.h"
#include "Bindings/FlowTest.h"
#include "Contract/ContractTest.h"
#include "Contract/TexSource.h"
#include "Bindings/ApiPlayers.h"
#include "Player/PlayerManager.h"
#include "Player/VideoPlayerImpl.h"
#include "Render/RenderBridge.h"

namespace CEVPlayer
{
	// ------------------------------------------------------------------------------------------------
	// Development-only measurement probes (S0). They create their own textures and never touch players
	// except cev_bench_play, which drives one through the public player interface.
	namespace DevProbe
	{
		namespace
		{
			using Clock = std::chrono::steady_clock;

			// cev_probe_r8
			int s_r8Id = 0;
			int s_r8W = 0;
			int s_r8H = 0;
			uint32 s_r8Frame = 0;
			std::vector<uint8> s_r8Buf;
			double s_r8MsSum = 0.0;
			float s_r8MsMax = 0.f;
			uint32 s_r8MsCount = 0;
			bool s_r8Rgba = false;       // M8: "rgba" argument, an RGBA8 texture of the same size (RAM comparison)
			double s_r8MbBefore = 0.0;   // private bytes before the texture was created

			// cev_probe_sync
			int s_syncId = 0;
			uint32 s_syncFrame = 0;
			uint32 s_syncIndex = 0;
			std::vector<uint32> s_syncBuf;
			constexpr int kSyncSize = 64;

			// cev_bench_play
			IVideoPlayer* s_benchPlayer = nullptr;

			// cev_bench_hint (M8): one consumer hint sent every frame
			std::string s_hintSurface;
			uint32_t s_hintAccept = 0;
			// cev_show_surface (M8): draw a surface's texture top left (screenshot evidence)
			std::string s_showSurface;

			bool RendererOk() { return gEnv != nullptr && gEnv->pRenderer != nullptr && !gEnv->IsDedicated(); }

			void ReleaseTextureId(int& id)
			{
				if (id > 0 && RendererOk())
					gEnv->pRenderer->RemoveTexture(static_cast<unsigned int>(id));
				id = 0;
			}

			void SyncColor(uint32 index, float& r, float& g, float& b)
			{
				r = (index & 1) ? 1.f : 0.f;
				g = (index & 2) ? 1.f : 0.f;
				b = (index & 4) ? 1.f : 0.f;
			}

			void LogTexture(const char* what, int id)
			{
				ITexture* const pTex = id > 0 ? gEnv->pRenderer->EF_GetTextureByID(id) : nullptr;
				if (pTex == nullptr)
				{
					LogAlways("%s: no texture object for id %d", what, id);
					return;
				}
				LogAlways("%s: id=%d name=<%s> %dx%d mips=%d type=%d src=%d dst=%d format=<%s> loaded=%d deviceBytes=%u flags=0x%x", what, id, pTex->GetName(),
					pTex->GetWidth(), pTex->GetHeight(), pTex->GetNumMips(), static_cast<int>(pTex->GetTextureType()), static_cast<int>(pTex->GetTextureSrcFormat()),
					static_cast<int>(pTex->GetTextureDstFormat()), pTex->GetFormatName(), pTex->IsTextureLoaded() ? 1 : 0, pTex->GetDeviceDataSize(), pTex->GetFlags());
			}
		}

		void CmdProbeR8(IConsoleCmdArgs* args)
		{
			if (!RendererOk())
				return;
			ReleaseTextureId(s_r8Id);
			if (args == nullptr || args->GetArgCount() < 3)
			{
				LogAlways("cev_probe_r8: off (usage: cev_probe_r8 <width> <height> [rgba])");
				return;
			}
			const int w = std::atoi(args->GetArg(1));
			const int h = std::atoi(args->GetArg(2));
			if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
			{
				LogAlways("cev_probe_r8: bad size");
				return;
			}
			s_r8W = w;
			s_r8H = h;
			s_r8Frame = 0;
			s_r8MsSum = 0.0;
			s_r8MsMax = 0.f;
			s_r8MsCount = 0;
			s_r8Rgba = args->GetArgCount() > 3 && stricmp(args->GetArg(3), "rgba") == 0;
			const ETEX_Format format = s_r8Rgba ? eTF_R8G8B8A8 : eTF_R8;
			s_r8Buf.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * (s_r8Rgba ? 4u : 1u), 0);
			s_r8MbBefore = CPlayerManager::ProcessPrivateMb();
			const double gpuBefore = CPlayerManager::ProcessGpuMb();
			const Clock::time_point t0 = Clock::now();
			s_r8Id = static_cast<int>(gEnv->pRenderer->UploadToVideoMemory(nullptr, w, h, format, format, 0, false, FILTER_BILINEAR, 0, s_r8Rgba ? "$CevProbeRgba" : "$CevProbeR8", FT_NOMIPS));
			const float ms = std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
			LogAlways("cev_probe_r8: UploadToVideoMemory(nullptr, %d, %d, %s, ..., FT_NOMIPS) -> id %d (%.3f ms), private %+.1f MB, gpu %+.1f MB (buffer %.1f MB)", w, h,
				s_r8Rgba ? "eTF_R8G8B8A8" : "eTF_R8", s_r8Id, ms, CPlayerManager::ProcessPrivateMb() - s_r8MbBefore, CPlayerManager::ProcessGpuMb() - gpuBefore,
				static_cast<double>(s_r8Buf.size()) / 1048576.0);
			LogTexture("cev_probe_r8", s_r8Id);
		}

		void CmdProbeSync(IConsoleCmdArgs* args)
		{
			if (!RendererOk())
				return;
			ReleaseTextureId(s_syncId);
			const bool on = args != nullptr && args->GetArgCount() > 1 && std::atoi(args->GetArg(1)) != 0;
			if (!on)
			{
				LogAlways("cev_probe_sync: off");
				return;
			}
			s_syncFrame = 0;
			s_syncBuf.assign(kSyncSize * kSyncSize, 0xff000000u);
			s_syncId = static_cast<int>(gEnv->pRenderer->UploadToVideoMemory(nullptr, kSyncSize, kSyncSize, eTF_R8G8B8A8, eTF_R8G8B8A8, 0, false, FILTER_POINT, 0, "$CevProbeSync", FT_NOMIPS));
			LogAlways("cev_probe_sync: texture id %d (bind it with: xtui_bind_texture <surface> <name> %d)", s_syncId, s_syncId);
			LogTexture("cev_probe_sync", s_syncId);
		}

		void CmdBenchPlay(IConsoleCmdArgs* args)
		{
			if (mEnv->pManager == nullptr)
				return;
			if (s_benchPlayer != nullptr)
			{
				mEnv->pManager->DestroyPlayer(s_benchPlayer);
				s_benchPlayer = nullptr;
			}
			if (args == nullptr || args->GetArgCount() < 3)
			{
				LogAlways("cev_bench_play: stopped (usage: cev_bench_play <path> <texture name> [loop 0|1])");
				return;
			}
			s_benchPlayer = mEnv->pManager->CreatePlayer(args->GetArg(2));
			if (s_benchPlayer == nullptr)
				return;
			s_benchPlayer->SetLoop(args->GetArgCount() < 4 || std::atoi(args->GetArg(3)) != 0);
			if (!s_benchPlayer->Load(args->GetArg(1)))
			{
				LogAlways("cev_bench_play: load rejected <%s>", args->GetArg(1));
				mEnv->pManager->DestroyPlayer(s_benchPlayer);
				s_benchPlayer = nullptr;
				return;
			}
			s_benchPlayer->Play();
			LogAlways("cev_bench_play: <%s> -> texture <%s>", args->GetArg(1), args->GetArg(2));
		}

		void CmdBenchStop(IConsoleCmdArgs*)
		{
			if (mEnv->pManager != nullptr && s_benchPlayer != nullptr)
				mEnv->pManager->DestroyPlayer(s_benchPlayer);
			s_benchPlayer = nullptr;
			LogAlways("cev_bench_stop");
		}

		void CmdBenchHint(IConsoleCmdArgs* args)
		{
			if (args == nullptr || args->GetArgCount() < 3)
			{
				if (!s_hintSurface.empty())
				{
					CryTexSourceHint h = {};
					h.size = sizeof(h);
					h.flags = 2u; // release
					h.consumer = 0xBE7C0001ull;
					if (const CryTexSourceApi* api = CryTexSource_v1())
						api->hint(s_hintSurface.c_str(), &h);
				}
				s_hintSurface.clear();
				LogAlways("cev_bench_hint: off (usage: cev_bench_hint <surface> <acceptLayouts>, e.g. 7 = RGBA8 | packed YUV420 | packed YUVA420)");
				return;
			}
			s_hintSurface = args->GetArg(1);
			s_hintAccept = static_cast<uint32_t>(std::strtoul(args->GetArg(2), nullptr, 0));
			LogAlways("cev_bench_hint: <%s> visible, acceptLayouts 0x%x, every frame", s_hintSurface.c_str(), s_hintAccept);
		}

		void CmdShowSurface(IConsoleCmdArgs* args)
		{
			s_showSurface = args != nullptr && args->GetArgCount() > 1 ? args->GetArg(1) : "";
			LogAlways("cev_show_surface: %s", s_showSurface.empty() ? "off" : s_showSurface.c_str());
		}

		void CmdStatsFile(IConsoleCmdArgs* args)
		{
			if (mEnv->pManager == nullptr)
				return;
			const char* const path = args != nullptr && args->GetArgCount() > 1 ? args->GetArg(1) : nullptr;
			if (path != nullptr && stricmp(path, "off") != 0)
				mEnv->m_cevStats = 1; // rows are written by the per-second stats step
			mEnv->pManager->SetStatsFile(path != nullptr && stricmp(path, "off") != 0 ? path : nullptr);
		}

		void BeforeRender()
		{
			if (!s_hintSurface.empty())
			{
				CryTexSourceHint h = {};
				h.size = sizeof(h);
				h.flags = 1u;
				h.pixelWidth = 1920;
				h.pixelHeight = 1080;
				h.consumer = 0xBE7C0001ull;
				h.acceptLayouts = s_hintAccept;
				if (const CryTexSourceApi* api = CryTexSource_v1())
					api->hint(s_hintSurface.c_str(), &h);
			}
			if (!RendererOk())
				return;
			if (s_r8Id > 0)
			{
				++s_r8Frame;
				const int w = s_r8W, h = s_r8H;
				const int bar = static_cast<int>((s_r8Frame * 8u) % static_cast<uint32>(w));
				for (int y = 0; y < h; ++y)
				{
					uint8* row = s_r8Buf.data() + static_cast<size_t>(y) * w * (s_r8Rgba ? 4 : 1);
					if (s_r8Rgba)
						continue; // RGBA: the content does not matter for the RAM / timing comparison
					for (int x = 0; x < w; ++x)
						row[x] = static_cast<uint8>(((x + y) >> 3) * 16 + s_r8Frame);
					for (int x = bar; x < bar + 16 && x < w; ++x)
						row[x] = 255;
				}
				const Clock::time_point t0 = Clock::now();
				gEnv->pRenderer->UpdateTextureInVideoMemory(static_cast<uint32>(s_r8Id), s_r8Buf.data(), 0, 0, w, h, s_r8Rgba ? eTF_R8G8B8A8 : eTF_R8);
				const float ms = std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
				s_r8MsSum += ms;
				s_r8MsMax = std::max(s_r8MsMax, ms);
				if (++s_r8MsCount == 120)
				{
					LogAlways("cev_probe_r8: %dx%d UpdateTextureInVideoMemory(%s) avg %.3f ms max %.3f ms over 120 frames (%u bytes each), private %+.1f MB since before the texture", w, h,
						s_r8Rgba ? "eTF_R8G8B8A8" : "eTF_R8", s_r8MsSum / s_r8MsCount, s_r8MsMax, static_cast<unsigned>(s_r8Buf.size()), CPlayerManager::ProcessPrivateMb() - s_r8MbBefore);
					s_r8MsSum = 0.0;
					s_r8MsMax = 0.f;
					s_r8MsCount = 0;
				}
			}
			if (s_syncId > 0)
			{
				++s_syncFrame;
				s_syncIndex = s_syncFrame % 8u;
				float r, g, b;
				SyncColor(s_syncIndex, r, g, b);
				const uint32 rgba = 0xff000000u | (b > 0.f ? 0x00ff0000u : 0u) | (g > 0.f ? 0x0000ff00u : 0u) | (r > 0.f ? 0x000000ffu : 0u);
				std::fill(s_syncBuf.begin(), s_syncBuf.end(), rgba);
				gEnv->pRenderer->UpdateTextureInVideoMemory(static_cast<uint32>(s_syncId), reinterpret_cast<unsigned char*>(s_syncBuf.data()), 0, 0, kSyncSize, kSyncSize, eTF_R8G8B8A8);
				// A: aux colour block submitted from UpdateBeforeRender (same step as the upload)
				IRenderAuxImage::Draw2dImage(20.f, 300.f, 80.f, 80.f, -1, 0.f, 0.f, 1.f, 1.f, 0.f, r, g, b, 1.f);
			}
		}

		void AfterRender()
		{
			if (!RendererOk())
				return;
			if (!s_showSurface.empty() && mEnv->pManager != nullptr)
			{
				if (IVideoPlayer* const p = mEnv->pManager->FindPlayerBySurface(s_showSurface.c_str()))
				{
					if (ITexture* const pTex = p->GetTexture())
					{
						const float dw = 480.f;
						const float dh = std::min(640.f, dw * static_cast<float>(pTex->GetHeight()) / static_cast<float>(std::max(1, pTex->GetWidth())));
						IRenderAuxImage::Draw2dImage(20.f, 20.f, dw, dh, pTex->GetTextureID(), 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f);
						IRenderAuxText::Draw2dLabel(20.f, 24.f + dh, 1.3f, ColorF(1.f, 1.f, 0.f, 1.f), false, "cev_show_surface %s: %s %dx%d", s_showSurface.c_str(),
							p->GetPixelLayout() == EVideoPixelLayout::RGBA8 ? "RGBA8" : "R8 packed", pTex->GetWidth(), pTex->GetHeight());
					}
				}
			}
			if (s_r8Id > 0)
			{
				const float dw = 480.f;
				const float dh = std::min(400.f, dw * static_cast<float>(s_r8H) / static_cast<float>(s_r8W));
				IRenderAuxImage::Draw2dImage(20.f, 20.f, dw, dh, s_r8Id, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f);
				IRenderAuxText::Draw2dLabel(20.f, 24.f + dh, 1.3f, ColorF(1.f, 1.f, 0.f, 1.f), false, "cev_probe_r8 %dx%d frame %u", s_r8W, s_r8H, s_r8Frame);
			}
			if (s_syncId > 0)
			{
				float r, g, b;
				SyncColor(s_syncIndex, r, g, b);
				// B: aux colour block submitted from UpdateAfterRender; C: the uploaded texture drawn by aux
				IRenderAuxImage::Draw2dImage(120.f, 300.f, 80.f, 80.f, -1, 0.f, 0.f, 1.f, 1.f, 0.f, r, g, b, 1.f);
				IRenderAuxImage::Draw2dImage(220.f, 300.f, 80.f, 80.f, s_syncId, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f);
				IRenderAuxText::Draw2dLabel(20.f, 384.f, 1.3f, ColorF(1.f, 1.f, 0.f, 1.f), false, "A aux@before  B aux@after  C aux(tex)   frame %u idx %u", s_syncFrame, s_syncIndex);
			}
		}

		void Shutdown()
		{
			ReleaseTextureId(s_r8Id);
			ReleaseTextureId(s_syncId);
			if (s_benchPlayer != nullptr && mEnv->pManager != nullptr && !mEnv->pManager->IsShutDown())
				mEnv->pManager->DestroyPlayer(s_benchPlayer);
			s_benchPlayer = nullptr;
		}
	}

	// ------------------------------------------------------------------------------------------------
	// Development-only test driver: cev_run_script <file> runs one console line per frame from the plug-in's own
	// update, immediately (IConsole::ExecuteString without deferring), so no engine deferred queue / exec cfg is
	// involved. `wait <seconds>` pauses, empty lines and lines starting with # or // are skipped.
	namespace RunScript
	{
		namespace
		{
			std::vector<std::string> s_lines;
			size_t s_next = 0;
			double s_waitUntil = 0.0;
			bool s_active = false;

			double Now() { return gEnv != nullptr && gEnv->pTimer != nullptr ? gEnv->pTimer->GetAsyncCurTime() : 0.0; }

			std::string Trim(const std::string& s)
			{
				const size_t a = s.find_first_not_of(" \t\r\n");
				if (a == std::string::npos)
					return std::string();
				const size_t b = s.find_last_not_of(" \t\r\n");
				return s.substr(a, b - a + 1);
			}

			bool ReadAll(const char* path, std::string& text)
			{
				std::ifstream file(path, std::ios::binary); // absolute / working directory path
				if (file)
				{
					std::stringstream buffer;
					buffer << file.rdbuf();
					text = buffer.str();
					return true;
				}
				if (gEnv == nullptr || gEnv->pCryPak == nullptr)
					return false;
				FILE* const pFile = gEnv->pCryPak->FOpen(path, "rb"); // game folder path
				if (pFile == nullptr)
					return false;
				const size_t size = gEnv->pCryPak->FGetSize(pFile);
				text.resize(size);
				const size_t read = size != 0 ? gEnv->pCryPak->FReadRaw(&text[0], 1, size, pFile) : 0;
				gEnv->pCryPak->FClose(pFile);
				text.resize(read);
				return true;
			}
		}

		void Start(const char* path)
		{
			std::string text;
			if (path == nullptr || !ReadAll(path, text))
			{
				LogWarning("cev_run_script: cannot read <%s>", path != nullptr ? path : "");
				return;
			}
			s_lines.clear();
			std::stringstream stream(text);
			std::string line;
			while (std::getline(stream, line))
				s_lines.push_back(Trim(line));
			s_next = 0;
			s_waitUntil = 0.0;
			s_active = true;
			LogAlways("cev_run_script: <%s>, %u lines", path, static_cast<unsigned>(s_lines.size()));
		}

		void Tick()
		{
			if (!s_active || gEnv == nullptr || gEnv->pConsole == nullptr)
				return;
			if (Now() < s_waitUntil)
				return;
			while (s_next < s_lines.size())
			{
				const std::string line = s_lines[s_next++];
				if (line.empty() || line[0] == '#' || line.compare(0, 2, "//") == 0)
					continue;
				if (line.compare(0, 5, "wait ") == 0)
				{
					const double seconds = std::atof(line.c_str() + 5);
					s_waitUntil = Now() + seconds;
					LogAlways("cev_run_script: wait %.2f s", seconds);
					return;
				}
				LogAlways("cev_run_script: > %s", line.c_str());
				gEnv->pConsole->ExecuteString(line.c_str(), false, false); // immediately, not through the deferred queue
				return; // one command per frame
			}
			s_active = false;
			LogAlways("cev_run_script: done");
		}

		void Shutdown()
		{
			s_active = false;
			s_lines.clear();
		}
	}

#if CEV_HAVE_LOADTIME_CALLBACKS
	// ------------------------------------------------------------------------------------------------
	// M10a: probe of the renderer's loading loop (engine patch 17, IRenderer::AddLoadtimeCallback). Measurement only:
	// it never goes through a player except reading the P6 surface player's clock. All logging is LogAlways with the
	// prefix "cev_probe_loadtime".
	namespace LoadtimeProbe
	{
		namespace
		{
			using Clock = std::chrono::steady_clock;
			constexpr int kTexSize = 256;
			constexpr int kOpenAtIteration = 30;           // P5
			constexpr size_t kReadBytes = 64 * 1024;       // P5
			constexpr size_t kHeaderBytes = 4096;          // P8
			constexpr float kNewLoopGap = 0.25f;           // seconds without a call: the next call starts a new loop

			bool s_on = false;
			int s_texId = 0;
			std::vector<uint32> s_texBuf;
			int s_stallMs = 0;
			int s_drawMode = 1;             // 0 none, 1 LoadtimeRender, 2 LoadtimeUpdate
			std::string s_openPath = "Videos/vp9_odd_321x181.webm";
			std::string s_p6Surface;
			std::string s_readbackDir;      // P4: where the read-back frames go (empty: no file, only the counts)
			CVideoPlayerImpl* s_p6Player = nullptr;
			uint64_t s_p6LastSerial = 0;
			// M10b sub-modes (argument 6): "open:<surface>:<path>" sends a contract `open` (autoplay, loop) at
			// LEVEL_LOAD_START_LOADINGSCREEN through the exported CryTexSource_v1 table (scenario C);
			// "destroy:<surface>" destroys that surface player at LEVEL_LOAD_START like `cev_play_surface <id>` (scenario G).
			std::string s_openSurface;
			std::string s_openSrc;
			bool s_opened = false;
			std::string s_destroySurface;

			std::mutex s_mutex; // guards the loop statistics below (render thread writes, main thread reads)
			int s_loopIndex = 0;
			uint32 s_iterations = 0;
			uint32 s_renderCalls = 0;
			uint32 s_updateFails = 0;
			uint32 s_drawCalls = 0;
			std::vector<float> s_dts;
			Clock::time_point s_lastCall;
			bool s_haveLastCall = false;
			bool s_loopOnMainThread = false;
			int s_lastTexState = -1;        // P3: 1 valid, 0 invalid, -1 unknown
			std::atomic<bool> s_p8Pending{ false };
			std::atomic<int> s_watch{ 0 };  // watchdog generation: non-zero while a file operation runs

			int64 WallMs()
			{
				SYSTEMTIME st;
				::GetLocalTime(&st);
				return ((st.wHour * 60 + st.wMinute) * 60 + st.wSecond) * 1000LL + st.wMilliseconds;
			}

			void PLog(const char* fmt, ...)
			{
				char buf[1024];
				va_list args;
				va_start(args, fmt);
				vsnprintf(buf, sizeof(buf), fmt, args);
				va_end(args);
				const int64 ms = WallMs();
				LogAlways("cev_probe_loadtime [%02d:%02d:%02d.%03d %s] %s", static_cast<int>(ms / 3600000), static_cast<int>(ms / 60000 % 60),
					static_cast<int>(ms / 1000 % 60), static_cast<int>(ms % 1000), CryGetCurrentThreadId() == gEnv->mMainThreadId ? "main" : "other", buf);
			}

			//! M10b: the contract table of this module, found the way a consumer finds it.
			const CryTexSourceApi* ContractApi()
			{
				HMODULE self = nullptr;
				::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&ContractApi), &self);
				using PFN_Source = const CryTexSourceApi* (*)(void);
				const PFN_Source fn = self != nullptr ? reinterpret_cast<PFN_Source>(::GetProcAddress(self, "CryTexSource_v1")) : nullptr;
				return fn != nullptr ? fn() : nullptr;
			}

			//! M10b: the contract snapshot of a surface (query through the exported table).
			void LogSnapshot(const char* where, const std::string& name)
			{
				const CryTexSourceApi* const api = name.empty() ? nullptr : ContractApi();
				if (api == nullptr)
					return;
				CryTexSourceInfo info = {};
				info.size = sizeof(info);
				const int r = api->query(name.c_str(), &info);
				PLog("snapshot %s <%s>: query %d state=%u serial=%llu t=%.3f/%.3f loops=%u tex=%d gen=%u", where, name.c_str(), r, info.state,
					static_cast<unsigned long long>(info.contentSerial), info.timeSeconds, info.durationSeconds, info.loopCount, info.textureId, info.generation);
			}

			// P3: describes our texture id now; returns 1 valid, 0 not.
			int TexState(char* out, size_t outSize)
			{
				ITexture* const pTex = s_texId > 0 ? gEnv->pRenderer->EF_GetTextureByID(s_texId) : nullptr;
				if (pTex == nullptr)
				{
					cry_sprintf(out, outSize, "id %d: no texture object", s_texId);
					return 0;
				}
				const bool ok = pTex->IsTextureLoaded() && pTex->GetWidth() == kTexSize && pTex->GetHeight() == kTexSize && pTex->GetDeviceDataSize() > 0;
				cry_sprintf(out, outSize, "id %d: name <%s> %dx%d loaded %d deviceBytes %u", s_texId, pTex->GetName(), pTex->GetWidth(), pTex->GetHeight(),
					pTex->IsTextureLoaded() ? 1 : 0, pTex->GetDeviceDataSize());
				return ok ? 1 : 0;
			}

			void FillTexture(uint32 iteration)
			{
				// magenta with a moving white bar (screenshots look for the magenta)
				const int bar = static_cast<int>((iteration * 4u) % kTexSize);
				for (int y = 0; y < kTexSize; ++y)
					for (int x = 0; x < kTexSize; ++x)
						s_texBuf[static_cast<size_t>(y) * kTexSize + x] = (x >= bar && x < bar + 16) ? 0xFFFFFFFFu : 0xFFFF00FFu;
			}

			void DrawTexture()
			{
				IRenderAuxImage::Draw2dImage(20.f, 20.f, 300.f, 300.f, s_texId, 0.f, 1.f, 1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f);
				++s_drawCalls;
			}

			// P4: reads the presented back buffer on the render thread (ReadFrameBuffer executes directly there), counts the
			// magenta pixels of the probe texture and writes the frame as a binary PPM.
			void ReadBack(const char* what)
			{
				const int w = gEnv->pRenderer->GetWidth();
				const int h = gEnv->pRenderer->GetHeight();
				if (w <= 0 || h <= 0)
					return;
				std::vector<uint32> px(static_cast<size_t>(w) * h, 0);
				const Clock::time_point t0 = Clock::now();
				const bool ok = gEnv->pRenderer->ReadFrameBuffer(px.data(), w, h, true, EReadTextureFormat::RGB8);
				const float ms = std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
				uint32 magenta = 0, lit = 0;
				for (uint32 c : px)
				{
					const uint32 r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
					magenta += (r > 200 && g < 70 && b > 200) ? 1u : 0u;
					lit += (r + g + b > 30) ? 1u : 0u;
				}
				std::string file;
				if (ok && !s_readbackDir.empty())
				{
					file = s_readbackDir + "/" + what + "_loop" + std::to_string(s_loopIndex) + ".ppm";
					std::ofstream out(file, std::ios::binary);
					out << "P6\n" << w << " " << h << "\n255\n";
					for (uint32 c : px)
					{
						const char rgb[3] = { static_cast<char>(c & 0xFF), static_cast<char>((c >> 8) & 0xFF), static_cast<char>((c >> 16) & 0xFF) };
						out.write(rgb, 3);
					}
				}
				PLog("P4 %s loop %d iteration %u: ReadFrameBuffer(presented, %dx%d) %s in %.2f ms, magenta pixels %u (texture drawn at 300x300 virtual units), lit pixels %u, file <%s>",
					what, s_loopIndex, s_iterations, w, h, ok ? "ok" : "FAILED", ms, magenta, lit, file.c_str());
			}

			// P5 / P8: CryPak open + read on the calling thread, watched by a 2 s watchdog thread.
			void TimedRead(const char* what, size_t bytes)
			{
				const int gen = s_watch.fetch_add(1) + 1;
				std::thread([gen, what]()
				{
					std::this_thread::sleep_for(std::chrono::seconds(2));
					if (s_watch.load() == gen)
						PLog("%s: STUCK, the open / read has not returned after 2 s (giving up waiting)", what);
				}).detach();
				const Clock::time_point t0 = Clock::now();
				FILE* const f = gEnv->pCryPak->FOpen(s_openPath.c_str(), "rb");
				const float openMs = std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
				size_t got = 0;
				if (f != nullptr)
				{
					std::vector<uint8> buf(bytes);
					got = gEnv->pCryPak->FReadRaw(buf.data(), 1, bytes, f);
					gEnv->pCryPak->FClose(f);
				}
				const float totalMs = std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
				s_watch.fetch_add(1000); // invalidates the watchdog
				PLog("%s: FOpen(%s) %s, read %u of %u bytes, open %.3f ms, total %.3f ms", what, s_openPath.c_str(), f != nullptr ? "ok" : "FAILED",
					static_cast<unsigned>(got), static_cast<unsigned>(bytes), openMs, totalMs);
			}

			void SummariseLoopLocked(const char* why)
			{
				if (s_iterations == 0)
					return;
				std::vector<float> d = s_dts;
				std::sort(d.begin(), d.end());
				const float mn = d.empty() ? 0.f : d.front();
				const float md = d.empty() ? 0.f : d[d.size() / 2];
				const float mx = d.empty() ? 0.f : d.back();
				uint32 over100 = 0;
				for (float v : d)
					over100 += v > 0.1f ? 1u : 0u;
				PLog("P2 loop %d summary (%s): iterations %u, LoadtimeRender calls %u, dt ms min %.2f median %.2f max %.2f, dt>100ms %u, update failures %u, draw calls %u, loop thread %s",
					s_loopIndex, why, s_iterations, s_renderCalls, mn * 1000.f, md * 1000.f, mx * 1000.f, over100, s_updateFails, s_drawCalls,
					s_loopOnMainThread ? "MAIN" : "not main");
				s_iterations = 0;
				s_renderCalls = 0;
				s_updateFails = 0;
				s_drawCalls = 0;
				s_dts.clear();
			}

			void LogP6(const char* where)
			{
				if (s_p6Player == nullptr)
					return;
				const uint64_t serial = s_p6Player->GetFrameSerial();
				PLog("P6 %s: t=%.3f state=%d serial=%llu (+%llu) audioFramesConsumed=%llu audioFramesCounted=%llu clock=%s", where, s_p6Player->GetTime(), static_cast<int>(s_p6Player->GetState()),
					static_cast<unsigned long long>(serial), static_cast<unsigned long long>(serial - s_p6LastSerial),
					static_cast<unsigned long long>(s_p6Player->ProbeAudioFramesConsumed()),
					static_cast<unsigned long long>(s_p6Player->ProbeAudioFramesCounted()), s_p6Player->ClockSourceName());
				s_p6LastSerial = serial;
			}

			// M10b: listener events of the P6 player (scenario B: Ended once, dispatched on the main thread after the load)
			struct SEventLog final : public IVideoPlayerListener
			{
				void OnEnded(IVideoPlayer& p) override { PLog("event Ended (player time %.3f)", p.GetTime()); }
				void OnLooped(IVideoPlayer& p) override { PLog("event Looped (player time %.3f)", p.GetTime()); }
				void OnError(IVideoPlayer&, int code, const char* message) override { PLog("event Error %d (%s)", code, message != nullptr ? message : ""); }
			};
			SEventLog s_eventLog;
			IVideoPlayer* s_eventPlayer = nullptr;

			// P7: a second callback added and removed from the main thread while the loop runs.
			struct SDummy final : public ILoadtimeCallback
			{
				std::atomic<uint32> calls{ 0 };
				std::atomic<bool> removed{ false };
				std::atomic<uint32> callsAfterRemove{ 0 };
				void LoadtimeUpdate(float) override
				{
					++calls;
					if (removed.load())
						++callsAfterRemove;
				}
				bool LoadtimeRender() override { return true; }
			};
			SDummy s_dummy;
			bool s_p7Done = false;

			struct SCallback final : public ILoadtimeCallback
			{
				void LoadtimeUpdate(float dt) override
				{
					std::lock_guard<std::mutex> lock(s_mutex);
					const Clock::time_point now = Clock::now();
					if (!s_haveLastCall || std::chrono::duration<float>(now - s_lastCall).count() > kNewLoopGap)
					{
						SummariseLoopLocked("gap");
						++s_loopIndex;
						s_loopOnMainThread = CryGetCurrentThreadId() == gEnv->mMainThreadId;
						PLog("loop %d: first LoadtimeUpdate, dt %.2f ms", s_loopIndex, dt * 1000.f);
					}
					s_lastCall = now;
					s_haveLastCall = true;
					++s_iterations;
					s_dts.push_back(dt);

					char desc[256];
					const int state = TexState(desc, sizeof(desc));
					if (state != s_lastTexState)
					{
						PLog("P3 loop %d iteration %u: texture state %d -> %d (%s)", s_loopIndex, s_iterations, s_lastTexState, state, desc);
						s_lastTexState = state;
					}
					if (state == 1)
					{
						FillTexture(s_iterations);
						gEnv->pRenderer->UpdateTextureInVideoMemory(static_cast<uint32>(s_texId), reinterpret_cast<unsigned char*>(s_texBuf.data()), 0, 0, kTexSize, kTexSize, eTF_R8G8B8A8);
					}
					else
						++s_updateFails;

					if (s_drawMode == 2 && state == 1)
						DrawTexture();

					if (s_p8Pending.exchange(false))
						TimedRead("P8 first iteration after LOADINGSCREEN", kHeaderBytes);
					if (s_iterations == kOpenAtIteration)
						TimedRead("P5 iteration 30", kReadBytes);
					if (s_iterations == 60)
						ReadBack(s_drawMode == 1 ? "render" : (s_drawMode == 2 ? "update" : "nodraw"));

					if (s_p6Player != nullptr && (s_iterations % 10) == 1)
					{
						char where[64];
						cry_sprintf(where, "loop %d iteration %u", s_loopIndex, s_iterations);
						LogP6(where);
					}
					if ((s_iterations % 10) == 1)
					{
						char where[64];
						cry_sprintf(where, "loop %d iteration %u", s_loopIndex, s_iterations);
						LogSnapshot(where, s_p6Surface);
						LogSnapshot(where, s_openSurface);
					}
				}

				bool LoadtimeRender() override
				{
					std::lock_guard<std::mutex> lock(s_mutex);
					++s_renderCalls;
					if (s_drawMode == 1 && s_lastTexState == 1)
						DrawTexture();
					return true;
				}
			};
			SCallback s_callback;

			struct SListener final : public ISystemEventListener
			{
				void OnSystemEvent(ESystemEvent event, UINT_PTR, UINT_PTR) override
				{
					const char* name = nullptr;
					switch (event)
					{
					case ESYSTEM_EVENT_LEVEL_LOAD_PREPARE: name = "LEVEL_LOAD_PREPARE"; break;
					case ESYSTEM_EVENT_LEVEL_LOAD_START_PRELOADINGSCREEN: name = "LEVEL_LOAD_START_PRELOADINGSCREEN"; break;
					case ESYSTEM_EVENT_LEVEL_LOAD_START_LOADINGSCREEN: name = "LEVEL_LOAD_START_LOADINGSCREEN"; break;
					case ESYSTEM_EVENT_LEVEL_LOAD_LOADINGSCREEN_ACTIVE: name = "LEVEL_LOAD_LOADINGSCREEN_ACTIVE"; break;
					case ESYSTEM_EVENT_LEVEL_LOAD_START: name = "LEVEL_LOAD_START"; break;
					case ESYSTEM_EVENT_LEVEL_LOAD_ENTITIES: name = "LEVEL_LOAD_ENTITIES"; break;
					case ESYSTEM_EVENT_LEVEL_LOAD_END: name = "LEVEL_LOAD_END"; break;
					case ESYSTEM_EVENT_LEVEL_LOAD_ERROR: name = "LEVEL_LOAD_ERROR"; break;
					case ESYSTEM_EVENT_LEVEL_PRECACHE_START: name = "LEVEL_PRECACHE_START"; break;
					case ESYSTEM_EVENT_LEVEL_PRECACHE_FIRST_FRAME: name = "LEVEL_PRECACHE_FIRST_FRAME"; break;
					case ESYSTEM_EVENT_LEVEL_PRECACHE_END: name = "LEVEL_PRECACHE_END"; break;
					case ESYSTEM_EVENT_LEVEL_GAMEPLAY_START: name = "LEVEL_GAMEPLAY_START"; break;
					case ESYSTEM_EVENT_LEVEL_UNLOAD_START: name = "LEVEL_UNLOAD_START"; break;
					case ESYSTEM_EVENT_LEVEL_UNLOAD: name = "LEVEL_UNLOAD"; break;
					case ESYSTEM_EVENT_LEVEL_POST_UNLOAD: name = "LEVEL_POST_UNLOAD"; break;
					default: return;
					}
					float sinceCall = -1.f;
					{
						std::lock_guard<std::mutex> lock(s_mutex);
						if (s_haveLastCall)
							sinceCall = std::chrono::duration<float>(Clock::now() - s_lastCall).count();
					}
					char desc[256];
					TexState(desc, sizeof(desc));
					PLog("event %s: last callback %.1f ms ago, texture %s", name, sinceCall * 1000.f, desc);

					if (event == ESYSTEM_EVENT_LEVEL_LOAD_START_LOADINGSCREEN)
					{
						s_p8Pending = true;
						if (!s_openSurface.empty())
						{
							if (const CryTexSourceApi* const api = ContractApi())
							{
								CryTexSourceOpen options = {};
								options.size = sizeof(options);
								options.flags = 1u | 2u; // autoplay, loop
								options.volume = 1.f;
								const int r = api->open(s_openSurface.c_str(), s_openSrc.c_str(), &options);
								s_opened = s_opened || r == 1;
								PLog("C: contract open(<%s>, <%s>, autoplay loop) at LOADINGSCREEN returned %d, HasPendingCommands %d", s_openSurface.c_str(), s_openSrc.c_str(), r,
									Contract::HasPendingCommands() ? 1 : 0);
							}
							LogSnapshot("at LOADINGSCREEN", s_openSurface);
						}
						if (!s_p6Surface.empty() && mEnv->pManager != nullptr)
						{
							s_p6Player = static_cast<CVideoPlayerImpl*>(mEnv->pManager->FindPlayerBySurface(s_p6Surface.c_str()));
							if (s_p6Player != nullptr)
							{
								s_p6LastSerial = s_p6Player->GetFrameSerial();
								if (s_eventPlayer != s_p6Player)
								{
									s_p6Player->AddListener(&s_eventLog);
									s_eventPlayer = s_p6Player;
								}
							}
							LogP6("at LOADINGSCREEN");
						}
					}
					if (event == ESYSTEM_EVENT_LEVEL_LOAD_START)
					{
						if (!s_destroySurface.empty() && mEnv->pManager != nullptr)
						{
							PLog("G: destroying surface <%s> on the main thread at LEVEL_LOAD_START (cev_play_surface <id>)", s_destroySurface.c_str());
							if (s_p6Player != nullptr && stricmp(s_p6Surface.c_str(), s_destroySurface.c_str()) == 0)
								s_p6Player = nullptr;
							mEnv->pManager->ConsolePlaySurface(s_destroySurface.c_str(), nullptr, false);
							PLog("G: destroyed; players %u, loading-loop callback registered %d", mEnv->pManager->GetPlayerCount(), mEnv->pManager->LoadtimeRegistered() ? 1 : 0);
						}
						if (!s_p7Done)
						{
							s_p7Done = true;
							const Clock::time_point t0 = Clock::now();
							gEnv->pRenderer->AddLoadtimeCallback(&s_dummy, 150);
							const float addMs = std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
							CrySleep(200);
							const uint32 before = s_dummy.calls.load();
							const Clock::time_point t1 = Clock::now();
							gEnv->pRenderer->RemoveLoadtimeCallback(&s_dummy);
							const float removeMs = std::chrono::duration<float, std::milli>(Clock::now() - t1).count();
							s_dummy.removed = true;
							PLog("P7: AddLoadtimeCallback(dummy, 150) returned in %.3f ms; dummy called %u times in 200 ms; RemoveLoadtimeCallback returned in %.3f ms (calls at remove %u)",
								addMs, before, removeMs, s_dummy.calls.load());
						}
						if (s_stallMs > 0)
						{
							PLog("stall: main thread sleeps %d ms (simulated long load)", s_stallMs);
							CrySleep(static_cast<unsigned>(s_stallMs));
							PLog("stall: done");
						}
					}
					if (event == ESYSTEM_EVENT_LEVEL_LOAD_END || event == ESYSTEM_EVENT_LEVEL_GAMEPLAY_START || event == ESYSTEM_EVENT_LEVEL_POST_UNLOAD)
					{
						if (s_p7Done)
							PLog("P7 check: dummy calls %u, calls after remove %u", s_dummy.calls.load(), s_dummy.callsAfterRemove.load());
						LogP6(name);
						LogSnapshot(name, s_p6Surface);
						LogSnapshot(name, s_openSurface);
					}
				}
			};
			SListener s_listener;

			void Off()
			{
				if (!s_on)
					return;
				gEnv->pRenderer->RemoveLoadtimeCallback(&s_callback);
				gEnv->pSystem->GetISystemEventDispatcher()->RemoveListener(&s_listener);
				{
					std::lock_guard<std::mutex> lock(s_mutex);
					SummariseLoopLocked("off");
				}
				if (s_texId > 0)
					gEnv->pRenderer->RemoveTexture(static_cast<unsigned int>(s_texId));
				s_texId = 0;
				s_p6Player = nullptr;
				if (s_eventPlayer != nullptr && mEnv->pManager != nullptr && mEnv->pManager->IsPlayer(s_eventPlayer))
					s_eventPlayer->RemoveListener(&s_eventLog);
				s_eventPlayer = nullptr;
				if (s_opened && !s_openSurface.empty())
				{
					if (const CryTexSourceApi* const api = ContractApi())
						api->close(s_openSurface.c_str());
					PLog("C: contract close(<%s>)", s_openSurface.c_str());
				}
				s_opened = false;
				s_openSurface.clear();
				s_destroySurface.clear();
				s_on = false;
				PLog("off: callback removed, texture released");
			}
		}

		void Cmd(IConsoleCmdArgs* args)
		{
			if (gEnv == nullptr || gEnv->pRenderer == nullptr || gEnv->IsDedicated())
				return;
			// P4 positive control in normal frames (main thread): "draw" submits the probe texture into the current frame,
			// "readback" reads the presented frame the same way the loop does.
			if (args != nullptr && args->GetArgCount() >= 2 && s_on && s_texId > 0)
			{
				if (std::strcmp(args->GetArg(1), "draw") == 0)
				{
					DrawTexture();
					return;
				}
				if (std::strcmp(args->GetArg(1), "readback") == 0)
				{
					ReadBack("control");
					return;
				}
			}
			const bool available = RenderBridge::LoadtimeAvailable();
			PLog("P1: load-time callbacks (XUI2D_Patched bit 7) %s", available ? "SET" : "not set");
			if (args == nullptr || args->GetArgCount() < 2 || std::atoi(args->GetArg(1)) == 0)
			{
				Off();
				return;
			}
			if (!available)
			{
				PLog("unavailable: the renderer does not report load-time callbacks");
				return;
			}
			if (s_on)
				Off();
			s_stallMs = args->GetArgCount() > 2 ? std::atoi(args->GetArg(2)) : 0;
			s_drawMode = args->GetArgCount() > 3 ? std::atoi(args->GetArg(3)) : 1;
			s_p6Surface = args->GetArgCount() > 4 && std::strcmp(args->GetArg(4), "-") != 0 ? args->GetArg(4) : "";
			s_readbackDir = args->GetArgCount() > 5 && std::strcmp(args->GetArg(5), "-") != 0 ? args->GetArg(5) : "";
			if (args->GetArgCount() > 6)
			{
				const std::string mode = args->GetArg(6);
				if (mode.compare(0, 5, "open:") == 0 && mode.find(':', 5) != std::string::npos)
				{
					const size_t colon = mode.find(':', 5);
					s_openSurface = mode.substr(5, colon - 5);
					s_openSrc = mode.substr(colon + 1);
				}
				else if (mode.compare(0, 8, "destroy:") == 0)
					s_destroySurface = mode.substr(8);
			}
			s_texBuf.assign(static_cast<size_t>(kTexSize) * kTexSize, 0xFFFF00FFu);
			s_texId = static_cast<int>(gEnv->pRenderer->UploadToVideoMemory(reinterpret_cast<unsigned char*>(s_texBuf.data()), kTexSize, kTexSize, eTF_R8G8B8A8, eTF_R8G8B8A8, 0,
				false, FILTER_BILINEAR, 0, "$CevProbeLoadtime", FT_NOMIPS));
			{
				std::lock_guard<std::mutex> lock(s_mutex);
				s_loopIndex = 0;
				s_iterations = 0;
				s_dts.clear();
				s_haveLastCall = false;
				s_lastTexState = -1;
			}
			s_p7Done = false;
			gEnv->pSystem->GetISystemEventDispatcher()->RegisterListener(&s_listener, "CEVPlayerLoadtimeProbe");
			gEnv->pRenderer->AddLoadtimeCallback(&s_callback, 100);
			s_on = true;
			char desc[256];
			TexState(desc, sizeof(desc));
			PLog("on: callback registered (order 100), stall %d ms, draw mode %d, P6 surface <%s>, open <%s> <%s>, destroy <%s>, texture %s", s_stallMs, s_drawMode, s_p6Surface.c_str(),
				s_openSurface.c_str(), s_openSrc.c_str(), s_destroySurface.c_str(), desc);
		}

		void Shutdown()
		{
			if (gEnv != nullptr && gEnv->pRenderer != nullptr)
				Off();
		}
	}
#endif

	void TickRunScript() { RunScript::Tick(); }
	void ShutdownRunScript() { RunScript::Shutdown(); }

	void DevProbesBeforeRender() { DevProbe::BeforeRender(); }
	void DevProbesAfterRender() { DevProbe::AfterRender(); }
	void DevProbesShutdown()
	{
		DevProbe::Shutdown();
#if CEV_HAVE_LOADTIME_CALLBACKS
		LoadtimeProbe::Shutdown(); // removes the probe's loading-loop callback (main thread) before the renderer goes
#endif
	}

	namespace
	{
		bool s_registered = false;

		// ---- cev_* ----
		void CmdPlay(IConsoleCmdArgs* args)
		{
			if (args == nullptr || mEnv->pManager == nullptr)
				return;
			if (args->GetArgCount() < 2)
			{
				LogAlways("usage: cev_play <path> [loop]   e.g. cev_play Videos/intro.webm 1");
				return;
			}
			bool loop = false;
			if (args->GetArgCount() > 2)
			{
				const char* const arg = args->GetArg(2);
				loop = std::strcmp(arg, "1") == 0 || stricmp(arg, "true") == 0 || stricmp(arg, "loop") == 0;
			}
			mEnv->pManager->ConsolePlay(args->GetArg(1), loop);
		}

		void CmdStop(IConsoleCmdArgs*)
		{
			if (mEnv->pManager != nullptr)
				mEnv->pManager->ConsoleStop();
		}

		//! Resolves a console target: a decimal number is a script player handle, anything else a surface id.
		//! Returns nullptr when no such player exists; isHandle tells which kind the target was read as.
		IVideoPlayer* FindTargetPlayer(const char* target, bool& isHandle)
		{
			char* end = nullptr;
			const long handle = std::strtol(target, &end, 10);
			isHandle = end != target && *end == '\0';
			return isHandle
				? (mEnv->pApi != nullptr ? mEnv->pApi->Find(static_cast<int>(handle), false) : nullptr)
				: mEnv->pManager->FindPlayerBySurface(target);
		}

		//! Pause / resume of one targeted player; logs the result or "no player".
		void PauseResumeTarget(const char* cmd, const char* target, bool pause)
		{
			bool isHandle = false;
			IVideoPlayer* const pPlayer = FindTargetPlayer(target, isHandle);
			if (pPlayer == nullptr)
			{
				LogAlways("%s: no player '%s'", cmd, target);
				return;
			}
			if (pause)
				pPlayer->Pause();
			else
				pPlayer->Resume();
			LogAlways("%s: %s '%s' %s", cmd, isHandle ? "handle" : "surface", target, pause ? "paused" : "resumed");
		}

		//! cev_pause [<handle|surface>]: without a target the cev_play player.
		void CmdPause(IConsoleCmdArgs* args)
		{
			if (mEnv->pManager == nullptr)
				return;
			if (args != nullptr && args->GetArgCount() > 1)
				PauseResumeTarget("cev_pause", args->GetArg(1), true);
			else
				mEnv->pManager->ConsolePause();
		}

		//! cev_resume [<handle|surface>]: without a target the cev_play player.
		void CmdResume(IConsoleCmdArgs* args)
		{
			if (mEnv->pManager == nullptr)
				return;
			if (args != nullptr && args->GetArgCount() > 1)
				PauseResumeTarget("cev_resume", args->GetArg(1), false);
			else
				mEnv->pManager->ConsoleResume();
		}

		//! cev_seek <seconds> seeks the cev_play player; cev_seek <handle|surface> <seconds> the given player.
		void CmdSeek(IConsoleCmdArgs* args)
		{
			if (args == nullptr || mEnv->pManager == nullptr)
				return;
			if (args->GetArgCount() < 2)
			{
				LogAlways("usage: cev_seek [<handle|surface>] <seconds>");
				return;
			}
			if (args->GetArgCount() < 3)
			{
				mEnv->pManager->ConsoleSeek(std::atof(args->GetArg(1)));
				return;
			}
			const char* const target = args->GetArg(1);
			bool isHandle = false;
			IVideoPlayer* const pPlayer = FindTargetPlayer(target, isHandle);
			if (pPlayer == nullptr)
			{
				LogAlways("cev_seek: no player '%s'", target);
				return;
			}
			const double seconds = std::atof(args->GetArg(2));
			pPlayer->Seek(seconds);
			LogAlways("cev_seek: %s '%s' seeked to %.3f s", isHandle ? "handle" : "surface", target, seconds);
		}

		//! cev_audiotrack <handle|surface> [name]: a target that is a decimal number is a script player handle,
		//! anything else a surface id. Without a name: reports the track in use and the listed ones.
		void CmdAudioTrack(IConsoleCmdArgs* args)
		{
			if (args == nullptr || mEnv->pManager == nullptr)
				return;
			if (args->GetArgCount() < 2)
			{
				LogAlways("usage: cev_audiotrack <handle|surface> [name]");
				return;
			}
			const char* const target = args->GetArg(1);
			bool isHandle = false;
			IVideoPlayer* const pPlayer = FindTargetPlayer(target, isHandle);
			if (pPlayer == nullptr)
			{
				LogAlways("cev_audiotrack: no player with %s <%s>", isHandle ? "handle" : "surface id", target);
				return;
			}
			const CVideoPlayerImpl* const pImpl = static_cast<const CVideoPlayerImpl*>(pPlayer); // every player is one
			if (args->GetArgCount() < 3)
			{
				LogAlways("cev_audiotrack: <%s> plays audio track <%s> of %d", target, pPlayer->GetAudioTrack(), pImpl->GetAudioTrackCount());
				for (int i = 0; i < pImpl->GetAudioTrackCount(); ++i)
					LogAlways("cev_audiotrack:   %d <%s> language <%s>", i, pImpl->GetAudioTrackName(i), pImpl->GetAudioTrackLanguage(i));
				return;
			}
			LogAlways("cev_audiotrack: <%s> -> <%s> (was <%s>)", target, args->GetArg(2), pPlayer->GetAudioTrack());
			pPlayer->SetAudioTrack(args->GetArg(2));
		}

		void CmdApiTest(IConsoleCmdArgs* args)
		{
			Api::StartApiTest(args != nullptr && args->GetArgCount() > 1 ? args->GetArg(1) : nullptr);
		}

		void CmdPlaySurface(IConsoleCmdArgs* args)
		{
			if (args == nullptr || mEnv->pManager == nullptr)
				return;
			if (args->GetArgCount() < 2)
			{
				LogAlways("usage: cev_play_surface <id> <path> [loop]   (without a path: destroy that surface player)");
				return;
			}
			bool loop = false;
			if (args->GetArgCount() > 3)
			{
				const char* const arg = args->GetArg(3);
				loop = std::strcmp(arg, "1") == 0 || stricmp(arg, "true") == 0 || stricmp(arg, "loop") == 0;
			}
			mEnv->pManager->ConsolePlaySurface(args->GetArg(1), args->GetArgCount() > 2 ? args->GetArg(2) : nullptr, loop);
		}

		//! M10: one line on the loading-loop state (cev_info, cev_surfaces).
		void LogLoadtimeState(const char* cmd)
		{
			LogAlways("%s: loadtime: available=%d registered=%d cev_loadtime=%d", cmd, RenderBridge::LoadtimeAvailable() ? 1 : 0,
				mEnv->pManager != nullptr && mEnv->pManager->LoadtimeRegistered() ? 1 : 0, mEnv->m_cevLoadtime);
		}

		void CmdSurfaces(IConsoleCmdArgs*)
		{
			Contract::LogSurfaces();
			LogLoadtimeState("cev_surfaces");
		}

		void CmdContractSelfTest(IConsoleCmdArgs* args)
		{
			Contract::StartSelfTest(args != nullptr && args->GetArgCount() > 1 ? args->GetArg(1) : nullptr,
				args != nullptr && args->GetArgCount() > 2 ? args->GetArg(2) : nullptr);
		}

		void CmdHiddenBench(IConsoleCmdArgs* args)
		{
			if (args == nullptr || args->GetArgCount() < 2)
			{
				LogAlways("usage: cev_hidden_bench <path> [cycles=10] [hidden seconds=4] [visible seconds=2]");
				return;
			}
			Contract::StartHiddenBench(args->GetArg(1), args->GetArgCount() > 2 ? std::atoi(args->GetArg(2)) : 10,
				args->GetArgCount() > 3 ? static_cast<float>(std::atof(args->GetArg(3))) : 4.f, args->GetArgCount() > 4 ? static_cast<float>(std::atof(args->GetArg(4))) : 2.f);
		}

		void CmdList(IConsoleCmdArgs*)
		{
			if (mEnv->pManager != nullptr)
				mEnv->pManager->ListPlayers();
		}

		void CmdInfo(IConsoleCmdArgs*)
		{
			// developer information that used to be the table queries InterfaceVersion / Caps (retired in M6b)
			LogAlways("cev_info: player interface version %u, caps 0x%x (1 Alpha, 2 Premultiplied, 4 MultiResolution, 8 Variants), texture source contract version %u, registered surfaces %u, players %u",
				kVideoPlayerInterfaceVersion, kPluginCaps, Contract::ApiVersion(), Contract::SurfaceCount(), mEnv->pManager != nullptr ? mEnv->pManager->GetPlayerCount() : 0u);
			LogAlways("cev_info: API table %u controls, %u queries, %u events; %u FlowGraph node classes",
				static_cast<unsigned>(Api::Controls().size()), static_cast<unsigned>(Api::Queries().size()), static_cast<unsigned>(Api::Events().size()), Api::FlowNodeCount());
			LogLoadtimeState("cev_info");
		}

		void CmdFlowTest(IConsoleCmdArgs* args)
		{
			Api::StartFlowTest(args != nullptr && args->GetArgCount() > 1 ? args->GetArg(1) : nullptr,
				args != nullptr && args->GetArgCount() > 2 ? args->GetArg(2) : nullptr);
		}

		void CmdRunScript(IConsoleCmdArgs* args)
		{
			if (args == nullptr || args->GetArgCount() < 2)
			{
				LogAlways("usage: cev_run_script <file>   (one console line per frame; wait <seconds>; # comments)");
				return;
			}
			RunScript::Start(args->GetArg(1));
		}
	}

	void RegisterConsole()
	{
		IConsole* const pConsole = gEnv != nullptr ? gEnv->pConsole : nullptr;
		if (pConsole == nullptr || s_registered)
			return;
		s_registered = true;

		REGISTER_COMMAND("cev_play", CmdPlay, VF_NULL, "Play a video fullscreen. cev_play <path> [loop], e.g. cev_play Videos/intro.webm 1 (.webm or .cevvideo)");
		REGISTER_COMMAND("cev_stop", CmdStop, VF_NULL, "Stop and remove the video started by cev_play");
		REGISTER_COMMAND("cev_pause", CmdPause, VF_NULL, "Pause a video. cev_pause [<handle|surface>] (handle = script player handle, otherwise a surface id; without a target: the cev_play player)");
		REGISTER_COMMAND("cev_resume", CmdResume, VF_NULL, "Resume a video. cev_resume [<handle|surface>] (handle = script player handle, otherwise a surface id; without a target: the cev_play player)");
		REGISTER_COMMAND("cev_seek", CmdSeek, VF_NULL, "Seek a video. cev_seek [<handle|surface>] <seconds> (handle = script player handle, otherwise a surface id; without a target: the cev_play player)");
		REGISTER_COMMAND("cev_audiotrack", CmdAudioTrack, VF_NULL, "Select the audio track of a player by its .cevvideo track name. cev_audiotrack <handle|surface> [name] (handle = script player handle, otherwise a surface id; without a name: show the current and the listed tracks)");
		REGISTER_COMMAND("cev_apitest", CmdApiTest, VF_NULL, "Self test of the script API table: creates a player through the table functions, plays [path] (default Videos/vp9_alpha.webm), checks state / time after 2 s, stops and destroys it, logs PASS / FAIL");
		REGISTER_COMMAND("cev_list", CmdList, VF_NULL, "List all CEVPlayer players with state, time and upload count");
		REGISTER_COMMAND("cev_play_surface", CmdPlaySurface, VF_NULL, "Play a video into a surface player (texture $cev:<id>). cev_play_surface <id> <path> [loop]; without a path the surface player is destroyed");
		REGISTER_COMMAND("cev_surfaces", CmdSurfaces, VF_NULL, "List the surfaces of the texture source contract: snapshot and aggregated consumer hints");
		REGISTER_COMMAND("cev_info", CmdInfo, VF_NULL, "Developer information: player interface version, capability bits, texture source contract version, registered surfaces, API table size");
		REGISTER_COMMAND("cev_flowtest", CmdFlowTest, VF_NULL, "Self test of the FlowGraph nodes: builds a flow graph in memory with every node class, drives it and checks the outputs. cev_flowtest [pathA] [pathB] (pathB: another size), logs PASS / FAIL");
		REGISTER_COMMAND("cev_run_script", CmdRunScript, VF_NULL, "Development: cev_run_script <file> runs a list of console commands, one per frame, immediately (not through the deferred queue); wait <seconds>, # comments");
		REGISTER_COMMAND("cev_contract_selftest", CmdContractSelfTest, VF_NULL, "Self test of the texture source contract through the CryTexSource_v1 export. cev_contract_selftest [pathA] [pathB] (two videos of different sizes), logs PASS / FAIL");
		// development-only measurement commands (S0)
		REGISTER_COMMAND("cev_probe_r8", DevProbe::CmdProbeR8, VF_NULL, "Measurement: cev_probe_r8 <w> <h> [rgba] creates an eTF_R8 (or RGBA8) dynamic texture, updates it every frame, draws it top left and logs the process memory it costs (no arguments = off)");
		REGISTER_COMMAND("cev_probe_sync", DevProbe::CmdProbeSync, VF_NULL, "Measurement: cev_probe_sync 1|0, a 64x64 RGBA texture filled each frame with a colour coding the frame index, next to aux blocks of the same colour");
		REGISTER_COMMAND("cev_bench_play", DevProbe::CmdBenchPlay, VF_NULL, "Measurement: cev_bench_play <path> <texture name> [loop 0|1] plays into a named texture (not fullscreen); no arguments = stop");
		REGISTER_COMMAND("cev_bench_stop", DevProbe::CmdBenchStop, VF_NULL, "Measurement: stop the cev_bench_play player");
		REGISTER_COMMAND("cev_hidden_bench", CmdHiddenBench, VF_NULL, "Measurement: cev_hidden_bench <path> [cycles] [hidden s] [visible s] plays into surface CevBench and toggles a consumer hint hidden / visible, logs the catch-up of every cycle");
		REGISTER_COMMAND("cev_bench_hint", DevProbe::CmdBenchHint, VF_NULL, "Measurement (M8): cev_bench_hint <surface> <acceptLayouts> sends a visible consumer hint for the surface every frame (no arguments = release)");
		REGISTER_COMMAND("cev_show_surface", DevProbe::CmdShowSurface, VF_NULL, "Measurement (M8): cev_show_surface <surface> draws the surface's texture top left as it is (an R8 packed texture shows the planes in grey); no argument = off");
#if CEV_HAVE_LOADTIME_CALLBACKS
		REGISTER_COMMAND("cev_probe_loadtime", LoadtimeProbe::Cmd, VF_NULL, "Measurement (M10a/M10b): cev_probe_loadtime 1 [stall ms] [draw 0|1 render|2 update] [P6 surface|-] [read-back dir|-] [open:<surface>:<path>|destroy:<surface>] registers an order-100 loading-loop callback that updates and draws a 256x256 texture and logs P1-P8; cev_probe_loadtime 0 removes it");
#endif
		REGISTER_COMMAND("cev_stats_file", DevProbe::CmdStatsFile, VF_NULL, "Measurement: cev_stats_file <path>|off writes the per-second cev_stats rows as CSV (turns cev_stats on)");

		REGISTER_CVAR2("cev_stats", &mEnv->m_cevStats, 0, VF_NULL, "1 = log and draw a stats line per player every second (decode / convert ms, uploads, dropped, allocs)");
		REGISTER_CVAR2("cev_decodeThreads", &mEnv->m_cevDecodeThreads, 0, VF_NULL, "Decode worker threads of the shared pool and libvpx threads per video; 0 = automatic. Applies to new pools / players");
		REGISTER_CVAR2("cev_decodeMode", &mEnv->m_cevDecodeMode, 0, VF_NULL, "Video decoder of players without an explicit SetDecode: 0 = Auto (GPU when available and the video is at least cev_decodeAutoMinHeight tall), 1 = GPU (Media Foundation, falls back to the CPU), 2 = CPU (libvpx). Applies at the next Load / Play from stopped");
		REGISTER_CVAR2("cev_decodeAutoMinHeight", &mEnv->m_cevDecodeAutoMinHeight, 1080, VF_NULL, "Decode = Auto uses the GPU for videos at least this many pixels tall");
		REGISTER_CVAR2("cev_log", &mEnv->m_cevLog, 0, VF_NULL, "1 = verbose CEVPlayer log");
		REGISTER_CVAR2("cev_audio_enable", &mEnv->m_cevAudioEnable, 1, VF_NULL, "0 = no audio output from videos (audio is not even decoded for players created afterwards)");

		REGISTER_CVAR2("cev_muteOnFocusLoss", &mEnv->m_cevMuteOnFocusLoss, 1, VF_NULL, "1 = video audio is muted while the game window has no focus");
		REGISTER_CVAR2("cev_disableEmbeddedAudio", &mEnv->m_cevDisableEmbeddedAudio, 0, VF_NULL, "1 = videos play silently and the plug-in never initialises the SDL audio subsystem (set it in the .cryproject console_variables to cover the whole run)");
		REGISTER_CVAR2("cev_variant", &mEnv->m_cevVariant, -1, VF_NULL, "Variant of a .cevvideo descriptor for players loaded afterwards: -1 = automatic (smallest not below the screen height when fullscreen, else the main video), 0 = main video, n = n-th <Variant>");
		REGISTER_CVAR2("cev_cueForceFallback", &mEnv->m_cevCueForceFallback, 0, VF_NULL, "1 = the cue route (audio from a CryAudio trigger) ignores the optional engine patch and follows the video clock (test of the fallback)");
		ConsoleRegistrationHelper::Register("cev_volume", &mEnv->m_cevVolume, 1.f, VF_NULL, "Master volume of all videos (0.0 - 1.0), multiplied with the player volume");
		REGISTER_CVAR2("cev_allowPlanar", &mEnv->m_cevAllowPlanar, 1, VF_NULL, "1 = surface players switch to a packed planar texture (R8: Y, U, V [, A]) while every consumer of the texture source contract accepts it; 0 = always RGBA8");
		ConsoleRegistrationHelper::Register("cev_hiddenStopSeconds", &mEnv->m_cevHiddenStopSeconds, 2.f, VF_NULL, "Surface players that no consumer sees only decode (no copy / conversion / upload); after this many seconds hidden in a row they stop decoding the video. 0 = never stop");
		REGISTER_CVAR2("cev_loadtime", &mEnv->m_cevLoadtime, 1, VF_NULL, "Keep texture-only players running during level loading screens (needs a renderer with load-time callbacks). 0 = pause everything while a level loads");
	}

	void UnregisterConsole()
	{
		IConsole* const pConsole = gEnv != nullptr ? gEnv->pConsole : nullptr;
		if (pConsole == nullptr || !s_registered)
			return;
		s_registered = false;

		static const char* const commands[] = { "cev_play", "cev_stop", "cev_pause", "cev_resume", "cev_seek", "cev_audiotrack", "cev_apitest", "cev_list", "cev_play_surface", "cev_surfaces", "cev_contract_selftest", "cev_info", "cev_flowtest", "cev_run_script", "cev_probe_r8", "cev_probe_sync", "cev_bench_play", "cev_bench_stop", "cev_stats_file", "cev_hidden_bench", "cev_bench_hint", "cev_show_surface" };
		for (const char* name : commands)
			pConsole->RemoveCommand(name);
#if CEV_HAVE_LOADTIME_CALLBACKS
		pConsole->RemoveCommand("cev_probe_loadtime");
#endif

		static const char* const variables[] = { "cev_stats", "cev_decodeThreads", "cev_decodeMode", "cev_decodeAutoMinHeight", "cev_log", "cev_audio_enable", "cev_muteOnFocusLoss", "cev_disableEmbeddedAudio", "cev_variant", "cev_cueForceFallback", "cev_volume", "cev_hiddenStopSeconds", "cev_allowPlanar", "cev_loadtime" };
		for (const char* name : variables)
			pConsole->UnregisterVariable(name, true);
	}
}
