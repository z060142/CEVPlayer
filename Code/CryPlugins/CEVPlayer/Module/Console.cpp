#include "StdAfx.h"
#include "PluginEnv.h"
#include "Console.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <CrySystem/ConsoleRegistration.h>
#include <CryRenderer/IRenderer.h>
#include <CryRenderer/IRenderAuxGeom.h>
#include <CryRenderer/ITexture.h>

#include "Bindings/ApiTable.h"
#include "Bindings/ApiTest.h"
#include "Bindings/FlowTest.h"
#include "Contract/ContractTest.h"
#include "Contract/TexSource.h"
#include "Player/PlayerManager.h"

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

	void TickRunScript() { RunScript::Tick(); }
	void ShutdownRunScript() { RunScript::Shutdown(); }

	void DevProbesBeforeRender() { DevProbe::BeforeRender(); }
	void DevProbesAfterRender() { DevProbe::AfterRender(); }
	void DevProbesShutdown() { DevProbe::Shutdown(); }

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

		void CmdPause(IConsoleCmdArgs*)
		{
			if (mEnv->pManager != nullptr)
				mEnv->pManager->ConsolePause();
		}

		void CmdResume(IConsoleCmdArgs*)
		{
			if (mEnv->pManager != nullptr)
				mEnv->pManager->ConsoleResume();
		}

		void CmdSeek(IConsoleCmdArgs* args)
		{
			if (args == nullptr || mEnv->pManager == nullptr)
				return;
			if (args->GetArgCount() < 2)
			{
				LogAlways("usage: cev_seek <seconds>");
				return;
			}
			mEnv->pManager->ConsoleSeek(std::atof(args->GetArg(1)));
		}

		void CmdLang(IConsoleCmdArgs* args)
		{
			if (args == nullptr || mEnv->pManager == nullptr)
				return;
			mEnv->pManager->ConsoleSetLanguage(args->GetArgCount() > 1 ? args->GetArg(1) : nullptr);
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

		void CmdSurfaces(IConsoleCmdArgs*)
		{
			Contract::LogSurfaces();
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
		REGISTER_COMMAND("cev_pause", CmdPause, VF_NULL, "Pause the video started by cev_play");
		REGISTER_COMMAND("cev_resume", CmdResume, VF_NULL, "Resume the video started by cev_play");
		REGISTER_COMMAND("cev_seek", CmdSeek, VF_NULL, "Seek the video started by cev_play. cev_seek <seconds>");
		REGISTER_COMMAND("cev_lang", CmdLang, VF_NULL, "Select the audio language of the video started by cev_play. cev_lang <name> (without a name: show the current one)");
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
	}

	void UnregisterConsole()
	{
		IConsole* const pConsole = gEnv != nullptr ? gEnv->pConsole : nullptr;
		if (pConsole == nullptr || !s_registered)
			return;
		s_registered = false;

		static const char* const commands[] = { "cev_play", "cev_stop", "cev_pause", "cev_resume", "cev_seek", "cev_lang", "cev_apitest", "cev_list", "cev_play_surface", "cev_surfaces", "cev_contract_selftest", "cev_info", "cev_flowtest", "cev_run_script", "cev_probe_r8", "cev_probe_sync", "cev_bench_play", "cev_bench_stop", "cev_stats_file", "cev_hidden_bench", "cev_bench_hint", "cev_show_surface" };
		for (const char* name : commands)
			pConsole->RemoveCommand(name);

		static const char* const variables[] = { "cev_stats", "cev_decodeThreads", "cev_decodeMode", "cev_decodeAutoMinHeight", "cev_log", "cev_audio_enable", "cev_muteOnFocusLoss", "cev_disableEmbeddedAudio", "cev_variant", "cev_cueForceFallback", "cev_volume", "cev_hiddenStopSeconds", "cev_allowPlanar" };
		for (const char* name : variables)
			pConsole->UnregisterVariable(name, true);
	}
}
