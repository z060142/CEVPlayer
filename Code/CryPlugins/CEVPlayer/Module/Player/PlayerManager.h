// Owns all bridge players, ticks them once per frame, dispatches their events at a fixed point and keeps
// renderer calls away from level loading and shutdown. Main thread only.
#pragma once

#include <cstdint>
#include <memory>
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
		void DestroyPlayer(IVideoPlayer* pPlayer);
		uint32_t GetPlayerCount() const { return static_cast<uint32_t>(m_players.size()); }
		bool IsPlayer(const IVideoPlayer* pPlayer) const;

		// ---- frame hooks (plug-in update steps) ----
		//! UpdateBeforeRender: ticks the players (texture uploads), then dispatches listener events.
		void UpdateBeforeRender();
		//! UpdateAfterRender: draws the fullscreen outputs on top of the frame and the stats overlay.
		void UpdateAfterRender();

		// ---- engine events ----
		void OnSystemEvent(ESystemEvent event);
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

		// ---- console ----
		void ListPlayers() const;
		void ConsolePlay(const char* path, bool loop);
		void ConsoleStop();
		void ConsolePause();
		void ConsoleResume();
		void ConsoleSeek(double seconds);

	private:
		class CConsoleListener;

		CVideoPlayerImpl* Find(const IVideoPlayer* pPlayer) const;
		void ReapZombies(bool force);
		void ReapPendingDestroy();
		void LogStats();
		void DestroyConsolePlayer();

		std::vector<std::unique_ptr<CVideoPlayerImpl>> m_players;
		std::vector<std::unique_ptr<CVideoPlayerImpl>> m_zombies;     // destroyed while renderer calls were not allowed
		std::vector<CVideoPlayerImpl*> m_pendingDestroy;              // destroyed from inside an event callback
		std::vector<CVideoPlayerImpl*> m_fullscreen;                  // draw order, last = top
		std::vector<CVideoPlayerImpl*> m_snapshot;                    // scratch for dispatching

		CAudioMixer m_audio;
		std::shared_ptr<cev::DecodePool> m_pool;
		uint32_t m_nextId = 1;
		bool m_levelLoading = false;
		bool m_shutdown = false;
		bool m_dispatching = false;
		bool m_statsApplied = false;

		IVideoPlayer* m_pConsolePlayer = nullptr;
		std::unique_ptr<CConsoleListener> m_consoleListener;

		float m_statsNextTime = 0.f;
	};
}
