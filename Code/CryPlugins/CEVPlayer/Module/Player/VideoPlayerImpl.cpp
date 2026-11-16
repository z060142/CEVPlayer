#include "StdAfx.h"
#include "PluginEnv.h"
#include "VideoPlayerImpl.h"

#include <algorithm>
#include <cstdio>

#include <CrySystem/File/ICryPak.h>
#include <CryRenderer/IShader.h>
#include <Cry3DEngine/IMaterial.h>

#include "Audio/AudioMixer.h"
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
			cfg.decodeAudio = mEnv->m_cevAudioEnable != 0 && gEnv != nullptr && !gEnv->IsDedicated();
			cfg.videoThreads = static_cast<uint32_t>(std::max(0, mEnv->m_cevDecodeThreads));
			cfg.enableStats = manager.GlobalStatsEnabled();
			cfg.pool = manager.AcquirePool();
			return cfg;
		}
	}

	CVideoPlayerImpl::CVideoPlayerImpl(CPlayerManager& manager, uint32_t id, const char* textureName)
		: m_manager(manager)
		, m_core(MakeConfig(manager))
		, m_id(id)
	{
		if (textureName != nullptr && textureName[0] != '\0')
		{
			m_textureName = textureName;
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
		if (m_destroyed)
			return false;

		StopVoice();
		m_core.Unload();
		m_phase = EPhase::None;
		m_info = cev::MediaInfo();
		m_seenLoops = 0;
		m_endedSent = false;
		m_pausedByLevelLoad = false;
		m_needUpload = false;
		m_frameSerial = 0;
		m_seekPending = false;
		m_eventCount = 0;

		const bool pathOk = path != nullptr && path[0] != '\0';
		m_path = pathOk ? path : "";
		if (!pathOk || gEnv == nullptr || gEnv->pCryPak == nullptr || !gEnv->pCryPak->IsFileExist(path))
		{
			m_phase = EPhase::Failed;
			m_loadStatus = pathOk ? cev::Status::IoError : cev::Status::InvalidArgument;
			LogError("player %u: cannot load <%s>: %s", m_id, m_path.c_str(), pathOk ? "file not found" : "empty path");
			Emit(EEvent::Error, static_cast<int>(m_loadStatus));
			return false;
		}

		const cev::Status status = m_core.Load(std::make_unique<CCryPakByteSource>(path));
		if (cev::Failed(status))
		{
			m_phase = EPhase::Failed;
			m_loadStatus = status;
			LogError("player %u: cannot start loading <%s>: %s", m_id, path, cev::StatusToString(status));
			Emit(EEvent::Error, static_cast<int>(status));
			return false;
		}
		m_phase = EPhase::Loading;
		m_loadStatus = cev::Status::Ok;
		LogDebug("player %u: loading <%s>", m_id, path);
		return true;
	}

	void CVideoPlayerImpl::Unload()
	{
		StopVoice();
		m_core.Unload();
		m_phase = EPhase::None;
		m_info = cev::MediaInfo();
		m_path.clear();
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
		m_info = m_core.GetInfo();
		m_phase = EPhase::Ready;
		m_loadStatus = cev::Status::Ok;
		Log("player %u ready: <%s> %dx%d %.2fs%s%s", m_id, m_path.c_str(), m_info.width, m_info.height, m_info.durationSeconds,
			m_info.hasAlpha ? " alpha" : "", m_info.hasAudio ? " audio" : "");

		m_core.SetLoop(m_loop);
		m_core.SetRate(static_cast<double>(m_rate));
		m_core.SetStatsEnabled(m_manager.GlobalStatsEnabled());
		EnsureTexture();
		StartVoice();
		Emit(EEvent::Ready);

		if (m_seekPending)
		{
			m_seekPending = false;
			m_core.Seek(m_seekTime);
		}
		if (m_playPending)
		{
			m_playPending = false;
			if (cev::Succeeded(m_core.Play()))
			{
				Emit(EEvent::Started);
				ReapplyLevelLoadPause();
			}
		}
	}

	void CVideoPlayerImpl::OnLoadFailed()
	{
		const cev::LoadError err = m_core.GetLoadError();
		m_phase = EPhase::Failed;
		m_loadStatus = err.status;
		m_playPending = false;
		LogError("player %u: loading <%s> failed: %s (%s)", m_id, m_path.c_str(), cev::StatusToString(err.status), cev::StatusDetailToString(err.detail));
		Emit(EEvent::Error, static_cast<int>(err.status));
	}

	// ------------------------------------------------------------------------------------------------ control
	void CVideoPlayerImpl::Play()
	{
		if (m_destroyed)
			return;
		if (m_phase == EPhase::Loading || m_phase == EPhase::None)
		{
			m_playPending = true; // starts as soon as the media is ready
			return;
		}
		if (m_phase != EPhase::Ready)
			return;
		const cev::PlayerState before = m_core.GetState();
		if (before == cev::PlayerState::Playing)
			return;
		if (cev::Succeeded(m_core.Play()))
		{
			m_endedSent = false;
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
		if (m_core.GetState() == cev::PlayerState::Playing && cev::Succeeded(m_core.Pause()))
			Emit(EEvent::Paused);
		m_pausedByLevelLoad = false;
	}

	void CVideoPlayerImpl::Resume()
	{
		if (m_destroyed || m_phase != EPhase::Ready)
			return;
		if (m_core.GetState() == cev::PlayerState::Paused && cev::Succeeded(m_core.Resume()))
		{
			m_pausedByLevelLoad = false;
			Emit(EEvent::Resumed);
			ReapplyLevelLoadPause();
		}
	}

	void CVideoPlayerImpl::Stop()
	{
		if (m_destroyed)
			return;
		if (m_phase != EPhase::Ready)
		{
			m_playPending = false;
			return;
		}
		if (cev::Succeeded(m_core.Stop()))
		{
			m_needUpload = true; // the first frame is shown again
			m_endedSent = false;
			m_pausedByLevelLoad = false;
			Emit(EEvent::Stopped);
		}
	}

	void CVideoPlayerImpl::Seek(double seconds)
	{
		if (m_destroyed)
			return;
		if (m_phase == EPhase::Ready)
		{
			if (cev::Succeeded(m_core.Seek(seconds)))
			{
				m_endedSent = false;
				m_needUpload = true;
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
		m_core.SetLoop(loop);
	}

	void CVideoPlayerImpl::SetRate(float rate)
	{
		if (!(rate > 0.f) || rate > 16.f)
			return;
		m_rate = rate;
		m_core.SetRate(static_cast<double>(rate));
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
		switch (m_core.GetState())
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
		return m_phase == EPhase::Ready ? m_core.GetTime() : 0.0;
	}

	// ------------------------------------------------------------------------------------------------ audio
	void CVideoPlayerImpl::StartVoice()
	{
		if (m_voice >= 0 || !m_info.hasAudio || mEnv->m_cevAudioEnable == 0 || gEnv == nullptr || gEnv->IsDedicated())
			return;
		m_voice = m_manager.Audio().AddVoice(&m_core, m_info.audioSampleRate, m_info.audioChannels);
		if (m_voice >= 0)
			m_manager.Audio().SetVoiceVolume(m_voice, m_volume);
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
	ITexture* CVideoPlayerImpl::GetTexture() const
	{
		return m_textureId != 0 ? RenderBridge::GetTexture(m_textureId) : nullptr;
	}

	bool CVideoPlayerImpl::EnsureTexture()
	{
		if (m_phase != EPhase::Ready || !m_info.hasVideo || m_info.width <= 0 || m_info.height <= 0)
			return false;
		if (m_textureId != 0 && (m_textureWidth != m_info.width || m_textureHeight != m_info.height))
		{
			if (!m_manager.RenderCallsAllowed())
				return false;
			ReleaseTexture(true); // size changed: new texture, bindings move over
		}
		if (m_textureId != 0)
			return true;
		if (!RenderBridge::Available() || !m_manager.RenderCallsAllowed())
			return false;

		RenderBridge::ProbeOnce();
		const int id = RenderBridge::CreateTexture(m_textureName.c_str(), m_info.width, m_info.height);
		if (id <= 0)
		{
			LogError("player %u: cannot create texture <%s> %dx%d", m_id, m_textureName.c_str(), m_info.width, m_info.height);
			return false;
		}
		m_textureId = id;
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
		if (m_textureId != 0)
		{
			RenderBridge::RemoveTexture(m_textureId);
			m_textureId = 0;
		}
		m_textureWidth = 0;
		m_textureHeight = 0;
		m_needUpload = false;
		m_releasePending = false;
	}

	void CVideoPlayerImpl::ReleaseRenderResources()
	{
		ReleaseTexture(false);
	}

	void CVideoPlayerImpl::UploadFrame()
	{
		cev::FrameView view;
		if (!m_core.GetFrame(view) || view.rgba == nullptr)
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
		if (RenderBridge::UpdateTexture(m_textureId, view.rgba, view.width, view.height))
		{
			++m_uploads;
			m_frameSerial = view.serial;
			m_core.NoteUpload(static_cast<uint64_t>(view.width) * static_cast<uint64_t>(view.height) * 4u);
			m_needUpload = false;
			Emit(EEvent::NewFrame, 0, view.serial);
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
		if (m_textureId != 0 && m_manager.RenderCallsAllowed())
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
		if (m_textureId == 0)
			return;
		ITexture* const pTexture = RenderBridge::GetTexture(m_textureId);
		if (pTexture == nullptr)
			return;
		for (SBinding& b : m_bindings)
		{
			if (b.applied || b.removeRequested)
				continue;
			ITexture* pPrevious = nullptr;
			if (RenderBridge::SetMaterialTexture(b.pMaterial, b.subMaterial, b.textureSlot, pTexture, &pPrevious))
			{
				b.pPrevious = pPrevious;
				b.applied = true;
			}
			else
			{
				LogWarning("player %u: cannot bind <%s> to material <%s> sub %d slot %d", m_id, m_textureName.c_str(), b.pMaterial->GetName(), b.subMaterial, b.textureSlot);
			}
		}
	}

	void CVideoPlayerImpl::RemoveAppliedBindings()
	{
		ITexture* const pTexture = m_textureId != 0 ? RenderBridge::GetTexture(m_textureId) : nullptr;
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
		ITexture* const pTexture = m_textureId != 0 ? RenderBridge::GetTexture(m_textureId) : nullptr;
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
	void CVideoPlayerImpl::Tick()
	{
		if (m_destroyed)
			return;
		if (m_releasePending && HasRenderResources())
			ReleaseTexture(false);

		if (m_phase == EPhase::Loading)
		{
			const cev::LoadState ls = m_core.GetLoadState();
			if (ls == cev::LoadState::Ready)
				OnLoaded();
			else if (ls == cev::LoadState::Failed)
				OnLoadFailed();
		}
		if (m_phase != EPhase::Ready)
			return;

		if (m_info.hasVideo)
		{
			if (m_textureId == 0)
				EnsureTexture(); // retry (renderer / level loading state changed)
			else if (!m_bindings.empty())
			{
				ProcessBindingRemovals();
				ApplyBindings();
			}
		}

		const bool fresh = m_core.Update();

		const uint32_t loops = m_core.GetLoopCount();
		while (m_seenLoops < loops)
		{
			++m_seenLoops;
			Emit(EEvent::Looped);
		}
		const cev::PlayerState state = m_core.GetState();
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

		if ((fresh || m_needUpload) && m_textureId != 0)
			UploadFrame();
	}

	// ------------------------------------------------------------------------------------------------ level loading
	void CVideoPlayerImpl::ReapplyLevelLoadPause()
	{
		if (m_manager.IsLevelLoading() && m_phase == EPhase::Ready && m_core.GetState() == cev::PlayerState::Playing)
		{
			m_core.Pause();
			m_pausedByLevelLoad = true;
		}
	}

	void CVideoPlayerImpl::OnLevelLoadBegin()
	{
		if (m_phase == EPhase::Ready && m_core.GetState() == cev::PlayerState::Playing)
		{
			m_core.Pause();
			m_pausedByLevelLoad = true;
		}
	}

	void CVideoPlayerImpl::OnLevelLoadEnd()
	{
		if (m_pausedByLevelLoad)
		{
			m_pausedByLevelLoad = false;
			if (m_phase == EPhase::Ready && m_core.GetState() == cev::PlayerState::Paused)
				m_core.Resume(); // continues from the paused position: the clock is re-synchronised
		}
	}

	// ------------------------------------------------------------------------------------------------ teardown
	void CVideoPlayerImpl::PrepareDestroy()
	{
		m_destroyed = true;
		m_listeners.clear();
		m_eventCount = 0;
		m_playPending = false;
		if (m_fullscreen)
		{
			m_fullscreen = false;
			m_manager.SetFullscreen(this, false);
		}
		StopVoice();
		m_core.Unload();
		m_phase = EPhase::None;
	}

	void CVideoPlayerImpl::ListState(char* buffer, size_t size) const
	{
		std::snprintf(buffer, size, "player %u  %s  t=%.2f/%.2f  %dx%d%s  uploads=%llu  tex=%s(%d)%s  <%s>",
			m_id, StateName(GetState()), GetTime(), GetDuration(), GetWidth(), GetHeight(), HasAlpha() ? " alpha" : "",
			static_cast<unsigned long long>(m_uploads), m_textureName.c_str(), m_textureId, m_fullscreen ? "  fullscreen" : "", m_path.c_str());
	}
}
