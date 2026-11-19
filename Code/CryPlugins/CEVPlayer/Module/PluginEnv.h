// Copyright (C) 2017-2020 Ilya Chernetsov. All rights reserved. Contacts: <chernecoff@gmail.com>
// License: https://github.com/afrostalin/CEVPlayer/blob/master/LICENSE

#pragma once

#include <string>

#include <CEVPlayer/IVideoPlayer.h>

namespace CEVPlayer
{
	//! EVideoPlayerCaps bits of this build (IVideoPlugin::GetVideoPlayerCaps and the API table's Caps query).
	constexpr uint32_t kPluginCaps = eVideoCap_Alpha | eVideoCap_Premultiplied | eVideoCap_Variants;

	class CPlayerManager;
	namespace Api { class CApiPlayers; }

#define TITLE "[CEVPlayer] "

#define VIDEO_FOLDER "Videos/"
#define VIDEO_FORMAT_EXT ".webm"

	// Global plugin environment (one static instance, never null)
	struct SPluginEnv
	{
		// Pointers (owned by CVideoPlugin)
		CPlayerManager* pManager = nullptr;
		Api::CApiPlayers* pApi = nullptr;  // handle registry of the script API (FlowGraph / Schematyc / CryVS)

		// Console variables (bound to the cvar system, written on the main thread, read anywhere)
		int m_cevLog = 0;         // cev_log: 1 = verbose
		int m_cevStats = 0;       // cev_stats: 1 = stats line every second (log + on screen)
		int m_cevDecodeThreads = 0; // cev_decodeThreads: 0 = auto
		int m_cevAudioEnable = 1; // cev_audio_enable
		float m_cevVolume = 1.f;  // cev_volume (master volume 0..1)
		int m_cevMuteOnFocusLoss = 1; // cev_muteOnFocusLoss
		int m_cevDisableEmbeddedAudio = 0; // cev_disableEmbeddedAudio: never initialise the SDL audio subsystem
		int m_cevCueForceFallback = 0; // cev_cueForceFallback: 1 = the cue route ignores the engine patch and follows the video clock
		int m_cevVariant = -1;    // cev_variant: -1 = automatic, 0 = main video, n = n-th <Variant> of the descriptor
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

#define Log(...) do { if(mEnv) { CryLog (TITLE __VA_ARGS__); } } while(0)
#define LogDebug(...) do { if(mEnv && mEnv->m_cevLog) { CryLog (TITLE __VA_ARGS__); } } while(0)
#define LogAlways(...) do { if(mEnv) { CryLogAlways (TITLE __VA_ARGS__);} } while(0)
#define LogWarning(...) do { if(mEnv) { CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_WARNING, TITLE __VA_ARGS__);} } while(0)
#define LogError(...) do { if(mEnv) { CryWarning(VALIDATOR_MODULE_RENDERER, VALIDATOR_ERROR, TITLE __VA_ARGS__);} } while(0)
}
