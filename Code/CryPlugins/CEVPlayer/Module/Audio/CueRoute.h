// Cue route (M3): the audio of a video is a CryAudio trigger; this clock is its transport.
//
// CCueClock is the cev::IClock of a cue player. PlayerCore drives it exactly like an InternalClock (Resume = play,
// Pause, Seek, loop = Seek(0), Stop = Pause + Seek(0)); the clock translates that into CryAudio requests on its own
// audio object and reads the time back:
//
//   * Resume / Start from time ~0: ExecuteTriggerWithCallbacks. The picture does NOT advance (time stays at the start)
//     until the "trigger executed" callback arrived; after kStartTimeoutSeconds (2 s) without it the clock continues on
//     the video clock and logs a warning.
//   * Running: when the optional engine patch is there (probe export CEVPlayer_AudioPatched of CryAudioSystem.dll,
//     bit 0, and this module was built against the patched CryAudio/IObject.h) and the implementation answers,
//     time = playback position of the trigger (smoothed, never backwards). Otherwise: time = wall clock (the
//     "internal" clock), one warning per run.
//   * Pause / Resume: pauseTrigger / resumeTrigger of the descriptor (SDL_mixer event actions "pause" / "resume").
//     Without them the audio cannot be paused: it keeps playing while the picture is held (limit, logged once).
//   * Seek: CryAudio cannot seek. Seek(~0) (loop, stop, replay) stops the trigger and starts it again; any other
//     target stops the audio (silent picture on the video clock until the next start from 0).
//   * Stop / destroy / level unload / shutdown: StopTrigger, remove the listener, release the object (all guarded
//     against an audio system that is already gone).
//
// Main thread only (PlayerCore calls its clock from the consumer thread; CryAudio delivers the callbacks in
// ExternalUpdate on the main thread). The audio thread is never touched except through CryAudio requests and the
// lock free GetTriggerPlaybackPosition().
#pragma once

#include <cstdint>
#include <string>

#include "CEVCore/IClock.h"

namespace CryAudio
{
	struct IObject;
	struct SRequestInfo;
}

namespace CEVPlayer
{
	struct SCueConfig
	{
		std::string trigger;        // start trigger (required)
		std::string pauseTrigger;   // optional, event action "pause"
		std::string resumeTrigger;  // optional, event action "resume"
	};

	namespace CueProbe
	{
		//! Probes CryAudioSystem.dll once for CEVPlayer_AudioPatched and logs the result once.
		//! True when the engine has the position query AND this module can call it AND cev_cueForceFallback is 0.
		bool PositionAvailable();
	}

	class CCueClock final : public cev::IClock
	{
	public:
		static constexpr double kStartTimeoutSeconds = 2.0;

		CCueClock(uint32_t playerId, const SCueConfig& config);
		~CCueClock() override;
		CCueClock(const CCueClock&) = delete;
		CCueClock& operator=(const CCueClock&) = delete;

		// ---- cev::IClock (main thread) ----
		double NowSeconds() override;
		void Start() override;
		void Pause() override;
		void Resume() override;
		void Seek(double seconds) override;
		void SetRate(double rate) override;
		double Rate() const override { return m_rate; }
		bool IsRunning() const override { return m_run == ERun::Running || m_run == ERun::Waiting; }

		//! Stops the trigger and releases everything CryAudio. Afterwards the clock is a plain wall clock. Idempotent,
		//! safe when the audio system no longer exists.
		void Shutdown();
		//! Level unload: the engine drops the level's audio data; stop our trigger, the picture goes on silently.
		void OnLevelUnload();

		//! "cue" while the playback position drives the clock, "internal" (video clock fallback / waiting / audio over).
		const char* SourceName() const;
		//! Diagnostics: last trigger position and the media time of the same moment (cev_list).
		bool GetDebugPosition(double& triggerSeconds, double& mediaSeconds) const;
		const std::string& Trigger() const { return m_config.trigger; }
		bool AudioActive() const { return m_audio == EAudio::Playing || m_audio == EAudio::PausedByTrigger || m_audio == EAudio::Starting; }

		//! Static callback target (registry by token, see the .cpp).
		static void OnAudioEvent(const CryAudio::SRequestInfo* pInfo);

	private:
		enum class ERun : uint8_t { Paused, Waiting, Running };
		enum class EAudio : uint8_t { Idle, Starting, Playing, PausedByTrigger, Finished, Failed };

		void HandleEvent(const CryAudio::SRequestInfo& info);
		bool EnsureObject();
		bool StartAudio();
		void StopAudio();
		void PauseAudio();
		void ResumeAudio();
		bool QueryPosition(double& seconds);
		static double WallNow();

		SCueConfig m_config;
		uint32_t m_playerId;
		uintptr_t m_token = 0;
		uint32_t m_triggerId = 0;
		uint32_t m_pauseId = 0;
		uint32_t m_resumeId = 0;
		CryAudio::IObject* m_pObject = nullptr;
		bool m_listening = false;

		ERun m_run = ERun::Paused;
		EAudio m_audio = EAudio::Idle;
		uintptr_t m_runId = 0;      // identifies the trigger instance the callbacks belong to
		double m_time = 0.0;        // media time at the last update
		double m_lastWall = 0.0;
		double m_waitSince = 0.0;
		double m_rate = 1.0;
		double m_lastPos = 0.0;
		bool m_posSeen = false;     // the position query answered during this run
		bool m_posWarned = false;   // one fallback warning per run
		bool m_triggerMissing = false; // the executed callback reported a failed request (no such trigger)
		bool m_limitWarned = false; // pause / rate / seek limits are reported once per clock
		bool m_positionDriven = false;
		bool m_dead = false;        // Shutdown(): no CryAudio access any more, plain wall clock
	};
}
