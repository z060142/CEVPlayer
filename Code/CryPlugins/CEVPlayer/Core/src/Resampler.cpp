#include "CEVCore/Resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace cev
{
	namespace
	{
		constexpr uint32_t kTableRes = 512;       // table entries per input frame
		constexpr double kCutoff = 0.93;          // passband edge as a fraction of the (input) Nyquist frequency
		constexpr double kKaiserBeta = 7.0;
		constexpr double kPi = 3.14159265358979323846;
		constexpr float kSqrtHalf = 0.70710678118654752f;

		double BesselI0(double x)
		{
			double sum = 1.0;
			double term = 1.0;
			const double q = x * x * 0.25;
			for (int k = 1; k < 64; ++k)
			{
				term *= q / (static_cast<double>(k) * k);
				sum += term;
				if (term < sum * 1e-17)
					break;
			}
			return sum;
		}
	}

	Resampler::Resampler() = default;
	Resampler::~Resampler() = default;

	void Resampler::DownmixCoefficients(uint32_t channels, float* left, float* right, uint32_t capacity)
	{
		if (left == nullptr || right == nullptr || capacity == 0)
			return;
		for (uint32_t i = 0; i < capacity; ++i)
			left[i] = right[i] = 0.0f;
		const auto set = [&](uint32_t idx, float l, float r)
		{
			if (idx < capacity)
			{
				left[idx] = l;
				right[idx] = r;
			}
		};
		const float a = kSqrtHalf;
		switch (channels)
		{
		case 0:
			break;
		case 1:
			set(0, 1.0f, 1.0f);
			break;
		case 2:
			set(0, 1.0f, 0.0f);
			set(1, 0.0f, 1.0f);
			break;
		case 3: // FL FC FR
			set(0, 1.0f, 0.0f); set(1, a, a); set(2, 0.0f, 1.0f);
			break;
		case 4: // FL FR BL BR
			set(0, 1.0f, 0.0f); set(1, 0.0f, 1.0f); set(2, a, 0.0f); set(3, 0.0f, a);
			break;
		case 5: // FL FC FR BL BR
			set(0, 1.0f, 0.0f); set(1, a, a); set(2, 0.0f, 1.0f); set(3, a, 0.0f); set(4, 0.0f, a);
			break;
		case 6: // FL FC FR BL BR LFE (Vorbis / Opus order), LFE dropped
			set(0, 1.0f, 0.0f); set(1, a, a); set(2, 0.0f, 1.0f); set(3, a, 0.0f); set(4, 0.0f, a);
			break;
		case 7: // FL FC FR SL SR BC LFE
			set(0, 1.0f, 0.0f); set(1, a, a); set(2, 0.0f, 1.0f); set(3, a, 0.0f); set(4, 0.0f, a); set(5, 0.5f, 0.5f);
			break;
		case 8: // FL FC FR SL SR BL BR LFE
			set(0, 1.0f, 0.0f); set(1, a, a); set(2, 0.0f, 1.0f); set(3, a, 0.0f); set(4, 0.0f, a);
			set(5, 0.5f, 0.0f); set(6, 0.0f, 0.5f);
			break;
		default: // no defined layout: first two channels
			set(0, 1.0f, 0.0f);
			set(1, 0.0f, 1.0f);
			return;
		}
		// Normalise so that the loudest possible sum is 1 (no clipping on full scale input).
		float sumL = 0.0f, sumR = 0.0f;
		for (uint32_t i = 0; i < capacity; ++i)
		{
			sumL += left[i];
			sumR += right[i];
		}
		const float norm = 1.0f / std::max(1.0f, std::max(sumL, sumR));
		for (uint32_t i = 0; i < capacity; ++i)
		{
			left[i] *= norm;
			right[i] *= norm;
		}
	}

	Status Resampler::Init(uint32_t srcRate, uint32_t srcChannels, uint32_t dstRate, uint32_t dstChannels,
		uint32_t maxBlockFrames)
	{
		Shutdown();
		if (srcRate < 1000 || srcRate > 768000 || dstRate < 1000 || dstRate > 768000 || srcChannels < 1 ||
			srcChannels > 64 || (dstChannels != 1 && dstChannels != 2) || maxBlockFrames == 0 || maxBlockFrames > (1u << 20))
			return Status::InvalidArgument;

		try
		{
			m_srcRate = srcRate;
			m_dstRate = dstRate;
			m_srcChannels = srcChannels;
			m_dstChannels = dstChannels;
			m_maxBlock = maxBlockFrames;
			m_rate = 1.0;
			DownmixCoefficients(srcChannels, m_coefL, m_coefR, 8);

			const double maxStep = static_cast<double>(srcRate) * kMaxRate / dstRate;
			const double maxScale = std::max(1.0, maxStep);
			m_maxHalfTaps = static_cast<uint32_t>(std::ceil(kHalfTaps * maxScale));
			const uint32_t cap = maxBlockFrames + 2 * m_maxHalfTaps + static_cast<uint32_t>(std::ceil(maxStep)) + 16;

			// Kernel table over u in [-kHalfTaps, +kHalfTaps]: cutoff * sinc(cutoff * u) * Kaiser(u / kHalfTaps).
			const uint32_t tableSize = 2 * kHalfTaps * kTableRes + 2;
			m_table.assign(tableSize, 0.0f);
			const double i0Beta = BesselI0(kKaiserBeta);
			for (uint32_t i = 0; i < tableSize; ++i)
			{
				const double u = static_cast<double>(i) / kTableRes - static_cast<double>(kHalfTaps);
				const double r = u / kHalfTaps;
				double v = 0.0;
				if (std::fabs(r) < 1.0)
				{
					const double arg = kPi * kCutoff * u;
					const double s = std::fabs(arg) < 1e-12 ? 1.0 : std::sin(arg) / arg;
					v = kCutoff * s * BesselI0(kKaiserBeta * std::sqrt(1.0 - r * r)) / i0Beta;
				}
				m_table[i] = static_cast<float>(v);
			}
			m_weights.assign(static_cast<size_t>(2) * m_maxHalfTaps + 2, 0.0f);
			m_buf.assign(static_cast<size_t>(cap) * 2, 0.0f);
			m_cap = cap;
		}
		catch (const std::bad_alloc&)
		{
			Shutdown();
			return Status::OutOfMemory;
		}
		Recompute();
		Reset();
		return Status::Ok;
	}

	void Resampler::Shutdown()
	{
		std::vector<float>().swap(m_table);
		std::vector<float>().swap(m_weights);
		std::vector<float>().swap(m_buf);
		m_cap = 0;
		m_len = 0;
		m_pos = 0.0;
		m_srcRate = m_dstRate = m_srcChannels = m_dstChannels = m_maxBlock = 0;
		m_rate = 1.0;
		m_step = 1.0;
		m_invScale = 1.0;
		m_halfTaps = m_maxHalfTaps = kHalfTaps;
		m_passthrough = false;
	}

	void Resampler::Recompute()
	{
		m_step = static_cast<double>(m_srcRate) * m_rate / static_cast<double>(m_dstRate);
		const double scale = std::max(1.0, m_step);
		m_invScale = 1.0 / scale;
		m_halfTaps = std::min(m_maxHalfTaps, static_cast<uint32_t>(std::ceil(kHalfTaps * scale)));
		m_passthrough = (m_step == 1.0);
	}

	void Resampler::Reset()
	{
		if (m_cap == 0)
			return;
		std::fill(m_buf.begin(), m_buf.begin() + static_cast<size_t>(m_maxHalfTaps) * 2, 0.0f);
		m_len = m_maxHalfTaps;
		m_pos = static_cast<double>(m_maxHalfTaps);
	}

	void Resampler::SetRate(double rate)
	{
		if (m_cap == 0 || !(rate > 0.0))
			return;
		rate = std::min(kMaxRate, std::max(kMinRate, rate));
		if (rate == m_rate)
			return;
		m_rate = rate;
		Recompute();
	}

	uint32_t Resampler::InputNeeded(uint32_t outFrames) const
	{
		if (m_cap == 0 || outFrames == 0)
			return 0;
		const double last = m_pos + static_cast<double>(outFrames - 1) * m_step;
		const double required = std::floor(last) + static_cast<double>(HalfTaps()) + 1.0;
		const double have = static_cast<double>(m_len);
		return required > have ? static_cast<uint32_t>(std::min(required - have, 4294967000.0)) : 0u;
	}

	void Resampler::Compact()
	{
		const uint32_t i0 = static_cast<uint32_t>(m_pos);
		if (i0 <= m_maxHalfTaps)
			return;
		const uint32_t shift = i0 - m_maxHalfTaps;
		std::memmove(m_buf.data(), m_buf.data() + static_cast<size_t>(shift) * 2,
			static_cast<size_t>(m_len - shift) * 2 * sizeof(float));
		m_len -= shift;
		m_pos -= static_cast<double>(shift);
	}

	uint32_t Resampler::InputSpace() const
	{
		if (m_cap == 0)
			return 0;
		const uint32_t i0 = static_cast<uint32_t>(m_pos);
		const uint32_t reclaim = i0 > m_maxHalfTaps ? i0 - m_maxHalfTaps : 0u;
		return m_cap - m_len + reclaim;
	}

	uint32_t Resampler::Push(const float* in, uint32_t frames)
	{
		if (m_cap == 0 || frames == 0)
			return 0;
		if (m_cap - m_len < frames)
			Compact();
		frames = std::min(frames, m_cap - m_len);
		float* dst = m_buf.data() + static_cast<size_t>(m_len) * 2;
		if (in == nullptr)
		{
			std::memset(dst, 0, static_cast<size_t>(frames) * 2 * sizeof(float));
		}
		else if (m_srcChannels == 2)
		{
			std::memcpy(dst, in, static_cast<size_t>(frames) * 2 * sizeof(float));
		}
		else if (m_srcChannels == 1)
		{
			for (uint32_t i = 0; i < frames; ++i)
				dst[2 * i] = dst[2 * i + 1] = in[i];
		}
		else
		{
			const uint32_t mixed = std::min(m_srcChannels, 8u);
			for (uint32_t i = 0; i < frames; ++i)
			{
				const float* s = in + static_cast<size_t>(i) * m_srcChannels;
				float l = 0.0f, r = 0.0f;
				for (uint32_t c = 0; c < mixed; ++c)
				{
					l += s[c] * m_coefL[c];
					r += s[c] * m_coefR[c];
				}
				dst[2 * i] = l;
				dst[2 * i + 1] = r;
			}
		}
		m_len += frames;
		return frames;
	}

	uint32_t Resampler::Pull(float* out, uint32_t maxFrames)
	{
		if (m_cap == 0 || out == nullptr || maxFrames == 0)
			return 0;
		const uint32_t half = HalfTaps();
		const float* buf = m_buf.data();
		const uint32_t outCh = m_dstChannels;
		uint32_t produced = 0;

		if (m_passthrough)
		{
			// step == 1: m_pos advances by whole frames only.
			while (produced < maxFrames)
			{
				const uint32_t i0 = static_cast<uint32_t>(m_pos);
				if (i0 >= m_len)
					break;
				const float l = buf[2 * i0];
				const float r = buf[2 * i0 + 1];
				if (outCh == 2)
				{
					out[2 * produced] = l;
					out[2 * produced + 1] = r;
				}
				else
				{
					out[produced] = 0.5f * (l + r);
				}
				m_pos += 1.0;
				++produced;
			}
			return produced;
		}

		const float* table = m_table.data();
		float* w = m_weights.data();
		const double invScale = m_invScale;
		const double halfWidth = static_cast<double>(kHalfTaps);
		const double tableLast = static_cast<double>(m_table.size() - 2);
		const int taps = static_cast<int>(2 * half);

		while (produced < maxFrames)
		{
			const double floorPos = std::floor(m_pos);
			const uint32_t i0 = static_cast<uint32_t>(floorPos);
			if (static_cast<uint64_t>(i0) + half >= m_len)
				break;
			const double frac = m_pos - floorPos;

			// weights for the input frames i0 - half + 1 .. i0 + half
			float sum = 0.0f;
			for (int k = 0; k < taps; ++k)
			{
				const double x = (static_cast<double>(k - static_cast<int>(half) + 1) - frac) * invScale;
				const double t = (x + halfWidth) * kTableRes;
				float wk = 0.0f;
				if (t >= 0.0 && t < tableLast)
				{
					const uint32_t ti = static_cast<uint32_t>(t);
					const float f = static_cast<float>(t - ti);
					wk = table[ti] + f * (table[ti + 1] - table[ti]);
				}
				w[k] = wk;
				sum += wk;
			}
			const float norm = sum > 1e-6f ? 1.0f / sum : 1.0f;

			const float* src = buf + static_cast<size_t>(i0 + 1 - half) * 2;
			float accL = 0.0f, accR = 0.0f;
			for (int k = 0; k < taps; ++k)
			{
				accL += w[k] * src[2 * k];
				accR += w[k] * src[2 * k + 1];
			}
			accL *= norm;
			accR *= norm;
			if (outCh == 2)
			{
				out[2 * produced] = accL;
				out[2 * produced + 1] = accR;
			}
			else
			{
				out[produced] = 0.5f * (accL + accR);
			}
			m_pos += m_step;
			++produced;
		}
		return produced;
	}

	void Resampler::Flush()
	{
		if (m_cap == 0)
			return;
		Push(nullptr, std::min(HalfTaps(), InputSpace()));
	}

	Status Resampler::Process(const float* in, uint32_t inFrames, float* out, uint32_t outCapacity,
		uint32_t* consumed, uint32_t* produced)
	{
		if (consumed != nullptr)
			*consumed = 0;
		if (produced != nullptr)
			*produced = 0;
		if (m_cap == 0)
			return Status::InvalidState;
		if ((in == nullptr && inFrames > 0) || (out == nullptr && outCapacity > 0))
			return Status::InvalidArgument;

		uint32_t c = 0, p = 0;
		for (;;)
		{
			const uint32_t take = std::min(std::min(inFrames - c, InputSpace()), m_maxBlock);
			if (take > 0)
				c += Push(in + static_cast<size_t>(c) * m_srcChannels, take);
			const uint32_t got = p < outCapacity ? Pull(out + static_cast<size_t>(p) * m_dstChannels, outCapacity - p) : 0u;
			p += got;
			if (take == 0 && got == 0)
				break;
		}
		if (consumed != nullptr)
			*consumed = c;
		if (produced != nullptr)
			*produced = p;
		return Status::Ok;
	}
}
