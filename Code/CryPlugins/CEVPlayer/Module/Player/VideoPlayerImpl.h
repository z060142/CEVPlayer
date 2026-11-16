// The bridge player: one cev::PlayerCore + its RGBA texture + its outputs. Main thread only (the decode
// workers and the audio thread only touch the PlayerCore, never this class).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <CEVPlayer/IVideoPlayer.h>

#include "CEVCore/PlayerCore.h"

namespace CEVPlayer
{
	class CPlayerManager;

	class CVideoPlayerImpl final : public IVideoPlayer
	{
	public:
		CVideoPlayerImpl(CPlayerManager& manager, uint32_t id, const char* textureName);
		~CVideoPlayerImpl() override;
		CVideoPlayerImpl(const CVideoPlayerImpl&) = delete;
		CVideoPlayerImpl& operator=(const CVideoPlayerImpl&) = delete;

		// ---- IVideoPlayer ----
		bool Load(const char* path) override;
		void Unload() override;
		const char* GetPath() const override { return m_path.c_str(); }
		void Play() override;
		void Pause() override;
		void Resume() override;
		void Stop() override;
		void Seek(double seconds) override;
		void SetLoop(bool loop) override;
		void SetRate(float rate) override;
		void SetVolume(float volume) override;
		EVideoPlayerState GetState() const override;
		bool IsLoaded() const override { return m_phase == EPhase::Ready; }
		bool IsLooping() const override { return m_loop; }
		double GetTime() const override;
		double GetDuration() const override { return m_phase == EPhase::Ready ? m_info.durationSeconds : 0.0; }
		int GetWidth() const override { return m_phase == EPhase::Ready ? m_info.width : 0; }
		int GetHeight() const override { return m_phase == EPhase::Ready ? m_info.height : 0; }
		bool HasAlpha() const override { return m_phase == EPhase::Ready && m_info.hasAlpha; }
		bool IsPremultiplied() const override { return true; }
		bool HasAudio() const override { return m_phase == EPhase::Ready && m_info.hasAudio; }
		uint64_t GetFrameSerial() const override { return m_frameSerial; }
		const char* GetTextureName() const override { return m_textureName.c_str(); }
		int GetTextureId() const override { return m_textureId; }
		ITexture* GetTexture() const override { return m_pTexture; }
		void SetFullscreen(bool enable, EVideoFit fit) override;
		bool IsFullscreen() const override { return m_fullscreen; }
		bool BindMaterial(IMaterial* pMaterial, int subMaterial, int textureSlot) override;
		void UnbindMaterial(IMaterial* pMaterial) override;
		void AddListener(IVideoPlayerListener* pListener) override;
		void RemoveListener(IVideoPlayerListener* pListener) override;

		// ---- manager side (main thread) ----
		uint32_t Id() const { return m_id; }
		//! One frame step: load completion, core Update, texture upload, event generation. Only called while
		//! renderer calls are allowed.
		void Tick();
		//! Sends the queued events to the listeners (end of the frame update).
		void DispatchEvents();
		//! Level loading started / ended: pauses / resumes the clock without any renderer call.
		void OnLevelLoadBegin();
		void OnLevelLoadEnd();
		//! Stops decoding / audio and drops listeners and outputs that need no renderer. After this only
		//! ReleaseRenderResources() and the destructor are valid.
		void PrepareDestroy();
		//! True while a texture or material binding still has to be released (needs renderer calls).
		bool HasRenderResources() const { return m_pTexture != nullptr || !m_bindings.empty(); }
		//! Unbinds materials and removes the texture. Only called while renderer calls are allowed (or at shutdown).
		void ReleaseRenderResources();
		void ListState(char* buffer, size_t size) const;
		uint64_t& StatsMark() { return m_statsMark; }
		uint64_t UploadCount() const { return m_uploads; }
		EVideoFit Fit() const { return m_fit; }
		cev::PlayerCore& Core() { return m_core; }
		bool IsDestroyed() const { return m_destroyed; }
		void MarkDestroyed() { m_destroyed = true; }
		void SetStatsEnabled(bool enabled) { m_core.SetStatsEnabled(enabled); }
		const std::string& Name() const { return m_textureName; }
		//! The texture this player owns (valid for name clash checks of the manager).
		bool HasTexture() const { return m_pTexture != nullptr; }

	private:
		enum class EPhase : uint8_t { None, Loading, Ready, Failed };
		enum class EEvent : uint8_t { Ready, Started, NewFrame, Looped, Ended, Error, Paused, Resumed, Stopped };
		struct SEvent
		{
			EEvent type = EEvent::Ready;
			int code = 0;
			uint64_t value = 0;
		};
		struct SBinding
		{
			IMaterial* pMaterial = nullptr; // reference held while bound
			int subMaterial = -1;
			int textureSlot = 0;
			ITexture* pPrevious = nullptr;  // texture the slot had before (reference owned by us while applied)
			bool applied = false;
			bool removeRequested = false;   // UnbindMaterial() while renderer calls were not allowed
			bool failed = false;            // the slot could not be bound: reported once, not retried until the texture changes
		};
		static constexpr uint32_t kMaxEvents = 32;

		void Emit(EEvent type, int code = 0, uint64_t value = 0);
		void OnLoaded();
		void OnLoadFailed();
		bool EnsureTexture();
		//! Ready -> Failed (texture creation refused, or the core reported a mid-stream failure): stops audio and
		//! presentation, queues OnError.
		void FailPlayer(cev::Status status, const char* reason);
		void ApplyBindings();
		void RemoveAppliedBindings();
		void ProcessBindingRemovals();
		void ReleaseTexture(bool keepBindings);
		void UploadFrame();
		void StartVoice();
		void StopVoice();
		void ReapplyLevelLoadPause();

		CPlayerManager& m_manager;
		cev::PlayerCore m_core;
		uint32_t m_id;
		std::string m_textureName;
		bool m_autoName = true;  // generated "$cev_video_<w>x<h>_<k>" (reusable, see CPlayerManager::MakeAutoTextureName)
		std::string m_path;

		EPhase m_phase = EPhase::None;
		cev::MediaInfo m_info;
		cev::Status m_loadStatus = cev::Status::Ok;

		// requests made before the media is ready
		bool m_playPending = false;
		bool m_seekPending = false;
		double m_seekTime = 0.0;
		bool m_loop = false;
		float m_rate = 1.f;
		float m_volume = 1.f;

		// presentation
		ITexture* m_pTexture = nullptr; // one reference owned by this player (RenderBridge::ReleaseTexture)
		int m_textureId = 0;
		int m_textureWidth = 0;
		int m_textureHeight = 0;
		bool m_needUpload = false;
		uint64_t m_frameSerial = 0;
		uint64_t m_uploads = 0;
		bool m_strideWarned = false;
		uint32_t m_seenLoops = 0;
		bool m_endedSent = false;
		bool m_pausedByLevelLoad = false;
		bool m_destroyed = false;
		bool m_releasePending = false; // Unload() while renderer calls were not allowed
		uint64_t m_statsMark = 0;

		bool m_fullscreen = false;
		EVideoFit m_fit = EVideoFit::Letterbox;
		std::vector<SBinding> m_bindings;
		int m_voice = -1;

		std::vector<IVideoPlayerListener*> m_listeners;
		SEvent m_events[kMaxEvents];
		uint32_t m_eventCount = 0;
		int m_dispatchDepth = 0;
	};
}
