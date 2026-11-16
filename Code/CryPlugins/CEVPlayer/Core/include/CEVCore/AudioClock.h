// AudioClock: IClock driven by the audio device. Media time = base media time + (output frames the device
// callback consumed since the base - output latency) / outputRate * playback rate, smoothed with the wall time
// that passed since the last callback. Use it as PlayerConfig::externalClock of the player whose audio the
// callback plays (the callback must call OnFramesConsumed() for every block it hands to the device, including
// blocks of silence, so that the clock keeps running through underruns and after the stream ended).
//
//   audio thread (device callback):   player.ReadAudioResampled(block, frames);  mix;  clock.OnFramesConsumed(frames);
//   consumer thread:                  player.Update() -> clock.NowSeconds()
//
// Pause / Resume / Seek / SetRate / Start have the semantics of InternalClock (they are what PlayerCore calls).
// Fallback: when the device stops calling OnFramesConsumed() (no device, device lost, suspended) for more than
// kStallSeconds the clock continues on the wall clock from the last audio timestamp, so video never freezes;
// when callbacks resume the clock continues from there without a backward jump. The returned time never goes
// backwards except through Seek() / Start().
//
// Threading: OnFramesConsumed() / SetOutputLatencyFrames() / SetOutputSampleRate() are lock free and may be
// called from the audio thread; every other method belongs to the consumer thread (they take a mutex, so other
// threads may call them too, but never from the audio callback).
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "CEVCore/IClock.h"

namespace cev
{
	class AudioClock final : public IClock
	{
	public:
		static constexpr double kStallSeconds = 0.25;

		// outputSampleRate: rate of the frames OnFramesConsumed() counts (the device rate).
		explicit AudioClock(uint32_t outputSampleRate = 48000);

		// ---- audio thread ----
		// `frames` output frames were handed to the device (call after mixing, once per callback).
		void OnFramesConsumed(uint32_t frames);
		// Frames queued in the device right after a callback returned, INCLUDING the block just delivered: the
		// audio of a block is heard that many frames after the callback ran (SDL callback model: about 1-2 x
		// obtained.samples). 0 = no latency compensation.
		void SetOutputLatencyFrames(uint32_t frames);
		void SetOutputSampleRate(uint32_t sampleRate);

		// Total frames reported so far.
		uint64_t FramesConsumed() const { return m_consumed.load(std::memory_order_acquire); }
		uint32_t OutputLatencyFrames() const { return m_latencyFrames.load(std::memory_order_relaxed); }
		uint32_t OutputSampleRate() const { return m_outRate.load(std::memory_order_relaxed); }
		// True while the clock is in the wall clock fallback (no callbacks for kStallSeconds).
		bool IsStalled() const;

		// Tests: replace the steady clock (nanoseconds). Call before the clock is used by other threads.
		using TimeSourceFn = int64_t (*)(void* user);
		void SetTimeSource(TimeSourceFn fn, void* user);

		// ---- IClock (consumer thread) ----
		double NowSeconds() override;
		void Start() override;
		void Pause() override;
		void Resume() override;
		void Seek(double seconds) override;
		void SetRate(double rate) override;
		double Rate() const override;
		bool IsRunning() const override;

	private:
		int64_t NowNs() const;
		void ReadCallbackStamp(uint64_t& frames, int64_t& ns) const;
		double ComputeLocked();     // advances the state, returns the media time (running only)
		void RebaseLocked(double media);

		std::atomic<uint64_t> m_consumed{0};
		std::atomic<int64_t> m_stampNs{0};          // time of the last OnFramesConsumed(), 0 = never
		std::atomic<uint32_t> m_latencyFrames{0};
		std::atomic<uint32_t> m_outRate{48000};

		TimeSourceFn m_timeFn = nullptr;
		void* m_timeUser = nullptr;

		mutable std::mutex m_mutex;
		double m_baseMedia = 0.0;
		uint64_t m_baseFrames = 0;
		int64_t m_baseNs = 0;
		double m_rate = 1.0;
		bool m_running = false;
		bool m_stalled = false;
		double m_lastMedia = 0.0;       // last value returned (monotonic clamp, wall fallback origin)
		int64_t m_lastNowNs = 0;
	};
}
