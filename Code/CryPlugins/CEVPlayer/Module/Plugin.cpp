// Copyright (C) 2017-2020 Ilya Chernetsov. All rights reserved. Contacts: <chernecoff@gmail.com>
// License: https://github.com/afrostalin/CEVPlayer/blob/master/LICENSE

#include "StdAfx.h"
#include "PluginEnv.h"
#include "Plugin.h"
#include "Console.h"

#include "Components/TextureVideoPlayerComponent.h"

#include <CrySchematyc/Env/IEnvRegistry.h>
#include <CrySchematyc/Env/EnvPackage.h>
#include <CrySchematyc/Utils/SharedString.h>
#include <CrySchematyc/CoreAPI.h>
#include <CryGame/IGameFramework.h>

#include <CryCore/Platform/platform_impl.inl>

namespace CEVPlayer
{
	namespace
	{
		//! The public API is main thread only: other threads get a warning and nothing else happens.
		bool CheckMainThread(const char* what)
		{
			if (IsMainThread())
				return true;
			LogWarning("<CVideoPlugin> %s must be called on the main thread, call ignored", what);
			return false;
		}
	}

	void CVideoPlugin::RegisterComponents(Schematyc::IEnvRegistrar& registrar)
	{
		Schematyc::CEnvRegistrationScope scope = registrar.Scope(IEntity::GetEntityScopeGUID());
		{
			// Texture video player component
			{
				Schematyc::CEnvRegistrationScope componentScope = scope.Register(SCHEMATYC_MAKE_ENV_COMPONENT(CTextureVideoPlayerComponent));
				CTextureVideoPlayerComponent::Register(componentScope);
			}
		}
	}

	CVideoPlugin::~CVideoPlugin()
	{
		ShutdownBridge(); // normally done at ESYSTEM_EVENT_FULL_SHUTDOWN already; idempotent

		if (m_listenerRegistered && gEnv != nullptr && gEnv->pSystem != nullptr)
		{
			gEnv->pSystem->GetISystemEventDispatcher()->RemoveListener(this);
		}

#ifndef CRY_IS_MONOLITHIC_BUILD
		if (gEnv != nullptr && gEnv->pSchematyc != nullptr && !gEnv->IsDedicated())
		{
			gEnv->pSchematyc->GetEnvRegistry().DeregisterPackage(CVideoPlugin::GetCID());
		}
#endif

		UnregisterConsole();

		mEnv->pInputDispatcher = nullptr;
		m_pInputDispatcher.reset();
		mEnv->pLegacy = nullptr;
		mEnv->pManager = nullptr;
		m_pLegacy.reset();
		m_pManager.reset();
		cev::DecodePool::ShutdownShared(); // idempotent: no worker thread may outlive this module
	}

	bool CVideoPlugin::Initialize(SSystemGlobalEnvironment& env, const SSystemInitParams& initParams)
	{
		// Nothing here touches the renderer (it may not exist yet): textures are created at the first tick that
		// needs them.
		m_pManager = std::make_unique<CPlayerManager>();
		m_pLegacy = std::make_unique<CLegacyPlayback>(*m_pManager);
		mEnv->pManager = m_pManager.get();
		mEnv->pLegacy = m_pLegacy.get();

		RegisterConsole();

		if (gEnv->pSystem != nullptr)
		{
			gEnv->pSystem->GetISystemEventDispatcher()->RegisterListener(this, "CVideoPlugin_Listener");
			m_listenerRegistered = true;
		}

		if (gEnv->pConsole != nullptr)
		{
			ICVar* pStartScreen = gEnv->pConsole->GetCVar("sys_rendersplashscreen");
			if (pStartScreen != nullptr)
			{
				pStartScreen->Set(0);
			}
		}

		// One tick per frame before the scene is rendered (frame step, texture upload, listener dispatch) and
		// the fullscreen aux draw after the game framework drew its own overlays (top layer).
		EnableUpdate(EUpdateStep::BeforeRender, true);
		EnableUpdate(EUpdateStep::AfterRender, true);
		return true;
	}

	void CVideoPlugin::UpdateBeforeRender()
	{
		if (m_pManager)
			m_pManager->UpdateBeforeRender();
	}

	void CVideoPlugin::UpdateAfterRender()
	{
		if (m_pManager)
			m_pManager->UpdateAfterRender();
	}

	void CVideoPlugin::ShutdownBridge()
	{
		// Order matters: the legacy layer owns players of the manager, the manager releases the textures (the
		// renderer is still alive at ESYSTEM_EVENT_FULL_SHUTDOWN / FAST_SHUTDOWN) and the audio device.
		if (m_pLegacy)
			m_pLegacy->Shutdown();
		if (m_pManager)
			m_pManager->Shutdown();
		// Every player is gone now (the manager released them and its pool reference): join the shared decode
		// workers before the module can be unloaded.
		cev::DecodePool::ShutdownShared();
		LogDebug("bridge shut down, decode pool released");
	}

	void CVideoPlugin::RegisterLegacyInput()
	{
		if (m_pInputDispatcher != nullptr || gEnv->pGameFramework == nullptr || gEnv->pGameFramework->GetIActionMapManager() == nullptr)
			return;

		m_pInputDispatcher = std::make_unique<CInputDispatcher>();
		mEnv->pInputDispatcher = m_pInputDispatcher.get();

		CLegacyPlayback* const pLegacy = m_pLegacy.get();
		m_pInputDispatcher->RegisterAction("video_plugin", "pause_ingame_video", [pLegacy](int activationMode, float value) { pLegacy->OnActionPause2DVideo((EInputState)activationMode, value); });
		m_pInputDispatcher->RegisterAction("video_plugin", "skip_ingame_video", [pLegacy](int activationMode, float value) { pLegacy->OnActionSkip2DVideo((EInputState)activationMode, value); });

		m_pInputDispatcher->BindAction("video_plugin", "pause_ingame_video", "Shift", eAID_KeyboardMouse, EKeyId::eKI_Space);
		m_pInputDispatcher->BindAction("video_plugin", "skip_ingame_video", "Enter", eAID_KeyboardMouse, EKeyId::eKI_Enter);
		m_pInputDispatcher->BindAction("video_plugin", "skip_ingame_video", "Escape", eAID_KeyboardMouse, EKeyId::eKI_Escape);
		m_pInputDispatcher->BindAction("video_plugin", "skip_ingame_video", "Space", eAID_KeyboardMouse, EKeyId::eKI_Escape);
		m_pInputDispatcher->BindAction("video_plugin", "skip_ingame_video", "xi_a", eAID_KeyboardMouse, EKeyId::eKI_XI_A);
		m_pInputDispatcher->BindAction("video_plugin", "skip_ingame_video", "xi_b", eAID_KeyboardMouse, EKeyId::eKI_XI_B);
		m_pInputDispatcher->BindAction("video_plugin", "skip_ingame_video", "xi_x", eAID_KeyboardMouse, EKeyId::eKI_XI_X);
		m_pInputDispatcher->BindAction("video_plugin", "skip_ingame_video", "xi_y", eAID_KeyboardMouse, EKeyId::eKI_XI_Y);
		m_pInputDispatcher->BindAction("video_plugin", "skip_ingame_video", "xi_start", eAID_KeyboardMouse, EKeyId::eKI_XI_Start);
		m_pInputDispatcher->BindAction("video_plugin", "skip_ingame_video", "xi_back", eAID_KeyboardMouse, EKeyId::eKI_XI_Back);
	}

	void CVideoPlugin::OnSystemEvent(ESystemEvent event, UINT_PTR wparam, UINT_PTR lparam)
	{
		if (event == ESYSTEM_EVENT_FULL_SHUTDOWN || event == ESYSTEM_EVENT_FAST_SHUTDOWN)
		{
			ShutdownBridge();
			return;
		}

		if (m_pManager)
			m_pManager->OnSystemEvent(event); // level loading pause / resume

		if (gEnv->IsDedicated())
		{
			return;
		}

		switch (event)
		{
		case ESYSTEM_EVENT_GAME_POST_INIT:
		{
			RegisterLegacyInput();
			if (m_pManager)
				m_pManager->RegisterLevelListener();
			break;
		}
		case ESYSTEM_EVENT_REGISTER_SCHEMATYC_ENV:
		{
			if (gEnv->pSchematyc != nullptr)
			{
				gEnv->pSchematyc->GetEnvRegistry().RegisterPackage(
					stl::make_unique<Schematyc::CEnvPackage>(
						CVideoPlugin::GetCID(),
						"EntityComponents",
						"Ilya Chernetsov",
						"CEVNetwork components",
						[this](Schematyc::IEnvRegistrar& registrar) { RegisterComponents(registrar); }
						)
				);
			}
			break;
		}
		case ESYSTEM_EVENT_LEVEL_UNLOAD:
			if (m_pLegacy)
			{
				// Flow graphs / entities of the level die: nobody may be called back for the videos that go on
				// (main window videos), and texture videos end with the level.
				m_pLegacy->DetachListeners();
				m_pLegacy->ClearTextureVideos();
			}
			break;
		case ESYSTEM_EVENT_GAME_PAUSED:
			if (m_pLegacy)
				m_pLegacy->PauseAllTextureVideos(true);
			break;
		case ESYSTEM_EVENT_GAME_RESUMED:
			if (m_pLegacy)
				m_pLegacy->PauseAllTextureVideos(false);
			break;
		case ESYSTEM_EVENT_EDITOR_GAME_MODE_CHANGED:
		{
			if (wparam == 0 && m_pLegacy) // Clear when exit from game mode
			{
				m_pLegacy->DetachListeners(); // the game mode's flow graphs are destroyed now
				m_pLegacy->ClearTextureVideos();
			}
			break;
		}
		default:
			break;
		}
	}

	// ---------------------------------------------------------------------------------------------- legacy API
	void CVideoPlugin::PlayVideoToMainWindow(const char* videoName, bool preload, bool looped, int audioTrack, bool isSkippable, bool canBePaused, IVideoPlayerEventListener* pVideoPlayerListener)
	{
		if (!CheckMainThread("PlayVideoToMainWindow") || gEnv->pSystem->IsQuitting())
			return;

		if (m_pLegacy)
		{
			m_pLegacy->Play2DVideo(pVideoPlayerListener, videoName, preload, looped, audioTrack, isSkippable, canBePaused);
		}
		else
		{
			LogError("<CVideoPlugin> Can't play 2D video, plug-in is not initialized!");
		}
	}

	void CVideoPlugin::PlayVideoToTexture(const char* videoName, const char* textureName, bool preload, bool looped, IVideoPlayerEventListener* pListener)
	{
		if (!CheckMainThread("PlayVideoToTexture") || gEnv->pSystem->IsQuitting())
			return;

		if (m_pLegacy)
		{
			m_pLegacy->PlayVideo(pListener, videoName, textureName, preload, looped);
		}
		else
		{
			LogError("<CVideoPlugin> Can't play texture video, plug-in is not initialized!");
		}
	}

	void CVideoPlugin::PauseVideo(EVideoType videoType, const char* textureName)
	{
		if (!m_pLegacy || !CheckMainThread("PauseVideo"))
			return;
		switch (videoType)
		{
		case EVideoType::ToMainWindow:
			m_pLegacy->Pause2DVideo();
			break;
		case EVideoType::ToTexture:
			m_pLegacy->PauseVideo(textureName);
			break;
		default:
			break;
		}
	}

	void CVideoPlugin::ResumeVideo(EVideoType videoType, const char* textureName)
	{
		if (!m_pLegacy || !CheckMainThread("ResumeVideo"))
			return;
		switch (videoType)
		{
		case EVideoType::ToMainWindow:
			m_pLegacy->Resume2DVideo();
			break;
		case EVideoType::ToTexture:
			m_pLegacy->ResumeVideo(textureName);
			break;
		default:
			break;
		}
	}

	void CVideoPlugin::StopVideo(EVideoType videoType, const char* textureName)
	{
		if (!m_pLegacy || !CheckMainThread("StopVideo"))
			return;
		switch (videoType)
		{
		case EVideoType::ToMainWindow:
			m_pLegacy->Stop2DVideo();
			break;
		case EVideoType::ToTexture:
			m_pLegacy->StopVideo(textureName);
			break;
		default:
			break;
		}
	}

	// ---------------------------------------------------------------------------------------------- player API
	IVideoPlayer* CVideoPlugin::CreatePlayer(const char* textureName)
	{
		if (!CheckMainThread("CreatePlayer"))
			return nullptr;
		return m_pManager ? m_pManager->CreatePlayer(textureName) : nullptr; // nullptr after shutdown
	}

	void CVideoPlugin::DestroyPlayer(IVideoPlayer* pPlayer)
	{
		if (!CheckMainThread("DestroyPlayer"))
			return;
		if (m_pManager)
			m_pManager->DestroyPlayer(pPlayer); // only compares the pointer with the live players: safe after shutdown
	}

	void CVideoPlugin::RemoveVideoListener(IVideoPlayerEventListener* pListener)
	{
		if (!CheckMainThread("RemoveVideoListener"))
			return;
		if (m_pLegacy)
			m_pLegacy->RemoveVideoListener(pListener);
	}

	uint32_t CVideoPlugin::GetPlayerCount() const
	{
		return m_pManager ? m_pManager->GetPlayerCount() : 0u;
	}

	CRYREGISTER_SINGLETON_CLASS(CVideoPlugin)
}
