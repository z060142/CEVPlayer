// Owns all bridge players, ticks them once per frame, dispatches their events at a fixed point and keeps
// renderer calls away from level loading and shutdown. Main thread only, except UpdateBeforeRender(dt), which does
// not assume the main thread: its time comes from the caller.
#pragma once

#include <cstdint>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <CrySystem/ISystem.h>

#include <CEVPlayer/IVideoPlayer.h>

#include "Audio/AudioMixer.h"
#include "CEVCore/DecodePool.h"
#include "Render/RenderBridge.h"

namespace CEVPlayer
{
	class CVideoPlayerImpl;

	class CPlayerManager final
	{
	public:
		CPlayerManager();
		~CPlayerManager();
		CPlayerManager(const CPlayerManager&) = delete;
		CPlayerManager& operator=(const CPlayerManager&) = delete;

		// ---- factory ----
		IVideoPlayer* CreatePlayer(const char* textureName);
		//! Surface player (M6): id = [A-Za-z0-9_.:-]{1,63}, unique case-insensitively. nullptr + error when the id is
		//! invalid or taken, or after shutdown. Its texture is "$cev:<id>".
		IVideoPlayer* CreatePlayerForSurface(const char* surfaceId);
		IVideoPlayer* FindPlayerBySurface(const char* surfaceId) const;
		static bool IsValidSurfaceId(const char* surfaceId);
		void DestroyPlayer(IVideoPlayer* pPlayer);
		uint32_t GetPlayerCount() const { return static_cast<uint32_t>(m_players.size()); }
		bool IsPlayer(const IVideoPlayer* pPlayer) const;

		// ---- frame hooks (plug-in update steps) ----
		//! Main thread only (reads the engine timer): drops a level loading flag the engine never cleared. Called by
		//! the plug-in right before UpdateBeforeRender.
		void CheckLevelLoadingTimeout();
		//! UpdateBeforeRender: ticks the players (texture uploads), then dispatches listener events. `dt` = seconds
		//! since the previous call, given by the caller; no engine timer or frame id is read inside.
		void UpdateBeforeRender(float dt);
		//! End of the plug-in's UpdateBeforeRender (after the players' and the probes' texture uploads): hands the
		//! fullscreen outputs (and the debug layers of the probes) to FullscreenOutput, which takes them here when its
		//! backend is XUI2D.
		void PresentFullscreenBeforeRender();
		//! UpdateAfterRender: the same for the XUI2DL backend, which takes them here.
		void UpdateAfterRender();
		//! M10: the tick of the renderer's loading loop (render thread: LoadtimeUpdate of the order-100 callback with
		//! patch 17, the XUI2DLApi::SetLoadtimeUpdate update with XUI2DL). Applies
		//! the queued contract commands and ticks the players that run during loads (RunsDuringLoad). No listener
		//! dispatch: events stay queued until the first UpdateBeforeRender after the load. No engine timer is read.
		void LoadtimeTick(float dt);
		//! M10: adds / removes the loading-loop callback depending on intent (live players or queued contract commands).
		//! Main thread only; a call from another thread returns at once. The decision is made under the tick lock, the
		//! engine call (Add / Remove, or XUI2DLApi::SetLoadtimeUpdate) without any lock: it must never run while this
		//! thread holds the tick lock (the renderer's dispatch holds its registry lock while our callback waits for the
		//! tick lock). The call goes to RenderBridge::LoadtimeSource() (L3a); removal goes to the source registered
		//! with. `forceRemove`: shutdown. L6: the same step adds / removes the XUI2D backend's loop drawing callback
		//! (order 150) while a fullscreen player exists and FullscreenOutput::LoopDrawAvailable().
		void UpdateLoadtimeRegistration(bool forceRemove = false);
		//! M10: players that output only to their texture keep running during level loads (cev_loadtime, callback
		//! registered).
		bool LoadModeActive() const;
		bool LoadtimeRegistered() const { return m_loadtimeRegistered; }
		//! L3a, development only (cev_probe_loadtime): with the XUI2DL source (one registrant) the probe cannot register
		//! its own update, so the manager's XUI2DL update calls `pHook(dt)` after LoadtimeTick. nullptr removes it.
		static void SetLoadtimeProbeHook(void (*pHook)(float dt));
		//! M10: the lock every tick entry holds (main thread entries and the loading-loop tick never run at once). Take
		//! it through CTickLock so the registration step can tell that this thread holds it.
		std::recursive_mutex& TickMutex() { return m_tickMutex; }
		class CTickLock
		{
		public:
			explicit CTickLock(std::recursive_mutex& m);
			~CTickLock();
			CTickLock(const CTickLock&) = delete;
			CTickLock& operator=(const CTickLock&) = delete;
			//! Depth of CTickLock held by the calling thread.
			static int HeldByThisThread();
		private:
			std::recursive_mutex& m_mutex;
		};

		// ---- engine events ----
		void OnSystemEvent(ESystemEvent event, UINT_PTR wparam = 0);
		//! Level system callbacks (failed loads only report OnLoadingError, no system event): registered once the
		//! game framework exists, removed at shutdown. Both are idempotent.
		void RegisterLevelListener();
		void UnregisterLevelListener();
		//! FULL_SHUTDOWN / FAST_SHUTDOWN (renderer still alive): releases everything. Idempotent.
		void Shutdown();
		bool IsShutDown() const { return m_shutdown; }

		// ---- services for the players ----
		bool IsLevelLoading() const { return m_levelLoading; }
		//! Renderer calls (texture create / upload / release, material binding) are allowed right now.
		bool RenderCallsAllowed() const;
		CAudioMixer& Audio() { return m_audio; }
		std::shared_ptr<cev::DecodePool> AcquirePool();
		void SetFullscreen(CVideoPlayerImpl* pPlayer, bool enable);
		bool GlobalStatsEnabled() const;
		//! Name for the texture of an auto-named player: "$cev_video_<w>x<h>_<k>" with the lowest k that no other
		//! player uses. The engine keeps textures that were drawn through the aux geometry alive (aux texture
		//! cache), so a fresh name per player would leak one texture per played video; reusing the names makes the
		//! renderer hand out the same texture object again.
		std::string MakeAutoTextureName(int width, int height, const CVideoPlayerImpl* pSelf) const;

		// ---- console ----
		void ListPlayers() const;
		//! S2: "<cmd>: player <n> surface <id>: flash: <k> clip(s)" for every player attached to Flash movie clips.
		void LogFlashClips(const char* cmd) const;
		void ConsolePlay(const char* path, bool loop);
		void ConsoleStop();
		void ConsolePause();
		void ConsoleResume();
		void ConsoleSeek(double seconds);
		//! cev_play_surface <id> <path> [loop]: creates the surface player (or reuses it) and plays `path` into its
		//! texture; path nullptr / empty destroys that surface player.
		void ConsolePlaySurface(const char* surfaceId, const char* path, bool loop);
		//! Measurement (S0): cev_stats_file. nullptr / "" closes the file.
		void SetStatsFile(const char* path);
		//! Measurement: this process's private bytes (commit charge) and its local video memory use, MB (-1 = unknown).
		static double ProcessPrivateMb();
		static double ProcessGpuMb();

	private:
		class CConsoleListener;
		class CLevelListener;
		class CLoadtimeCallback;
		//! L3a: the update registered with XUI2DLApi::SetLoadtimeUpdate (`pUser` = this manager).
		static void XUI2DLLoadtimeUpdate(void* pUser, float dt);

		CVideoPlayerImpl* Find(const IVideoPlayer* pPlayer) const;
		void ReapZombies(bool force);
		void ReapPendingDestroy();
		//! Applies the open / close / control requests the texture source contract queued since the last tick.
		void ProcessContractCommands();
		void LogStats();
		void PresentFullscreen(bool beforeRender);   //!< under the tick lock
		void DestroyConsolePlayer();
		bool TextureNameInUse(const std::string& name, const CVideoPlayerImpl* pSelf) const;
		void BeginLevelLoading(const char* why);
		void EndLevelLoading(const char* why);
		void NoteLevelActivity(float timeoutSeconds);

		std::vector<std::unique_ptr<CVideoPlayerImpl>> m_players;
		std::vector<std::unique_ptr<CVideoPlayerImpl>> m_zombies;     // destroyed while renderer calls were not allowed
		std::vector<CVideoPlayerImpl*> m_pendingDestroy;              // destroyed from inside an event callback
		std::vector<CVideoPlayerImpl*> m_fullscreen;                  // draw order, last = top
		std::vector<CVideoPlayerImpl*> m_snapshot;                    // scratch for dispatching
		std::vector<CVideoPlayerImpl*> m_frozenPlayers;               // L3c: fullscreen players frozen for one LoadtimeTick

		CAudioMixer m_audio;
		std::shared_ptr<cev::DecodePool> m_pool;
		uint32_t m_nextId = 1;
		bool m_levelLoading = false;
		float m_levelDeadline = 0.f;   // safety net: loading flag is dropped when nothing was heard until then
		bool m_shutdown = false;
		bool m_dispatching = false;
		bool m_statsApplied = false;
		uint32_t m_mfTimeoutsLogged = 0; // cev::MediaFoundationShutdownTimeouts() already reported
		void CheckDecoderShutdown();

		IVideoPlayer* m_pConsolePlayer = nullptr;
		std::unique_ptr<CConsoleListener> m_consoleListener;
		std::unique_ptr<CLevelListener> m_levelListener;
		bool m_levelListenerRegistered = false;

		float m_statsNextTime = 0.f;
		double m_clock = 0.0;        // sum of the dt given to UpdateBeforeRender (stats timing)
		std::vector<IVideoPlayer*> m_consoleSurfaces; // created by cev_play_surface

		// M10: level loading loop
		std::recursive_mutex m_tickMutex;
		std::unique_ptr<CLoadtimeCallback> m_loadtimeCallback; // only with CEV_HAVE_LOADTIME_CALLBACKS
		std::atomic<bool> m_loadtimeRegistered{ false }; // written by the main thread, read by LoadModeActive() on any thread
		RenderBridge::ELoadtimeSource m_loadtimeSource = RenderBridge::ELoadtimeSource::None; // main thread: where it is registered
		bool m_loopDrawRegistered = false; // L6, main thread: FullscreenOutput::LoopDrawCallback() is registered (order 150)
		bool m_inLoadTick = false;   // set while LoadtimeTick runs (renderer calls are allowed then)
	};
}
