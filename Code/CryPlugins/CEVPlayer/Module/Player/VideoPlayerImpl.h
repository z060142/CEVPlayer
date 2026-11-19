// The bridge player: one cev::PlayerCore + its RGBA texture + its outputs. The IVideoPlayer methods are main thread
// only (the decode workers and the audio thread only touch the PlayerCore, never this class). Tick() does not assume
// the main thread (docs/design/M6-ui-source.md section 9): its time comes from the caller.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <CEVPlayer/IVideoPlayer.h>

#include "Audio/CueRoute.h"
#include "CEVCore/AudioClock.h"
#include "CEVCore/PlayerCore.h"
#include "Player/VideoDescriptor.h"

namespace CEVPlayer
{
	class CPlayerManager;

	class CVideoPlayerImpl final : public IVideoPlayer
	{
	public:
		//! `surfaceId` non-empty: a surface player (texture "$cev:<id>", snapshot for the texture source contract).
		CVideoPlayerImpl(CPlayerManager& manager, uint32_t id, const char* textureName, const char* surfaceId = nullptr);
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
		bool IsPremultiplied() const override { return m_layout == EVideoPixelLayout::RGBA8; }
		bool HasAudio() const override { return m_phase == EPhase::Ready && m_hasAudio; }
		uint64_t GetFrameSerial() const override { return m_frameSerial; }
		const char* GetTextureName() const override { return m_textureName.c_str(); }
		int GetTextureId() const override { return m_textureId; }
		ITexture* GetTexture() const override { return m_pTexture; }
		void SetFullscreen(bool enable, EVideoFit fit) override;
		bool IsFullscreen() const override { return m_fullscreen; }
		//! A fullscreen player that was Stop()ped draws nothing until the next Play() (texture / material outputs keep
		//! showing the first frame).
		bool FullscreenHidden() const { return m_fullscreenHidden; }
		bool BindMaterial(IMaterial* pMaterial, int subMaterial, int textureSlot) override;
		void UnbindMaterial(IMaterial* pMaterial) override;
		void AddListener(IVideoPlayerListener* pListener) override;
		void RemoveListener(IVideoPlayerListener* pListener) override;
		void SetAudioTrack(const char* name) override;
		const char* GetAudioTrack() const override { return m_audioTrack.c_str(); }
		//! Audio tracks of the loaded descriptor (A1; 0 without a descriptor). Out of range: "".
		int GetAudioTrackCount() const;
		const char* GetAudioTrackName(int index) const;
		const char* GetAudioTrackLanguage(int index) const;
		void SetVariant(int variant) override;
		int GetVariant() const override { return m_variantIndex < 0 ? 0 : m_variantIndex; }
		const char* GetSurfaceId() const override { return m_surfaceId.c_str(); }
		uint32_t GetGeneration() const override { return m_generation; }
		EVideoPixelLayout GetPixelLayout() const override { return m_layout; }
		EVideoDecodeBackend GetDecodeBackend() const override;

		// ---- manager side (main thread) ----
		uint32_t Id() const { return m_id; }
		//! One frame step: load completion, core Update, texture upload, event generation, and at its end the
		//! contract snapshot of a surface player. Only called while renderer calls are allowed. `dt` = seconds since
		//! the previous step, given by the caller (the clocks of the cores are their own; dt is not used yet).
		void Tick(float dt);
		//! Sends the queued events to the listeners (end of the frame update).
		void DispatchEvents();
		//! Level loading started / ended: pauses / resumes the clock without any renderer call.
		void OnLevelLoadBegin();
		void OnLevelLoadEnd();
		//! Stops decoding / audio and drops listeners and outputs that need no renderer. After this only
		//! ReleaseRenderResources() and the destructor are valid.
		void PrepareDestroy();
		// ---- M8 diagnostics (contract selftest) ----
		uint64_t LoadCount() const { return m_loads; }          //!< LoadInternal calls (a layout switch never reloads)
		uint64_t TickCount() const { return m_tick; }           //!< Tick() calls
		uint64_t TextureTick() const { return m_textureTick; }  //!< tick in which the current texture was made
		uint64_t FirstUploadTick() const { return m_firstUploadTick; } //!< tick of the first upload into it
		//! Hash (FNV-1a 64) of every uploaded buffer while enabled (costs a pass over the bytes: tests only).
		void SetUploadChecksum(bool enable) { m_checksumUploads = enable; }
		uint64_t LastUploadChecksum() const { return m_lastUploadChecksum; }
		uint64_t LastUploadSerial() const { return m_frameSerial; }
		//! "rgba8", "yuv420" or "yuva420".
		const char* LayoutName() const;
		//! True while a texture or material binding still has to be released (needs renderer calls).
		bool HasRenderResources() const { return m_pTexture != nullptr || !m_bindings.empty(); }
		//! Unbinds materials and removes the texture. Only called while renderer calls are allowed (or at shutdown).
		void ReleaseRenderResources();
		void ListState(char* buffer, size_t size) const;
		uint64_t& StatsMark() { return m_statsMark; }
		uint64_t UploadCount() const { return m_uploads; }
		//! Measurement (S0): main thread time of the texture update call over the last uploads (avg / p99, ms) and
		//! the bytes of the last upload. Clears the window.
		void TakeUploadTiming(float& avgMs, float& p99Ms, uint32_t& samples);
		uint64_t LastUploadBytes() const { return m_lastUploadBytes; }
		EVideoFit Fit() const { return m_fit; }
		//! Values last given to SetRate / SetVolume (API table queries Rate / Volume).
		float GetRate() const { return m_rate; }
		float GetVolume() const { return m_volume; }
		//! M7: pause the video decoding while no consumer of the surface sees the picture (default on). Players without
		//! a surface id, fullscreen and material outputs always count as visible.
		void SetPauseDecodeWhenHidden(bool enable) { m_pauseDecodeWhenHidden = enable; }
		//! M9: SetDecode (cev::DecodeMode value: 0 Auto, 1 GPU, 2 CPU; -1 = follow cev_decodeMode). Takes effect at the
		//! next Load / Play from Stopped or Ended (a playing video is never reloaded for it).
		void SetDecodeRequest(int mode) { m_decodeRequest = mode; }
		int EffectiveDecodeMode() const;
		//! Backend in use with the fallback reason (default values before Ready).
		cev::DecodeBackendInfo DecodeInfo() const;
		//! "gpu", "gpu+cpu-alpha", "cpu" or "cpu (reason)" for logs and cev_surfaces.
		void DecodeText(char* buffer, size_t size, bool withReason) const;
		bool GetPauseDecodeWhenHidden() const { return m_pauseDecodeWhenHidden; }
		//! Contract control MUTE: silences the voice without touching the volume value.
		void SetMuted(bool muted);
		bool IsMuted() const { return m_muted; }
		//! The player was made by a contract `open` (its `close` destroys it).
		void SetOwnedByOpen(bool owned) { m_ownedByOpen = owned; }
		bool OwnedByOpen() const { return m_ownedByOpen; }
		//! Aggregated hints of the surface as seen at the last tick (0 consumers: nobody hinted).
		int HintPixelHeight() const { return m_hintPixelHeight; }
		uint32_t HintConsumers() const { return m_hintConsumers; }
		//! "visible", "decode-only" or "stopped" (the core's VideoVisibility).
		const char* VisibilityName() const;
		cev::PlayerCore& Core() { return *m_core; }
		//! "audio" while an audio voice drives the clock (AudioClock), "cue" while the playback position of the cue
		//! trigger does, otherwise "internal" (wall clock / video clock).
		const char* ClockSourceName() const { return m_voice >= 0 ? "audio" : (m_cueClock ? m_cueClock->SourceName() : "internal"); }
		//! Measurement (M10a): audio frames the output device consumed through this player's AudioClock (0 without one).
		uint64_t ProbeAudioFramesConsumed() const { return m_audioClock ? m_audioClock->FramesConsumed() : 0; }
		//! Measurement (M10a): of those, the frames that counted as media time (decoded audio, not silence).
		uint64_t ProbeAudioFramesCounted() const { return m_audioClock ? m_audioClock->FramesCounted() : 0; }
		//! Level unload: a cue trigger is stopped (the engine drops the level's audio data).
		void OnLevelUnload();
		//! Lateness of the picture on screen against the clock (clock time - presented frame timestamp, ms).
		bool GetAvOffsetMs(double& ms) const;
		bool IsDestroyed() const { return m_destroyed; }
		void MarkDestroyed() { m_destroyed = true; }
		void SetStatsEnabled(bool enabled) { m_core->SetStatsEnabled(enabled); }
		const std::string& Name() const { return m_textureName; }
		//! The texture this player owns (valid for name clash checks of the manager).
		bool HasTexture() const { return m_pTexture != nullptr; }

	private:
		enum class EPhase : uint8_t { None, Loading, Ready, Failed };
		enum class EEvent : uint8_t { Ready, Started, NewFrame, Looped, Ended, Error, Paused, Resumed, Stopped, Marker };
		static constexpr size_t kMarkerNameLength = 48;
		struct SEvent
		{
			EEvent type = EEvent::Ready;
			int code = 0;
			uint64_t value = 0;
			double time = 0.0;                 // Marker: media time
			char name[kMarkerNameLength] = {}; // Marker: name (truncated)
		};
		//! What Load() decided for one media: files, audio track and audio route.
		struct SPlan
		{
			std::string videoPath;
			std::string externalPath; // external audio file (mixed route), empty = none
			std::string audioTrack;
			bool cue = false;         // route cue / trigger-only track: the audio is a CryAudio trigger
			std::string trigger;      // cue: start trigger (empty = nothing to play)
			std::string pauseTrigger;
			std::string resumeTrigger;
		};
		enum class ERestore : uint8_t { None, Playing, Paused };
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
		void EmitMarker(const SVideoDescriptor::SMarker& marker);
		bool LoadInternal(const char* path, bool reload);
		bool ChoosePlan(const char* path, SPlan& plan);
		void ResolveAudio(SPlan& plan, bool report);
		std::string PickVariantPath();
		void BuildCores(const SPlan& plan);
		void ReloadInPlace();
		void UpdateMarkers(bool looped);
		void ResyncMarkers();
		// ---- the video core and the optional external audio core always move together
		cev::Status CoresPlay();
		void CoresPause();
		void CoresResume();
		void CoresStop();
		cev::Status CoresSeek(double seconds);
		void CoresSetRate(double rate);
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
		//! M8: the layout the texture should have now (section 3 rule 5, task M8 item 3) and whether only consumer
		//! preferences ask for it (those changes wait for the hold; forced RGBA8 does not).
		EVideoPixelLayout DesiredLayout(bool& consumerDriven) const;
		//! Switches the texture to DesiredLayout() when it differs: new texture + upload of the current frame in this
		//! tick, generation +1; the core's RGBA output follows. Keeps the old texture while no frame data exists.
		void UpdateLayout();
		void StartVoice();
		void StopVoice();
		void ReapplyLevelLoadPause();
		void TickBody(float dt);
		//! Visibility of this tick from the aggregated hints (M7) -> PlayerCore::SetVisible.
		void ApplyVisibility(float dt);
		//! Height the automatic variant choice aims at: the window height (fullscreen) or the aggregated pixel height of
		//! the surface's consumers; 0 = no automatic choice.
		int VariantTargetHeight() const;
		//! Surface players: writes the snapshot that CryTexSource query returns (end of Tick; no engine call).
		void PublishSnapshot();

		CPlayerManager& m_manager;
		// Order matters: the clock is referenced by the cores, so it is declared first (destroyed last); the
		// voice is removed from the mixer before any of them is reset (StopVoice).
		std::unique_ptr<cev::AudioClock> m_audioClock; // null: the cores run on their InternalClock
		std::unique_ptr<CCueClock> m_cueClock;         // cue route: transport of the CryAudio trigger (instead of m_audioClock)
		std::unique_ptr<cev::PlayerCore> m_core;       // video (and the embedded audio track)
		std::unique_ptr<cev::PlayerCore> m_audioCore;  // external audio file of a track, audio only
		uint32_t m_id;
		std::string m_textureName;
		bool m_autoName = true;  // generated "$cev_video_<w>x<h>_<k>" (reusable, see CPlayerManager::MakeAutoTextureName)
		std::string m_path;
		// surface id (M6): "" = not a surface player
		std::string m_surfaceId;
		bool m_surfaceRegistered = false; // registered with the contract table, removed by PrepareDestroy
		uint32_t m_generation = 0;        // +1 per tick in which the texture was created / released / resized
		uint64_t m_contentSerial = 0;     // +1 per upload and per generation change, never decreases
		bool m_textureChanged = false;    // set by EnsureTexture / ReleaseTexture, folded into m_generation by PublishSnapshot
		// M8 layout
		EVideoPixelLayout m_layout = EVideoPixelLayout::RGBA8; // layout of the texture (and of the next one EnsureTexture makes)
		bool m_textureR8 = false;         // the current texture is R8
		uint64_t m_tick = 0;
		uint64_t m_layoutTick = 0;        // tick of the last layout switch (0 = none yet)
		uint64_t m_textureTick = 0;
		uint64_t m_firstUploadTick = 0;
		uint64_t m_loads = 0;
		bool m_checksumUploads = false;
		uint64_t m_lastUploadChecksum = 0;

		EPhase m_phase = EPhase::None;
		cev::MediaInfo m_info;
		cev::Status m_loadStatus = cev::Status::Ok;
		bool m_hasAudio = false;       // audible audio exists (embedded track or external file)

		// descriptor / audio track / variant
		SVideoDescriptor m_desc;
		bool m_descValid = false;
		SPlan m_plan;
		std::string m_requestedTrack;    // SetAudioTrack(), empty = automatic choice
		std::string m_audioTrack;        // audio track in use
		std::string m_unknownTrackWarned; // requested name already warned about as not listed (warn once)
		int m_variantIndex = -1;         // -1 = main video
		int m_variantRequest = kVariantFollowCvar; // SetVariant()
		bool m_reloading = false;        // Load() in progress because of an audio track / variant / decoder change
		double m_reloadTime = 0.0;
		ERestore m_reloadRestore = ERestore::None;
		bool m_silentWarned = false;
		double m_audioWaitSince = -1.0;  // steady seconds: the video core became Ready while the external audio file still loads

		// markers
		size_t m_markerNext = 0;
		bool m_markerResync = true;
		double m_markerPrev = 0.0;

		// requests made before the media is ready
		bool m_playPending = false;
		bool m_seekPending = false;
		double m_seekTime = 0.0;
		bool m_loop = false;
		float m_rate = 1.f;
		float m_volume = 1.f;
		bool m_muted = false;
		bool m_pauseDecodeWhenHidden = true;
		bool m_ownedByOpen = false;
		int m_hintPixelHeight = 0;
		uint32_t m_hintConsumers = 0;
		uint8_t m_lastVisibility = 0;     // cev::VideoVisibility of the previous tick (stats window reset on change)
		bool m_startedAfterReload = false; // Play() reloaded for another variant: OnLoaded sends Started
		// M9 decoder choice
		int m_decodeRequest = -1;          // SetDecode(), -1 = cev_decodeMode
		int m_loadedDecodeMode = 2;        // mode the current core was loaded with
		bool m_backendHardware = false;    // hardware decode at the last check (fallback warning during playback)

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
		// measurement (S0): upload call timing, cleared by TakeUploadTiming
		static constexpr uint32_t kUploadTimingWindow = 512;
		float m_uploadMs[kUploadTimingWindow] = {};
		uint32_t m_uploadMsCount = 0;
		uint64_t m_lastUploadBytes = 0;

		bool m_fullscreen = false;
		bool m_fullscreenHidden = false; // set by Stop(), cleared by Play()
		EVideoFit m_fit = EVideoFit::Letterbox;
		std::vector<SBinding> m_bindings;
		int m_voice = -1;

		std::vector<IVideoPlayerListener*> m_listeners;
		SEvent m_events[kMaxEvents];
		uint32_t m_eventCount = 0;
		int m_dispatchDepth = 0;
	};
}
