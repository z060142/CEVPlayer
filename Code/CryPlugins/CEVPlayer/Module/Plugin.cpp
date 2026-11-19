// Copyright (C) 2017-2020 Ilya Chernetsov. All rights reserved. Contacts: <chernecoff@gmail.com>
// License: https://github.com/afrostalin/CEVPlayer/blob/master/LICENSE

#include "StdAfx.h"
#include "PluginEnv.h"
#include "Plugin.h"
#include "Console.h"

#include "Components/VideoPlayerComponent.h"
#include "Bindings/ApiTable.h"
#include "Bindings/ApiTest.h"
#include "Bindings/FlowTest.h"
#include "Contract/ContractTest.h"
#include "Contract/TexSource.h"
#if CEV_HAVE_CRYVS
#include "Bindings/CryVSEnv.h"
#endif

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
			// Video Player component (functions and signals generated from the API table)
			{
				Schematyc::CEnvRegistrationScope componentScope = scope.Register(SCHEMATYC_MAKE_ENV_COMPONENT(CVideoPlayerComponent));
				CVideoPlayerComponent::Register(componentScope);
				unsigned controls = 0;
				for (const Api::SEntry& e : Api::Controls())
					controls += (e.flags & Api::eEntry_Standalone) == 0 ? 1u : 0u;
				LogAlways("Schematyc: registered component 'Video Player' (%u control and %u query functions, %u signals)",
					controls, static_cast<unsigned>(Api::Queries().size()), static_cast<unsigned>(Api::Events().size()));
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

		mEnv->pApi = nullptr;
		m_pApi.reset();
		mEnv->pManager = nullptr;
		m_pManager.reset();
		cev::DecodePool::ShutdownShared(); // idempotent: no worker thread may outlive this module
	}

	bool CVideoPlugin::Initialize(SSystemGlobalEnvironment& env, const SSystemInitParams& initParams)
	{
		// Nothing here touches the renderer (it may not exist yet): textures are created at the first tick that
		// needs them.
		Contract::Open(); // the CryTexSource_v1 export starts answering for surface players
		m_pManager = std::make_unique<CPlayerManager>();
		mEnv->pManager = m_pManager.get();
		m_pApi = std::make_unique<Api::CApiPlayers>(*m_pManager);
		mEnv->pApi = m_pApi.get();

		RegisterConsole();
		Api::SelfCheck(); // duplicate GUIDs / names in the API table -> warning (assert in debug)
		LogAlways("FlowGraph: %u node classes (CEVPlayer:*, CEVPlayer:UI:*, CEVPlayer:Advanced:*)", Api::FlowNodeCount());

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
		// dt comes from the time source the manager used before (engine async time); the manager's tick reads no
		// engine timer itself (it also runs from the load-time callback later, M10).
		const float now = gEnv->pTimer != nullptr ? gEnv->pTimer->GetAsyncCurTime() : 0.f;
		const float dt = m_lastUpdateTime < 0.f ? 0.f : now - m_lastUpdateTime;
		m_lastUpdateTime = now;
		if (m_pManager)
		{
			m_pManager->CheckLevelLoadingTimeout(); // main thread only
			m_pManager->UpdateBeforeRender(dt);
		}
		if (m_pApi)
			m_pApi->Update(); // Skipped events, records destroyed from inside a callback
		Api::TickApiTest();
		Api::TickFlowTest();
		Contract::TickSelfTest();
#if CEV_HAVE_CRYVS
		CryVSBind::Tick(); // WatchSurfaceID components, cev_cryvstest
#endif
		TickRunScript(); // development test driver (cev_run_script)
		DevProbesBeforeRender(); // measurement only (S0)
	}

	void CVideoPlugin::UpdateAfterRender()
	{
		if (m_pManager)
			m_pManager->UpdateAfterRender();
		DevProbesAfterRender(); // measurement only (S0)
	}

	void CVideoPlugin::ShutdownBridge()
	{
#if CEV_HAVE_CRYVS
		CryVSBind::Shutdown();
#endif
		// The script API goes first: it removes its player listeners and the skip input listener while the players
		// still exist (the manager destroys them below).
		ShutdownRunScript();
		Api::ShutdownFlowTest(); // its graph owns players: before the API registry goes
		Api::ShutdownApiTest();
		Contract::ShutdownSelfTest();
		if (m_pApi)
			m_pApi->Shutdown();
		DevProbesShutdown(); // measurement probes (S0): their textures and the cev_bench_play player
		// The manager releases the textures (the renderer is still alive at ESYSTEM_EVENT_FULL_SHUTDOWN /
		// FAST_SHUTDOWN) and the audio device.
		if (m_pManager)
			m_pManager->Shutdown();
		Contract::Close(); // the export stays callable until the process ends: query 0, hint ignored
		// Every player is gone now (the manager released them and its pool reference): join the shared decode
		// workers before the module can be unloaded.
		cev::DecodePool::ShutdownShared();
		LogDebug("bridge shut down, decode pool released");
	}

	void CVideoPlugin::OnSystemEvent(ESystemEvent event, UINT_PTR wparam, UINT_PTR lparam)
	{
		if (event == ESYSTEM_EVENT_FULL_SHUTDOWN || event == ESYSTEM_EVENT_FAST_SHUTDOWN)
		{
			ShutdownBridge();
			return;
		}

		if (m_pManager)
			m_pManager->OnSystemEvent(event, wparam); // level loading pause / resume, window focus

		if (gEnv->IsDedicated())
		{
			return;
		}

		switch (event)
		{
		case ESYSTEM_EVENT_GAME_POST_INIT:
		{
			if (m_pManager)
				m_pManager->RegisterLevelListener();
#if CEV_HAVE_CRYVS
			CryVSBind::Initialize(); // community tree only: gEnv->pCryVS exists from here on (nullptr = skipped)
#endif
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
			if (m_pApi)
				m_pApi->DestroyScriptOwned(); // players of flow graph nodes / scripts: their graphs are gone, nothing may stay on screen
			break;
		case ESYSTEM_EVENT_EDITOR_GAME_MODE_CHANGED:
		{
			if (wparam == 0 && m_pApi)
				m_pApi->DestroyScriptOwned(); // the flow graphs of the game mode are reset: their Screen players must not stay on screen
			break;
		}
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

	IVideoPlayer* CVideoPlugin::CreatePlayerForSurface(const char* surfaceId)
	{
		if (!CheckMainThread("CreatePlayerForSurface"))
			return nullptr;
		return m_pManager ? m_pManager->CreatePlayerForSurface(surfaceId) : nullptr;
	}

	IVideoPlayer* CVideoPlugin::FindPlayerBySurface(const char* surfaceId) const
	{
		if (!CheckMainThread("FindPlayerBySurface"))
			return nullptr;
		return m_pManager ? m_pManager->FindPlayerBySurface(surfaceId) : nullptr;
	}

	uint32_t CVideoPlugin::GetPlayerCount() const
	{
		return m_pManager ? m_pManager->GetPlayerCount() : 0u;
	}

	CRYREGISTER_SINGLETON_CLASS(CVideoPlugin)
}
