#include "CEVCore/FrameRing.h"

#include <cstring>

namespace cev
{
	namespace
	{
		inline int AlignUp(int v, int a) { return (v + a - 1) / a * a; }

		constexpr int kMaxWidth = 8192;
		constexpr int kMaxHeight = 8192;
		constexpr int64_t kMaxPixels = 8192ll * 4608ll;            // 8K UHD
		constexpr uint64_t kMaxRingBytes = 1ull << 30;             // all slots together
	}

	Status FrameRing::Init(const FrameFormat& format, uint32_t slotCount)
	{
		Shutdown();
		if (format.width <= 0 || format.height <= 0 || slotCount < 2 || slotCount > 64)
			return Status::InvalidArgument;
		if (format.chromaShiftX < 0 || format.chromaShiftX > 2 || format.chromaShiftY < 0 || format.chromaShiftY > 2)
			return Status::InvalidArgument;
		if (format.width > kMaxWidth || format.height > kMaxHeight ||
			static_cast<int64_t>(format.width) * format.height > kMaxPixels) // 8192 x 4608
			return Status::Unsupported;

		// Total memory is computed (in 64 bit) and checked against the budget before anything is allocated.
		const int yStride = AlignUp(format.width, 16);
		const int cStride = AlignUp(format.ChromaWidth(), 16);
		const size_t ySize = static_cast<size_t>(yStride) * format.height;
		const size_t cSize = static_cast<size_t>(cStride) * format.ChromaHeight();
		const size_t aSize = format.hasAlpha ? ySize : 0;
		const size_t total = ySize + 2 * cSize + aSize;
		const uint64_t rgbaBytes = format.withRgba ? static_cast<uint64_t>(format.width) * format.height * 4 : 0;
		if ((static_cast<uint64_t>(total) + rgbaBytes) * slotCount > kMaxRingBytes)
			return Status::OutOfMemory;

		try
		{
			m_format = format;
			m_slots = std::make_unique<SlotStorage[]>(slotCount);
			m_readyQueue = std::make_unique<int[]>(slotCount);

			for (uint32_t i = 0; i < slotCount; ++i)
			{
				SlotStorage& s = m_slots[i];
				s.yuv = std::make_unique<uint8_t[]>(total);
				// Neutral grey / opaque so a never written slot is harmless.
				std::memset(s.yuv.get(), 16, ySize);
				std::memset(s.yuv.get() + ySize, 128, 2 * cSize);
				s.data.plane[0] = s.yuv.get();
				s.data.stride[0] = yStride;
				s.data.plane[1] = s.yuv.get() + ySize;
				s.data.stride[1] = cStride;
				s.data.plane[2] = s.yuv.get() + ySize + cSize;
				s.data.stride[2] = cStride;
				if (format.hasAlpha)
				{
					s.data.plane[3] = s.yuv.get() + ySize + 2 * cSize;
					s.data.stride[3] = yStride;
					std::memset(s.data.plane[3], 255, aSize);
				}
				if (format.withRgba)
				{
					const size_t rgbaSize = static_cast<size_t>(format.width) * format.height * 4;
					s.rgbaMem = std::make_unique<uint8_t[]>(rgbaSize); // zero initialised
					s.data.rgba = s.rgbaMem.get();
					s.data.rgbaStride = format.width * 4;
				}
				s.state = State::Free;
			}

			m_slotCount = slotCount;
			m_readyHead = 0;
			m_readyCount = 0;
			m_current = -1;
			return Status::Ok;
		}
		catch (...)
		{
			Shutdown(); // frees whatever was allocated so far
			return Status::OutOfMemory;
		}
	}

	void FrameRing::Shutdown()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_slots.reset();
		m_readyQueue.reset();
		m_slotCount = 0;
		m_readyHead = 0;
		m_readyCount = 0;
		m_current = -1;
		++m_epoch;
	}

	int FrameRing::AcquireWrite(uint64_t& epoch)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (uint32_t i = 0; i < m_slotCount; ++i)
		{
			if (m_slots[i].state == State::Free)
			{
				m_slots[i].state = State::Writing;
				epoch = m_epoch;
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	bool FrameRing::Commit(int slot, uint64_t epoch, double pts)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (slot < 0 || static_cast<uint32_t>(slot) >= m_slotCount || m_slots[slot].state != State::Writing)
			return false;
		if (epoch != m_epoch)
		{
			FreeSlotLocked(slot);
			return false;
		}
		m_slots[slot].data.pts = pts;
		m_slots[slot].state = State::Ready;
		m_readyQueue[(m_readyHead + m_readyCount) % m_slotCount] = slot;
		++m_readyCount;
		return true;
	}

	void FrameRing::Abandon(int slot)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (slot >= 0 && static_cast<uint32_t>(slot) < m_slotCount && m_slots[slot].state == State::Writing)
			FreeSlotLocked(slot);
	}

	int FrameRing::PopForTime(double time, double epsilon, uint32_t& dropped)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		const double limit = time + epsilon;
		while (m_readyCount > 0)
		{
			const int front = m_readyQueue[m_readyHead];
			if (m_slots[front].data.pts > limit)
				return -1;

			const bool overtaken = m_readyCount >= 2 &&
				m_slots[m_readyQueue[(m_readyHead + 1) % m_slotCount]].data.pts <= limit;

			m_readyHead = (m_readyHead + 1) % m_slotCount;
			--m_readyCount;

			if (overtaken)
			{
				FreeSlotLocked(front);
				++dropped;
				continue;
			}

			if (m_current >= 0)
				FreeSlotLocked(m_current);
			m_slots[front].state = State::Held;
			m_current = front;
			return front;
		}
		return -1;
	}

	int FrameRing::Current() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_current;
	}

	uint32_t FrameRing::ReadyCount() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_readyCount;
	}

	uint32_t FrameRing::FreeCount() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		uint32_t n = 0;
		for (uint32_t i = 0; i < m_slotCount; ++i)
			n += m_slots[i].state == State::Free ? 1u : 0u;
		return n;
	}

	uint32_t FrameRing::InFlightCount() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		uint32_t n = 0;
		for (uint32_t i = 0; i < m_slotCount; ++i)
			n += (m_slots[i].state == State::Ready || m_slots[i].state == State::Writing) ? 1u : 0u;
		return n;
	}

	void FrameRing::FlushReady()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		while (m_readyCount > 0)
		{
			FreeSlotLocked(m_readyQueue[m_readyHead]);
			m_readyHead = (m_readyHead + 1) % m_slotCount;
			--m_readyCount;
		}
		++m_epoch;
	}

	void FrameRing::ReleaseCurrent()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_current >= 0)
		{
			FreeSlotLocked(m_current);
			m_current = -1;
		}
	}
}
