// The player of a video preview: CEVPlayerCore decodes, Qt Multimedia plays the sound (PreviewAudio.h), the panel
// paints the frames (VideoPreviewWidget.h). Nothing goes through the engine renderer, the engine audio system or the
// runtime plug-in, which does not even have to be loaded.
//
// What it plays is decided like the runtime module decides it (CEVCore/DescriptorPlan.h, shared code): variant (the
// cvar cev_variant when the runtime plug-in registered it; the preview is never fullscreen, so no automatic choice),
// language chain g_languageAudio, g_language, default language of the descriptor, external language file of the mixed
// route, A/V offset. Limitation: a cue route (audio from an audio middleware trigger) is not auditioned; the preview
// plays the embedded track instead and says so.
//
// Main thread only (the audio device thread only reaches the Core's audio path). Like the runtime module the video
// core and the optional external audio core share one cev::AudioClock and move together; without an audio device the
// cores run on their internal (wall) clock.
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "CEVCore/AudioClock.h"
#include "CEVCore/DecodePool.h"
#include "CEVCore/DescriptorPlan.h"
#include "CEVCore/PlayerCore.h"
#include "Editor/PreviewAudio.h"

namespace CEVEditor
{
	enum class EPreviewAudio
	{
		AsAuthored,    //!< route and language of the descriptor (cue route: the embedded track, see above)
		EmbeddedTrack, //!< the audio track inside the video, whatever the descriptor says
		Muted,         //!< as authored, volume 0
	};

	//! Deletes the leftovers of an earlier version in Assets/.cevpreview (temporary descriptors it wrote).
	void RemoveStalePreviewFiles();

	struct SPreviewRequest
	{
		std::string descriptorRel;   //!< game relative path of the .cevvideo
		std::string videoRel;        //!< main video, game relative (EmbeddedTrack)
		double avOffsetMs = 0.0;     //!< EmbeddedTrack: the offset of the edited descriptor
		EPreviewAudio audio = EPreviewAudio::AsAuthored;
		float volume = 1.f;
	};

	class CPreviewSession final
	{
	public:
		CPreviewSession();
		~CPreviewSession();
		CPreviewSession(const CPreviewSession&) = delete;
		CPreviewSession& operator=(const CPreviewSession&) = delete;

		//! Reads the descriptor, decides files and audio source and starts loading (asynchronous). False and `error`
		//! when the descriptor cannot be read or the video does not exist.
		bool Load(const SPreviewRequest& request, std::string& error);
		//! Stops the sound, joins the decode threads, frees everything. Safe to call repeatedly.
		void Release();

		bool HasMedia() const { return m_phase != EPhase::None; }
		bool IsLoading() const { return m_phase == EPhase::Loading; }
		bool IsReady() const { return m_phase == EPhase::Ready; }
		//! One step (from the panel's timer): load completion, Core Update of both cores. True when a new frame became
		//! current or the state changed in a way the picture shows (ready, failed).
		bool Tick();
		//! Cleared by the reader.
		bool TakeReady() { const bool v = m_readyEvent; m_readyEvent = false; return v; }
		bool TakeError(std::string& message) { if (m_errorText.empty()) return false; message.swap(m_errorText); m_errorText.clear(); return true; }

		// ---- transport (both cores, like CVideoPlayerImpl::Cores*) ----
		void Play();
		void Pause();
		void Resume();
		void Stop();
		void Seek(double seconds);
		void SetLoop(bool loop);
		void SetVolume(float volume);
		//! Window hidden / game mode: pauses playback and the audio device; Suspend(false) continues where it was.
		void Suspend(bool suspend);
		bool IsSuspended() const { return m_suspended; }

		cev::PlayerState State() const;
		double Time() const;
		double Duration() const;
		const cev::MediaInfo& Info() const;
		bool HasAudio() const { return m_hasAudio; }
		bool GetFrame(cev::FrameView& out) const;
		uint64_t FrameSerial() const;
		//! Short notes for the status line (variant, language, external file, cue limitation, no audio device).
		const std::string& Notes() const { return m_notes; }
		bool AudioDeviceRunning() const { return m_output.IsRunning(); }
		const std::string& AudioDeviceError() const { return m_audioError; }

		//! Preview sessions with a decode pool right now (self test: 0 after the window closed).
		static int LivePools() { return s_livePools.load(); }

	private:
		enum class EPhase { None, Loading, Ready, Failed };
		void OnLoaded();
		void Fail(const std::string& message);
		void StartAudio();

		EPhase m_phase = EPhase::None;
		// Order: the clock is referenced by the cores and the audio device, the pool by the cores; the audio output is
		// stopped before anything else is reset (Release()).
		std::shared_ptr<cev::DecodePool> m_pool;
		std::unique_ptr<cev::AudioClock> m_audioClock;
		std::unique_ptr<cev::PlayerCore> m_core;       // video (and the embedded audio track)
		std::unique_ptr<cev::PlayerCore> m_audioCore;  // external language file, audio only
		CPreviewAudioOutput m_output;
		uint32_t m_deviceRate = 0;
		std::string m_audioError;
		bool m_hasAudio = false;
		bool m_loop = false;
		float m_volume = 1.f;
		bool m_suspended = false;
		bool m_resumeAfterSuspend = false;
		bool m_playPending = false;
		bool m_seekPending = false;
		double m_seekTime = 0.0;
		bool m_readyEvent = false;
		std::string m_errorText;
		std::string m_notes;
		std::string m_externalPath;
		double m_audioWaitSince = -1.0;
		uint32_t m_seenLoops = 0;
		static std::atomic<int> s_livePools;
	};
}
