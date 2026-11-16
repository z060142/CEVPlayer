#include "CEVCore/AudioClock.h"

#include <algorithm>
#include <chrono>

namespace cev
{
	AudioClock::AudioClock(uint32_t outputSampleRate)
	{
		m_outRate.store(std::max(1u, outputSampleRate), std::memory_order_relaxed);
	}

	int64_t AudioClock::NowNs() const
	{
		if (m_timeFn != nullptr)
			return m_timeFn(m_timeUser);
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	void AudioClock::SetTimeSource(TimeSourceFn fn, void* user)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_timeFn = fn;
		m_timeUser = user;
	}

	void AudioClock::OnFramesConsumed(uint32_t frames) { OnFramesConsumed(frames, frames); }

	void AudioClock::OnFramesConsumed(uint32_t frames, uint32_t audibleFrames)
	{
		audibleFrames = std::min(audibleFrames, frames);
		// Stamps first, counters second: a reader that sees the new stamp with the old counter only
		// underestimates the elapsed time (the consumer clamps to monotonic), never overestimates.
		int64_t now = NowNs();
		if (now == 0)
			now = 1; // 0 means "never called"
		const bool countSilence = m_countSilence.load(std::memory_order_relaxed);
		if (audibleFrames > 0 && countSilence)
		{
			// Audio is back after a timed out wait: underruns hold the clock again.
			m_countSilence.store(false, std::memory_order_relaxed);
		}
		const uint32_t counted = (countSilence || m_audioEnded.load(std::memory_order_relaxed)) ? frames : audibleFrames;
		m_stampNs.store(now, std::memory_order_release);
		m_interpNs.store(counted == frames && frames > 0 ? now : 0, std::memory_order_release);
		if (counted > 0)
			m_countedNs.store(now, std::memory_order_release);
		m_media.fetch_add(counted, std::memory_order_acq_rel);
		m_consumed.fetch_add(frames, std::memory_order_acq_rel);
	}

	void AudioClock::SetOutputLatencyFrames(uint32_t frames) { m_latencyFrames.store(frames, std::memory_order_relaxed); }

	void AudioClock::SetOutputSampleRate(uint32_t sampleRate)
	{
		m_outRate.store(std::max(1u, sampleRate), std::memory_order_relaxed);
	}

	void AudioClock::ReadCallbackStamp(uint64_t& frames, int64_t& stampNs, int64_t& interpNs) const
	{
		for (int attempt = 0; attempt < 4; ++attempt)
		{
			const uint64_t c1 = m_media.load(std::memory_order_acquire);
			stampNs = m_stampNs.load(std::memory_order_acquire);
			interpNs = m_interpNs.load(std::memory_order_acquire);
			const uint64_t c2 = m_media.load(std::memory_order_acquire);
			frames = c1;
			if (c1 == c2)
				return;
		}
	}

	bool AudioClock::IsStalled() const
	{
		const int64_t stamp = m_stampNs.load(std::memory_order_acquire);
		if (stamp == 0)
			return true;
		return static_cast<double>(NowNs() - stamp) * 1e-9 > kStallSeconds;
	}

	bool AudioClock::CountsSilence() const
	{
		return m_countSilence.load(std::memory_order_relaxed) || m_audioEnded.load(std::memory_order_relaxed);
	}

	void AudioClock::RebaseLocked(double media, bool newAudioEpoch)
	{
		const int64_t now = NowNs();
		if (newAudioEpoch)
		{
			m_audioEnded.store(false, std::memory_order_relaxed);
			m_countSilence.store(false, std::memory_order_relaxed);
		}
		m_baseMedia = media;
		m_baseFrames = static_cast<int64_t>(m_media.load(std::memory_order_acquire));
		m_baseNs = now;
		m_holdSinceNs = now;
		m_lastMedia = media;
		m_lastNowNs = now;
		m_stalled = false;
	}

	double AudioClock::ComputeLocked()
	{
		const int64_t nowNs = NowNs();
		uint64_t frames = 0;
		int64_t stampNs = 0;
		int64_t interpNs = 0;
		ReadCallbackStamp(frames, stampNs, interpNs);

		const double wallSinceLast = static_cast<double>(std::max<int64_t>(0, nowNs - m_lastNowNs)) * 1e-9;
		const double tWall = m_lastMedia + wallSinceLast * m_rate;
		const double dt = stampNs != 0 ? static_cast<double>(std::max<int64_t>(0, nowNs - stampNs)) * 1e-9 : 1e9;
		const double latency = static_cast<double>(m_latencyFrames.load(std::memory_order_relaxed));

		double result = 0.0;
		if (dt > kStallSeconds)
		{
			// The device is not delivering: continue on the wall clock from the last value.
			m_stalled = true;
			result = tWall;
		}
		else if (!m_audioEnded.load(std::memory_order_relaxed) && !m_countSilence.load(std::memory_order_relaxed) &&
			static_cast<double>(nowNs - std::max(m_holdSinceNs, m_countedNs.load(std::memory_order_acquire))) * 1e-9 > kStallSeconds)
		{
			// The callbacks run but no audible audio arrived for kStallSeconds (no audio, decoder stalled): stop
			// holding. Silence counts from now on; the clock continues from the held value on the wall clock.
			m_countSilence.store(true, std::memory_order_relaxed);
			m_baseMedia = tWall;
			m_baseFrames = static_cast<int64_t>(frames) - static_cast<int64_t>(latency);
			m_stalled = false;
			result = tWall;
		}
		else
		{
			const double consumed = static_cast<double>(static_cast<int64_t>(frames) - m_baseFrames);
			const double rate = static_cast<double>(m_outRate.load(std::memory_order_relaxed));
			const double wall = interpNs != 0 ? static_cast<double>(std::max<int64_t>(0, nowNs - interpNs)) * 1e-9 : 0.0;
			const double heard = std::max(0.0, (consumed - latency) / rate + wall);
			double audio = m_baseMedia + heard * m_rate;
			if (m_stalled)
			{
				// Callbacks are back after a stall: shift the base so that the clock continues where the wall
				// clock fallback left it instead of jumping back.
				m_baseMedia += tWall - audio;
				audio = tWall;
				m_stalled = false;
			}
			result = std::max(audio, m_lastMedia);
		}
		m_lastMedia = result;
		m_lastNowNs = nowNs;
		return result;
	}

	double AudioClock::NowSeconds()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_running ? ComputeLocked() : m_baseMedia;
	}

	void AudioClock::Start()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		RebaseLocked(0.0, true);
		m_running = true;
	}

	void AudioClock::Pause()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_running)
		{
			m_baseMedia = ComputeLocked();
			m_running = false;
		}
	}

	void AudioClock::Resume()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_running)
		{
			RebaseLocked(m_baseMedia, false);
			m_running = true;
		}
	}

	void AudioClock::Seek(double seconds)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		RebaseLocked(seconds, true);
	}

	void AudioClock::SetRate(double rate)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!(rate > 0.0))
			return;
		const double media = m_running ? ComputeLocked() : m_baseMedia;
		m_rate = rate;
		RebaseLocked(media, false);
	}

	void AudioClock::NotifyAudioEnded()
	{
		m_audioEnded.store(true, std::memory_order_relaxed);
	}

	double AudioClock::Rate() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_rate;
	}

	bool AudioClock::IsRunning() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_running;
	}
}
