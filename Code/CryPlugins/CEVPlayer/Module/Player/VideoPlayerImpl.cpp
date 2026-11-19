#include "StdAfx.h"
#include "PluginEnv.h"
#include "VideoPlayerImpl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <CrySystem/File/ICryPak.h>
#include <CryRenderer/IRenderer.h>
#include <CryRenderer/IShader.h>
#include <Cry3DEngine/IMaterial.h>

#include "Audio/AudioMixer.h"
#include "Contract/TexSource.h"
#include "Player/CryPakByteSource.h"
#include "Player/PlayerManager.h"
#include "Render/RenderBridge.h"

namespace CEVPlayer
{
	namespace
	{
		const char* StateName(EVideoPlayerState state)
		{
			switch (state)
			{
			case EVideoPlayerState::Stopped: return "Stopped";
			case EVideoPlayerState::Playing: return "Playing";
			case EVideoPlayerState::Paused:  return "Paused";
			case EVideoPlayerState::Ended:   return "Ended";
			case EVideoPlayerState::Loading: return "Loading";
			case EVideoPlayerState::Error:   return "Error";
			}
			return "?";
		}

		cev::PlayerConfig MakeConfig(CPlayerManager& manager)
		{
			cev::PlayerConfig cfg;
			cfg.outputRgba = true;
			cfg.decodeAudio = false; // BuildCores() decides per media
			cfg.videoThreads = static_cast<uint32_t>(std::max(0, mEnv->m_cevDecodeThreads));
			cfg.enableStats = manager.GlobalStatsEnabled();
			cfg.pool = manager.AcquirePool();
			return cfg;
		}

		// Seconds on a steady clock (timeouts that must not depend on the game timer / pauses).
		double SteadySeconds()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
		// The external audio file may take this long to load after the video is ready (then the video starts without it).
		constexpr double kExternalAudioTimeoutSeconds = 5.0;

		bool FileExists(const std::string& path)
		{
			return !path.empty() && gEnv != nullptr && gEnv->pCryPak != nullptr && gEnv->pCryPak->IsFileExist(path.c_str());
		}

		// cev::DescriptorFileExistsFn over CryPak (the descriptor's file names are CryPak paths)
		bool PakFileExists(const char* path, void*)
		{
			return path != nullptr && FileExists(path);
		}

		//! cvar string value, empty when the cvar does not exist.
		const char* CVarString(const char* name)
		{
			ICVar* const pVar = gEnv != nullptr && gEnv->pConsole != nullptr ? gEnv->pConsole->GetCVar(name) : nullptr;
			const char* const value = pVar != nullptr ? pVar->GetString() : nullptr;
			return value != nullptr ? value : "";
		}
	}

	CVideoPlayerImpl::CVideoPlayerImpl(CPlayerManager& manager, uint32_t id, const char* textureName, const char* surfaceId)
		: m_manager(manager)
		, m_id(id)
	{
		m_core = std::make_unique<cev::PlayerCore>(MakeConfig(manager)); // replaced by BuildCores() at every Load()
		if (surfaceId != nullptr && surfaceId[0] != '\0')
		{
			// The engine texture name of a surface player is an internal detail; it never changes with the media.
			m_surfaceId = surfaceId;
			m_textureName = std::string("$cev:") + surfaceId;
			m_autoName = false;
			Contract::RegisterSurface(m_surfaceId.c_str());
			m_surfaceRegistered = true;
		}
		else if (textureName != nullptr && textureName[0] != '\0')
		{
			bool changed = false;
			m_textureName = NormalizeTextureName(textureName, &changed);
			m_autoName = false;
			if (changed)
				LogWarning("player %u: texture name <%s> is used as <%s> (names must start with '$' and are limited in length)", id, textureName, m_textureName.c_str());
		}
		else
		{
			char buffer[48];
			std::snprintf(buffer, sizeof(buffer), "$cev_video_%u", id);
			m_textureName = buffer;
		}
	}

	CVideoPlayerImpl::~CVideoPlayerImpl()
	{
		PrepareDestroy();
		// The manager releases renderer resources at safe points; this is the last resort (e.g. shutdown order).
		if (HasRenderResources() && RenderBridge::Available())
			ReleaseRenderResources();
	}

	// ------------------------------------------------------------------------------------------------ events
	void CVideoPlayerImpl::Emit(EEvent type, int code, uint64_t value)
	{
		if (m_destroyed)
			return;
		if (type == EEvent::NewFrame && m_eventCount > 0 && m_events[m_eventCount - 1].type == EEvent::NewFrame)
		{
			m_events[m_eventCount - 1].value = value; // coalesce: one OnNewFrame per dispatch
			return;
		}
		if (m_eventCount >= kMaxEvents)
		{
			if (type == EEvent::NewFrame)
				return;
			m_eventCount = kMaxEvents - 1; // drop the newest queued event for a more important one
		}
		m_events[m_eventCount].type = type;
		m_events[m_eventCount].code = code;
		m_events[m_eventCount].value = value;
		++m_eventCount;
	}

	void CVideoPlayerImpl::EmitMarker(const SVideoDescriptor::SMarker& marker)
	{
		if (m_destroyed)
			return;
		if (m_eventCount >= kMaxEvents)
			m_eventCount = kMaxEvents - 1; // a marker outranks the newest queued event
		SEvent& ev = m_events[m_eventCount++];
		ev = SEvent();
		ev.type = EEvent::Marker;
		ev.time = marker.time;
		std::strncpy(ev.name, marker.name.c_str(), kMarkerNameLength - 1);
		ev.name[kMarkerNameLength - 1] = '\0';
	}

	void CVideoPlayerImpl::AddListener(IVideoPlayerListener* pListener)
	{
		if (pListener == nullptr || m_destroyed)
			return;
		if (std::find(m_listeners.begin(), m_listeners.end(), pListener) == m_listeners.end())
			m_listeners.push_back(pListener);
	}

	void CVideoPlayerImpl::RemoveListener(IVideoPlayerListener* pListener)
	{
		if (pListener == nullptr)
			return;
		std::vector<IVideoPlayerListener*>::iterator it = std::find(m_listeners.begin(), m_listeners.end(), pListener);
		if (it == m_listeners.end())
			return;
		if (m_dispatchDepth > 0)
			*it = nullptr; // compacted when the dispatch loop ends
		else
			m_listeners.erase(it);
	}

	// Main thread only: listener callbacks reach entity / flow graph / Schematyc code.
	void CVideoPlayerImpl::DispatchEvents()
	{
		if (m_eventCount == 0 || m_destroyed)
			return;

		SEvent local[kMaxEvents];
		const uint32_t count = m_eventCount;
		for (uint32_t i = 0; i < count; ++i)
			local[i] = m_events[i];
		m_eventCount = 0;

		++m_dispatchDepth;
		for (uint32_t e = 0; e < count && !m_destroyed; ++e)
		{
			const SEvent& ev = local[e];
			for (size_t i = 0; i < m_listeners.size() && !m_destroyed; ++i)
			{
				IVideoPlayerListener* const pListener = m_listeners[i];
				if (pListener == nullptr)
					continue;
				switch (ev.type)
				{
				case EEvent::Ready:    pListener->OnReady(*this); break;
				case EEvent::Started:  pListener->OnStarted(*this); break;
				case EEvent::NewFrame: pListener->OnNewFrame(*this, ev.value); break;
				case EEvent::Looped:   pListener->OnLooped(*this); break;
				case EEvent::Ended:    pListener->OnEnded(*this); break;
				case EEvent::Error:    pListener->OnError(*this, ev.code, cev::StatusToString(static_cast<cev::Status>(ev.code))); break;
				case EEvent::Paused:   pListener->OnPaused(*this); break;
				case EEvent::Resumed:  pListener->OnResumed(*this); break;
				case EEvent::Stopped:  pListener->OnStopped(*this); break;
				case EEvent::Marker:   pListener->OnMarker(*this, ev.name, ev.time); break;
				}
			}
		}
		--m_dispatchDepth;
		if (m_dispatchDepth == 0)
			m_listeners.erase(std::remove(m_listeners.begin(), m_listeners.end(), static_cast<IVideoPlayerListener*>(nullptr)), m_listeners.end());
	}

	// ------------------------------------------------------------------------------------------------ media
	bool CVideoPlayerImpl::Load(const char* path)
	{
		return LoadInternal(path, false);
	}

	// Main thread only: opens files (CryPak) and reads cvars.
	bool CVideoPlayerImpl::LoadInternal(const char* path, bool reload)
	{
		if (m_destroyed)
			return false;

		StopVoice(); // before any core goes away: the audio callback must not touch them
		m_audioCore.reset();
		m_core->Unload();
		m_phase = EPhase::None;
		m_info = cev::MediaInfo();
		m_hasAudio = false;
		m_seenLoops = 0;
		m_endedSent = false;
		m_pausedByLevelLoad = false;
		m_needUpload = false;
		m_frameSerial = 0;
		m_seekPending = false;
		m_silentWarned = false;
		m_audioWaitSince = -1.0;
		m_markerResync = true;
		m_markerNext = 0;
		m_markerPrev = 0.0;
		if (!reload)
			m_eventCount = 0;

		const bool pathOk = path != nullptr && path[0] != '\0';
		m_path = pathOk ? path : "";
		m_plan = SPlan();
		m_desc = SVideoDescriptor();
		m_descValid = false;
		m_variantIndex = -1;
		m_language.clear();
		if (!pathOk || gEnv == nullptr || gEnv->pCryPak == nullptr || !gEnv->pCryPak->IsFileExist(path))
		{
			m_phase = EPhase::Failed;
			m_loadStatus = pathOk ? cev::Status::IoError : cev::Status::InvalidArgument;
			LogError("player %u: cannot load <%s>: %s", m_id, m_path.c_str(), pathOk ? "file not found" : "empty path");
			Emit(EEvent::Error, static_cast<int>(m_loadStatus));
			return false;
		}

		SPlan plan;
		if (!ChoosePlan(path, plan))
		{
			m_phase = EPhase::Failed;
			m_loadStatus = cev::Status::InvalidData;
			LogError("player %u: cannot load <%s>: unusable descriptor or missing video file", m_id, m_path.c_str());
			Emit(EEvent::Error, static_cast<int>(m_loadStatus));
			return false;
		}
		m_plan = plan;
		m_language = plan.language;

		BuildCores(plan);
		const cev::Status status = m_core->Load(std::make_unique<CCryPakByteSource>(plan.videoPath.c_str()));
		if (cev::Failed(status))
		{
			m_phase = EPhase::Failed;
			m_loadStatus = status;
			LogError("player %u: cannot start loading <%s>: %s", m_id, plan.videoPath.c_str(), cev::StatusToString(status));
			Emit(EEvent::Error, static_cast<int>(status));
			return false;
		}
		if (m_audioCore)
		{
			const cev::Status astatus = m_audioCore->Load(std::make_unique<CCryPakByteSource>(plan.externalPath.c_str()));
			if (cev::Failed(astatus))
			{
				LogWarning("player %u: cannot start loading the audio file <%s>: %s, playing without audio", m_id, plan.externalPath.c_str(), cev::StatusToString(astatus));
				m_audioCore.reset();
			}
		}
		m_phase = EPhase::Loading;
		m_loadStatus = cev::Status::Ok;
		LogDebug("player %u: loading <%s>%s%s%s", m_id, plan.videoPath.c_str(), m_audioCore ? " + audio <" : "", m_audioCore ? plan.externalPath.c_str() : "", m_audioCore ? ">" : "");
		return true;
	}

	//! Fills language, external file and cue flag from the descriptor and the language requests (the decision is
	//! cev::ResolveDescriptorAudio, shared with the Sandbox preview; the warnings are logged here).
	void CVideoPlayerImpl::ResolveAudio(SPlan& plan, bool report) const
	{
		plan.externalPath.clear();
		plan.language.clear();
		plan.cue = false;
		plan.trigger.clear();
		plan.pauseTrigger.clear();
		plan.resumeTrigger.clear();
		if (!m_descValid)
			return;

		// request chain: SetLanguage, g_languageAudio, g_language, descriptor default
		const char* const candidates[4] = { m_requestedLanguage.c_str(), CVarString("g_languageAudio"), CVarString("g_language"), m_desc.defaultLanguage.c_str() };
		cev::DescriptorAudioPlan audio;
		cev::ResolveDescriptorAudio(m_desc, candidates, 4, &PakFileExists, nullptr, audio);
		plan.language = audio.language;
		plan.externalPath = audio.externalPath;
		plan.cue = audio.cue;
		plan.trigger = audio.trigger;
		plan.pauseTrigger = audio.pauseTrigger;
		plan.resumeTrigger = audio.resumeTrigger;
		if (!report)
			return;
		if (m_desc.route == EVideoRoute::Cue && plan.trigger.empty())
			LogWarning("player %u: <%s> uses route=\"cue\" but has no trigger for language <%s>: playing without audio", m_id, m_path.c_str(), plan.language.c_str());
		else if (!audio.missingExternal.empty())
			LogWarning("player %u: audio file <%s> of language <%s> not found, using the embedded track", m_id, audio.missingExternal.c_str(), plan.language.c_str());
	}

	std::string CVideoPlayerImpl::PickVariantPath()
	{
		m_variantIndex = -1;
		const int forced = m_variantRequest != kVariantFollowCvar ? m_variantRequest : mEnv->m_cevVariant;
		const bool autoPick = m_fullscreen && gEnv != nullptr && gEnv->pRenderer != nullptr;
		const int outputHeight = autoPick ? gEnv->pRenderer->GetHeight() : 0;
		const cev::DescriptorVariantChoice choice = cev::PickDescriptorVariant(m_desc, forced, autoPick, outputHeight, &PakFileExists, nullptr);
		if (choice.forcedOutOfRange)
			LogWarning("player %u: cev_variant %d does not exist in <%s> (%u variants), using the main video", m_id, forced, m_path.c_str(), static_cast<unsigned>(m_desc.variants.size()));
		if (!choice.missingFile.empty())
			LogWarning("player %u: variant file <%s> not found, using the main video", m_id, choice.missingFile.c_str());
		if (choice.variant < 0)
			return choice.videoPath;
		m_variantIndex = choice.variant;
		Log("player %u: variant %d <%s> %dx%d chosen", m_id, m_variantIndex, choice.videoPath.c_str(), choice.width, choice.height);
		return choice.videoPath;
	}

	bool CVideoPlayerImpl::ChoosePlan(const char* path, SPlan& plan)
	{
		plan = SPlan();
		plan.videoPath = path;
		if (IsDescriptorPath(path))
		{
			if (!LoadVideoDescriptor(path, m_desc))
				return false;
			m_descValid = true;
			plan.videoPath = PickVariantPath();
			ResolveAudio(plan, true);
			Log("player %u: descriptor <%s> route=%s video=<%s> language=<%s>%s%s markers=%u", m_id, path, RouteName(m_desc.route), plan.videoPath.c_str(),
				plan.language.c_str(), plan.externalPath.empty() ? "" : " external=", plan.externalPath.c_str(), static_cast<unsigned>(m_desc.markers.size()));
			if (m_desc.avOffsetMs != 0.0)
			{
				Log("player %u: avOffsetMs=%.1f of <%s> applied (picture %s)", m_id, m_desc.avOffsetMs, path, m_desc.avOffsetMs > 0.0 ? "later" : "earlier");
				if (std::fabs(m_desc.avOffsetMs) > cev::PlayerCore::kMaxAvOffsetSeconds * 1000.0)
					LogWarning("player %u: avOffsetMs=%.1f of <%s> is limited to +-%.0f ms", m_id, m_desc.avOffsetMs, path, cev::PlayerCore::kMaxAvOffsetSeconds * 1000.0);
			}
		}
		if (!FileExists(plan.videoPath))
		{
			LogError("player %u: video file <%s> not found", m_id, plan.videoPath.c_str());
			return false;
		}
		return true;
	}

	//! (Re)creates the clock and the cores for `plan`. Clock rule, decided here once per media (never swapped while
	//! it plays): the player runs on an AudioClock when its audio can reach the mixer (audio enabled, SDL audio not
	//! disabled by cev_disableEmbeddedAudio and not failed, route is not cue); otherwise on the InternalClock. An
	//! AudioClock whose voice never starts (no audio track, device failure at the first open) degrades to the wall
	//! clock by itself; ClockSourceName() then reports "internal".
	void CVideoPlayerImpl::BuildCores(const SPlan& plan)
	{
		StopVoice();
		m_audioCore.reset();
		m_core.reset();
		m_audioClock.reset();
		m_cueClock.reset(); // stops its trigger (destructor)

		const bool dedicated = gEnv == nullptr || gEnv->IsDedicated();
		const bool audioWanted = mEnv->m_cevAudioEnable != 0 && !dedicated && !plan.cue;
		const bool embedDisabled = mEnv->m_cevDisableEmbeddedAudio != 0;
		const bool deviceOk = !m_manager.Audio().DeviceFailed();
		const bool useClock = audioWanted && !embedDisabled && deviceOk;
		const bool external = useClock && !plan.externalPath.empty();

		cev::PlayerConfig cfg = MakeConfig(m_manager);
		cfg.avOffsetSeconds = m_descValid ? m_desc.avOffsetMs / 1000.0 : 0.0; // all routes (PlayerCore clamps it)
		if (useClock)
			m_audioClock = std::make_unique<cev::AudioClock>(CAudioMixer::kDeviceRate);
		cfg.externalClock = m_audioClock.get();
		// Cue route: the clock is the transport of the CryAudio trigger. Not affected by cev_disableEmbeddedAudio
		// (no SDL device of ours is involved); cev_audio_enable = 0 and dedicated servers play it silently.
		if (plan.cue && !plan.trigger.empty() && mEnv->m_cevAudioEnable != 0 && !dedicated && gEnv != nullptr && gEnv->pAudioSystem != nullptr)
		{
			SCueConfig cue;
			cue.trigger = plan.trigger;
			cue.pauseTrigger = plan.pauseTrigger;
			cue.resumeTrigger = plan.resumeTrigger;
			m_cueClock = std::make_unique<CCueClock>(m_id, cue);
			cfg.externalClock = m_cueClock.get();
		}
		if (embedDisabled && audioWanted)
		{
			// Decode just enough to know that the video has audio (reported once), nothing is ever played.
			cfg.decodeAudio = true;
			cfg.audioBufferMs = 50;
		}
		else
		{
			cfg.decodeAudio = useClock && !external; // the external file replaces the embedded track
			if (cfg.decodeAudio)
				cfg.outputSampleRate = CAudioMixer::kDeviceRate;
		}
		m_core = std::make_unique<cev::PlayerCore>(cfg);

		if (external)
		{
			cev::PlayerConfig acfg = MakeConfig(m_manager);
			acfg.outputRgba = false;
			acfg.enableStats = false;
			acfg.decodeAudio = true;
			acfg.decodeVideo = false; // an external language file is audio only: never decode its picture, even if it has one
			acfg.externalClock = m_audioClock.get(); // the same clock as the video: the audio is the master
			acfg.outputSampleRate = CAudioMixer::kDeviceRate;
			m_audioCore = std::make_unique<cev::PlayerCore>(acfg);
		}
	}

	void CVideoPlayerImpl::Unload()
	{
		StopVoice();
		m_audioCore.reset();
		m_core->Unload();
		if (m_cueClock)
			m_cueClock->Shutdown(); // trigger stopped, object released; the clock object stays (the core still points to it)
		m_phase = EPhase::None;
		m_info = cev::MediaInfo();
		m_hasAudio = false;
		m_path.clear();
		m_desc = SVideoDescriptor();
		m_descValid = false;
		m_plan = SPlan();
		m_language.clear();
		m_reloading = false;
		m_playPending = false;
		m_seekPending = false;
		m_pausedByLevelLoad = false;
		m_needUpload = false;
		m_endedSent = false;
		m_eventCount = 0;
		SetFullscreen(false, m_fit);
		if (HasRenderResources())
		{
			if (m_manager.RenderCallsAllowed())
				ReleaseTexture(false);
			else
				m_releasePending = true;
		}
	}

	void CVideoPlayerImpl::OnLoaded()
	{
		m_info = m_core->GetInfo();
		m_phase = EPhase::Ready;
		m_loadStatus = cev::Status::Ok;

		// The external audio file: usable only when it has audio; otherwise the video plays silently.
		if (m_audioCore)
		{
			if (m_audioCore->GetLoadState() == cev::LoadState::Ready && m_audioCore->GetInfo().hasAudio)
			{
				m_hasAudio = true;
			}
			else
			{
				LogWarning("player %u: audio file <%s> could not be loaded or has no audio, playing without audio", m_id, m_plan.externalPath.c_str());
				m_audioCore.reset();
			}
		}
		else if (m_cueClock)
		{
			m_hasAudio = true; // the sound is the CryAudio trigger (the video file itself is silent / its track is not played)
		}
		else
		{
			m_hasAudio = m_info.hasAudio && !m_plan.cue;
		}
		Log("player %u ready: <%s> %dx%d %.2fs%s%s", m_id, m_plan.videoPath.c_str(), m_info.width, m_info.height, m_info.durationSeconds,
			m_info.hasAlpha ? " alpha" : "", m_hasAudio ? " audio" : "");

		m_core->SetLoop(m_loop);
		CoresSetRate(static_cast<double>(m_rate));
		m_core->SetStatsEnabled(m_manager.GlobalStatsEnabled());
		EnsureTexture();
		if (m_phase != EPhase::Ready)
			return; // the texture was refused: the player failed (OnError queued)

		if (mEnv->m_cevDisableEmbeddedAudio != 0 && !m_silentWarned && (m_info.hasAudio || !m_plan.externalPath.empty()))
		{
			m_silentWarned = true;
			LogWarning("player %u: <%s> has audio but cev_disableEmbeddedAudio is set: playing silently", m_id, m_path.c_str());
		}
		StartVoice();

		if (m_reloading)
		{
			// Language change: continue where the old media was, without reporting a new Ready / Started.
			m_reloading = false;
			CoresSeek(m_reloadTime);
			m_needUpload = true;
			if (m_reloadRestore != ERestore::None)
			{
				CoresPlay();
				if (m_reloadRestore == ERestore::Paused)
					CoresPause();
				else
					ReapplyLevelLoadPause();
			}
			return;
		}

		Emit(EEvent::Ready);
		if (m_seekPending)
		{
			m_seekPending = false;
			CoresSeek(m_seekTime);
		}
		if (m_playPending)
		{
			m_playPending = false;
			if (cev::Succeeded(CoresPlay()))
			{
				Emit(EEvent::Started);
				ReapplyLevelLoadPause();
			}
		}
	}

	void CVideoPlayerImpl::OnLoadFailed()
	{
		const cev::LoadError err = m_core->GetLoadError();
		m_phase = EPhase::Failed;
		m_loadStatus = err.status;
		m_playPending = false;
		m_reloading = false;
		LogError("player %u: loading <%s> failed: %s (%s)", m_id, m_plan.videoPath.c_str(), cev::StatusToString(err.status), cev::StatusDetailToString(err.detail));
		Emit(EEvent::Error, static_cast<int>(err.status));
	}

	void CVideoPlayerImpl::FailPlayer(cev::Status status, const char* reason)
	{
		LogError("player %u: <%s> failed: %s (%s)", m_id, m_path.c_str(), cev::StatusToString(status), reason != nullptr ? reason : "");
		StopVoice();
		m_audioCore.reset();
		m_core->Unload();
		if (m_cueClock)
			m_cueClock->Shutdown();
		m_phase = EPhase::Failed;
		m_loadStatus = status;
		m_playPending = false;
		m_seekPending = false;
		m_reloading = false;
		m_pausedByLevelLoad = false;
		m_needUpload = false;
		m_endedSent = false;
		Emit(EEvent::Error, static_cast<int>(status));
	}

	// ------------------------------------------------------------------------------------------------ core group
	// The video core and the external audio core share one clock (the audio core's AudioClock): every control
	// call goes to both so that their states never diverge. The audio core is never Update()d to its own end (see
	// Tick), it is only a PCM source.
	cev::Status CVideoPlayerImpl::CoresPlay()
	{
		if (m_audioCore && m_core->GetState() == cev::PlayerState::Ended)
			m_audioCore->Seek(0.0); // the video core rewinds inside Play(); the audio follows
		const cev::Status st = m_core->Play();
		if (m_audioCore && cev::Succeeded(st))
			m_audioCore->Play();
		return st;
	}

	void CVideoPlayerImpl::CoresPause()
	{
		m_core->Pause();
		if (m_audioCore)
			m_audioCore->Pause();
	}

	void CVideoPlayerImpl::CoresResume()
	{
		m_core->Resume();
		if (m_audioCore)
			m_audioCore->Resume();
	}

	void CVideoPlayerImpl::CoresStop()
	{
		if (m_audioCore)
			m_audioCore->Stop();
		m_core->Stop(); // last: it leaves the shared clock at 0
	}

	cev::Status CVideoPlayerImpl::CoresSeek(double seconds)
	{
		if (m_audioCore)
			m_audioCore->Seek(seconds); // clamps to its own duration and moves the clock: the video seek goes last
		return m_core->Seek(seconds);
	}

	void CVideoPlayerImpl::CoresSetRate(double rate)
	{
		m_core->SetRate(rate);
		if (m_audioCore)
			m_audioCore->SetRate(rate);
	}

	// ------------------------------------------------------------------------------------------------ control
	void CVideoPlayerImpl::Play()
	{
		if (m_destroyed)
			return;
		m_fullscreenHidden = false; // a Stop() hid the fullscreen picture: Play shows it again
		if (m_phase == EPhase::Loading || m_phase == EPhase::None)
		{
			m_playPending = true; // starts as soon as the media is ready
			return;
		}
		if (m_phase != EPhase::Ready)
			return;
		const cev::PlayerState before = m_core->GetState();
		if (before == cev::PlayerState::Playing)
			return;
		if (cev::Succeeded(CoresPlay()))
		{
			m_endedSent = false;
			if (before == cev::PlayerState::Ended || before == cev::PlayerState::Stopped)
				m_markerResync = true;
			Emit(before == cev::PlayerState::Paused ? EEvent::Resumed : EEvent::Started);
			ReapplyLevelLoadPause();
		}
	}

	void CVideoPlayerImpl::Pause()
	{
		if (m_destroyed)
			return;
		if (m_phase != EPhase::Ready)
		{
			m_playPending = false;
			return;
		}
		if (m_core->GetState() == cev::PlayerState::Playing)
		{
			CoresPause();
			Emit(EEvent::Paused);
		}
		m_pausedByLevelLoad = false;
	}

	void CVideoPlayerImpl::Resume()
	{
		if (m_destroyed || m_phase != EPhase::Ready)
			return;
		if (m_core->GetState() == cev::PlayerState::Paused)
		{
			CoresResume();
			m_pausedByLevelLoad = false;
			Emit(EEvent::Resumed);
			ReapplyLevelLoadPause();
		}
	}

	void CVideoPlayerImpl::Stop()
	{
		if (m_destroyed)
			return;
		m_fullscreenHidden = true; // Stop (also: a skipped video) takes the fullscreen picture off the screen until Play()
		if (m_phase != EPhase::Ready)
		{
			m_playPending = false;
			return;
		}
		CoresStop();
		m_needUpload = true; // the first frame is shown again
		m_endedSent = false;
		m_pausedByLevelLoad = false;
		m_markerResync = true;
		Emit(EEvent::Stopped);
	}

	void CVideoPlayerImpl::Seek(double seconds)
	{
		if (m_destroyed)
			return;
		if (m_phase == EPhase::Ready)
		{
			if (cev::Succeeded(CoresSeek(seconds)))
			{
				m_endedSent = false;
				m_needUpload = true;
				m_markerResync = true;
			}
		}
		else if (m_phase == EPhase::Loading || m_phase == EPhase::None)
		{
			m_seekPending = true;
			m_seekTime = seconds;
		}
	}

	void CVideoPlayerImpl::SetLoop(bool loop)
	{
		m_loop = loop;
		m_core->SetLoop(loop);
	}

	void CVideoPlayerImpl::SetRate(float rate)
	{
		if (!(rate > 0.f)) // NaN / <= 0: ignored; the rest is clamped to the range PlayerCore supports (like PlayerCore::SetRate)
			return;
		rate = static_cast<float>(std::min(cev::PlayerCore::kMaxRate, std::max(cev::PlayerCore::kMinRate, static_cast<double>(rate))));
		m_rate = rate;
		CoresSetRate(static_cast<double>(rate));
	}

	void CVideoPlayerImpl::SetVolume(float volume)
	{
		m_volume = std::min(1.f, std::max(0.f, volume));
		if (m_voice >= 0)
			m_manager.Audio().SetVoiceVolume(m_voice, m_volume);
	}

	EVideoPlayerState CVideoPlayerImpl::GetState() const
	{
		switch (m_phase)
		{
		case EPhase::None:    return EVideoPlayerState::Stopped;
		case EPhase::Loading: return EVideoPlayerState::Loading;
		case EPhase::Failed:  return EVideoPlayerState::Error;
		case EPhase::Ready:   break;
		}
		switch (m_core->GetState())
		{
		case cev::PlayerState::Stopped: return EVideoPlayerState::Stopped;
		case cev::PlayerState::Playing: return EVideoPlayerState::Playing;
		case cev::PlayerState::Paused:  return m_pausedByLevelLoad ? EVideoPlayerState::Playing : EVideoPlayerState::Paused;
		case cev::PlayerState::Ended:   return EVideoPlayerState::Ended;
		}
		return EVideoPlayerState::Stopped;
	}

	double CVideoPlayerImpl::GetTime() const
	{
		return m_phase == EPhase::Ready ? m_core->GetTime() : 0.0;
	}

	bool CVideoPlayerImpl::GetAvOffsetMs(double& ms) const
	{
		if (m_phase != EPhase::Ready || !m_info.hasVideo)
			return false;
		cev::FrameView view;
		if (!m_core->GetFrame(view))
			return false;
		ms = (m_core->GetTime() - view.pts) * 1000.0;
		return true;
	}

	// ------------------------------------------------------------------------------------------------ languages
	void CVideoPlayerImpl::SetLanguage(const char* name)
	{
		if (m_destroyed)
			return;
		const std::string requested = name != nullptr ? name : "";
		if (!requested.empty() && m_descValid && FindLanguage(m_desc, requested.c_str()) < 0)
		{
			LogWarning("player %u: language <%s> is not listed in <%s>, ignored", m_id, requested.c_str(), m_path.c_str());
			return;
		}
		if (!requested.empty() && !m_descValid && (m_phase == EPhase::Ready || m_phase == EPhase::Loading))
			LogWarning("player %u: <%s> has no descriptor and therefore no languages, language <%s> ignored", m_id, m_path.c_str(), requested.c_str());
		m_requestedLanguage = requested;
		if (!m_descValid || (m_phase != EPhase::Ready && m_phase != EPhase::Loading))
			return; // applied by the next Load()

		SPlan next = m_plan;
		ResolveAudio(next, false);
		if (next.language == m_plan.language && next.externalPath == m_plan.externalPath && next.cue == m_plan.cue && next.trigger == m_plan.trigger)
			return; // same audio source
		ReloadForLanguage();
	}

	void CVideoPlayerImpl::SetVariant(int variant)
	{
		if (m_destroyed)
			return;
		if (variant < kVariantFollowCvar)
			variant = kVariantFollowCvar;
		if (variant == m_variantRequest)
			return;
		m_variantRequest = variant;
		if (!m_descValid || m_desc.variants.empty() || (m_phase != EPhase::Ready && m_phase != EPhase::Loading))
			return; // applied by the next Load()
		ReloadForLanguage();
	}

	//! Remembers time and state, loads the media again (new language or variant) and continues there.
	void CVideoPlayerImpl::ReloadForLanguage()
	{
		const bool wasReady = m_phase == EPhase::Ready;
		double time = 0.0;
		ERestore restore = ERestore::None;
		if (wasReady)
		{
			const cev::PlayerState state = m_core->GetState();
			time = m_core->GetTime();
			if (state == cev::PlayerState::Playing || m_pausedByLevelLoad)
				restore = ERestore::Playing;
			else if (state == cev::PlayerState::Paused)
				restore = ERestore::Paused;
			else if (state == cev::PlayerState::Ended)
				time = 0.0;
		}
		const bool playPending = m_playPending;
		const bool seekPending = m_seekPending;
		const double seekTime = m_seekTime;
		const std::string path = m_path;
		Log("player %u: language / variant change, reloading <%s> at %.2fs", m_id, path.c_str(), time);

		if (!LoadInternal(path.c_str(), true))
			return; // OnError is queued
		m_playPending = playPending;
		m_seekPending = seekPending;
		m_seekTime = seekTime;
		if (wasReady)
		{
			m_reloading = true;
			m_reloadTime = time;
			m_reloadRestore = restore;
		}
	}

	// ------------------------------------------------------------------------------------------------ markers
	void CVideoPlayerImpl::ResyncMarkers()
	{
		// next marker = first one at or after the current time (those before count as passed)
		const double t = m_core->GetTime() - 1e-6;
		size_t i = 0;
		while (i < m_desc.markers.size() && m_desc.markers[i].time < t)
			++i;
		m_markerNext = i;
		m_markerResync = false;
		m_markerPrev = m_core->GetTime();
	}

	void CVideoPlayerImpl::UpdateMarkers(bool looped)
	{
		const std::vector<SVideoDescriptor::SMarker>& markers = m_desc.markers;
		if (markers.empty())
			return;
		if (looped)
		{
			// The loop restarted the clock: markers up to the end of the video that were not reached yet fire
			// now, then the list starts over.
			while (m_markerNext < markers.size() && markers[m_markerNext].time <= m_info.durationSeconds)
				EmitMarker(markers[m_markerNext++]);
			m_markerResync = true;
		}
		if (m_markerResync)
			ResyncMarkers();
		const double t = m_core->GetTime();
		if (t < m_markerPrev - 1e-3)
		{
			ResyncMarkers(); // went backwards without a seek call (e.g. the core rewound)
		}
		while (m_markerNext < markers.size() && markers[m_markerNext].time <= t + 1e-6)
			EmitMarker(markers[m_markerNext++]);
		m_markerPrev = t;
	}

	// ------------------------------------------------------------------------------------------------ audio
	void CVideoPlayerImpl::StartVoice()
	{
		if (m_voice >= 0 || !m_audioClock || !m_hasAudio || mEnv->m_cevAudioEnable == 0 || mEnv->m_cevDisableEmbeddedAudio != 0
			|| gEnv == nullptr || gEnv->IsDedicated())
			return;
		cev::PlayerCore* const pSource = m_audioCore ? m_audioCore.get() : m_core.get();
		m_voice = m_manager.Audio().AddVoice(pSource, m_audioClock.get());
		if (m_voice >= 0)
		{
			m_manager.Audio().SetVoiceVolume(m_voice, m_volume);
			Log("player %u: audio voice %d (%s), clock = audio", m_id, m_voice, m_audioCore ? "external file" : "embedded track");
		}
		else
		{
			Log("player %u: no audio voice, clock = internal (wall clock)", m_id);
		}
	}

	void CVideoPlayerImpl::StopVoice()
	{
		if (m_voice >= 0)
		{
			m_manager.Audio().RemoveVoice(m_voice);
			m_voice = -1;
		}
	}

	// ------------------------------------------------------------------------------------------------ textures
	bool CVideoPlayerImpl::EnsureTexture()
	{
		if (m_phase != EPhase::Ready || !m_info.hasVideo || m_info.width <= 0 || m_info.height <= 0)
			return false;
		if (m_pTexture != nullptr && (m_textureWidth != m_info.width || m_textureHeight != m_info.height))
		{
			if (!m_manager.RenderCallsAllowed())
				return false;
			ReleaseTexture(true); // size changed: new texture, bindings move over
		}
		if (m_pTexture != nullptr)
			return true;
		if (!RenderBridge::Available() || !m_manager.RenderCallsAllowed())
			return false;

		RenderBridge::ProbeOnce();
		if (m_autoName)
			m_textureName = m_manager.MakeAutoTextureName(m_info.width, m_info.height, this);
		const char* failure = "";
		ITexture* const pTexture = RenderBridge::CreateTexture(m_textureName.c_str(), m_info.width, m_info.height, &failure);
		if (pTexture == nullptr)
		{
			LogError("player %u: cannot create texture <%s> %dx%d: %s", m_id, m_textureName.c_str(), m_info.width, m_info.height, failure);
			FailPlayer(cev::Status::Unsupported, failure);
			return false;
		}
		m_pTexture = pTexture;
		m_textureId = pTexture->GetTextureID();
		m_textureChanged = true;
		LogDebug("player %u: texture <%s> id %d %dx%d", m_id, m_textureName.c_str(), m_textureId, m_info.width, m_info.height);
		m_textureWidth = m_info.width;
		m_textureHeight = m_info.height;
		m_needUpload = true;
		ApplyBindings();
		return true;
	}

	void CVideoPlayerImpl::ReleaseTexture(bool keepBindings)
	{
		RemoveAppliedBindings();
		if (!keepBindings)
		{
			for (SBinding& b : m_bindings)
			{
				if (b.pMaterial != nullptr)
					b.pMaterial->Release();
			}
			m_bindings.clear();
		}
		else
		{
			for (SBinding& b : m_bindings)
				b.failed = false; // the new texture gets a fresh try
		}
		if (m_pTexture != nullptr)
		{
			RenderBridge::ReleaseTexture(m_pTexture);
			m_pTexture = nullptr;
			m_textureChanged = true;
		}
		m_textureId = 0;
		m_textureWidth = 0;
		m_textureHeight = 0;
		m_needUpload = false;
		m_releasePending = false;
	}

	void CVideoPlayerImpl::ReleaseRenderResources()
	{
		ReleaseTexture(false);
	}

	void CVideoPlayerImpl::TakeUploadTiming(float& avgMs, float& p99Ms, uint32_t& samples)
	{
		const uint32_t n = std::min(m_uploadMsCount, kUploadTimingWindow);
		samples = m_uploadMsCount;
		avgMs = 0.f;
		p99Ms = 0.f;
		if (n > 0)
		{
			float values[kUploadTimingWindow];
			double sum = 0.0;
			for (uint32_t i = 0; i < n; ++i)
			{
				values[i] = m_uploadMs[i];
				sum += values[i];
			}
			uint32_t k = (n * 99u + 99u) / 100u;
			k = k == 0 ? 0 : std::min(k - 1, n - 1);
			std::nth_element(values, values + k, values + n);
			avgMs = static_cast<float>(sum / n);
			p99Ms = values[k];
		}
		m_uploadMsCount = 0;
	}

	void CVideoPlayerImpl::UploadFrame()
	{
		cev::FrameView view;
		if (!m_core->GetFrame(view) || view.rgba == nullptr)
			return;
		if (view.width != m_textureWidth || view.height != m_textureHeight)
			return;
		if (view.rgbaStride != view.width * 4)
		{
			// UpdateTextureInVideoMemory takes tightly packed rows only.
			if (!m_strideWarned)
			{
				m_strideWarned = true;
				LogError("player %u: RGBA stride %d is not tightly packed (%d), frames are not uploaded", m_id, view.rgbaStride, view.width * 4);
			}
			return;
		}
		const std::chrono::steady_clock::time_point uploadStart = std::chrono::steady_clock::now();
		const bool uploaded = RenderBridge::UpdateTexture(m_pTexture, view.rgba, view.width, view.height);
		if (uploaded)
		{
			// measurement (S0): main thread cost of the update call
			const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - uploadStart).count();
			m_uploadMs[m_uploadMsCount % kUploadTimingWindow] = ms;
			++m_uploadMsCount;
			m_lastUploadBytes = static_cast<uint64_t>(view.width) * static_cast<uint64_t>(view.height) * 4u;
			++m_uploads;
			++m_contentSerial;
			m_frameSerial = view.serial;
			m_core->NoteUpload(static_cast<uint64_t>(view.width) * static_cast<uint64_t>(view.height) * 4u);
			m_needUpload = false;
			Emit(EEvent::NewFrame, 0, view.serial);
		}
		else
		{
			// The texture no longer matches our buffer (another user recreated the same name at a different
			// size / format): never write into it, give up instead of retrying every frame.
			FailPlayer(cev::Status::InvalidState, "the output texture was changed by someone else");
		}
	}

	// ------------------------------------------------------------------------------------------------ material slots
	bool CVideoPlayerImpl::BindMaterial(IMaterial* pMaterial, int subMaterial, int textureSlot)
	{
		if (m_destroyed || pMaterial == nullptr || textureSlot < 0 || textureSlot >= EFTT_MAX)
			return false;
		for (SBinding& b : m_bindings)
		{
			if (b.pMaterial == pMaterial && b.subMaterial == subMaterial && b.textureSlot == textureSlot)
			{
				b.removeRequested = false;
				return true;
			}
		}
		pMaterial->AddRef();
		SBinding binding;
		binding.pMaterial = pMaterial;
		binding.subMaterial = subMaterial;
		binding.textureSlot = textureSlot;
		m_bindings.push_back(binding);
		if (m_pTexture != nullptr && m_manager.RenderCallsAllowed())
			ApplyBindings();
		return true;
	}

	void CVideoPlayerImpl::UnbindMaterial(IMaterial* pMaterial)
	{
		for (SBinding& b : m_bindings)
		{
			if (pMaterial == nullptr || b.pMaterial == pMaterial)
				b.removeRequested = true;
		}
		if (m_manager.RenderCallsAllowed())
			ProcessBindingRemovals();
	}

	void CVideoPlayerImpl::ApplyBindings()
	{
		ITexture* const pTexture = m_pTexture;
		if (pTexture == nullptr)
			return;
		for (SBinding& b : m_bindings)
		{
			if (b.applied || b.removeRequested || b.failed)
				continue;
			ITexture* pPrevious = nullptr;
			if (RenderBridge::SetMaterialTexture(b.pMaterial, b.subMaterial, b.textureSlot, pTexture, &pPrevious))
			{
				b.pPrevious = pPrevious;
				b.applied = true;
			}
			else
			{
				b.failed = true; // reported once, not retried every frame
				LogWarning("player %u: cannot bind <%s> to material <%s> sub %d slot %d (not retried)", m_id, m_textureName.c_str(), b.pMaterial->GetName(), b.subMaterial, b.textureSlot);
			}
		}
	}

	void CVideoPlayerImpl::RemoveAppliedBindings()
	{
		ITexture* const pTexture = m_pTexture;
		for (SBinding& b : m_bindings)
		{
			if (b.applied)
			{
				RenderBridge::RestoreMaterialTexture(b.pMaterial, b.subMaterial, b.textureSlot, pTexture, b.pPrevious);
				b.pPrevious = nullptr;
				b.applied = false;
			}
		}
	}

	void CVideoPlayerImpl::ProcessBindingRemovals()
	{
		ITexture* const pTexture = m_pTexture;
		for (std::vector<SBinding>::iterator it = m_bindings.begin(); it != m_bindings.end();)
		{
			if (!it->removeRequested)
			{
				++it;
				continue;
			}
			if (it->applied)
				RenderBridge::RestoreMaterialTexture(it->pMaterial, it->subMaterial, it->textureSlot, pTexture, it->pPrevious);
			if (it->pMaterial != nullptr)
				it->pMaterial->Release();
			it = m_bindings.erase(it);
		}
	}

	// ------------------------------------------------------------------------------------------------ outputs
	void CVideoPlayerImpl::SetFullscreen(bool enable, EVideoFit fit)
	{
		m_fit = fit;
		if (enable == m_fullscreen)
			return;
		if (enable && m_destroyed)
			return;
		m_fullscreen = enable;
		m_manager.SetFullscreen(this, enable);
	}

	// ------------------------------------------------------------------------------------------------ frame step
	void CVideoPlayerImpl::Tick(float dt)
	{
		(void)dt; // the cores run on their own clocks (audio / steady clock); kept for the load-time tick (M10)
		if (m_destroyed)
			return;
		TickBody();
		if (m_surfaceRegistered)
			PublishSnapshot();
	}

	void CVideoPlayerImpl::PublishSnapshot()
	{
		// No engine call here (rule 10): only members of this player.
		if (m_textureChanged)
		{
			m_textureChanged = false;
			++m_generation;
			++m_contentSerial;
		}
		CryTexSourceInfo info = {};
		info.size = sizeof(info);
		info.textureId = m_textureId;
		info.width = GetWidth();
		info.height = GetHeight();
		const EVideoPlayerState state = GetState();
		info.flags = 1u /* premultiplied */ | (HasAlpha() ? 2u : 0u) | (state == EVideoPlayerState::Playing ? 4u : 0u);
		info.contentSerial = m_contentSerial;
		info.generation = m_generation;
		info.layout = CTS_LAYOUT_RGBA8;
		switch (m_phase)
		{
		case EPhase::None:    info.state = 0; break;
		case EPhase::Loading: info.state = 1; break;
		case EPhase::Failed:  info.state = 6; info.errorCode = static_cast<int32_t>(m_loadStatus); break;
		case EPhase::Ready:
			switch (state)
			{
			case EVideoPlayerState::Playing: info.state = 3; break;
			case EVideoPlayerState::Paused:  info.state = 4; break;
			case EVideoPlayerState::Ended:   info.state = 5; break;
			default:                         info.state = 2; break;
			}
			break;
		}
		info.timeSeconds = GetTime();
		info.durationSeconds = GetDuration();
		info.loopCount = m_seenLoops;
		Contract::PublishSnapshot(m_surfaceId.c_str(), info);
	}

	void CVideoPlayerImpl::TickBody()
	{
		if (m_releasePending && HasRenderResources())
			ReleaseTexture(false);

		if (m_phase == EPhase::Loading)
		{
			const cev::LoadState ls = m_core->GetLoadState();
			if (ls == cev::LoadState::Ready)
			{
				// The external language file loads in parallel: wait for it (a Ready video must not drop an audio
				// file that is merely still loading). Failed / Ready-without-audio is handled by OnLoaded().
				bool audioPending = m_audioCore && m_audioCore->GetLoadState() == cev::LoadState::Loading;
				if (audioPending)
				{
					const double now = SteadySeconds();
					if (m_audioWaitSince < 0.0)
						m_audioWaitSince = now;
					if (now - m_audioWaitSince >= kExternalAudioTimeoutSeconds)
					{
						LogWarning("player %u: audio file <%s> still loading after %.0f s, playing without it", m_id, m_plan.externalPath.c_str(), kExternalAudioTimeoutSeconds);
						m_audioCore.reset();
						audioPending = false;
					}
				}
				if (!audioPending)
					OnLoaded();
			}
			else if (ls == cev::LoadState::Failed)
				OnLoadFailed();
		}
		if (m_phase == EPhase::Ready && m_core->GetLoadState() == cev::LoadState::Failed)
		{
			// The core gave up while playing (stream changed resolution, internal error): stop presenting.
			const cev::LoadError err = m_core->GetLoadError();
			FailPlayer(err.status, cev::StatusDetailToString(err.detail));
			return;
		}
		if (m_phase != EPhase::Ready)
			return;

		if (m_info.hasVideo)
		{
			if (m_pTexture == nullptr)
				EnsureTexture(); // retry (renderer / level loading state changed)
			else if (!m_bindings.empty())
			{
				ProcessBindingRemovals();
				ApplyBindings();
			}
		}

		const bool fresh = m_core->Update();

		const uint32_t loops = m_core->GetLoopCount();
		const bool looped = m_seenLoops < loops;
		while (m_seenLoops < loops)
		{
			++m_seenLoops;
			Emit(EEvent::Looped);
		}
		const cev::PlayerState state = m_core->GetState();
		if (m_audioCore)
		{
			// The video core ran first. The audio file follows the loop; it is never Update()d to its own end
			// (that would pause the shared clock): only while the clock is clearly before its duration, which is
			// all the worker wake-up inside Update() is needed for.
			if (looped)
				m_audioCore->Seek(0.0);
			if (state == cev::PlayerState::Ended)
			{
				if (m_audioCore->GetState() == cev::PlayerState::Playing)
					m_audioCore->Pause();
			}
			else if (state == cev::PlayerState::Playing && m_audioCore->GetState() == cev::PlayerState::Playing
				&& m_core->GetTime() < m_audioCore->GetInfo().durationSeconds - 0.05)
			{
				m_audioCore->Update();
			}
		}
		if (state == cev::PlayerState::Ended)
		{
			if (!m_endedSent)
			{
				m_endedSent = true;
				Emit(EEvent::Ended);
			}
		}
		else
		{
			m_endedSent = false;
		}
		UpdateMarkers(looped);

		if ((fresh || m_needUpload) && m_pTexture != nullptr)
			UploadFrame();
	}

	// ------------------------------------------------------------------------------------------------ level loading
	void CVideoPlayerImpl::ReapplyLevelLoadPause()
	{
		if (m_manager.IsLevelLoading() && m_phase == EPhase::Ready && m_core->GetState() == cev::PlayerState::Playing)
		{
			CoresPause();
			m_pausedByLevelLoad = true;
		}
	}

	void CVideoPlayerImpl::OnLevelLoadBegin()
	{
		if (m_phase == EPhase::Ready && m_core->GetState() == cev::PlayerState::Playing)
		{
			CoresPause();
			m_pausedByLevelLoad = true;
		}
	}

	void CVideoPlayerImpl::OnLevelLoadEnd()
	{
		if (m_pausedByLevelLoad)
		{
			m_pausedByLevelLoad = false;
			if (m_phase == EPhase::Ready && m_core->GetState() == cev::PlayerState::Paused)
				CoresResume(); // continues from the paused position: the clock is re-synchronised
		}
	}

	// ------------------------------------------------------------------------------------------------ teardown
	void CVideoPlayerImpl::PrepareDestroy()
	{
		m_destroyed = true;
		if (m_surfaceRegistered)
		{
			m_surfaceRegistered = false; // CryTexSource query returns 0 for this id from now on
			Contract::UnregisterSurface(m_surfaceId.c_str());
		}
		m_listeners.clear();
		m_eventCount = 0;
		m_playPending = false;
		if (m_fullscreen)
		{
			m_fullscreen = false;
			m_manager.SetFullscreen(this, false);
		}
		StopVoice();
		m_audioCore.reset();
		m_core->Unload();
		if (m_cueClock)
			m_cueClock->Shutdown();
		m_phase = EPhase::None;
	}

	void CVideoPlayerImpl::OnLevelUnload()
	{
		if (m_cueClock)
			m_cueClock->OnLevelUnload();
	}

	void CVideoPlayerImpl::ListState(char* buffer, size_t size) const
	{
		const float peak = m_voice >= 0 ? m_manager.Audio().TakeVoicePeak(m_voice) : 0.f;
		char cue[200] = "";
		if (m_cueClock)
		{
			double trigger = 0.0;
			double media = 0.0;
			if (m_cueClock->GetDebugPosition(trigger, media))
				std::snprintf(cue, sizeof(cue), "  cue=<%s> triggerPos=%.3f mediaT=%.3f diff=%+.1fms", m_cueClock->Trigger().c_str(), trigger, media, (trigger - media) * 1000.0);
			else
				std::snprintf(cue, sizeof(cue), "  cue=<%s> triggerPos=n/a", m_cueClock->Trigger().c_str());
		}
		std::snprintf(buffer, size, "player %u  %s  t=%.2f/%.2f  %dx%d%s  uploads=%llu  tex=%s(%d)%s  audio=%s  peak=%.3f  clock=%s%s  lang=%s  variant=%s  <%s>",
			m_id, StateName(GetState()), GetTime(), GetDuration(), GetWidth(), GetHeight(), HasAlpha() ? " alpha" : "",
			static_cast<unsigned long long>(m_uploads), m_textureName.c_str(), m_textureId, m_fullscreen ? (m_fullscreenHidden ? "  fullscreen(hidden)" : "  fullscreen") : "",
			!m_hasAudio ? "none" : (m_voice >= 0 ? (m_audioCore ? "external" : "embedded") : (m_cueClock ? "cue" : "silent")), peak, ClockSourceName(), cue,
			m_language.empty() ? "-" : m_language.c_str(), m_variantIndex < 0 ? "main" : std::to_string(m_variantIndex).c_str(), m_path.c_str());
	}
}
