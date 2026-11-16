// Internal: preallocated interleaved float PCM ring. Producer = decode worker, consumer = audio thread.
// A short mutex protects the indices (copy sizes are small), which keeps Clear() on seek trivially correct.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace cev
{
	class AudioRing
	{
	public:
		void Init(uint32_t channels, uint32_t capacityFrames)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_channels = std::max(1u, channels);
			m_capacity = std::max(1u, capacityFrames);
			m_data.assign(static_cast<size_t>(m_capacity) * m_channels, 0.0f);
			m_head = 0;
			m_count = 0;
		}

		void Shutdown()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			std::vector<float>().swap(m_data);
			m_capacity = 0;
			m_head = 0;
			m_count = 0;
		}

		void Clear()
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_head = 0;
			m_count = 0;
		}

		uint32_t Capacity() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_capacity;
		}

		uint32_t Buffered() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_count;
		}

		uint32_t FreeFrames() const
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			return m_capacity - m_count;
		}

		// Writes min(frames, free) frames (src == nullptr writes silence). Returns frames written.
		uint32_t Write(const float* src, uint32_t frames)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			frames = std::min(frames, m_capacity - m_count);
			uint32_t tail = (m_head + m_count) % m_capacity;
			uint32_t left = frames;
			while (left > 0)
			{
				const uint32_t run = std::min(left, m_capacity - tail);
				float* dst = m_data.data() + static_cast<size_t>(tail) * m_channels;
				if (src != nullptr)
				{
					std::memcpy(dst, src, static_cast<size_t>(run) * m_channels * sizeof(float));
					src += static_cast<size_t>(run) * m_channels;
				}
				else
				{
					std::memset(dst, 0, static_cast<size_t>(run) * m_channels * sizeof(float));
				}
				tail = (tail + run) % m_capacity;
				left -= run;
			}
			m_count += frames;
			return frames;
		}

		// Reads min(frames, buffered) frames. Returns frames read.
		uint32_t Read(float* dst, uint32_t frames)
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			frames = std::min(frames, m_count);
			uint32_t left = frames;
			while (left > 0)
			{
				const uint32_t run = std::min(left, m_capacity - m_head);
				std::memcpy(dst, m_data.data() + static_cast<size_t>(m_head) * m_channels,
					static_cast<size_t>(run) * m_channels * sizeof(float));
				dst += static_cast<size_t>(run) * m_channels;
				m_head = (m_head + run) % m_capacity;
				left -= run;
			}
			m_count -= frames;
			return frames;
		}

	private:
		mutable std::mutex m_mutex;
		std::vector<float> m_data;
		uint32_t m_channels = 1;
		uint32_t m_capacity = 0;
		uint32_t m_head = 0;
		uint32_t m_count = 0;
	};
}
