#include "StdAfx.h"
#include "PluginEnv.h"
#include "Console.h"

#include <cstdlib>
#include <cstring>

#include <CrySystem/ConsoleRegistration.h>

#include "Bindings/ApiTest.h"
#include "Compat/LegacyPlayback.h"
#include "Player/PlayerManager.h"

namespace CEVPlayer
{
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

		REGISTER_CVAR2("cev_stats", &mEnv->m_cevStats, 0, VF_NULL, "1 = log and draw a stats line per player every second (decode / convert ms, uploads, dropped, allocs)");
		REGISTER_CVAR2("cev_decodeThreads", &mEnv->m_cevDecodeThreads, 0, VF_NULL, "Decode worker threads of the shared pool and libvpx threads per video; 0 = automatic. Applies to new pools / players");
		REGISTER_CVAR2("cev_log", &mEnv->m_cevLog, 0, VF_NULL, "1 = verbose CEVPlayer log");
		REGISTER_CVAR2("cev_audio_enable", &mEnv->m_cevAudioEnable, 1, VF_NULL, "0 = no audio output from videos (audio is not even decoded for players created afterwards)");

		REGISTER_CVAR2("cev_muteOnFocusLoss", &mEnv->m_cevMuteOnFocusLoss, 1, VF_NULL, "1 = video audio is muted while the game window has no focus");
		REGISTER_CVAR2("cev_disableEmbeddedAudio", &mEnv->m_cevDisableEmbeddedAudio, 0, VF_NULL, "1 = videos play silently and the plug-in never initialises the SDL audio subsystem (set it in the .cryproject console_variables to cover the whole run)");
		REGISTER_CVAR2("cev_variant", &mEnv->m_cevVariant, -1, VF_NULL, "Variant of a .cevvideo descriptor for players loaded afterwards: -1 = automatic (smallest not below the screen height when fullscreen, else the main video), 0 = main video, n = n-th <Variant>");
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

		static const char* const commands[] = { "cev_play", "cev_stop", "cev_pause", "cev_resume", "cev_seek", "cev_lang", "cev_apitest", "cev_list", "vp_play", "vp_pause", "vp_resume", "vp_stop" };
		for (const char* name : commands)
			pConsole->RemoveCommand(name);

		static const char* const variables[] = { "cev_stats", "cev_decodeThreads", "cev_log", "cev_audio_enable", "cev_muteOnFocusLoss", "cev_disableEmbeddedAudio", "cev_variant", "cev_volume", "vp_debug_log", "vp_debug_draw", "vp_disableLogs", "vp_audio_volume" };
		for (const char* name : variables)
			pConsole->UnregisterVariable(name, true);
	}
}
