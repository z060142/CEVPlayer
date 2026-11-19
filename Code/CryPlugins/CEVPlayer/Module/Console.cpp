#include "StdAfx.h"
#include "PluginEnv.h"
#include "Console.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <CrySystem/ConsoleRegistration.h>
#include <CryRenderer/IRenderer.h>
#include <CryRenderer/IRenderAuxGeom.h>

#include "Bindings/ApiTest.h"
#include "Contract/ContractTest.h"
#include "Contract/TexSource.h"
#include "Compat/LegacyPlayback.h"
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

			// cev_probe_sync
			int s_syncId = 0;
			uint32 s_syncFrame = 0;
			uint32 s_syncIndex = 0;
			std::vector<uint32> s_syncBuf;
			constexpr int kSyncSize = 64;

			// cev_bench_play
			IVideoPlayer* s_benchPlayer = nullptr;

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
				LogAlways("cev_probe_r8: off (usage: cev_probe_r8 <width> <height>)");
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
			s_r8Buf.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0);
			const Clock::time_point t0 = Clock::now();
			s_r8Id = static_cast<int>(gEnv->pRenderer->UploadToVideoMemory(nullptr, w, h, eTF_R8, eTF_R8, 0, false, FILTER_BILINEAR, 0, "$CevProbeR8", FT_NOMIPS));
			const float ms = std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
			LogAlways("cev_probe_r8: UploadToVideoMemory(nullptr, %d, %d, eTF_R8, eTF_R8, ..., FT_NOMIPS) -> id %d (%.3f ms)", w, h, s_r8Id, ms);
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
			if (!RendererOk())
				return;
			if (s_r8Id > 0)
			{
				++s_r8Frame;
				const int w = s_r8W, h = s_r8H;
				const int bar = static_cast<int>((s_r8Frame * 8u) % static_cast<uint32>(w));
				for (int y = 0; y < h; ++y)
				{
					uint8* row = s_r8Buf.data() + static_cast<size_t>(y) * w;
					for (int x = 0; x < w; ++x)
						row[x] = static_cast<uint8>(((x + y) >> 3) * 16 + s_r8Frame);
					for (int x = bar; x < bar + 16 && x < w; ++x)
						row[x] = 255;
				}
				const Clock::time_point t0 = Clock::now();
				gEnv->pRenderer->UpdateTextureInVideoMemory(static_cast<uint32>(s_r8Id), s_r8Buf.data(), 0, 0, w, h, eTF_R8);
				const float ms = std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
				s_r8MsSum += ms;
				s_r8MsMax = std::max(s_r8MsMax, ms);
				if (++s_r8MsCount == 120)
				{
					LogAlways("cev_probe_r8: %dx%d UpdateTextureInVideoMemory(eTF_R8) avg %.3f ms max %.3f ms over 120 frames (%d bytes each)", w, h,
						s_r8MsSum / s_r8MsCount, s_r8MsMax, w * h);
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

		void CmdList(IConsoleCmdArgs*)
		{
			if (mEnv->pManager != nullptr)
				mEnv->pManager->ListPlayers();
		}

		// ---- legacy vp_* ----
		void CmdLegacyPlay(IConsoleCmdArgs* args)
		{
			if (args != nullptr && args->GetArgCount() > 1 && mEnv->pLegacy != nullptr)
				mEnv->pLegacy->Play2DVideo(nullptr, args->GetArg(1));
		}

		void CmdLegacyPause(IConsoleCmdArgs*)
		{
			if (mEnv->pLegacy != nullptr)
				mEnv->pLegacy->Pause2DVideo();
		}

		void CmdLegacyResume(IConsoleCmdArgs*)
		{
			if (mEnv->pLegacy != nullptr)
				mEnv->pLegacy->Resume2DVideo();
		}

		void CmdLegacyStop(IConsoleCmdArgs*)
		{
			if (mEnv->pLegacy != nullptr)
				mEnv->pLegacy->Stop2DVideo();
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
		REGISTER_COMMAND("cev_contract_selftest", CmdContractSelfTest, VF_NULL, "Self test of the texture source contract through the CryTexSource_v1 export. cev_contract_selftest [pathA] [pathB] (two videos of different sizes), logs PASS / FAIL");
		// development-only measurement commands (S0)
		REGISTER_COMMAND("cev_probe_r8", DevProbe::CmdProbeR8, VF_NULL, "Measurement: cev_probe_r8 <w> <h> creates an eTF_R8 dynamic texture, updates it every frame and draws it top left (no arguments = off)");
		REGISTER_COMMAND("cev_probe_sync", DevProbe::CmdProbeSync, VF_NULL, "Measurement: cev_probe_sync 1|0, a 64x64 RGBA texture filled each frame with a colour coding the frame index, next to aux blocks of the same colour");
		REGISTER_COMMAND("cev_bench_play", DevProbe::CmdBenchPlay, VF_NULL, "Measurement: cev_bench_play <path> <texture name> [loop 0|1] plays into a named texture (not fullscreen); no arguments = stop");
		REGISTER_COMMAND("cev_bench_stop", DevProbe::CmdBenchStop, VF_NULL, "Measurement: stop the cev_bench_play player");
		REGISTER_COMMAND("cev_stats_file", DevProbe::CmdStatsFile, VF_NULL, "Measurement: cev_stats_file <path>|off writes the per-second cev_stats rows as CSV (turns cev_stats on)");

		REGISTER_CVAR2("cev_stats", &mEnv->m_cevStats, 0, VF_NULL, "1 = log and draw a stats line per player every second (decode / convert ms, uploads, dropped, allocs)");
		REGISTER_CVAR2("cev_decodeThreads", &mEnv->m_cevDecodeThreads, 0, VF_NULL, "Decode worker threads of the shared pool and libvpx threads per video; 0 = automatic. Applies to new pools / players");
		REGISTER_CVAR2("cev_log", &mEnv->m_cevLog, 0, VF_NULL, "1 = verbose CEVPlayer log");
		REGISTER_CVAR2("cev_audio_enable", &mEnv->m_cevAudioEnable, 1, VF_NULL, "0 = no audio output from videos (audio is not even decoded for players created afterwards)");

		REGISTER_CVAR2("cev_muteOnFocusLoss", &mEnv->m_cevMuteOnFocusLoss, 1, VF_NULL, "1 = video audio is muted while the game window has no focus");
		REGISTER_CVAR2("cev_disableEmbeddedAudio", &mEnv->m_cevDisableEmbeddedAudio, 0, VF_NULL, "1 = videos play silently and the plug-in never initialises the SDL audio subsystem (set it in the .cryproject console_variables to cover the whole run)");
		REGISTER_CVAR2("cev_variant", &mEnv->m_cevVariant, -1, VF_NULL, "Variant of a .cevvideo descriptor for players loaded afterwards: -1 = automatic (smallest not below the screen height when fullscreen, else the main video), 0 = main video, n = n-th <Variant>");
		REGISTER_CVAR2("cev_cueForceFallback", &mEnv->m_cevCueForceFallback, 0, VF_NULL, "1 = the cue route (audio from a CryAudio trigger) ignores the optional engine patch and follows the video clock (test of the fallback)");
		ConsoleRegistrationHelper::Register("cev_volume", &mEnv->m_cevVolume, 1.f, VF_NULL, "Master volume of all videos (0.0 - 1.0), multiplied with the player volume and vp_audio_volume");

		// Legacy (kept for existing projects)
		REGISTER_COMMAND("vp_play", CmdLegacyPlay, VF_NULL, "Play video file. Ex. vp_play logo (without extension name)");
		REGISTER_COMMAND("vp_pause", CmdLegacyPause, VF_NULL, "Pause current playing video");
		REGISTER_COMMAND("vp_resume", CmdLegacyResume, VF_NULL, "Resume current paused video");
		REGISTER_COMMAND("vp_stop", CmdLegacyStop, VF_NULL, "Stop current playing/paused video");
		REGISTER_CVAR2("vp_debug_log", &mEnv->m_bDebugLog, 0, VF_NULL, "Video plugin debug log");
		REGISTER_CVAR2("vp_debug_draw", &mEnv->m_bDebugDraw, 0, VF_NULL, "Video plugin debug draw (same overlay as cev_stats)");
		REGISTER_CVAR2("vp_disableLogs", &mEnv->m_bDisableLog, 0, VF_NULL, "Disable logs from video plugin");
		ConsoleRegistrationHelper::Register("vp_audio_volume", &mEnv->m_audioVolume, 1.f, VF_NULL, "Audio volume of all videos (0.0 - 1.0)");
	}

	void UnregisterConsole()
	{
		IConsole* const pConsole = gEnv != nullptr ? gEnv->pConsole : nullptr;
		if (pConsole == nullptr || !s_registered)
			return;
		s_registered = false;

		static const char* const commands[] = { "cev_play", "cev_stop", "cev_pause", "cev_resume", "cev_seek", "cev_lang", "cev_apitest", "cev_list", "cev_play_surface", "cev_surfaces", "cev_contract_selftest", "cev_probe_r8", "cev_probe_sync", "cev_bench_play", "cev_bench_stop", "cev_stats_file", "vp_play", "vp_pause", "vp_resume", "vp_stop" };
		for (const char* name : commands)
			pConsole->RemoveCommand(name);

		static const char* const variables[] = { "cev_stats", "cev_decodeThreads", "cev_log", "cev_audio_enable", "cev_muteOnFocusLoss", "cev_disableEmbeddedAudio", "cev_variant", "cev_cueForceFallback", "cev_volume", "vp_debug_log", "vp_debug_draw", "vp_disableLogs", "vp_audio_volume" };
		for (const char* name : variables)
			pConsole->UnregisterVariable(name, true);
	}
}
