// Owns all bridge players, ticks them once per frame, dispatches their events at a fixed point and keeps
// renderer calls away from level loading and shutdown. Main thread only, except UpdateBeforeRender(dt), which does
// not assume the main thread (docs/design/M6-ui-source.md section 9): its time comes from the caller.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <CrySystem/ISystem.h>

#include <CEVPlayer/IVideoPlayer.h>

#include "Audio/AudioMixer.h"
#include "CEVCore/DecodePool.h"

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
		//! UpdateAfterRender: draws the fullscreen outputs on top of the frame and the stats overlay.
		void UpdateAfterRender();

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
		void ConsolePlay(const char* path, bool loop);
		void ConsoleStop();
		void ConsolePause();
		void ConsoleResume();
		void ConsoleSeek(double seconds);
		//! cev_lang: nullptr / empty only reports the current language and the ones the descriptor offers.
		void ConsoleSetLanguage(const char* name);
		//! cev_play_surface <id> <path> [loop]: creates the surface player (or reuses it) and plays `path` into its
		//! texture; path nullptr / empty destroys that surface player.
		void ConsolePlaySurface(const char* surfaceId, const char* path, bool loop);
		//! Measurement (S0): cev_stats_file. nullptr / "" closes the file.
		void SetStatsFile(const char* path);

	private:
		class CConsoleListener;
		class CLevelListener;

		CVideoPlayerImpl* Find(const IVideoPlayer* pPlayer) const;
		void ReapZombies(bool force);
		void ReapPendingDestroy();
		void LogStats();
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

		CAudioMixer m_audio;
		std::shared_ptr<cev::DecodePool> m_pool;
		uint32_t m_nextId = 1;
		bool m_levelLoading = false;
		float m_levelDeadline = 0.f;   // safety net: loading flag is dropped when nothing was heard until then
		bool m_shutdown = false;
		bool m_dispatching = false;
		bool m_statsApplied = false;

		IVideoPlayer* m_pConsolePlayer = nullptr;
		std::unique_ptr<CConsoleListener> m_consoleListener;
		std::unique_ptr<CLevelListener> m_levelListener;
		bool m_levelListenerRegistered = false;

		float m_statsNextTime = 0.f;
		double m_clock = 0.0;        // sum of the dt given to UpdateBeforeRender (stats timing)
		std::vector<IVideoPlayer*> m_consoleSurfaces; // created by cev_play_surface
	};
}
