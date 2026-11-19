// Copyright (C) 2017-2019 Ilya Chernetsov. All rights reserved. Contacts: <chernecoff@gmail.com>
// License: https://github.com/afrostalin/CEVPlayer/blob/master/LICENSE

#pragma once

#include <CrySystem/ICryPlugin.h>
#include "IVideoPlayer.h"

namespace CEVPlayer
{
	struct IVideoPlugin : public Cry::IEnginePlugin
	{
		CRYINTERFACE_DECLARE_GUID(IVideoPlugin, "{4DCA4F01-AE95-4735-AADA-50E270418BB8}"_cry_guid);
	public:
		// ---------------------------------------------------------------------------------------------
		// Player API (see IVideoPlayer.h). Do not reorder. The legacy virtuals of the original plug-in
		// (PlayVideoToMainWindow, PlayVideoToTexture, Pause/Resume/StopVideo, RemoveVideoListener) were removed in M6b
		// (interface version 5): rebuild every module that calls IVideoPlugin against this header.
		// ---------------------------------------------------------------------------------------------

		//! kVideoPlayerInterfaceVersion of the plug-in binary. Check it before using the functions below.
		virtual uint32_t GetVideoPlayerInterfaceVersion() const = 0;
		//! EVideoPlayerCaps bits.
		virtual uint32_t GetVideoPlayerCaps() const = 0;
		//! Creates an empty player (call Load() on it). `textureName` is the name the output texture is registered
		//! under, normally "$something" (a missing "$" is added with a warning); nullptr / empty = generated
		//! "$cev_video_<w>x<h>_<k>" (reused by later players, its final value is known once the player IsLoaded()).
		//! Returns nullptr when the plug-in is shutting down. Main thread only (other threads: warning, nullptr).
		//!
		//! Lifetime: the returned IVideoPlayer* is owned by the plug-in. It stays valid until you pass it to
		//! DestroyPlayer or the plug-in shuts down (ESYSTEM_EVENT_FULL_SHUTDOWN / FAST_SHUTDOWN): then every player
		//! is destroyed without further callbacks and all IVideoPlayer pointers are invalid; do not call them
		//! afterwards (DestroyPlayer on such a stale pointer is ignored).
		virtual IVideoPlayer* CreatePlayer(const char* textureName = nullptr) = 0;
		//! Destroys a player created by CreatePlayer (releases media, texture, bindings). nullptr is ignored.
		//! Main thread only (other threads: warning, nothing happens).
		virtual void DestroyPlayer(IVideoPlayer* pPlayer) = 0;
		virtual uint32_t GetPlayerCount() const = 0;

		// ---------------------------------------------------------------------------------------------
		// Appended in player interface version 5: do not reorder.
		// ---------------------------------------------------------------------------------------------

		//! Creates an empty player identified by a surface id (characters A-Z a-z 0-9 _ . : -, 1 to 63 of them,
		//! compared case-insensitively). The id stays the same across Load(), language, variant and size changes;
		//! other plug-ins find the player by it through the texture source contract (docs/design/M6-ui-source.md
		//! section 3). Returns nullptr (and logs an error) when the id is invalid or already used, or when the
		//! plug-in is shutting down. Lifetime as CreatePlayer; destroy it with DestroyPlayer. Main thread only.
		virtual IVideoPlayer* CreatePlayerForSurface(const char* surfaceId) = 0;
		//! The live player created with this surface id (case-insensitive), nullptr if there is none. Main thread only.
		virtual IVideoPlayer* FindPlayerBySurface(const char* surfaceId) const = 0;
	};
}
