// Copyright (C) 2017-2019 Ilya Chernetsov. All rights reserved. Contacts: <chernecoff@gmail.com>
// License: https://github.com/afrostalin/CEVPlayer/blob/master/LICENSE

#pragma once

#include <CrySystem/ICryPlugin.h>
#include "IVideoPluginListeners.h"
#include "IVideoPlayer.h"

namespace CEVPlayer
{
	enum class EVideoType : int
	{
		ToMainWindow = 0,
		ToTexture,
	};

	struct IVideoPlugin : public Cry::IEnginePlugin
	{
		CRYINTERFACE_DECLARE_GUID(IVideoPlugin, "{4DCA4F01-AE95-4735-AADA-50E270418BB8}"_cry_guid);
	public:
		// ---------------------------------------------------------------------------------------------
		// Legacy API (unchanged names, signatures and vtable order). Implemented on top of IVideoPlayer.
		// ---------------------------------------------------------------------------------------------

		//! Stream video to main window
		virtual void PlayVideoToMainWindow(const char* videoName, bool preload = false, bool looped = false, int audioTrack = 0,
			bool isSkippable = false, bool canBePaused = false, IVideoPlayerEventListener * pVideoPlayerListener = nullptr) = 0;
		//! Stream video to texture only
		virtual void PlayVideoToTexture(const char* videoName, const char* textureName, bool preload = false, bool looped = false, IVideoPlayerEventListener* pListener = nullptr) = 0;

		//! Pause video - textureName argument used only for pausing video streamed to texture
		virtual void PauseVideo(EVideoType videoType, const char* textureName = nullptr) = 0;

		//! Resume video - textureName argument used only for pausing video streamed to texture
		virtual void ResumeVideo(EVideoType videoType, const char* textureName = nullptr) = 0;

		//! Stop video - textureName argument used only for pausing video streamed to texture
		virtual void StopVideo(EVideoType videoType, const char* textureName = nullptr) = 0;

		// ---------------------------------------------------------------------------------------------
		// Player API (see IVideoPlayer.h). Appended after the legacy virtuals: do not reorder.
		// ---------------------------------------------------------------------------------------------

		//! kVideoPlayerInterfaceVersion of the plug-in binary. Check it before using the functions below.
		virtual uint32_t GetVideoPlayerInterfaceVersion() const = 0;
		//! EVideoPlayerCaps bits.
		virtual uint32_t GetVideoPlayerCaps() const = 0;
		//! Creates an empty player (call Load() on it). `textureName` is the name the output texture is registered
		//! under, normally "$something"; nullptr / empty = generated "$cev_video_<id>". Returns nullptr when the
		//! plug-in is shutting down. Main thread only.
		virtual IVideoPlayer* CreatePlayer(const char* textureName = nullptr) = 0;
		//! Destroys a player created by CreatePlayer (releases media, texture, bindings). nullptr is ignored.
		virtual void DestroyPlayer(IVideoPlayer* pPlayer) = 0;
		virtual uint32_t GetPlayerCount() const = 0;
	};
}
