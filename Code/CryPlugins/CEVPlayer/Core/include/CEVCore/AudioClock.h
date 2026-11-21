// AudioClock: IClock driven by the audio device. Media time = base media time + (audible output frames the
// device callback consumed since the base - output latency) / outputRate * playback rate, smoothed with the wall
// time that passed since the last callback. Use it as PlayerConfig::externalClock of the player whose audio the
// callback plays.
//
//   audio thread (device callback):   got = player.ReadAudioResampled(block, frames);  mix;
//                                     clock.OnFramesConsumed(frames, got);
//   consumer thread:                  player.Update() -> clock.NowSeconds()
//
// The callback must report EVERY block it hands to the device, including blocks of silence, and pass the number
// of frames of the block that came from decoded audio (the return value of ReadAudioResampled(); the audio is at
// the start of the block) as `audibleFrames`.
//
// Waiting for audio (the A/V sync contract)
//   After Start() / Seek() (loop, stop -> play and so on go through Seek()) the audio that belongs to the new
//   position reaches the callback later than the clock is rebased: the decoder applies the seek on a worker
//   thread and ReadAudioResampled() returns silence until then. The same happens after an underrun (empty PCM
//   ring). Silent frames are therefore NOT media time: only the audible frames of the blocks advance the clock,
//   so the clock holds (at the seek target / at the position where the audio ran dry) while the callback delivers
//   silence and continues with the first audible block, shifted by the output latency like the rest of the audio.
//   The clock then stays locked to what is heard, with no lasting offset.
//   Silent frames do count (the old behaviour, a pure wall driven clock) when
//     * the legacy OnFramesConsumed(frames) is used (every block is audible), or
//     * IClock::NotifyAudioEnded() was called (PlayerCore does that when the media has no audio or the audio is
//       over and the PCM ring is drained, so that a clip whose audio is shorter than its video keeps running;
//       cleared by the next Seek() / Start()), or
//     * no audible frame arrived for kStallSeconds while the callbacks keep coming (no audio stream, decoder
//       stalled, ...): the clock then continues on the wall clock from where it held, never backwards, and
//       stays in that mode until audible audio comes back (then underruns hold the clock again).
//   Limit: a block that was mixed from stale audio concurrently with a Seek() can end the wait up to one block
//   early (the clock leads by at most one block).
//   Do not use an AudioClock for media without audio unless NotifyAudioEnded() is delivered (PlayerCore does).
//
// Pause / Resume / Seek / SetRate / Start have the semantics of InternalClock (they are what PlayerCore calls).
// SetRate() takes any rate > 0, PlayerCore::SetRate() clamps to PlayerCore::kMinRate .. kMaxRate first.
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
		// `frames` output frames were handed to the device (call after mixing, once per callback);
		// `audibleFrames` (<= frames, clamped) of them came from decoded audio: pass the value returned by
		// PlayerCore::ReadAudioResampled(). See "Waiting for audio" above.
		void OnFramesConsumed(uint32_t frames, uint32_t audibleFrames);
		// Legacy: the whole block counts as audio (OnFramesConsumed(frames, frames)).
		void OnFramesConsumed(uint32_t frames);
		// Frames queued in the device right after a callback returned, INCLUDING the block just delivered: the
		// audio of a block is heard that many frames after the callback ran (SDL callback model: about 1-2 x
		// obtained.samples). 0 = no latency compensation.
		void SetOutputLatencyFrames(uint32_t frames);
		void SetOutputSampleRate(uint32_t sampleRate);

		// Total frames reported so far (audible or not).
		uint64_t FramesConsumed() const { return m_consumed.load(std::memory_order_acquire); }
		// Frames that count as media time so far (audible frames, plus the silent ones while they count).
		uint64_t FramesCounted() const { return m_media.load(std::memory_order_acquire); }
		uint32_t OutputLatencyFrames() const { return m_latencyFrames.load(std::memory_order_relaxed); }
		uint32_t OutputSampleRate() const { return m_outRate.load(std::memory_order_relaxed); }
		// True while the clock is in the wall clock fallback (no callbacks for kStallSeconds).
		bool IsStalled() const;
		// True while silent frames count as media time (audio ended / no audio / waiting timed out).
		bool CountsSilence() const;

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
		void NotifyAudioEnded() override;

	private:
		int64_t NowNs() const;
		void ReadCallbackStamp(uint64_t& frames, int64_t& stampNs, int64_t& interpNs) const;
		double ComputeLocked();     // advances the state, returns the media time (running only)
		void RebaseLocked(double media, bool newAudioEpoch);

		// audio thread -> consumer
		std::atomic<uint64_t> m_consumed{0};        // all frames
		std::atomic<uint64_t> m_media{0};           // frames that count as media time
		std::atomic<int64_t> m_stampNs{0};          // time of the last OnFramesConsumed(), 0 = never
		std::atomic<int64_t> m_interpNs{0};         // = stamp when the last block counted completely (wall interpolation), else 0
		std::atomic<int64_t> m_countedNs{0};        // time of the last block that counted at least one frame
		std::atomic<uint32_t> m_latencyFrames{0};
		std::atomic<uint32_t> m_outRate{48000};
		// consumer -> audio thread
		std::atomic<bool> m_audioEnded{false};      // NotifyAudioEnded(): silence counts
		std::atomic<bool> m_countSilence{false};    // waiting for audio timed out: silence counts until audio is back

		TimeSourceFn m_timeFn = nullptr;
		void* m_timeUser = nullptr;

		mutable std::mutex m_mutex;
		double m_baseMedia = 0.0;
		int64_t m_baseFrames = 0;       // m_media value that belongs to m_baseMedia
		int64_t m_baseNs = 0;
		int64_t m_holdSinceNs = 0;      // start of the current wait for audio (rebase / resume)
		double m_rate = 1.0;
		bool m_running = false;
		bool m_stalled = false;
		double m_lastMedia = 0.0;       // last value returned (monotonic clamp, wall fallback origin)
		int64_t m_lastNowNs = 0;
	};
}
