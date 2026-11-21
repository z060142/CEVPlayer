// Streaming resampler: any source rate / channel count -> dstRate (default 48 kHz) stereo, with a variable
// speed factor (playback rate 0.25 .. 4.0, varispeed: pitch follows the rate).
//
//   * Windowed sinc (Kaiser), 24 taps at ratio <= 1, the kernel is stretched (anti-alias) when the step per
//     output frame exceeds one input frame. Weights come from a precomputed table (linear interpolation).
//   * Channel mapping happens on the input side: 1 -> duplicated to L/R, 2 -> as is, 3..8 -> stereo downmix
//     (Vorbis / Opus family 1 channel order: 3 FL FC FR, 4 FL FR BL BR, 5 FL FC FR BL BR, 6 FL FC FR BL BR LFE,
//     7 FL FC FR SL SR BC LFE, 8 FL FC FR SL SR BL BR LFE; ITU-R BS.775 coefficients, centre / surround -3 dB,
//     LFE dropped, the sum normalised so that a full scale input cannot clip). More than 8 channels: the first
//     two are used as L / R.
//   * Allocation free after Init(). Not thread safe: one owner (the audio thread) calls everything except
//     Init() / Shutdown(), which must not overlap with other calls.
//   * Output frame k corresponds to the input position k * step (zero phase, the first output equals input 0).
//     When src rate == dst rate and the rate is 1.0 the signal passes through unchanged (bit exact).
#pragma once

#include <cstdint>
#include <vector>

#include "CEVCore/Status.h"

namespace cev
{
	class Resampler
	{
	public:
		static constexpr double kMinRate = 0.25;
		static constexpr double kMaxRate = 4.0;
		static constexpr uint32_t kHalfTaps = 12;     // kernel half width in input frames at step <= 1

		Resampler();
		~Resampler();
		Resampler(const Resampler&) = delete;
		Resampler& operator=(const Resampler&) = delete;

		// srcRate 1000..768000, srcChannels 1..64, dstRate 1000..768000, dstChannels 1 or 2,
		// maxBlockFrames = largest Push() (input frames). Allocates here and only here.
		Status Init(uint32_t srcRate, uint32_t srcChannels, uint32_t dstRate = 48000, uint32_t dstChannels = 2,
			uint32_t maxBlockFrames = 1024);
		void Shutdown();
		bool IsInitialized() const { return m_cap != 0; }

		// Drops all buffered input and filter history: the next output corresponds to the next pushed frame.
		void Reset();
		// Playback speed (clamped to kMinRate..kMaxRate). Takes effect for the next Pull().
		void SetRate(double rate);
		double Rate() const { return m_rate; }

		// ---- pull style ----
		// Input frames (beyond what is buffered) needed before Pull(outFrames) can deliver outFrames.
		uint32_t InputNeeded(uint32_t outFrames) const;
		// Largest frame count Push() accepts right now (>= maxBlockFrames when the caller pulls what it pushes).
		uint32_t InputSpace() const;
		// Appends `frames` interleaved input frames (srcChannels). in == nullptr appends silence.
		// Returns the number of frames accepted (min(frames, InputSpace())).
		uint32_t Push(const float* in, uint32_t frames);
		// Produces up to maxFrames output frames into `out` (dstChannels interleaved); returns the count.
		uint32_t Pull(float* out, uint32_t maxFrames);
		// Appends the silence that flushes the filter tail (call once at the end of a stream, then Pull()).
		void Flush();

		// ---- push style convenience ----
		// Pushes as much of `in` as fits and pulls up to outCapacity frames; *consumed / *produced are frame counts.
		// Input that was not consumed must be passed again by the caller.
		Status Process(const float* in, uint32_t inFrames, float* out, uint32_t outCapacity,
			uint32_t* consumed, uint32_t* produced);

		uint32_t SrcRate() const { return m_srcRate; }
		uint32_t DstRate() const { return m_dstRate; }
		uint32_t SrcChannels() const { return m_srcChannels; }
		uint32_t DstChannels() const { return m_dstChannels; }
		// Input frames per output frame (srcRate * rate / dstRate).
		double Step() const { return m_step; }
		// Group delay in input frames (the kernel half width at the current step, 0 when passing through).
		uint32_t LatencyInputFrames() const { return HalfTaps(); }

		// Downmix matrix used for `channels` input channels (coefficient per input channel for L and R).
		static void DownmixCoefficients(uint32_t channels, float* left, float* right, uint32_t capacity);

	private:
		uint32_t HalfTaps() const { return m_passthrough ? 0u : m_halfTaps; }
		void Recompute();
		void Compact();

		uint32_t m_srcRate = 0;
		uint32_t m_dstRate = 0;
		uint32_t m_srcChannels = 0;
		uint32_t m_dstChannels = 0;
		uint32_t m_maxBlock = 0;
		double m_rate = 1.0;
		double m_step = 1.0;
		double m_invScale = 1.0;       // 1 / max(1, step)
		uint32_t m_halfTaps = kHalfTaps;
		uint32_t m_maxHalfTaps = kHalfTaps;
		bool m_passthrough = false;

		float m_coefL[8] = {};
		float m_coefR[8] = {};

		std::vector<float> m_table;    // kernel samples, kTableRes per input frame, spans -kHalfTaps .. +kHalfTaps
		std::vector<float> m_weights;  // scratch: one output frame worth of weights
		std::vector<float> m_buf;      // stereo history + pending input
		uint32_t m_cap = 0;            // capacity of m_buf in frames
		uint32_t m_len = 0;            // valid frames
		double m_pos = 0.0;            // position of the next output frame (index into m_buf)
	};
}
