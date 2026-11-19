#include "CEVCore/FrameRing.h"

#include <cstring>
#include <new>

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

		// Total memory is computed (in 64 bit) and checked against the budget before anything is allocated. The RGBA
		// buffers always count (they may be allocated later on demand).
		const bool packed = format.Packable();
		const int cw = format.ChromaWidth();
		const int ch = format.ChromaHeight();
		const int yStride = packed ? format.width : AlignUp(format.width, 16);
		const int cStride = packed ? format.width : AlignUp(cw, 16);
		const size_t ySize = static_cast<size_t>(yStride) * format.height;
		const size_t cSize = packed ? static_cast<size_t>(format.width) * ch : static_cast<size_t>(cStride) * ch; // packed: U and V side by side
		const size_t aSize = format.hasAlpha ? ySize : 0;
		const size_t total = packed ? ySize + cSize + aSize : ySize + 2 * cSize + aSize;
		const uint64_t rgbaBytes = static_cast<uint64_t>(format.width) * format.height * 4;
		if ((static_cast<uint64_t>(total) + rgbaBytes) * slotCount > kMaxRingBytes)
			return Status::OutOfMemory;

		try
		{
			m_format = format;
			m_packed = packed;
			m_rgbaCount = 0;
			m_slots = std::make_unique<SlotStorage[]>(slotCount);
			m_readyQueue = std::make_unique<int[]>(slotCount);

			for (uint32_t i = 0; i < slotCount; ++i)
			{
				SlotStorage& s = m_slots[i];
				s.yuv = std::make_unique<uint8_t[]>(total);
				uint8_t* const base = s.yuv.get();
				// Neutral grey / opaque so a never written slot is harmless.
				std::memset(base, 16, ySize);
				s.data.plane[0] = base;
				s.data.stride[0] = yStride;
				if (packed)
				{
					std::memset(base + ySize, 128, cSize);
					s.data.packed = base;
					s.data.plane[1] = base + ySize;
					s.data.plane[2] = base + ySize + cw;
					s.data.stride[1] = format.width;
					s.data.stride[2] = format.width;
				}
				else
				{
					std::memset(base + ySize, 128, 2 * cSize);
					s.data.plane[1] = base + ySize;
					s.data.stride[1] = cStride;
					s.data.plane[2] = base + ySize + cSize;
					s.data.stride[2] = cStride;
				}
				if (format.hasAlpha)
				{
					s.data.plane[3] = base + total - aSize;
					s.data.stride[3] = yStride;
					std::memset(s.data.plane[3], 255, aSize);
				}
				if (format.withRgba)
				{
					s.rgbaMem = std::make_unique<uint8_t[]>(static_cast<size_t>(rgbaBytes)); // zero initialised
					s.data.rgba = s.rgbaMem.get();
					s.data.rgbaStride = format.width * 4;
					++m_rgbaCount;
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

	PlaneRect FrameRing::PackedPlane(int plane) const
	{
		PlaneRect r;
		if (!m_packed)
			return r;
		const int w = m_format.width, h = m_format.height, cw = m_format.ChromaWidth(), ch = m_format.ChromaHeight();
		switch (plane)
		{
		case 0: r = {0, 0, w, h}; break;
		case 1: r = {0, h, cw, ch}; break;
		case 2: r = {cw, h, cw, ch}; break;
		case 3: if (m_format.hasAlpha) r = {0, h + ch, w, h}; break;
		default: break;
		}
		return r;
	}

	bool FrameRing::EnsureRgba(uint32_t slot)
	{
		if (slot >= m_slotCount)
			return false;
		SlotStorage& s = m_slots[slot];
		s.data.rgbaValid = false;
		if (s.rgbaMem)
			return true;
		std::unique_ptr<uint8_t[]> mem(new (std::nothrow) uint8_t[static_cast<size_t>(m_format.width) * m_format.height * 4]);
		if (!mem)
			return false;
		std::lock_guard<std::mutex> lock(m_mutex); // pointer swap visible to ReleaseFreeRgba / RgbaBufferCount
		s.rgbaMem = std::move(mem);
		s.data.rgba = s.rgbaMem.get();
		s.data.rgbaStride = m_format.width * 4;
		++m_rgbaCount;
		return true;
	}

	void FrameRing::ReleaseRgba(uint32_t slot)
	{
		if (slot >= m_slotCount)
			return;
		std::unique_ptr<uint8_t[]> old;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			SlotStorage& s = m_slots[slot];
			s.data.rgbaValid = false;
			if (!s.rgbaMem)
				return;
			old = std::move(s.rgbaMem);
			s.data.rgba = nullptr;
			s.data.rgbaStride = 0;
			--m_rgbaCount;
		}
	}

	void FrameRing::ReleaseFreeRgba()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (uint32_t i = 0; i < m_slotCount; ++i)
		{
			SlotStorage& s = m_slots[i];
			if (s.state != State::Free || !s.rgbaMem)
				continue;
			s.rgbaMem.reset();
			s.data.rgba = nullptr;
			s.data.rgbaStride = 0;
			s.data.rgbaValid = false;
			--m_rgbaCount;
		}
	}

	uint32_t FrameRing::RgbaBufferCount() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_rgbaCount;
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
		m_rgbaCount = 0;
		m_packed = false;
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
