#include "StdAfx.h"
#include "PluginEnv.h"
#include "PlayerManager.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include <CryGame/IGameFramework.h>
#include <CryRenderer/IRenderer.h>
#include <ILevelSystem.h>

#include "Player/VideoPlayerImpl.h"
#include "Render/FullscreenOutput.h"
#include "Render/RenderBridge.h"

namespace CEVPlayer
{
	namespace
	{
		// Level loading flag safety net (seconds without any sign of life from the level system / the loading
		// screen events). A failed load only reports OnLoadingError; this covers whatever is not reported at all.
		constexpr float kLoadIdleTimeout = 120.f;
		constexpr float kLoadCompleteTimeout = 15.f; // after OnLoadingComplete only the precache steps are left

		float NowSeconds()
		{
			return gEnv != nullptr && gEnv->pTimer != nullptr ? gEnv->pTimer->GetAsyncCurTime() : 0.f;
		}

		// Scratch buffers of the per-frame output step (kept across frames: no allocation in steady state).
		std::vector<FullscreenOutput::SLayer> s_layers;
		std::vector<std::string> s_statsLines;
		std::vector<const char*> s_statsLinePtrs;
	}

	//! Listener of the player started by cev_play: reports and removes the player when it is over.
	class CPlayerManager::CConsoleListener final : public IVideoPlayerListener
	{
	public:
		explicit CConsoleListener(CPlayerManager& manager) : m_manager(manager) {}

		void OnReady(IVideoPlayer& player) override
		{
			LogAlways("cev_play: ready <%s> %dx%d %.2fs%s", player.GetPath(), player.GetWidth(), player.GetHeight(), player.GetDuration(), player.HasAlpha() ? " alpha" : "");
		}
		void OnEnded(IVideoPlayer& player) override
		{
			LogAlways("cev_play: ended <%s>", player.GetPath());
			m_manager.DestroyConsolePlayer();
		}
		void OnError(IVideoPlayer& player, int errorCode, const char* message) override
		{
			LogAlways("cev_play: error %d (%s) <%s>", errorCode, message != nullptr ? message : "", player.GetPath());
			m_manager.DestroyConsolePlayer();
		}

	private:
		CPlayerManager& m_manager;
	};

	std::string NormalizeTextureName(const char* name, bool* pChanged)
	{
		if (pChanged != nullptr)
			*pChanged = false;
		if (name == nullptr || *name == 0)
			return std::string();
		std::string result;
		if (name[0] != '$')
			result = "$";
		result += name;
		constexpr size_t kMaxNameLength = 100;
		if (result.size() > kMaxNameLength)
			result.resize(kMaxNameLength);
		if (pChanged != nullptr)
			*pChanged = result != name;
		return result;
	}

	//! Clears the loading flag when the level system reports a failed load (it sends no system event then).
	class CPlayerManager::CLevelListener final : public ILevelSystemListener
	{
	public:
		explicit CLevelListener(CPlayerManager& manager) : m_manager(manager) {}

		bool OnLoadingStart(ILevelInfo*) override { m_manager.NoteLevelActivity(kLoadIdleTimeout); return true; }
		void OnLoadingLevelEntitiesStart(ILevelInfo*) override { m_manager.NoteLevelActivity(kLoadIdleTimeout); }
		void OnLoadingProgress(ILevelInfo*, int) override { m_manager.NoteLevelActivity(kLoadIdleTimeout); }
		// Not the end yet: LEVEL_LOAD_END / GAMEPLAY_START follow after the precache steps, renderer calls stay
		// off until then. The short deadline only matters when those events never come.
		void OnLoadingComplete(ILevelInfo*) override { m_manager.NoteLevelActivity(kLoadCompleteTimeout); }
		void OnLoadingError(ILevelInfo*, const char*) override { m_manager.EndLevelLoading("level system: loading error"); }
		void OnUnloadComplete(ILevelInfo*) override { m_manager.EndLevelLoading("level system: unload complete"); }

	private:
		CPlayerManager& m_manager;
	};

	CPlayerManager::CPlayerManager()
		: m_consoleListener(std::make_unique<CConsoleListener>(*this))
		, m_levelListener(std::make_unique<CLevelListener>(*this))
	{
		m_players.reserve(16);
		m_snapshot.reserve(16);
		m_fullscreen.reserve(4);
	}

	CPlayerManager::~CPlayerManager()
	{
		Shutdown();
	}

	bool CPlayerManager::GlobalStatsEnabled() const
	{
		return mEnv->m_cevStats != 0 || mEnv->m_bDebugDraw != 0;
	}

	void CPlayerManager::RegisterLevelListener()
	{
		if (m_levelListenerRegistered || m_shutdown || gEnv == nullptr || gEnv->pGameFramework == nullptr)
			return;
		ILevelSystem* const pLevelSystem = gEnv->pGameFramework->GetILevelSystem();
		if (pLevelSystem == nullptr)
			return;
		pLevelSystem->AddListener(m_levelListener.get());
		m_levelListenerRegistered = true;
		Log("level system listener registered");
	}

	void CPlayerManager::UnregisterLevelListener()
	{
		if (!m_levelListenerRegistered)
			return;
		m_levelListenerRegistered = false;
		if (gEnv != nullptr && gEnv->pGameFramework != nullptr)
		{
			if (ILevelSystem* const pLevelSystem = gEnv->pGameFramework->GetILevelSystem())
				pLevelSystem->RemoveListener(m_levelListener.get());
		}
	}

	void CPlayerManager::BeginLevelLoading(const char* why)
	{
		if (m_levelLoading || m_shutdown)
			return;
		m_levelLoading = true;
		m_levelDeadline = NowSeconds() + kLoadIdleTimeout;
		LogDebug("level loading started (%s): clocks paused, no renderer calls", why);
		for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
			p->OnLevelLoadBegin();
	}

	void CPlayerManager::EndLevelLoading(const char* why)
	{
		if (!m_levelLoading)
			return;
		m_levelLoading = false;
		LogDebug("level loading finished (%s): resuming", why);
		for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
			p->OnLevelLoadEnd();
	}

	void CPlayerManager::NoteLevelActivity(float timeoutSeconds)
	{
		if (m_levelLoading)
			m_levelDeadline = NowSeconds() + timeoutSeconds;
	}

	bool CPlayerManager::TextureNameInUse(const std::string& name, const CVideoPlayerImpl* pSelf) const
	{
		for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
		{
			if (p.get() != pSelf && p->HasTexture() && stricmp(p->Name().c_str(), name.c_str()) == 0)
				return true;
		}
		for (const std::unique_ptr<CVideoPlayerImpl>& p : m_zombies)
		{
			if (p.get() != pSelf && p->HasTexture() && stricmp(p->Name().c_str(), name.c_str()) == 0)
				return true;
		}
		return false;
	}

	std::string CPlayerManager::MakeAutoTextureName(int width, int height, const CVideoPlayerImpl* pSelf) const
	{
		char buffer[64];
		for (int k = 0; k < 4096; ++k)
		{
			std::snprintf(buffer, sizeof(buffer), "$cev_video_%dx%d_%d", width, height, k);
			if (!TextureNameInUse(buffer, pSelf))
				break;
		}
		return buffer;
	}

	bool CPlayerManager::RenderCallsAllowed() const
	{
		return !m_shutdown && !m_levelLoading && RenderBridge::Available();
	}

	std::shared_ptr<cev::DecodePool> CPlayerManager::AcquirePool()
	{
		if (!m_pool)
			m_pool = cev::DecodePool::AcquireShared(static_cast<uint32_t>(std::max(0, mEnv->m_cevDecodeThreads)));
		return m_pool;
	}

	// ------------------------------------------------------------------------------------------------ factory
	CVideoPlayerImpl* CPlayerManager::Find(const IVideoPlayer* pPlayer) const
	{
		for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
		{
			if (p.get() == pPlayer)
				return p.get();
		}
		return nullptr;
	}

	bool CPlayerManager::IsPlayer(const IVideoPlayer* pPlayer) const
	{
		const CVideoPlayerImpl* p = Find(pPlayer);
		return p != nullptr && !p->IsDestroyed();
	}

	IVideoPlayer* CPlayerManager::CreatePlayer(const char* textureName)
	{
		if (m_shutdown)
			return nullptr;
		if (textureName != nullptr && textureName[0] != '\0')
		{
			const std::string normalized = NormalizeTextureName(textureName);
			for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
			{
				if (!p->IsDestroyed() && stricmp(p->GetTextureName(), normalized.c_str()) == 0)
				{
					LogWarning("texture name <%s> is already used by player %u, both would write the same texture", normalized.c_str(), p->Id());
					break;
				}
			}
		}
		m_players.push_back(std::make_unique<CVideoPlayerImpl>(*this, m_nextId++, textureName));
		return m_players.back().get();
	}

	void CPlayerManager::DestroyPlayer(IVideoPlayer* pPlayer)
	{
		if (pPlayer == nullptr)
			return;
		CVideoPlayerImpl* const impl = Find(pPlayer);
		if (impl == nullptr || impl->IsDestroyed())
			return;
		if (impl == m_pConsolePlayer)
			m_pConsolePlayer = nullptr;

		impl->PrepareDestroy();
		if (m_dispatching)
		{
			m_pendingDestroy.push_back(impl); // a listener callback is running: remove after the dispatch loop
			return;
		}

		for (std::vector<std::unique_ptr<CVideoPlayerImpl>>::iterator it = m_players.begin(); it != m_players.end(); ++it)
		{
			if (it->get() != impl)
				continue;
			std::unique_ptr<CVideoPlayerImpl> owned = std::move(*it);
			m_players.erase(it);
			if (owned->HasRenderResources())
			{
				if (RenderCallsAllowed())
					owned->ReleaseRenderResources();
				else
					m_zombies.push_back(std::move(owned)); // released at the first tick after the level load
			}
			break;
		}
	}

	void CPlayerManager::ReapPendingDestroy()
	{
		if (m_pendingDestroy.empty())
			return;
		std::vector<CVideoPlayerImpl*> pending;
		pending.swap(m_pendingDestroy);
		for (CVideoPlayerImpl* p : pending)
		{
			// PrepareDestroy() already ran: DestroyPlayer() would return early, so retire the entry directly.
			for (std::vector<std::unique_ptr<CVideoPlayerImpl>>::iterator it = m_players.begin(); it != m_players.end(); ++it)
			{
				if (it->get() != p)
					continue;
				std::unique_ptr<CVideoPlayerImpl> owned = std::move(*it);
				m_players.erase(it);
				if (owned->HasRenderResources())
				{
					if (RenderCallsAllowed())
						owned->ReleaseRenderResources();
					else
						m_zombies.push_back(std::move(owned));
				}
				break;
			}
		}
	}

	void CPlayerManager::ReapZombies(bool force)
	{
		if (m_zombies.empty())
			return;
		if (!force && !RenderCallsAllowed())
			return;
		for (std::unique_ptr<CVideoPlayerImpl>& z : m_zombies)
		{
			if (z->HasRenderResources() && RenderBridge::Available())
				z->ReleaseRenderResources();
		}
		m_zombies.clear();
	}

	void CPlayerManager::SetFullscreen(CVideoPlayerImpl* pPlayer, bool enable)
	{
		m_fullscreen.erase(std::remove(m_fullscreen.begin(), m_fullscreen.end(), pPlayer), m_fullscreen.end());
		if (enable)
			m_fullscreen.push_back(pPlayer); // later = on top
	}

	// ------------------------------------------------------------------------------------------------ frame
	void CPlayerManager::UpdateBeforeRender()
	{
		// No renderer calls and no frame stepping while a level loads or after shutdown. Clocks of playing
		// players are paused for the duration (OnSystemEvent), so nothing is lost.
		if (m_shutdown)
			return;
		if (m_levelLoading && NowSeconds() > m_levelDeadline)
		{
			LogWarning("level loading flag was not cleared by the engine for a long time: assuming the load ended");
			EndLevelLoading("timeout");
		}
		if (m_players.empty())
			s_statsLines.clear(); // no stale overlay lines once the last player is gone
		if (m_levelLoading)
			return;
		if (m_players.empty() && m_zombies.empty())
			return;

		ReapZombies(false);

		const bool wantStats = GlobalStatsEnabled();
		if (wantStats != m_statsApplied)
		{
			m_statsApplied = wantStats;
			for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
				p->SetStatsEnabled(wantStats);
		}

		for (size_t i = 0; i < m_players.size(); ++i)
			m_players[i]->Tick();

		// The one fixed dispatch point for listener callbacks.
		m_snapshot.clear();
		for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
			m_snapshot.push_back(p.get());
		m_dispatching = true;
		for (size_t i = 0; i < m_snapshot.size(); ++i)
		{
			if (!m_snapshot[i]->IsDestroyed())
				m_snapshot[i]->DispatchEvents();
		}
		m_dispatching = false;
		ReapPendingDestroy();

		if (wantStats)
			LogStats();
		else if (!s_statsLines.empty())
			s_statsLines.clear();
	}

	void CPlayerManager::UpdateAfterRender()
	{
		if (!RenderCallsAllowed())
			return;

		s_layers.clear();
		for (const CVideoPlayerImpl* p : m_fullscreen)
		{
			if (!p->IsLoaded() || p->GetTextureId() <= 0)
				continue;
			FullscreenOutput::SLayer layer;
			layer.textureId = p->GetTextureId();
			layer.textureWidth = p->GetWidth();
			layer.textureHeight = p->GetHeight();
			layer.fit = p->Fit();
			s_layers.push_back(layer);
		}
		if (!s_layers.empty())
			FullscreenOutput::Draw(s_layers.data(), s_layers.size());

		if (GlobalStatsEnabled() && !s_statsLines.empty())
		{
			s_statsLinePtrs.clear();
			for (const std::string& line : s_statsLines)
				s_statsLinePtrs.push_back(line.c_str());
			FullscreenOutput::DrawTextLines(s_statsLinePtrs.data(), s_statsLinePtrs.size());
		}
	}

	void CPlayerManager::LogStats()
	{
		const float now = gEnv != nullptr && gEnv->pTimer != nullptr ? gEnv->pTimer->GetAsyncCurTime() : 0.f;
		if (now < m_statsNextTime)
			return;
		m_statsNextTime = now + 1.f;

		s_statsLines.clear();
		char line[320];
		for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
		{
			if (!p->IsLoaded())
				continue;
			const cev::StatsSnapshot s = p->Core().GetStats();
			const uint64_t uploads = p->UploadCount();
			const uint64_t perSecond = uploads - p->StatsMark();
			p->StatsMark() = uploads;
			std::snprintf(line, sizeof(line),
				"[CEV stats] #%u t=%.1f/%.1f decodeMs avg=%.2f p99=%.2f convertMs avg=%.2f p99=%.2f uploads=%llu (+%llu/s) dropped=%llu allocs=%llu ring=%.1f",
				p->Id(), p->GetTime(), p->GetDuration(), s.decodeMs.avg, s.decodeMs.p99, s.convertMs.avg, s.convertMs.p99,
				static_cast<unsigned long long>(uploads), static_cast<unsigned long long>(perSecond),
				static_cast<unsigned long long>(s.framesDropped), static_cast<unsigned long long>(s.allocs), s.ringOccupancy.avg);
			s_statsLines.push_back(line);
			CryLogAlways("%s", line);
		}
	}

	// ------------------------------------------------------------------------------------------------ engine events
	void CPlayerManager::OnSystemEvent(ESystemEvent event)
	{
		switch (event)
		{
		case ESYSTEM_EVENT_LEVEL_LOAD_START_LOADINGSCREEN:
			BeginLevelLoading("loading screen started");
			break;
		case ESYSTEM_EVENT_LEVEL_LOAD_END:
		case ESYSTEM_EVENT_LEVEL_GAMEPLAY_START:
		case ESYSTEM_EVENT_LEVEL_LOAD_ERROR:
			EndLevelLoading("system event");
			break;
		case ESYSTEM_EVENT_FULL_SHUTDOWN:
		case ESYSTEM_EVENT_FAST_SHUTDOWN:
			Shutdown();
			break;
		default:
			break;
		}
	}

	void CPlayerManager::Shutdown()
	{
		if (m_shutdown)
			return;
		// The renderer is still alive when this runs (FULL_SHUTDOWN is sent before the plug-ins and the renderer
		// are released, FAST_SHUTDOWN just before the renderer is). Release every texture now.
		m_shutdown = true;
		UnregisterLevelListener();
		m_levelLoading = false;
		m_pConsolePlayer = nullptr;
		m_dispatching = false;

		for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
			p->PrepareDestroy();
		for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
		{
			if (p->HasRenderResources() && RenderBridge::Available())
				p->ReleaseRenderResources();
		}
		m_players.clear();
		m_pendingDestroy.clear();
		m_fullscreen.clear();
		ReapZombies(true);

		m_audio.Shutdown();
		m_pool.reset(); // joins the decode workers (no player holds the pool any more)
		s_statsLines.clear();
	}

	// ------------------------------------------------------------------------------------------------ console
	void CPlayerManager::ListPlayers() const
	{
		LogAlways("cev_list: %u player(s), render stage %d, audio voices %u (device %s), level loading %s",
			GetPlayerCount(), RenderBridge::Stage(), m_audio.VoiceCount(), m_audio.IsDeviceOpen() ? "open" : "closed", m_levelLoading ? "yes" : "no");
		char line[512];
		for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
		{
			if (p->IsDestroyed())
				continue;
			p->ListState(line, sizeof(line));
			LogAlways("  %s", line);
		}
	}

	void CPlayerManager::DestroyConsolePlayer()
	{
		IVideoPlayer* const p = m_pConsolePlayer;
		m_pConsolePlayer = nullptr;
		DestroyPlayer(p);
	}

	void CPlayerManager::ConsolePlay(const char* path, bool loop)
	{
		if (m_shutdown || path == nullptr || path[0] == '\0')
			return;
		DestroyConsolePlayer();
		IVideoPlayer* const p = CreatePlayer(nullptr); // generated, reusable texture name (fullscreen only)
		if (p == nullptr)
			return;
		m_pConsolePlayer = p;
		p->AddListener(m_consoleListener.get());
		p->SetLoop(loop);
		p->SetFullscreen(true, EVideoFit::Letterbox);
		LogAlways("cev_play: <%s>%s", path, loop ? " (loop)" : "");
		if (!p->Load(path))
		{
			LogAlways("cev_play: load rejected <%s>", path);
			DestroyConsolePlayer();
			return;
		}
		p->Play();
	}

	void CPlayerManager::ConsoleStop()
	{
		if (m_pConsolePlayer == nullptr)
		{
			LogAlways("cev_stop: no cev_play video is running");
			return;
		}
		LogAlways("cev_stop");
		DestroyConsolePlayer();
	}

	void CPlayerManager::ConsolePause()
	{
		if (m_pConsolePlayer == nullptr)
			LogAlways("cev_pause: no cev_play video is running");
		else
			m_pConsolePlayer->Pause();
	}

	void CPlayerManager::ConsoleResume()
	{
		if (m_pConsolePlayer == nullptr)
			LogAlways("cev_resume: no cev_play video is running");
		else
			m_pConsolePlayer->Resume();
	}

	void CPlayerManager::ConsoleSeek(double seconds)
	{
		if (m_pConsolePlayer == nullptr)
			LogAlways("cev_seek: no cev_play video is running");
		else
			m_pConsolePlayer->Seek(seconds);
	}
}
