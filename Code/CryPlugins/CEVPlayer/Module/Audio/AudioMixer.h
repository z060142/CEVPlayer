// Plug-in wide SDL2 audio output (M1).
//
// One device for the whole plug-in: opened lazily by the first player that has audio, closed once at plug-in
// shutdown. SDL is initialised with SDL_InitSubSystem(SDL_INIT_AUDIO) and released with SDL_QuitSubSystem; this
// code never calls SDL_Init / SDL_Quit (the engine SDL_mixer implementation owns those).
//
// M1 limits: the device is 48000 Hz float stereo and there is no resampler, so a stream with another sample
// rate (e.g. 44100 Hz Vorbis) gets no audio output (logged once per stream). The video clock is still the
// internal clock, audio is pulled at device speed and is not used for synchronisation yet (M2).
#pragma once

#include <cstdint>
#include <mutex>

namespace cev { class PlayerCore; }

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

		//! Registers a stream. The core must stay alive and loaded until RemoveVoice() returns. Returns
		//! kInvalidVoice (and logs) when the stream cannot be played (rate / channel count / no device).
		int AddVoice(cev::PlayerCore* pCore, uint32_t sampleRate, uint32_t channels);
		void SetVoiceVolume(int voice, float volume);
		//! After return the audio thread no longer touches the core.
		void RemoveVoice(int voice);
		//! Closes the device and releases the SDL audio subsystem. Safe to call repeatedly. Remove all voices first.
		void Shutdown();

		uint32_t VoiceCount() const;
		bool IsDeviceOpen() const { return m_device != 0; }

		//! Audio thread entry: mixes `frames` stereo frames into `out`.
		void Mix(float* out, uint32_t frames);

	private:
		struct SVoice
		{
			cev::PlayerCore* pCore = nullptr;
			uint32_t channels = 0;
			float volume = 1.f;
			bool used = false;
		};

		bool EnsureDevice();

		mutable std::mutex m_mutex; // guards m_voices; the audio callback holds it while mixing
		SVoice m_voices[kMaxVoices];
		uint32_t m_voiceCount = 0;
		uint32_t m_device = 0;      // SDL_AudioDeviceID
		bool m_subsystemInit = false;
		bool m_deviceFailed = false;
		float m_scratch[1024 * 2];
	};
}
