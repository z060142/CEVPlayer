#include "StdAfx.h"
#include "Editor/PreviewSession.h"

#include <chrono>
#include <filesystem>

#include <CrySystem/IConsole.h>

#include "Asset/VideoAssetMeta.h"
#include "Asset/VideoDescriptorIO.h"
#include "Editor/PreviewMedia.h"

namespace CEVEditor
{
	namespace
	{
		constexpr uint32_t kPreviewDecodeWorkers = 2;
		// The external audio file may take this long to load after the video is ready (then the video starts without it).
		constexpr double kExternalAudioTimeoutSeconds = 5.0;

		double SteadySeconds()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		ICVar* FindCVar(const char* name)
		{
			return gEnv != nullptr && gEnv->pConsole != nullptr ? gEnv->pConsole->GetCVar(name) : nullptr;
		}

		const char* CVarString(const char* name)
		{
			ICVar* const pVar = FindCVar(name);
			const char* const value = pVar != nullptr ? pVar->GetString() : nullptr;
			return value != nullptr ? value : "";
		}

		const cev::MediaInfo g_noInfo;
	}

	std::atomic<int> CPreviewSession::s_livePools{0};

	void RemoveStalePreviewFiles()
	{
		std::error_code ec;
		const std::filesystem::path dir = std::filesystem::u8path(AssetsAbsolute(".cevpreview"));
		if (std::filesystem::is_directory(dir, ec))
			std::filesystem::remove_all(dir, ec);
	}

	CPreviewSession::CPreviewSession() = default;

	CPreviewSession::~CPreviewSession()
	{
		Release();
	}

	void CPreviewSession::Release()
	{
		m_output.Stop(); // first: the audio device must not read from cores that go away
		m_audioCore.reset();
		m_core.reset();     // joins with its worker task
		m_audioClock.reset();
		if (m_pool)
		{
			m_pool.reset(); // last reference: the worker threads are joined here
			--s_livePools;
		}
		m_phase = EPhase::None;
		m_hasAudio = false;
		m_suspended = false;
		m_resumeAfterSuspend = false;
		m_playPending = false;
		m_seekPending = false;
		m_readyEvent = false;
		m_errorText.clear();
		m_notes.clear();
		m_externalPath.clear();
		m_audioError.clear();
		m_audioWaitSince = -1.0;
		m_seenLoops = 0;
	}

	bool CPreviewSession::Load(const SPreviewRequest& request, std::string& error)
	{
		Release();
		m_volume = request.volume;

		// ---- what to play (the runtime module's decisions, CEVCore/DescriptorPlan.h)
		SDescriptor editorDesc;
		if (!LoadDescriptor(AssetsAbsolute(request.descriptorRel), editorDesc, error))
			return false;
		const cev::VideoDescriptor desc = ToCoreDescriptor(editorDesc, request.descriptorRel);

		ICVar* const pVariantVar = FindCVar("cev_variant"); // registered by the runtime plug-in; without it the main video
		const int forced = pVariantVar != nullptr ? pVariantVar->GetIVal() : -1;
		const cev::DescriptorVariantChoice variant = cev::PickDescriptorVariant(desc, forced, false, 0, &PreviewFileExists, nullptr);

		m_trackNames.clear();
		for (const cev::VideoDescriptor::SAudioTrack& track : desc.audioTracks)
			m_trackNames.push_back(track.name);
		m_followGameLanguage = desc.followGameLanguage;
		m_audioTrack.clear();
		std::string videoPath = variant.videoPath;
		double avOffsetSeconds = cev::DescriptorAvOffsetSeconds(desc);
		cev::DescriptorAudioPlan audio;
		std::string notes;
		if (request.audio == EPreviewAudio::EmbeddedTrack)
		{
			// the audio track inside the video: no track choice, no external file (the main video, as the earlier
			// temporary descriptor did)
			videoPath = request.videoRel;
			avOffsetSeconds = request.avOffsetMs / 1000.0;
		}
		else
		{
			// the chosen track, else default / first; the game language is not followed in the preview (A1)
			cev::ResolveDescriptorAudio(desc, request.audioTrack.c_str(), nullptr, 0, &PreviewFileExists, nullptr, audio);
			m_audioTrack = audio.track;
			if (variant.variant > 0)
				notes += ", variant " + std::to_string(variant.variant) + " (" + std::to_string(variant.height) + "p)";
			if (variant.forcedOutOfRange || !variant.missingFile.empty())
				notes += ", requested variant not available: main video";
			if (!audio.track.empty())
				notes += ", audio track " + audio.track;
			if (audio.unknownRequest)
				notes += ", audio track " + request.audioTrack + " not listed";
			if (desc.followGameLanguage)
				notes += ", follows the game language in the game (not in the preview)";
			if (!audio.externalPath.empty())
				notes += ", audio file " + audio.externalPath;
			if (!audio.missingExternal.empty())
				notes += ", audio file " + audio.missingExternal + " not found: embedded track";
			if (audio.cue)
				notes += ", preview plays the embedded audio; the cue route is not auditioned here";
		}
		if (!PreviewFileExists(videoPath.c_str(), nullptr))
		{
			error = "video file " + videoPath + " not found";
			return false;
		}

		// ---- cores and clock (CVideoPlayerImpl::BuildCores without the cue route and the engine cvars)
		const SPreviewAudioFormat device = ProbePreviewAudioDevice();
		m_deviceRate = device.sampleRate;
		m_audioError = device.error;
		const bool useClock = m_deviceRate != 0;
		m_externalPath = (useClock && !audio.cue) ? audio.externalPath : std::string();
		const bool external = !m_externalPath.empty();
		if (!useClock)
			notes += ", " + (m_audioError.empty() ? std::string("no audio device") : m_audioError) + ": playing silently";

		m_pool = std::make_shared<cev::DecodePool>(kPreviewDecodeWorkers); // own threads: gone when the session is
		if (m_pool->WorkerCount() == 0)
		{
			m_pool.reset();
			error = "no decode thread could be started";
			return false;
		}
		++s_livePools;
		if (useClock)
			m_audioClock = std::make_unique<cev::AudioClock>(m_deviceRate);

		cev::PlayerConfig cfg;
		cfg.outputRgba = true;
		cfg.pool = m_pool;
		cfg.avOffsetSeconds = avOffsetSeconds;
		cfg.externalClock = m_audioClock.get();
		if (useClock)
		{
			cfg.decodeAudio = !external; // the external file replaces the embedded track
			if (cfg.decodeAudio)
				cfg.outputSampleRate = m_deviceRate;
		}
		else
		{
			// decode just enough to know that the media has audio (status line), nothing is played
			cfg.decodeAudio = true;
			cfg.audioBufferMs = 50;
		}
		m_core = std::make_unique<cev::PlayerCore>(cfg);
		if (external)
		{
			cev::PlayerConfig acfg;
			acfg.outputRgba = false;
			acfg.decodeAudio = true;
			acfg.decodeVideo = false;
			acfg.pool = m_pool;
			acfg.externalClock = m_audioClock.get();
			acfg.outputSampleRate = m_deviceRate;
			m_audioCore = std::make_unique<cev::PlayerCore>(acfg);
		}

		const cev::Status status = m_core->Load(std::make_unique<CPreviewPakSource>(videoPath));
		if (cev::Failed(status))
		{
			error = std::string("cannot start loading ") + videoPath + ": " + cev::StatusToString(status);
			Release();
			return false;
		}
		if (m_audioCore && cev::Failed(m_audioCore->Load(std::make_unique<CPreviewPakSource>(m_externalPath))))
		{
			notes += ", audio file could not be opened: no sound";
			m_audioCore.reset();
		}
		m_notes = notes;
		m_phase = EPhase::Loading;
		CryLogAlways("[CEVEditor] preview loads <%s>%s%s%s (audio device %s)", videoPath.c_str(), external ? " + audio <" : "", external ? m_externalPath.c_str() : "",
			external ? ">" : "", useClock ? std::to_string(m_deviceRate).c_str() : m_audioError.c_str());
		return true;
	}

	void CPreviewSession::Fail(const std::string& message)
	{
		m_output.Stop();
		m_audioCore.reset();
		if (m_core)
			m_core->Unload();
		m_phase = EPhase::Failed;
		m_playPending = false;
		m_seekPending = false;
		m_errorText = message;
	}

	void CPreviewSession::StartAudio()
	{
		if (!m_audioClock || !m_hasAudio || m_output.IsRunning())
			return;
		cev::PlayerCore* const pSource = m_audioCore ? m_audioCore.get() : m_core.get();
		std::string error;
		if (!m_output.Start(m_deviceRate, pSource, m_audioClock.get(), m_volume, error))
		{
			m_audioError = error;
			m_notes += ", " + error + ": no sound";
			CryLogAlways("[CEVEditor] preview: %s, playing without sound", error.c_str());
		}
	}

	void CPreviewSession::OnLoaded()
	{
		m_phase = EPhase::Ready;
		if (m_audioCore && m_audioCore->GetLoadState() != cev::LoadState::Ready)
			m_audioCore.reset(); // failed: the embedded track is not decoded either (as in the runtime module), no sound
		m_hasAudio = m_audioCore ? m_audioCore->GetInfo().hasAudio : m_core->GetInfo().hasAudio;
		m_core->SetLoop(m_loop);
		StartAudio();
		m_readyEvent = true;
		if (m_seekPending)
		{
			m_seekPending = false;
			Seek(m_seekTime);
		}
		if (m_playPending)
		{
			m_playPending = false;
			Play();
		}
	}

	bool CPreviewSession::Tick()
	{
		if (m_phase == EPhase::Loading)
		{
			const cev::LoadState ls = m_core->GetLoadState();
			if (ls == cev::LoadState::Ready)
			{
				bool audioPending = m_audioCore && m_audioCore->GetLoadState() == cev::LoadState::Loading;
				if (audioPending)
				{
					const double now = SteadySeconds();
					if (m_audioWaitSince < 0.0)
						m_audioWaitSince = now;
					if (now - m_audioWaitSince >= kExternalAudioTimeoutSeconds)
					{
						m_audioCore.reset();
						m_notes += ", audio file still loading after 5 s: no sound";
						audioPending = false;
					}
				}
				if (!audioPending)
					OnLoaded();
			}
			else if (ls == cev::LoadState::Failed)
			{
				const cev::LoadError err = m_core->GetLoadError();
				Fail(std::string(cev::StatusToString(err.status)) + " (" + cev::StatusDetailToString(err.detail) + ")");
				return true;
			}
		}
		if (m_phase != EPhase::Ready)
			return false;
		if (m_core->GetLoadState() == cev::LoadState::Failed)
		{
			const cev::LoadError err = m_core->GetLoadError();
			Fail(std::string(cev::StatusToString(err.status)) + " (" + cev::StatusDetailToString(err.detail) + ")");
			return true;
		}

		const bool fresh = m_core->Update();
		const uint32_t loops = m_core->GetLoopCount();
		const bool looped = m_seenLoops < loops;
		m_seenLoops = loops;
		const cev::PlayerState state = m_core->GetState();
		if (m_audioCore)
		{
			// the audio file follows the loop and is never Update()d to its own end (CVideoPlayerImpl::TickBody)
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
		return fresh;
	}

	void CPreviewSession::Play()
	{
		if (m_phase == EPhase::Loading)
		{
			m_playPending = true;
			return;
		}
		if (m_phase != EPhase::Ready)
			return;
		if (m_suspended)
		{
			m_resumeAfterSuspend = true;
			return;
		}
		if (m_audioCore && m_core->GetState() == cev::PlayerState::Ended)
			m_audioCore->Seek(0.0);
		const cev::Status st = m_core->Play();
		if (m_audioCore && cev::Succeeded(st))
			m_audioCore->Play();
	}

	void CPreviewSession::Pause()
	{
		m_playPending = false;
		m_resumeAfterSuspend = false;
		if (m_phase != EPhase::Ready)
			return;
		m_core->Pause();
		if (m_audioCore)
			m_audioCore->Pause();
	}

	void CPreviewSession::Resume()
	{
		if (m_phase != EPhase::Ready)
			return;
		if (m_suspended)
		{
			m_resumeAfterSuspend = true;
			return;
		}
		m_core->Resume();
		if (m_audioCore)
			m_audioCore->Resume();
	}

	void CPreviewSession::Stop()
	{
		m_playPending = false;
		m_resumeAfterSuspend = false;
		if (m_phase != EPhase::Ready)
			return;
		if (m_audioCore)
			m_audioCore->Stop();
		m_core->Stop(); // last: it leaves the shared clock at 0
	}

	void CPreviewSession::Seek(double seconds)
	{
		if (m_phase == EPhase::Loading)
		{
			m_seekPending = true;
			m_seekTime = seconds;
			return;
		}
		if (m_phase != EPhase::Ready)
			return;
		if (m_audioCore)
			m_audioCore->Seek(seconds); // clamps to its own duration and moves the clock: the video seek goes last
		m_core->Seek(seconds);
	}

	void CPreviewSession::SetLoop(bool loop)
	{
		m_loop = loop;
		if (m_core)
			m_core->SetLoop(loop);
	}

	void CPreviewSession::SetVolume(float volume)
	{
		m_volume = volume;
		m_output.SetVolume(volume);
	}

	void CPreviewSession::Suspend(bool suspend)
	{
		if (suspend == m_suspended)
			return;
		if (suspend)
		{
			m_resumeAfterSuspend = m_phase == EPhase::Ready && m_core->GetState() == cev::PlayerState::Playing;
			if (m_resumeAfterSuspend)
			{
				m_core->Pause();
				if (m_audioCore)
					m_audioCore->Pause();
			}
			m_output.Suspend(true);
			m_suspended = true;
			return;
		}
		m_suspended = false;
		m_output.Suspend(false);
		if (m_resumeAfterSuspend && m_phase == EPhase::Ready)
		{
			m_core->Resume();
			if (m_audioCore)
				m_audioCore->Resume();
		}
		m_resumeAfterSuspend = false;
	}

	cev::PlayerState CPreviewSession::State() const
	{
		return m_phase == EPhase::Ready ? m_core->GetState() : cev::PlayerState::Stopped;
	}

	double CPreviewSession::Time() const
	{
		return m_phase == EPhase::Ready ? m_core->GetTime() : 0.0;
	}

	double CPreviewSession::Duration() const
	{
		return m_phase == EPhase::Ready ? m_core->GetInfo().durationSeconds : 0.0;
	}

	const cev::MediaInfo& CPreviewSession::Info() const
	{
		return m_phase == EPhase::Ready ? m_core->GetInfo() : g_noInfo;
	}

	bool CPreviewSession::GetFrame(cev::FrameView& out) const
	{
		return m_phase == EPhase::Ready && m_core->GetFrame(out) && out.rgba != nullptr;
	}

	uint64_t CPreviewSession::FrameSerial() const
	{
		cev::FrameView fv;
		return GetFrame(fv) ? fv.serial : 0;
	}
}
