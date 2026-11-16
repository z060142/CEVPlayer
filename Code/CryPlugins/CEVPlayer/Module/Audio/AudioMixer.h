// Plug-in wide SDL2 audio output (M2a).
//
// One device for the whole plug-in: opened lazily by the first voice, closed once at plug-in shutdown. SDL is
// initialised with SDL_InitSubSystem(SDL_INIT_AUDIO) and released with SDL_QuitSubSystem; this code never calls
// SDL_Init / SDL_Quit (the engine SDL_mixer implementation owns those). With cev_disableEmbeddedAudio = 1 the SDL
// audio subsystem is never touched (AddVoice refuses before EnsureDevice()).
//
// Format: 48000 Hz float stereo. Every voice is a PlayerCore whose audio is pulled with ReadAudioResampled(), so
// any source rate / channel count plays. The voice's AudioClock is advanced by every callback with the full
// block size (also for silence), which is what makes it the playback clock of its player.
//
// Mixing (audio thread) neither allocates nor logs. Gain = voice volume x cev_volume x vp_audio_volume, muted
// while the main window has no focus and cev_muteOnFocusLoss is set; changes are ramped over one block.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace cev
{
	class PlayerCore;
	class AudioClock;
}

namespace CEVPlayer
{
	class CAudioMixer
	{
	public:
		static constexpr int kMaxVoices = 16;
		static constexpr uint32_t kDeviceRate = 48000;
		static constexpr int kInvalidVoice = -1;

		CAudioMixer();
		~CAudioMixer();
		CAudioMixer(const CAudioMixer&) = delete;
		CAudioMixer& operator=(const CAudioMixer&) = delete;

		//! Registers a stream: `pCore` (loaded, with PlayerConfig::outputSampleRate = kDeviceRate) delivers the
		//! audio, `pClock` is the clock the core runs on (advanced by this voice's callbacks). Both must stay
		//! alive until RemoveVoice() returns. Returns kInvalidVoice (and logs) when the stream cannot be played
		//! (no resampler, no device, audio disabled by cev_disableEmbeddedAudio, too many voices).
		int AddVoice(cev::PlayerCore* pCore, cev::AudioClock* pClock);
		void SetVoiceVolume(int voice, float volume);
		//! Largest absolute source sample (before any gain) the voice delivered since the last call; diagnostics
		//! for cev_list (0 = silence / nothing played).
		float TakeVoicePeak(int voice);
		//! After return the audio thread no longer touches the core / clock.
		void RemoveVoice(int voice);
		//! Closes the device and releases the SDL audio subsystem. Safe to call repeatedly. Remove all voices first.
		//! Logs whether the SDL audio subsystem was ever initialised (once).
		void Shutdown();

		//! ESYSTEM_EVENT_CHANGE_FOCUS.
		void SetFocus(bool focused) { m_focused.store(focused, std::memory_order_relaxed); }
		bool HasFocus() const { return m_focused.load(std::memory_order_relaxed); }

		uint32_t VoiceCount() const;
		bool IsDeviceOpen() const { return m_device != 0; }
		//! True when opening the SDL device failed (audio stays off for the rest of the run).
		bool DeviceFailed() const { return m_deviceFailed; }
		//! True once SDL_InitSubSystem(SDL_INIT_AUDIO) was called.
		bool EverInitialized() const { return m_everInit; }

		//! Audio thread entry: mixes `frames` stereo frames into `out`.
		void Mix(float* out, uint32_t frames);

	private:
		struct SVoice
		{
			cev::PlayerCore* pCore = nullptr;
			cev::AudioClock* pClock = nullptr;
			float volume = 1.f;
			float gain = 0.f;      // gain applied at the end of the previous block (audio thread state)
			bool used = false;
		};

		bool EnsureDevice();

		mutable std::mutex m_mutex; // guards m_voices; the audio callback holds it while mixing
		SVoice m_voices[kMaxVoices];
		uint32_t m_voiceCount = 0;
		uint32_t m_device = 0;      // SDL_AudioDeviceID
		uint32_t m_latencyFrames = 0; // estimate handed to the voice clocks
		bool m_subsystemInit = false;
		bool m_everInit = false;
		bool m_deviceFailed = false;
		bool m_shutdownLogged = false;
		std::atomic<bool> m_focused{true};
		std::atomic<float> m_peak[kMaxVoices];
		float m_scratch[1024 * 2];
	};
}
