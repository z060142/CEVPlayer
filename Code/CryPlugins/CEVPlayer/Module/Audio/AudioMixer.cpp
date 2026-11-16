#include "StdAfx.h"
#include "PluginEnv.h"
#include "AudioMixer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "CEVCore/AudioClock.h"
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

		inline float Clamp01(float v)
		{
			return std::min(1.f, std::max(0.f, v));
		}
	}

	CAudioMixer::CAudioMixer()
	{
		for (std::atomic<float>& p : m_peak)
			p.store(0.f, std::memory_order_relaxed);
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

		// Subsystem init is reference counted by SDL: always take our own reference (even when the engine already
		// initialised audio, it may quit its reference while we still play) and give it back in Shutdown() / on
		// every failure path, so the calls stay balanced.
		if (!m_subsystemInit)
		{
			m_everInit = true;
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
		// Frames queued in the device after a callback returned, including the block just delivered: with the
		// SDL callback model that is one to two device buffers; take the middle.
		m_latencyFrames = static_cast<uint32_t>(std::max(0, static_cast<int>(obtained.samples))) * 3u / 2u;
		Log("audio device opened: %d Hz, %d channels, %d samples (latency estimate %u frames)", obtained.freq, static_cast<int>(obtained.channels),
			static_cast<int>(obtained.samples), m_latencyFrames);
		return true;
	}

	int CAudioMixer::AddVoice(cev::PlayerCore* pCore, cev::AudioClock* pClock)
	{
		if (pCore == nullptr || pClock == nullptr)
			return kInvalidVoice;
		if (mEnv != nullptr && mEnv->m_cevDisableEmbeddedAudio != 0)
			return kInvalidVoice; // the SDL audio subsystem must stay untouched
		if (pCore->GetInfo().outputAudioSampleRate != kDeviceRate)
		{
			LogWarning("audio stream cannot be converted to the %u Hz device format: no audio for this video", kDeviceRate);
			return kInvalidVoice;
		}
		if (!EnsureDevice())
			return kInvalidVoice;

		pClock->SetOutputSampleRate(kDeviceRate);
		pClock->SetOutputLatencyFrames(m_latencyFrames);

		int handle = kInvalidVoice;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			for (int i = 0; i < kMaxVoices; ++i)
			{
				if (!m_voices[i].used)
				{
					m_voices[i] = SVoice();
					m_voices[i].pCore = pCore;
					m_voices[i].pClock = pClock;
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
		m_voices[voice].volume = Clamp01(volume);
	}

	float CAudioMixer::TakeVoicePeak(int voice)
	{
		if (voice < 0 || voice >= kMaxVoices)
			return 0.f;
		return m_peak[voice].exchange(0.f, std::memory_order_relaxed);
	}

	void CAudioMixer::RemoveVoice(int voice)
	{
		if (voice < 0 || voice >= kMaxVoices)
			return;
		bool none = false;
		{
			// The audio callback holds this mutex while it reads from the core and advances the clock, so once the
			// lock is taken the callback is not inside this voice any more.
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
		if (!m_shutdownLogged)
		{
			m_shutdownLogged = true;
			if (!m_everInit)
				LogAlways("SDL audio subsystem was never initialized");
			else
				Log("SDL audio subsystem was initialized%s", m_deviceFailed ? " (the device could not be opened)" : "");
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

		// Plain reads of cvars written by the main thread: a torn / stale value only changes the volume for a block.
		float master = 1.f;
		bool enabled = true;
		if (mEnv != nullptr)
		{
			enabled = mEnv->m_cevAudioEnable != 0;
			master = Clamp01(mEnv->m_cevVolume) * Clamp01(mEnv->m_audioVolume);
			if (mEnv->m_cevMuteOnFocusLoss != 0 && !m_focused.load(std::memory_order_relaxed))
				master = 0.f;
		}
		if (!enabled)
			master = 0.f;

		std::lock_guard<std::mutex> lock(m_mutex);
		for (int voiceIndex = 0; voiceIndex < kMaxVoices; ++voiceIndex)
		{
			SVoice& v = m_voices[voiceIndex];
			if (!v.used || v.pCore == nullptr)
				continue;
			float peak = 0.f;
			const float target = master * v.volume;
			const float startGain = v.gain;

			uint32_t done = 0;
			bool starved = false;
			while (done < frames && !starved)
			{
				const uint32_t n = std::min(frames - done, kChunkFrames);
				// Always pull, even when muted, so that the PCM ring keeps draining at playback speed.
				const uint32_t got = v.pCore->ReadAudioResampled(m_scratch, n);
				if (got == 0)
					break; // silence (not playing / underrun / seeking / no audio)
				for (uint32_t i = 0; i < got * 2u; ++i)
					peak = std::max(peak, std::fabs(m_scratch[i]));
				if (startGain > 0.f || target > 0.f)
				{
					float* dst = out + static_cast<size_t>(done) * 2u;
					// Linear ramp from the previous block's gain to the target across the whole block.
					const float step = (target - startGain) / static_cast<float>(frames);
					float g = startGain + step * static_cast<float>(done);
					for (uint32_t i = 0; i < got; ++i)
					{
						g += step;
						dst[2u * i] += m_scratch[2u * i] * g;
						dst[2u * i + 1u] += m_scratch[2u * i + 1u] * g;
					}
				}
				done += n;
				starved = got < n;
			}
			v.gain = target;
			if (peak > m_peak[voiceIndex].load(std::memory_order_relaxed))
				m_peak[voiceIndex].store(peak, std::memory_order_relaxed);
			// The clock counts every block handed to the device, also silence (underrun, paused, ended).
			if (v.pClock != nullptr)
				v.pClock->OnFramesConsumed(frames);
		}

		for (uint32_t i = 0; i < frames * 2u; ++i)
			out[i] = std::min(1.f, std::max(-1.f, out[i]));
	}
}
