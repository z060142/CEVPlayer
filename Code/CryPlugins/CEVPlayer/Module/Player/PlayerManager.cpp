#include "StdAfx.h"
#include "PluginEnv.h"
#include "PlayerManager.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include <CryGame/IGameFramework.h>
#include <CryRenderer/IRenderer.h>
#include <ILevelSystem.h>

#include "Console.h"
#include "Contract/TexSource.h"
#include "Player/VideoPlayerImpl.h"
#include "Render/FullscreenOutput.h"
#include "Render/RenderBridge.h"

// measurement (S0): process / GPU memory in the stats CSV
#include <CryCore/Platform/CryWindows.h>
#include <psapi.h>
#include <dxgi1_4.h>

namespace CEVPlayer
{
	namespace
	{
		// Level loading flag safety net (seconds without any sign of life from the level system / the loading
		// screen events). A failed load only reports OnLoadingError; this covers whatever is not reported at all.
		constexpr float kLoadIdleTimeout = 120.f;
		constexpr float kLoadCompleteTimeout = 15.f; // after OnLoadingComplete only the precache steps are left

		//! Main thread only (engine timer): level loading bookkeeping and the stats file name stamp, never the tick.
		float NowSeconds()
		{
			return gEnv != nullptr && gEnv->pTimer != nullptr ? gEnv->pTimer->GetAsyncCurTime() : 0.f;
		}

		// Scratch buffers of the per-frame output step (kept across frames: no allocation in steady state).
		std::vector<FullscreenOutput::SLayer> s_layers;

		std::vector<Contract::SCommand> s_contractCommands; // scratch of ProcessContractCommands

		// ---- measurement (S0): cev_stats_file
		FILE* s_statsFile = nullptr;
		float s_statsFileStart = 0.f;

		double ProcessMemoryMb(bool privateBytes)
		{
			PROCESS_MEMORY_COUNTERS_EX pmc = {};
			pmc.cb = sizeof(pmc);
			// K32GetProcessMemoryInfo lives in kernel32 (Windows 7+); looked up so no psapi.lib link is needed.
			using PFN_GetPmi = BOOL(WINAPI*)(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD);
			static const PFN_GetPmi s_getPmi = reinterpret_cast<PFN_GetPmi>(::GetProcAddress(::GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo"));
			if (s_getPmi == nullptr || !s_getPmi(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
				return -1.0;
			return static_cast<double>(privateBytes ? pmc.PrivateUsage : pmc.WorkingSetSize) / (1024.0 * 1024.0);
		}

		//! M9 measurement: whole-process CPU time / wall time since the previous call, in % of one core (-1 the first
		//! time). Called once per stats second.
		double ProcessCpuPercent()
		{
			static ULONGLONG s_lastCpu = 0;
			static LARGE_INTEGER s_lastWall = {};
			FILETIME c, e, k, u;
			if (!::GetProcessTimes(::GetCurrentProcess(), &c, &e, &k, &u))
				return -1.0;
			const ULONGLONG cpu = (static_cast<ULONGLONG>(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (static_cast<ULONGLONG>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
			LARGE_INTEGER now, freq;
			::QueryPerformanceCounter(&now);
			::QueryPerformanceFrequency(&freq);
			double pct = -1.0;
			if (s_lastWall.QuadPart != 0 && now.QuadPart > s_lastWall.QuadPart)
			{
				const double wall = static_cast<double>(now.QuadPart - s_lastWall.QuadPart) / static_cast<double>(freq.QuadPart);
				pct = static_cast<double>(cpu - s_lastCpu) * 1e-7 / wall * 100.0;
			}
			s_lastCpu = cpu;
			s_lastWall = now;
			return pct;
		}

		//! This process's local video memory use on adapter 0 (IDXGIAdapter3::QueryVideoMemoryInfo), MB; -1 = unavailable.
		double ProcessGpuMemoryMb()
		{
			static IDXGIAdapter3* s_adapter = nullptr;
			static bool s_tried = false;
			if (!s_tried)
			{
				s_tried = true;
				if (HMODULE hDxgi = ::LoadLibraryA("dxgi.dll"))
				{
					using PFN_Create = HRESULT(WINAPI*)(REFIID, void**);
					if (PFN_Create pCreate = reinterpret_cast<PFN_Create>(::GetProcAddress(hDxgi, "CreateDXGIFactory1")))
					{
						IDXGIFactory1* pFactory = nullptr;
						if (SUCCEEDED(pCreate(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&pFactory))) && pFactory != nullptr)
						{
							IDXGIAdapter1* pAdapter = nullptr;
							if (SUCCEEDED(pFactory->EnumAdapters1(0, &pAdapter)) && pAdapter != nullptr)
							{
								pAdapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&s_adapter));
								pAdapter->Release();
							}
							pFactory->Release();
						}
					}
				}
			}
			if (s_adapter == nullptr)
				return -1.0;
			DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
			if (FAILED(s_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
				return -1.0;
			return static_cast<double>(info.CurrentUsage) / (1024.0 * 1024.0);
		}
	}

	double CPlayerManager::ProcessPrivateMb() { return ProcessMemoryMb(true); }
	double CPlayerManager::ProcessGpuMb() { return ProcessGpuMemoryMb(); }

	void CPlayerManager::SetStatsFile(const char* path)
	{
		if (s_statsFile != nullptr)
		{
			std::fclose(s_statsFile);
			s_statsFile = nullptr;
			LogAlways("cev_stats_file: closed");
		}
		if (path == nullptr || path[0] == '\0')
			return;
		s_statsFile = std::fopen(path, "w");
		if (s_statsFile == nullptr)
		{
			LogAlways("cev_stats_file: cannot open <%s>", path);
			return;
		}
		s_statsFileStart = static_cast<float>(m_clock);
		std::fprintf(s_statsFile, "wall_s,player,t,width,height,alpha,decode_avg_ms,decode_p99_ms,copy_avg_ms,copy_p99_ms,color_avg_ms,color_p99_ms,"
			"convert_avg_ms,convert_p99_ms,upload_avg_ms,upload_p99_ms,uploads_per_s,upload_bytes_per_frame,upload_bytes_per_s,frames_decoded,dropped,ring_avg,"
			"proc_ws_mb,proc_private_mb,gpu_local_mb,players,"
			"visibility,decode_only_frames,copy_bytes,color_bytes,catchups,catchup_last_ms,catchup_last_frames,variant,hint_consumers,hint_px_h,layout,rgba_buffers,"
			"decode,decode_hw,readback_avg_ms,readback_p99_ms,proc_cpu_pct\n");
		// baseline row (player 0): memory before / without the measured players
		std::fprintf(s_statsFile, "0.00,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,%.1f,%.1f,%.1f,%u,-,0,0,0,0,0,0,0,0,0,-,0,-,0,0,0,0\n", ProcessMemoryMb(false), ProcessMemoryMb(true),
			ProcessGpuMemoryMb(), GetPlayerCount());
		std::fflush(s_statsFile);
		LogAlways("cev_stats_file: writing <%s> (one row per player per second while cev_stats is on)", path);
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
		void OnMarker(IVideoPlayer& player, const char* name, double time) override
		{
			LogAlways("cev_play: marker <%s> time=%.3f (player time %.3f)", name != nullptr ? name : "", time, player.GetTime());
		}
		void OnLooped(IVideoPlayer& player) override
		{
			LogAlways("cev_play: looped <%s>", player.GetPath());
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

#if CEV_HAVE_LOADTIME_CALLBACKS
	//! M10: the order-100 callback of the renderer's loading loop (engine patch 17). Its update is the players' tick
	//! while the main thread is held by a level load.
	class CPlayerManager::CLoadtimeCallback final : public ILoadtimeCallback
	{
	public:
		explicit CLoadtimeCallback(CPlayerManager& manager) : m_manager(manager) {}
		void LoadtimeUpdate(float dt) override { m_manager.LoadtimeTick(dt); }
		bool LoadtimeRender() override { return true; }

	private:
		CPlayerManager& m_manager;
	};
#else
	class CPlayerManager::CLoadtimeCallback final {};
#endif

	CPlayerManager::CPlayerManager()
		: m_consoleListener(std::make_unique<CConsoleListener>(*this))
		, m_levelListener(std::make_unique<CLevelListener>(*this))
#if CEV_HAVE_LOADTIME_CALLBACKS
		, m_loadtimeCallback(std::make_unique<CLoadtimeCallback>(*this))
#endif
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
		return mEnv->m_cevStats != 0;
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
		const bool loadMode = LoadModeActive();
		LogDebug("level loading started (%s): %s", why, loadMode ? "texture-only players keep running in the loading loop" : "clocks paused, no renderer calls");
		for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
			p->OnLevelLoadBegin(loadMode && p->RunsDuringLoad());
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
		// During a level load only the loading-loop tick may call the renderer (M10); calls from the main thread
		// (e.g. destroying a player) are still deferred (zombies).
		return !m_shutdown && RenderBridge::Available() && (!m_levelLoading || m_inLoadTick);
	}

	bool CPlayerManager::LoadModeActive() const
	{
		return mEnv->m_cevLoadtime != 0 && m_loadtimeRegistered;
	}

	namespace
	{
		thread_local int t_tickLockDepth = 0; // CTickLock depth of this thread
	}

	CPlayerManager::CTickLock::CTickLock(std::recursive_mutex& m) : m_mutex(m)
	{
		m_mutex.lock();
		++t_tickLockDepth;
	}

	CPlayerManager::CTickLock::~CTickLock()
	{
		--t_tickLockDepth;
		m_mutex.unlock();
	}

	int CPlayerManager::CTickLock::HeldByThisThread() { return t_tickLockDepth; }

	void CPlayerManager::UpdateLoadtimeRegistration(bool forceRemove)
	{
#if CEV_HAVE_LOADTIME_CALLBACKS
		if (!IsMainThread())
			return;
		if (CTickLock::HeldByThisThread() > 0)
		{
			// Add / Remove wait for the renderer's registry lock, which its dispatch holds while our callback waits for
			// the tick lock: never call them under it.
			static bool s_reported = false;
			if (!s_reported)
			{
				s_reported = true;
				LogError("defect: load-time registration attempted under the tick lock");
			}
			return;
		}
		bool want = false;
		{
			CTickLock lock(m_tickMutex); // decision only
			want = !forceRemove && !m_shutdown && mEnv->m_cevLoadtime != 0 && RenderBridge::LoadtimeAvailable()
				&& (!m_players.empty() || Contract::HasPendingCommands());
		}
		// engine call without any lock held
		if (want == m_loadtimeRegistered.load() || gEnv == nullptr || gEnv->pRenderer == nullptr)
			return;
		if (want)
			gEnv->pRenderer->AddLoadtimeCallback(m_loadtimeCallback.get(), 100);
		else
			gEnv->pRenderer->RemoveLoadtimeCallback(m_loadtimeCallback.get());
		m_loadtimeRegistered = want;
		LogDebug("loading-loop callback %s", want ? "registered (order 100)" : "removed");
#endif
	}

	void CPlayerManager::LoadtimeTick(float dt)
	{
		CTickLock lock(m_tickMutex);
		if (m_shutdown || mEnv->m_cevLoadtime == 0)
			return;
		dt = std::min(std::max(dt, 0.f), 0.25f); // a longer gap is a stall: the clocks do not catch up
		m_clock += static_cast<double>(dt);
		m_inLoadTick = true;
		ProcessContractCommands();
		for (size_t i = 0; i < m_players.size(); ++i)
		{
			if (m_players[i]->RunsDuringLoad())
				m_players[i]->Tick(dt);
		}
		m_inLoadTick = false;
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

	bool CPlayerManager::IsValidSurfaceId(const char* surfaceId)
	{
		if (surfaceId == nullptr)
			return false;
		size_t n = 0;
		for (const char* c = surfaceId; *c != '\0'; ++c, ++n)
		{
			const char ch = *c;
			const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == ':' || ch == '-';
			if (!ok || n >= 63)
				return false;
		}
		return n >= 1;
	}

	IVideoPlayer* CPlayerManager::FindPlayerBySurface(const char* surfaceId) const
	{
		if (surfaceId == nullptr || surfaceId[0] == '\0')
			return nullptr;
		for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
		{
			if (!p->IsDestroyed() && p->GetSurfaceId()[0] != '\0' && stricmp(p->GetSurfaceId(), surfaceId) == 0)
				return p.get();
		}
		return nullptr;
	}

	IVideoPlayer* CPlayerManager::CreatePlayerForSurface(const char* surfaceId)
	{
		CTickLock lock(m_tickMutex);
		if (m_shutdown)
			return nullptr;
		if (!IsValidSurfaceId(surfaceId))
		{
			LogError("surface id <%s> is invalid (1 to 63 characters of A-Z a-z 0-9 _ . : -), no player created", surfaceId != nullptr ? surfaceId : "");
			return nullptr;
		}
		if (const IVideoPlayer* const pExisting = FindPlayerBySurface(surfaceId))
		{
			LogError("surface id <%s> is already used by player %u, no player created", surfaceId, static_cast<const CVideoPlayerImpl*>(pExisting)->Id());
			return nullptr;
		}
		m_players.push_back(std::make_unique<CVideoPlayerImpl>(*this, m_nextId++, nullptr, surfaceId));
		return m_players.back().get(); // registration follows at the next call site (UpdateBeforeRender, load events)
	}

	IVideoPlayer* CPlayerManager::CreatePlayer(const char* textureName)
	{
		CTickLock lock(m_tickMutex);
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
		return m_players.back().get(); // registration follows at the next call site (UpdateBeforeRender, load events)
	}

	void CPlayerManager::DestroyPlayer(IVideoPlayer* pPlayer)
	{
		if (pPlayer == nullptr)
			return;
		CTickLock lock(m_tickMutex);
		CVideoPlayerImpl* const impl = Find(pPlayer);
		if (impl == nullptr || impl->IsDestroyed())
			return;
		if (impl == m_pConsolePlayer)
			m_pConsolePlayer = nullptr;
		m_consoleSurfaces.erase(std::remove(m_consoleSurfaces.begin(), m_consoleSurfaces.end(), static_cast<IVideoPlayer*>(impl)), m_consoleSurfaces.end());

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
	void CPlayerManager::CheckLevelLoadingTimeout()
	{
		CTickLock lock(m_tickMutex);
		if (m_shutdown)
			return;
		if (m_levelLoading && NowSeconds() > m_levelDeadline)
		{
			LogWarning("level loading flag was not cleared by the engine for a long time: assuming the load ended");
			EndLevelLoading("timeout");
		}
	}

	void CPlayerManager::UpdateBeforeRender(float dt)
	{
		// No renderer calls and no frame stepping while a level loads or after shutdown. Clocks of playing
		// players are paused for the duration (OnSystemEvent), so nothing is lost; texture-only players run in the
		// loading loop instead (LoadtimeTick, M10).
		UpdateLoadtimeRegistration(); // before the tick lock (the engine call must not run under it)
		CTickLock lock(m_tickMutex);
		if (m_shutdown)
			return;
		m_clock += static_cast<double>(dt);
		if (m_levelLoading)
			return;
		ProcessContractCommands(); // may create the first player
		CheckDecoderShutdown();
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
			m_players[i]->Tick(dt);

		// The one fixed dispatch point for listener callbacks. Main thread only (callbacks reach entity / flow /
		// Schematyc code); the load-time tick (M10) must not dispatch here.
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
	}

	void CPlayerManager::ProcessContractCommands()
	{
		// Runs in the manager's own tick: the only place where contract requests reach the players (rule 10). No
		// listener is called from here (Load / Play only queue events, they are dispatched at the fixed point).
		Contract::TakeCommands(s_contractCommands);
		for (const Contract::SCommand& c : s_contractCommands)
		{
			CVideoPlayerImpl* p = static_cast<CVideoPlayerImpl*>(FindPlayerBySurface(c.name.c_str()));
			switch (c.type)
			{
			case Contract::SCommand::EType::Open:
			{
				if (p != nullptr && !p->OwnedByOpen())
				{
					// level logic created the same id between the open and this tick: it keeps it
					LogWarning("contract: open <%s>: the surface is owned by level logic, request ignored", c.name.c_str());
					Contract::SetOwnedByOpen(c.name.c_str(), false, nullptr);
					break;
				}
				if (p == nullptr)
				{
					p = static_cast<CVideoPlayerImpl*>(CreatePlayerForSurface(c.name.c_str()));
					if (p == nullptr)
					{
						Contract::PublishOpenError(c.name.c_str(), static_cast<int32_t>(cev::Status::InvalidState));
						break;
					}
				}
				p->SetOwnedByOpen(true);
				Contract::SetOwnedByOpen(c.name.c_str(), true, c.src.c_str());
				Log("contract: open <%s> <%s> flags 0x%x volume %.2f", c.name.c_str(), c.src.c_str(), c.openFlags, static_cast<double>(c.volume));
				p->SetLoop((c.openFlags & 2u) != 0);
				p->SetMuted((c.openFlags & 4u) != 0);
				p->SetVolume(c.volume);
				p->Load(c.src.c_str()); // a failure shows as state 6 (error) in the snapshot
				if ((c.openFlags & 1u) != 0)
					p->Play();
				break;
			}
			case Contract::SCommand::EType::Close:
				if (p != nullptr && p->OwnedByOpen())
				{
					Log("contract: close <%s>", c.name.c_str());
					DestroyPlayer(p);
				}
				else if (p == nullptr)
				{
					Contract::UnregisterSurface(c.name.c_str()); // opened and closed before any player existed
				}
				break;
			case Contract::SCommand::EType::Control:
				if (p == nullptr)
					break;
				switch (c.verb)
				{
				case CTS_VERB_PLAY:   p->Play(); break;
				case CTS_VERB_PAUSE:  p->Pause(); break;
				case CTS_VERB_SEEK:   p->Seek(c.value); break; // remembered while loading
				case CTS_VERB_RATE:   p->SetRate(static_cast<float>(c.value)); break;
				case CTS_VERB_LOOP:   p->SetLoop(c.value != 0.0); break;
				case CTS_VERB_VOLUME: p->SetVolume(static_cast<float>(c.value)); break;
				case CTS_VERB_MUTE:   p->SetMuted(c.value != 0.0); break;
				default: break;
				}
				break;
			}
		}
		s_contractCommands.clear();
	}

	void CPlayerManager::UpdateAfterRender()
	{
		CTickLock lock(m_tickMutex);
		if (!RenderCallsAllowed())
			return;

		s_layers.clear();
		for (const CVideoPlayerImpl* p : m_fullscreen)
		{
			if (!p->IsLoaded() || p->GetTextureId() <= 0 || p->FullscreenHidden())
				continue;
			const CVideoPlayerImpl::SPackedParams packed = p->PackedParams();
			FullscreenOutput::SLayer layer;
			layer.textureId = p->GetTextureId();
			layer.imageWidth = p->GetWidth();
			layer.imageHeight = p->GetHeight();
			layer.fit = p->Fit();
			layer.layout = packed.packed ? p->GetPixelLayout() : EVideoPixelLayout::RGBA8;
			layer.hasAlpha = p->HasAlpha();
			for (int i = 0; i < 4; ++i)
			{
				layer.planeY[i] = static_cast<uint32>(packed.planeY[i]);
				layer.planeU[i] = static_cast<uint32>(packed.planeU[i]);
				layer.planeV[i] = static_cast<uint32>(packed.planeV[i]);
				layer.planeA[i] = static_cast<uint32>(packed.planeA[i]);
			}
			layer.colorMatrix = packed.colorMatrix;
			layer.colorRange = packed.colorRange;
			layer.chromaSiting = packed.chromaSiting;
			s_layers.push_back(layer);
		}
		const FullscreenOutput::SDebugLayer* pDebug = nullptr;
		size_t debugCount = 0;
		DevProbesDebugLayers(pDebug, debugCount);
		FullscreenOutput::Present(s_layers.data(), s_layers.size(), pDebug, debugCount);
	}

	void CPlayerManager::LogStats()
	{
		const float now = static_cast<float>(m_clock);
		if (now < m_statsNextTime)
			return;
		m_statsNextTime = now + 1.f;

		const double cpuPct = ProcessCpuPercent(); // one value per second for all rows
		char line[800];
		for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
		{
			if (!p->IsLoaded())
				continue;
			const cev::StatsSnapshot s = p->Core().GetStats();
			const uint64_t uploads = p->UploadCount();
			const uint64_t perSecond = uploads - p->StatsMark();
			p->StatsMark() = uploads;
			double avOffsetMs = 0.0;
			const bool haveAvOffset = p->GetAvOffsetMs(avOffsetMs);
			float uploadAvg = 0.f, uploadP99 = 0.f;
			uint32_t uploadSamples = 0;
			p->TakeUploadTiming(uploadAvg, uploadP99, uploadSamples);
			const cev::DecodeBackendInfo backend = p->DecodeInfo();
			char decode[64];
			p->DecodeText(decode, sizeof(decode), false);
			std::snprintf(line, sizeof(line),
				"[CEV stats] #%u t=%.1f/%.1f decodeMs avg=%.2f p99=%.2f convertMs avg=%.2f p99=%.2f (copy %.2f/%.2f color %.2f/%.2f) uploadMs avg=%.2f p99=%.2f uploads=%llu (+%llu/s) dropped=%llu allocs=%llu ring=%.1f clock=%s avOffsetMs=%.1f"
				" | %s decodeOnly=%llu copyB=%llu colorB=%llu catchUp=%llu last %.2fms/%uf variant=%d hints=%u px_h=%d layout=%s rgbaBuffers=%u"
				" | decode=%s hw=%d readbackMs avg=%.2f p99=%.2f procCpu=%.0f%%",
				p->Id(), p->GetTime(), p->GetDuration(), s.decodeMs.avg, s.decodeMs.p99, s.convertMs.avg, s.convertMs.p99,
				s.copyMs.avg, s.copyMs.p99, s.colorMs.avg, s.colorMs.p99, uploadAvg, uploadP99,
				static_cast<unsigned long long>(uploads), static_cast<unsigned long long>(perSecond),
				static_cast<unsigned long long>(s.framesDropped), static_cast<unsigned long long>(s.allocs), s.ringOccupancy.avg,
				p->ClockSourceName(), haveAvOffset ? avOffsetMs : 0.0,
				p->VisibilityName(), static_cast<unsigned long long>(s.framesDecodeOnly), static_cast<unsigned long long>(s.copyBytes),
				static_cast<unsigned long long>(s.colorBytes), static_cast<unsigned long long>(s.catchUps), static_cast<double>(s.lastCatchUpMs),
				s.lastCatchUpFrames, p->GetVariant(), p->HintConsumers(), p->HintPixelHeight(), p->LayoutName(), p->Core().RgbaBufferCount(),
				decode, backend.hardware ? 1 : 0, s.readbackMs.avg, s.readbackMs.p99, cpuPct);
			if (s_statsFile != nullptr)
			{
				const uint64_t bytes = p->LastUploadBytes();
				std::fprintf(s_statsFile, "%.2f,%u,%.3f,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%llu,%llu,%llu,%llu,%.2f,%.1f,%.1f,%.1f,%u,%s,%llu,%llu,%llu,%llu,%.3f,%u,%d,%u,%d,%s,%u,%s,%d,%.3f,%.3f,%.1f\n",
					now - s_statsFileStart, p->Id(), p->GetTime(), p->GetWidth(), p->GetHeight(), p->HasAlpha() ? 1 : 0,
					s.decodeMs.avg, s.decodeMs.p99, s.copyMs.avg, s.copyMs.p99, s.colorMs.avg, s.colorMs.p99, s.convertMs.avg, s.convertMs.p99,
					uploadAvg, uploadP99, static_cast<unsigned long long>(perSecond), static_cast<unsigned long long>(bytes),
					static_cast<unsigned long long>(bytes * perSecond), static_cast<unsigned long long>(s.framesDecoded),
					static_cast<unsigned long long>(s.framesDropped), s.ringOccupancy.avg,
					ProcessMemoryMb(false), ProcessMemoryMb(true), ProcessGpuMemoryMb(), GetPlayerCount(),
					p->VisibilityName(), static_cast<unsigned long long>(s.framesDecodeOnly), static_cast<unsigned long long>(s.copyBytes),
					static_cast<unsigned long long>(s.colorBytes), static_cast<unsigned long long>(s.catchUps), static_cast<double>(s.lastCatchUpMs),
					s.lastCatchUpFrames, p->GetVariant(), p->HintConsumers(), p->HintPixelHeight(), p->LayoutName(), p->Core().RgbaBufferCount(),
					decode, backend.hardware ? 1 : 0, s.readbackMs.avg, s.readbackMs.p99, cpuPct);
				std::fflush(s_statsFile);
			}
			CryLogAlways("%s", line);
		}
	}

	// ------------------------------------------------------------------------------------------------ engine events
	void CPlayerManager::OnSystemEvent(ESystemEvent event, UINT_PTR wparam)
	{
		// Registration before the tick lock: LOADINGSCREEN before BeginLevelLoading (it decides who keeps running),
		// LEVEL_LOAD_START after every listener's LOADINGSCREEN handling (catches an `open` sent while a UI warmed up).
		if (event == ESYSTEM_EVENT_LEVEL_LOAD_START_LOADINGSCREEN || event == ESYSTEM_EVENT_LEVEL_LOAD_START)
			UpdateLoadtimeRegistration();
		if (event == ESYSTEM_EVENT_FULL_SHUTDOWN || event == ESYSTEM_EVENT_FAST_SHUTDOWN)
		{
			Shutdown(); // takes the lock itself, after removing the callback
			return;
		}
		CTickLock lock(m_tickMutex);
		switch (event)
		{
		case ESYSTEM_EVENT_CHANGE_FOCUS:
			m_audio.SetFocus(wparam != 0); // muted while unfocused when cev_muteOnFocusLoss is set
			break;
		case ESYSTEM_EVENT_LEVEL_LOAD_START_LOADINGSCREEN:
			BeginLevelLoading("loading screen started");
			break;
		case ESYSTEM_EVENT_LEVEL_LOAD_END:
		case ESYSTEM_EVENT_LEVEL_GAMEPLAY_START:
		case ESYSTEM_EVENT_LEVEL_LOAD_ERROR:
			EndLevelLoading("system event");
			break;
		case ESYSTEM_EVENT_LEVEL_UNLOAD:
			// The engine drops the level's audio data: cue triggers are stopped (the pictures go on silently).
			for (const std::unique_ptr<CVideoPlayerImpl>& p : m_players)
				p->OnLevelUnload();
			break;
		default:
			break;
		}
	}

	//! M9: a Media Foundation decode thread that did not stop within its bounded join (2 s) was left running
	//! detached; that is a defect to report.
	void CPlayerManager::CheckDecoderShutdown()
	{
		const uint32_t timeouts = cev::MediaFoundationShutdownTimeouts();
		if (timeouts == m_mfTimeoutsLogged)
			return;
		m_mfTimeoutsLogged = timeouts;
		LogError("defect: %u Media Foundation decode thread(s) did not stop within 2 s and were left running detached", timeouts);
	}

	void CPlayerManager::Shutdown()
	{
		if (m_shutdown)
			return;
		UpdateLoadtimeRegistration(true); // remove the loading-loop callback first, without the tick lock
		CTickLock lock(m_tickMutex);
		if (m_shutdown)
			return;
		// The renderer is still alive when this runs (FULL_SHUTDOWN is sent before the plug-ins and the renderer
		// are released, FAST_SHUTDOWN just before the renderer is). Release every texture now.
		m_shutdown = true;
		UnregisterLevelListener();
		m_levelLoading = false;
		m_pConsolePlayer = nullptr;
		m_consoleSurfaces.clear();
		m_dispatching = false;

		FullscreenOutput::Shutdown(); // the stage lets go of the player textures before they are released
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
		CheckDecoderShutdown();

		m_audio.Shutdown();
		m_pool.reset(); // drops the manager's reference only: DecodePool::ShutdownShared() in CVideoPlugin::ShutdownBridge(),
		                 // right after this at the engine's shutdown event, drops the last one and joins the decode workers
		SetStatsFile(nullptr);
	}

	// ------------------------------------------------------------------------------------------------ console
	void CPlayerManager::ListPlayers() const
	{
		LogAlways("cev_list: %u player(s), render stage %d, audio voices %u (device %s%s%s), level loading %s",
			GetPlayerCount(), RenderBridge::Stage(), m_audio.VoiceCount(), m_audio.IsDeviceOpen() ? "open" : "closed",
			mEnv->m_cevDisableEmbeddedAudio != 0 ? ", cev_disableEmbeddedAudio" : "", m_audio.DeviceFailed() ? ", device failed" : "", m_levelLoading ? "yes" : "no");
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

	void CPlayerManager::ConsolePlaySurface(const char* surfaceId, const char* path, bool loop)
	{
		if (m_shutdown || surfaceId == nullptr)
			return;
		IVideoPlayer* p = FindPlayerBySurface(surfaceId);
		if (path == nullptr || path[0] == '\0')
		{
			if (p == nullptr)
			{
				LogAlways("cev_play_surface: no surface <%s>", surfaceId);
				return;
			}
			LogAlways("cev_play_surface: <%s> destroyed", surfaceId);
			DestroyPlayer(p);
			return;
		}
		if (p == nullptr)
		{
			p = CreatePlayerForSurface(surfaceId);
			if (p == nullptr)
				return;
			m_consoleSurfaces.push_back(p);
		}
		p->SetLoop(loop);
		LogAlways("cev_play_surface: <%s> texture <%s> plays <%s>%s", surfaceId, p->GetTextureName(), path, loop ? " (loop)" : "");
		if (!p->Load(path))
		{
			LogAlways("cev_play_surface: load rejected <%s>", path);
			return;
		}
		p->Play();
	}

	void CPlayerManager::ConsoleSeek(double seconds)
	{
		if (m_pConsolePlayer == nullptr)
			LogAlways("cev_seek: no cev_play video is running");
		else
			m_pConsolePlayer->Seek(seconds);
	}
}
