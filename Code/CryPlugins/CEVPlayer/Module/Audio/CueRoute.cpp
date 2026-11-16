#include "StdAfx.h"
#include "PluginEnv.h"
#include "CueRoute.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <CryCore/Platform/CryWindows.h> // GetModuleHandleA / GetProcAddress
#include <CryAudio/IAudioSystem.h>
#include <CryAudio/IObject.h>

namespace CEVPlayer
{
	namespace
	{
		// A cue restarts only from the very beginning: the CryAudio trigger cannot start in the middle.
		constexpr double kRestartWindowSeconds = 0.05;
		// Clock smoothing: the position is blended into the running estimate with this time constant (seconds); a
		// difference above kSnapSeconds ahead resynchronises at once, behind it the clock waits for the audio.
		constexpr double kBlendSeconds = 0.12;
		constexpr double kSnapSeconds = 0.25;
		constexpr double kPositionGraceSeconds = 0.5; // after "playing" the implementation has this long to answer the query

		// ---- does the CryAudio::IObject this module was compiled against have the optional query?
		template <class T, class = void> struct HasPositionApi : std::false_type {};
		template <class T>
		struct HasPositionApi<T, std::void_t<decltype(std::declval<const T&>().GetTriggerPlaybackPosition(CryAudio::ControlId(), std::declval<float&>()))>> : std::true_type {};

		template <class T>
		bool CallPositionApi(const T* pObject, CryAudio::ControlId id, float& out, std::true_type)
		{
			return pObject->GetTriggerPlaybackPosition(id, out);
		}
		template <class T>
		bool CallPositionApi(const T*, CryAudio::ControlId, float&, std::false_type)
		{
			return false; // built against an unpatched CryAudio/IObject.h: the slot is not declared, never call it
		}

		constexpr bool kBuiltWithPositionApi = HasPositionApi<CryAudio::IObject>::value;

		// ---- callbacks: token -> clock (main thread only; a callback of a destroyed clock finds nothing)
		std::unordered_map<uintptr_t, CCueClock*>& Registry()
		{
			static std::unordered_map<uintptr_t, CCueClock*> registry;
			return registry;
		}
		uintptr_t s_nextToken = 0x43455600;

		void IgnoreCallback(const CryAudio::SRequestInfo* const) {}
	}

	// ------------------------------------------------------------------------------------------------ probe
	namespace CueProbe
	{
		namespace
		{
			int s_state = -1; // -1 unknown, 0 not patched, 1 patched
		}

		bool PositionAvailable()
		{
			if (s_state < 0)
			{
				int bits = 0;
#if CRY_PLATFORM_WINDOWS
				// CryAudioSystem.dll is loaded by CrySystem before any plug-in. Only ask when it is loaded already.
				if (HMODULE hModule = ::GetModuleHandleA("CryAudioSystem.dll"))
				{
					using PFN_AudioPatched = int (*)();
					const PFN_AudioPatched pPatched = reinterpret_cast<PFN_AudioPatched>(::GetProcAddress(hModule, "CEVPlayer_AudioPatched"));
					if (pPatched != nullptr)
						bits = pPatched();
				}
#endif
				s_state = (bits & 1) != 0 ? 1 : 0;
				if (s_state == 1 && kBuiltWithPositionApi)
					LogAlways("audio probe: CEVPlayer_AudioPatched found (bits %d), cue route follows the playback position of the trigger", bits);
				else if (s_state == 1)
					LogAlways("audio probe: CEVPlayer_AudioPatched found, but this plug-in was built against an unpatched CryAudio/IObject.h: cue route follows the video clock");
				else
					LogAlways("audio probe: CEVPlayer_AudioPatched not found, cue route follows the video clock (no playback position)");
			}
			if (mEnv != nullptr && mEnv->m_cevCueForceFallback != 0)
				return false;
			return s_state == 1 && kBuiltWithPositionApi;
		}
	}

	// ------------------------------------------------------------------------------------------------ clock
	double CCueClock::WallNow()
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	CCueClock::CCueClock(uint32_t playerId, const SCueConfig& config)
		: m_config(config)
		, m_playerId(playerId)
	{
		m_token = s_nextToken++;
		m_triggerId = CryAudio::StringToId(m_config.trigger.c_str());
		m_pauseId = m_config.pauseTrigger.empty() ? 0 : CryAudio::StringToId(m_config.pauseTrigger.c_str());
		m_resumeId = m_config.resumeTrigger.empty() ? 0 : CryAudio::StringToId(m_config.resumeTrigger.c_str());
		m_lastWall = WallNow();
	}

	CCueClock::~CCueClock()
	{
		Shutdown();
	}

	bool CCueClock::EnsureObject()
	{
		if (m_dead)
			return false;
		if (m_pObject != nullptr)
			return true;
		CryAudio::IAudioSystem* const pAudio = gEnv != nullptr ? gEnv->pAudioSystem : nullptr;
		if (pAudio == nullptr)
			return false;

		Registry()[m_token] = this;
		pAudio->AddRequestListener(&CCueClock::OnAudioEvent, reinterpret_cast<void*>(m_token),
			CryAudio::ESystemEvents::TriggerExecuted | CryAudio::ESystemEvents::TriggerFinished);
		m_listening = true;

		char name[48];
		std::snprintf(name, sizeof(name), "CEVPlayer_cue_%u", m_playerId);
		m_pObject = pAudio->CreateObject(CryAudio::SCreateObjectData(name, CryAudio::EOcclusionType::Ignore));
		if (m_pObject == nullptr)
		{
			LogWarning("player %u: cannot create the CryAudio object of cue trigger <%s>", m_playerId, m_config.trigger.c_str());
			return false;
		}
		LogDebug("player %u: cue audio object <%s>, trigger <%s> pause <%s> resume <%s>", m_playerId, name, m_config.trigger.c_str(),
			m_config.pauseTrigger.c_str(), m_config.resumeTrigger.c_str());
		return true;
	}

	bool CCueClock::StartAudio()
	{
		if (!EnsureObject())
		{
			m_audio = EAudio::Failed;
			if (!m_dead)
				LogWarning("player %u: no CryAudio system: cue trigger <%s> is not played, the picture follows the video clock", m_playerId, m_config.trigger.c_str());
			return false;
		}
		++m_runId;
		m_audio = EAudio::Starting;
		m_posSeen = false;
		m_posWarned = false;
		m_triggerMissing = false;
		m_positionDriven = false;

		const CryAudio::STriggerCallbackData callbackData(m_triggerId, CryAudio::ESystemEvents::TriggerExecuted | CryAudio::ESystemEvents::TriggerFinished, &IgnoreCallback);
		// Executed: callback when the request was processed; Finished: when all event instances ended. Both reach
		// OnAudioEvent() on the main thread (ExternalUpdate). pUserData carries the run id: callbacks of an older
		// instance (stopped on a loop / seek) are recognised and ignored.
		const CryAudio::SRequestUserData userData(
			CryAudio::ERequestFlags::CallbackOnExternalOrCallingThread | CryAudio::ERequestFlags::SubsequentCallbackOnExternalThread,
			reinterpret_cast<void*>(m_token), reinterpret_cast<void*>(m_runId), reinterpret_cast<void*>(m_token));
		m_pObject->ExecuteTriggerWithCallbacks(callbackData, INVALID_ENTITYID, userData);
		LogDebug("player %u: cue trigger <%s> executed (run %u)", m_playerId, m_config.trigger.c_str(), static_cast<unsigned>(m_runId));
		return true;
	}

	void CCueClock::StopAudio()
	{
		++m_runId; // whatever the old instance still reports is stale
		if (m_pObject != nullptr && !m_dead && (m_audio == EAudio::Starting || m_audio == EAudio::Playing || m_audio == EAudio::PausedByTrigger))
			m_pObject->StopTrigger(m_triggerId);
		m_audio = EAudio::Idle;
		m_positionDriven = false;
	}

	void CCueClock::PauseAudio()
	{
		if (m_audio != EAudio::Playing)
			return;
		if (m_pauseId != 0 && m_pObject != nullptr && !m_dead)
		{
			m_pObject->ExecuteTrigger(m_pauseId);
			m_audio = EAudio::PausedByTrigger;
		}
		else if (!m_limitWarned)
		{
			m_limitWarned = true;
			LogWarning("player %u: cue audio <%s> cannot be paused (no pauseTrigger in the descriptor): the sound keeps playing while the picture is held and the picture catches up on resume", m_playerId, m_config.trigger.c_str());
		}
	}

	void CCueClock::ResumeAudio()
	{
		if (m_audio != EAudio::PausedByTrigger)
			return;
		if (m_resumeId != 0 && m_pObject != nullptr && !m_dead)
		{
			m_pObject->ExecuteTrigger(m_resumeId);
			m_audio = EAudio::Playing;
		}
		else
		{
			LogWarning("player %u: cue trigger <%s> was paused but has no resumeTrigger: the sound stays paused", m_playerId, m_config.trigger.c_str());
		}
	}

	bool CCueClock::QueryPosition(double& seconds)
	{
		if (m_pObject == nullptr || m_dead || !CueProbe::PositionAvailable())
			return false;
		float value = 0.f;
		if (!CallPositionApi(m_pObject, m_triggerId, value, std::integral_constant<bool, kBuiltWithPositionApi>()))
			return false;
		seconds = static_cast<double>(value);
		return true;
	}

	// ------------------------------------------------------------------------------------------------ IClock
	double CCueClock::NowSeconds()
	{
		if (m_run == ERun::Paused)
			return m_time;
		const double now = WallNow();

		if (m_run == ERun::Waiting)
		{
			if (m_audio == EAudio::Playing)
			{
				m_run = ERun::Running; // the sound started: the picture starts now
				m_lastWall = now;
			}
			else if (m_audio == EAudio::Failed || m_audio == EAudio::Finished)
			{
				if (m_triggerMissing)
					LogWarning("player %u: CryAudio has no trigger <%s> (or the request failed): the picture follows the video clock", m_playerId, m_config.trigger.c_str());
				else
					LogWarning("player %u: cue trigger <%s> did not start playing (sample missing or finished at once): the picture follows the video clock", m_playerId, m_config.trigger.c_str());
				m_run = ERun::Running;
				m_lastWall = now;
			}
			else if (now - m_waitSince >= kStartTimeoutSeconds)
			{
				LogWarning("player %u: no \"trigger started\" callback for <%s> after %.1f s: the picture follows the video clock (the sound is cancelled)", m_playerId, m_config.trigger.c_str(), kStartTimeoutSeconds);
				StopAudio();
				m_audio = EAudio::Failed;
				m_run = ERun::Running;
				m_lastWall = now;
			}
			else
			{
				return m_time; // waiting for the sound: the picture does not advance
			}
		}

		const double dt = std::max(0.0, now - m_lastWall);
		m_lastWall = now;
		const bool audible = m_audio == EAudio::Playing;
		double estimate = m_time + dt * (audible ? 1.0 : m_rate);
		bool driven = false;

		if (audible)
		{
			double position = 0.0;
			if (QueryPosition(position))
			{
				m_posSeen = true;
				m_lastPos = position;
				const double error = position - estimate;
				if (error > kSnapSeconds)
					estimate = position;
				else if (error < -kSnapSeconds)
					estimate = m_time; // the sound is far behind (device stall): wait for it
				else
					estimate += error * std::min(1.0, dt / kBlendSeconds);
				estimate = std::max(estimate, m_time); // never backwards
				driven = true;
			}
			else if (!m_posWarned && now - m_waitSince >= kPositionGraceSeconds && !m_posSeen)
			{
				m_posWarned = true;
				LogWarning("player %u: no playback position for cue trigger <%s> (%s): the picture follows the video clock, sound and picture were started together and may drift",
					m_playerId, m_config.trigger.c_str(), CueProbe::PositionAvailable() ? "the audio implementation does not report one" : (mEnv->m_cevCueForceFallback != 0 ? "cev_cueForceFallback is set" : "engine patch 02-cryaudio-trigger-position not available"));
			}
		}
		m_positionDriven = driven;
		m_time = estimate;
		return m_time;
	}

	void CCueClock::Start()
	{
		if (m_audio != EAudio::Idle)
			StopAudio();
		m_run = ERun::Paused;
		m_time = 0.0;
		Resume();
	}

	void CCueClock::Pause()
	{
		if (m_run == ERun::Paused)
			return;
		if (m_run == ERun::Running)
		{
			NowSeconds(); // freeze the time
			PauseAudio();
		}
		else
		{
			StopAudio(); // still waiting for the start: it starts again from 0 on resume
		}
		m_run = ERun::Paused;
	}

	void CCueClock::Resume()
	{
		if (m_run != ERun::Paused)
			return;
		const double now = WallNow();
		m_lastWall = now;
		if (m_audio == EAudio::PausedByTrigger)
		{
			ResumeAudio();
			m_run = ERun::Running;
		}
		else if (m_audio == EAudio::Playing)
		{
			m_run = ERun::Running; // the sound never stopped (no pauseTrigger): the clock catches up with it
		}
		else if (m_time < kRestartWindowSeconds)
		{
			if (StartAudio())
			{
				m_run = ERun::Waiting;
				m_waitSince = now;
			}
			else
			{
				m_run = ERun::Running;
			}
		}
		else
		{
			m_run = ERun::Running; // no sound can start in the middle
			if (m_audio == EAudio::Idle && !m_limitWarned && !m_dead)
			{
				m_limitWarned = true;
				LogWarning("player %u: cue trigger <%s> cannot start at %.2f s: the picture plays without sound until the next start from the beginning", m_playerId, m_config.trigger.c_str(), m_time);
			}
		}
	}

	void CCueClock::Seek(double seconds)
	{
		const bool wasActive = m_audio != EAudio::Idle && m_audio != EAudio::Failed && m_audio != EAudio::Finished;
		if (m_audio != EAudio::Idle)
			StopAudio();
		m_time = seconds > 0.0 ? seconds : 0.0;
		const double now = WallNow();
		m_lastWall = now;
		if (m_run == ERun::Paused)
			return; // a later Resume() decides whether the sound can start
		if (m_time < kRestartWindowSeconds)
		{
			// loop / replay: the trigger starts again from its beginning, the picture waits for it
			if (StartAudio())
			{
				m_run = ERun::Waiting;
				m_waitSince = now;
			}
			else
			{
				m_run = ERun::Running;
			}
		}
		else
		{
			m_run = ERun::Running;
			if (wasActive && !m_limitWarned)
			{
				m_limitWarned = true;
				LogWarning("player %u: cue audio cannot seek (to %.2f s): the sound is stopped, the picture goes on without it until the next start from the beginning", m_playerId, m_time);
			}
		}
	}

	void CCueClock::SetRate(double rate)
	{
		if (!(rate > 0.0))
			return;
		m_rate = rate;
		if (rate != 1.0 && !m_limitWarned && AudioActive())
		{
			m_limitWarned = true;
			LogWarning("player %u: the cue audio always plays at 1x, the rate %.2f only applies to the picture without sound", m_playerId, rate);
		}
	}

	// ------------------------------------------------------------------------------------------------ teardown / events
	void CCueClock::Shutdown()
	{
		if (m_dead)
			return;
		CryAudio::IAudioSystem* const pAudio = gEnv != nullptr ? gEnv->pAudioSystem : nullptr;
		Registry().erase(m_token);
		if (pAudio != nullptr)
		{
			// The audio system may be on its way out (shutdown order): only talk to it while the pointer is still published.
			if (m_pObject != nullptr && AudioActive())
				m_pObject->StopTrigger(m_triggerId);
			if (m_listening)
				pAudio->RemoveRequestListener(&CCueClock::OnAudioEvent, reinterpret_cast<void*>(m_token));
			if (m_pObject != nullptr)
				pAudio->ReleaseObject(m_pObject);
		}
		m_pObject = nullptr;
		m_listening = false;
		m_dead = true;
		m_audio = EAudio::Idle;
		m_positionDriven = false;
		++m_runId;
	}

	void CCueClock::OnLevelUnload()
	{
		if (m_dead || m_pObject == nullptr || !AudioActive())
			return;
		LogWarning("player %u: level unload: cue trigger <%s> stopped, the picture goes on without sound", m_playerId, m_config.trigger.c_str());
		const double t = NowSeconds();
		StopAudio();
		m_audio = EAudio::Failed;
		if (m_run == ERun::Waiting)
		{
			m_run = ERun::Running;
			m_lastWall = WallNow();
		}
		m_time = t;
	}

	void CCueClock::OnAudioEvent(const CryAudio::SRequestInfo* pInfo)
	{
		if (pInfo == nullptr)
			return;
		std::unordered_map<uintptr_t, CCueClock*>& registry = Registry();
		const auto it = registry.find(reinterpret_cast<uintptr_t>(pInfo->pOwner));
		if (it != registry.end() && it->second != nullptr)
			it->second->HandleEvent(*pInfo);
	}

	void CCueClock::HandleEvent(const CryAudio::SRequestInfo& info)
	{
		if (m_dead || reinterpret_cast<uintptr_t>(info.pUserData) != m_runId)
			return; // stale: belongs to an instance that was stopped / replaced
		if (info.systemEvent == CryAudio::ESystemEvents::TriggerExecuted)
		{
			if (m_audio != EAudio::Starting)
				return;
			if (info.requestResult == CryAudio::ERequestResult::Failure)
			{
				m_triggerMissing = true; // reported by NowSeconds() (once, the picture is waiting for it)
				m_audio = EAudio::Failed;
			}
			else
			{
				m_audio = EAudio::Playing;
				m_waitSince = WallNow();
				LogDebug("player %u: cue trigger <%s> started", m_playerId, m_config.trigger.c_str());
			}
		}
		else if (info.systemEvent == CryAudio::ESystemEvents::TriggerFinished)
		{
			if (m_audio == EAudio::Starting || m_audio == EAudio::Playing || m_audio == EAudio::PausedByTrigger)
			{
				LogDebug("player %u: cue trigger <%s> finished", m_playerId, m_config.trigger.c_str());
				m_audio = EAudio::Finished;
				m_positionDriven = false;
			}
		}
	}

	// ------------------------------------------------------------------------------------------------ diagnostics
	const char* CCueClock::SourceName() const
	{
		return m_positionDriven ? "cue" : "internal";
	}

	bool CCueClock::GetDebugPosition(double& triggerSeconds, double& mediaSeconds) const
	{
		if (!m_posSeen || m_audio != EAudio::Playing)
			return false;
		triggerSeconds = m_lastPos;
		mediaSeconds = m_time;
		return true;
	}
}
