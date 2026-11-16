// Copyright (C) 2017-2020 Ilya Chernetsov. All rights reserved. Contacts: <chernecoff@gmail.com>
// License: https://github.com/afrostalin/CEVPlayer/blob/master/LICENSE

#pragma once

#include <string>

namespace CEVPlayer
{
	class CPlayerManager;
	class CLegacyPlayback;
	class CInputDispatcher;

#define TITLE "[CEVPlayer] "

#define VIDEO_FOLDER "Videos/"
#define VIDEO_FORMAT_EXT ".webm"

	// Global plugin environment (one static instance, never null)
	struct SPluginEnv
	{
		// Pointers (owned by CVideoPlugin)
		CPlayerManager* pManager = nullptr;
		CLegacyPlayback* pLegacy = nullptr;
		CInputDispatcher* pInputDispatcher = nullptr;

		// Console variables (bound to the cvar system, written on the main thread, read anywhere)
		int m_bDebugLog = 0;      // vp_debug_log (legacy)
		int m_bDebugDraw = 0;     // vp_debug_draw (legacy): same text overlay as cev_stats
		int m_bDisableLog = 0;    // vp_disableLogs
		float m_audioVolume = 1.f; // vp_audio_volume (global volume 0..1)
		int m_cevLog = 0;         // cev_log: 1 = verbose
		int m_cevStats = 0;       // cev_stats: 1 = stats line every second (log + on screen)
		int m_cevDecodeThreads = 0; // cev_decodeThreads: 0 = auto
		int m_cevAudioEnable = 1; // cev_audio_enable
	};

	extern SPluginEnv* mEnv;

	//! The public API (IVideoPlugin, IVideoPlayer) is main thread only.
	inline bool IsMainThread()
	{
		return gEnv == nullptr || gEnv->mMainThreadId == THREADID_NULL || CryGetCurrentThreadId() == gEnv->mMainThreadId;
	}

	//! Texture names must start with '$' to be reachable from materials / Flash. Prefixes a missing '$' and caps
	//! the length (the renderer copies names into a 128 byte buffer). `*pChanged` tells whether it had to change
	//! anything. nullptr / empty -> empty string.
	std::string NormalizeTextureName(const char* name, bool* pChanged = nullptr);

#define Log(...) do { if(mEnv && !mEnv->m_bDisableLog) { CryLog (TITLE __VA_ARGS__); } } while(0)
#define LogDebug(...) do { if(mEnv && (mEnv->m_bDebugLog || mEnv->m_cevLog)) { CryLog (TITLE __VA_ARGS__); } } while(0)
#define LogAlways(...) do { if(mEnv && !mEnv->m_bDisableLog) { CryLogAlways (TITLE __VA_ARGS__);} } while(0)
#define LogWarning(...) do { if(mEnv && !mEnv->m_bDisableLog) { CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_WARNING, TITLE __VA_ARGS__);} } while(0)
#define LogError(...) do { if(mEnv && !mEnv->m_bDisableLog) { CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_ERROR, TITLE __VA_ARGS__);} } while(0)
}
