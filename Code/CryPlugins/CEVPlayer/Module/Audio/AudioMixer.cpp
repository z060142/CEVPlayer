#include "StdAfx.h"
#include "PluginEnv.h"
#include "AudioMixer.h"

#include <algorithm>
#include <cstring>

#include "CEVCore/PlayerCore.h"

#ifndef NOMINMAX
	#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
	#define _CRT_SECURE_NO_WARNINGS
#endif
#define SDL_MAIN_HANDLED
#include <SDL.h>

namespace CEVPlayer
{
	namespace
	{
		constexpr uint32_t kChunkFrames = 1024; // matches CAudioMixer::m_scratch (stereo)

		void SDLCALL AudioCallback(void* userData, Uint8* stream, int len)
		{
			CAudioMixer* const pMixer = static_cast<CAudioMixer*>(userData);
			const uint32_t frames = static_cast<uint32_t>(len) / (2u * sizeof(float));
			pMixer->Mix(reinterpret_cast<float*>(stream), frames);
		}
	}

	CAudioMixer::CAudioMixer()
	{
		std::memset(m_scratch, 0, sizeof(m_scratch));
	}

	CAudioMixer::~CAudioMixer()
	{
		Shutdown();
	}

	bool CAudioMixer::EnsureDevice()
	{
		if (m_device != 0)
			return true;
		if (m_deviceFailed)
			return false;

		if (SDL_WasInit(SDL_INIT_AUDIO) == 0)
		{
			// Subsystem init is reference counted by SDL: this does not disturb the engine's own audio.
			if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
			{
				LogError("SDL audio init failed (%s), video audio disabled", SDL_GetError());
				m_deviceFailed = true;
				return false;
			}
			m_subsystemInit = true;
		}

		SDL_AudioSpec wanted;
		SDL_zero(wanted);
		wanted.freq = static_cast<int>(kDeviceRate);
		wanted.format = AUDIO_F32SYS;
		wanted.channels = 2;
		wanted.samples = 1024;
		wanted.callback = AudioCallback;
		wanted.userdata = this;

		SDL_AudioSpec obtained;
		SDL_zero(obtained);
		// allowed_changes = 0: SDL converts to the device format itself, so the callback always gets our format.
		m_device = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);
		if (m_device == 0)
		{
			LogError("could not open the SDL audio device (%s), video audio disabled", SDL_GetError());
			m_deviceFailed = true;
			if (m_subsystemInit)
			{
				SDL_QuitSubSystem(SDL_INIT_AUDIO);
				m_subsystemInit = false;
			}
			return false;
		}
		SDL_PauseAudioDevice(m_device, 1); // unpaused when the first voice arrives
		LogDebug("audio device opened: %d Hz, %d channels, %d samples", obtained.freq, static_cast<int>(obtained.channels), static_cast<int>(obtained.samples));
		return true;
	}

	int CAudioMixer::AddVoice(cev::PlayerCore* pCore, uint32_t sampleRate, uint32_t channels)
	{
		if (pCore == nullptr)
			return kInvalidVoice;
		if (sampleRate != kDeviceRate)
		{
			LogWarning("audio stream has %u Hz, the M1 audio device is %u Hz without a resampler: no audio for this video", sampleRate, kDeviceRate);
			return kInvalidVoice;
		}
		if (channels < 1 || channels > 2)
		{
			LogWarning("audio stream has %u channels, M1 plays mono and stereo only: no audio for this video", channels);
			return kInvalidVoice;
		}
		if (!EnsureDevice())
			return kInvalidVoice;

		int handle = kInvalidVoice;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			for (int i = 0; i < kMaxVoices; ++i)
			{
				if (!m_voices[i].used)
				{
					m_voices[i].pCore = pCore;
					m_voices[i].channels = channels;
					m_voices[i].volume = 1.f;
					m_voices[i].used = true;
					++m_voiceCount;
					handle = i;
					break;
				}
			}
		}
		if (handle == kInvalidVoice)
		{
			LogWarning("too many audio voices (%d)", kMaxVoices);
			return kInvalidVoice;
		}
		SDL_PauseAudioDevice(m_device, 0);
		return handle;
	}

	void CAudioMixer::SetVoiceVolume(int voice, float volume)
	{
		if (voice < 0 || voice >= kMaxVoices)
			return;
		std::lock_guard<std::mutex> lock(m_mutex);
		m_voices[voice].volume = std::min(1.f, std::max(0.f, volume));
	}

	void CAudioMixer::RemoveVoice(int voice)
	{
		if (voice < 0 || voice >= kMaxVoices)
			return;
		bool none = false;
		{
			// The audio callback holds this mutex while it reads from the core, so once the lock is taken the
			// callback is not inside ReadAudio() of this voice any more.
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_voices[voice].used)
			{
				m_voices[voice] = SVoice();
				--m_voiceCount;
			}
			none = (m_voiceCount == 0);
		}
		if (none && m_device != 0)
			SDL_PauseAudioDevice(m_device, 1);
	}

	void CAudioMixer::Shutdown()
	{
		if (m_device != 0)
		{
			SDL_CloseAudioDevice(m_device); // returns after the callback thread stopped
			m_device = 0;
		}
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			for (SVoice& v : m_voices)
				v = SVoice();
			m_voiceCount = 0;
		}
		if (m_subsystemInit)
		{
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
			m_subsystemInit = false;
		}
	}

	uint32_t CAudioMixer::VoiceCount() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_voiceCount;
	}

	void CAudioMixer::Mix(float* out, uint32_t frames)
	{
		std::memset(out, 0, static_cast<size_t>(frames) * 2u * sizeof(float));

		const bool enabled = mEnv != nullptr && mEnv->m_cevAudioEnable != 0;
		const float master = mEnv != nullptr ? std::min(1.f, std::max(0.f, mEnv->m_audioVolume)) : 1.f;

		std::lock_guard<std::mutex> lock(m_mutex);
		for (SVoice& v : m_voices)
		{
			if (!v.used || v.pCore == nullptr)
				continue;
			const float gain = enabled ? master * v.volume : 0.f;

			uint32_t done = 0;
			while (done < frames)
			{
				const uint32_t n = std::min(frames - done, kChunkFrames);
				// Always pull, even when muted, so that the PCM ring keeps draining at playback speed.
				const uint32_t got = v.pCore->ReadAudio(m_scratch, n);
				if (got == 0)
					break; // silence (not playing / underrun / seeking)
				if (gain > 0.f)
				{
					float* dst = out + static_cast<size_t>(done) * 2u;
					if (v.channels == 2)
					{
						for (uint32_t i = 0; i < got * 2u; ++i)
							dst[i] += m_scratch[i] * gain;
					}
					else
					{
						for (uint32_t i = 0; i < got; ++i)
						{
							const float s = m_scratch[i] * gain;
							dst[2u * i] += s;
							dst[2u * i + 1u] += s;
						}
					}
				}
				done += n;
				if (got < n)
					break;
			}
		}

		for (uint32_t i = 0; i < frames * 2u; ++i)
			out[i] = std::min(1.f, std::max(-1.f, out[i]));
	}
}
