// Copyright (C) 2017-2020 Ilya Chernetsov. All rights reserved. Contacts: <chernecoff@gmail.com>
// License: https://github.com/afrostalin/CEVPlayer/blob/master/LICENSE

#pragma once

#include <memory>

#include <CryEntitySystem/IEntityClass.h>
#include <CEVPlayer/IVideoPlugin.h>

#include "PluginEnv.h"

#include "Bindings/ApiPlayers.h"
#include "Compat/LegacyPlayback.h"
#include "Input/InputDispatcher.h"
#include "Player/PlayerManager.h"

namespace CEVPlayer
{
	class CVideoPlugin
		: public IVideoPlugin
		, public ISystemEventListener
	{
	public:
		CRYINTERFACE_BEGIN()
			CRYINTERFACE_ADD(IVideoPlugin)
			CRYINTERFACE_ADD(Cry::IEnginePlugin)
			CRYINTERFACE_END()

			CRYGENERATE_SINGLETONCLASS_GUID(CVideoPlugin, "CEVPlayer", "{C14CBDCB-1CFF-41B9-A220-E16E19A21096}"_cry_guid)

			PLUGIN_FLOWNODE_REGISTER;
		PLUGIN_FLOWNODE_UNREGISTER;

		virtual ~CVideoPlugin();
	public:
		// ICryPlugin
		virtual const char* GetName() const override { return "CEVPlayer"; }
		virtual const char* GetCategory() const override { return "Render"; }
		virtual bool        Initialize(SSystemGlobalEnvironment& env, const SSystemInitParams& initParams) override;
		virtual void        UpdateBeforeRender() override;
		virtual void        UpdateAfterRender() override;
		// ~ICryPlugin

		// ISystemEventListener
		virtual void        OnSystemEvent(ESystemEvent event, UINT_PTR wparam, UINT_PTR lparam) override;
		// ~ISystemEventListener

		// IVideoPlugin (legacy)
		virtual void PlayVideoToMainWindow(const char* videoName, bool preload = false, bool looped = false, int audioTrack = 0,
			bool isSkippable = false, bool canBePaused = false, IVideoPlayerEventListener * pVideoPlayerListener = nullptr) override;
		virtual void PlayVideoToTexture(const char* videoName, const char* textureName, bool preload = false, bool looped = false, IVideoPlayerEventListener* pListener = nullptr) override;
		virtual void PauseVideo(EVideoType videoType, const char* textureName = nullptr) override;
		virtual void ResumeVideo(EVideoType videoType, const char* textureName = nullptr) override;
		virtual void StopVideo(EVideoType videoType, const char* textureName = nullptr) override;
		// IVideoPlugin (player API)
		virtual uint32_t GetVideoPlayerInterfaceVersion() const override { return kVideoPlayerInterfaceVersion; }
		virtual uint32_t GetVideoPlayerCaps() const override { return kPluginCaps; }
		virtual IVideoPlayer* CreatePlayer(const char* textureName = nullptr) override;
		virtual void DestroyPlayer(IVideoPlayer* pPlayer) override;
		virtual uint32_t GetPlayerCount() const override;
		virtual void RemoveVideoListener(IVideoPlayerEventListener* pListener) override;
		virtual IVideoPlayer* CreatePlayerForSurface(const char* surfaceId) override;
		virtual IVideoPlayer* FindPlayerBySurface(const char* surfaceId) const override;
		// ~IVideoPlugin
	protected:
		void        RegisterComponents(Schematyc::IEnvRegistrar& registrar);
		void        RegisterLegacyInput();
		void        ShutdownBridge();
	private:
		std::unique_ptr<CPlayerManager> m_pManager;
		std::unique_ptr<Api::CApiPlayers> m_pApi; // after the manager: destroyed before it
		std::unique_ptr<CLegacyPlayback> m_pLegacy;
		std::unique_ptr<CInputDispatcher> m_pInputDispatcher;
		bool m_listenerRegistered = false;
		float m_lastUpdateTime = -1.f; // engine async time of the previous UpdateBeforeRender (dt for the manager)
	};
}
