// Fixed slot frame ring. Producer = decode worker, consumer = main thread.
// All memory is allocated once in Init(); steady state never allocates.
//
// A slot is Free -> Writing (producer) -> Ready (queued in pts order) -> Held (consumer's current frame) -> Free.
// The ring lock is only held for index bookkeeping, never while decoding or converting.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

#include "CEVCore/Status.h"

namespace cev
{
	struct FrameFormat
	{
		int width = 0;
		int height = 0;
		int chromaShiftX = 1;      // 1 = 4:2:0 / 4:2:2, 0 = 4:4:4 / 4:4:0
		int chromaShiftY = 1;      // 1 = 4:2:0 / 4:4:0, 0 = 4:2:2 / 4:4:4
		bool hasAlpha = false;     // allocate the A plane
		bool withRgba = true;      // allocate the premultiplied RGBA buffer

		int ChromaWidth() const { return (width + (1 << chromaShiftX) - 1) >> chromaShiftX; }
		int ChromaHeight() const { return (height + (1 << chromaShiftY) - 1) >> chromaShiftY; }
	};

	class FrameRing
	{
	public:
		struct Slot
		{
			uint8_t* plane[4] = {nullptr, nullptr, nullptr, nullptr}; // Y, U, V, A (A only when hasAlpha)
			int stride[4] = {0, 0, 0, 0};
			uint8_t* rgba = nullptr;  // width*height*4, premultiplied, only when withRgba
			int rgbaStride = 0;
			double pts = 0.0;         // seconds
		};

		FrameRing() = default;
		~FrameRing() = default;
		FrameRing(const FrameRing&) = delete;
		FrameRing& operator=(const FrameRing&) = delete;

		// Allocates `slotCount` (>= 2) slots. Must not be called while other threads use the ring.
		Status Init(const FrameFormat& format, uint32_t slotCount);
		void Shutdown();
		bool IsInitialized() const { return m_slotCount != 0; }

		const FrameFormat& Format() const { return m_format; }
		uint32_t SlotCount() const { return m_slotCount; }
		// Slot memory pointers are constant after Init(); pts is only valid for slots you own.
		Slot& GetSlot(uint32_t index) { return m_slots[index].data; }
		const Slot& GetSlot(uint32_t index) const { return m_slots[index].data; }

		// ---- producer ----
		// Returns a free slot index (>= 0) and the current epoch, or -1 when the ring is full.
		int AcquireWrite(uint64_t& epoch);
		// Queues the slot as the newest ready frame. Returns false (and frees the slot) if the epoch is stale
		// (a flush happened since AcquireWrite).
		bool Commit(int slot, uint64_t epoch, double pts);
		void Abandon(int slot);

		// ---- consumer ----
		// Picks the frame to present at `time`: the newest ready frame with pts <= time + epsilon. Ready frames
		// that are overtaken (another due frame follows) are dropped and counted in `dropped`.
		// Returns the new current slot, or -1 if the current frame stays. The previous current slot is released.
		int PopForTime(double time, double epsilon, uint32_t& dropped);
		int Current() const;
		uint32_t ReadyCount() const;
		uint32_t FreeCount() const;
		// Occupancy for stats: ready + writing slots.
		uint32_t InFlightCount() const;
		// Discards all ready frames and invalidates slots currently being written (epoch++).
		void FlushReady();
		// Releases the consumer's current frame (e.g. on Unload).
		void ReleaseCurrent();

	private:
		enum class State : uint8_t { Free, Writing, Ready, Held };

		struct SlotStorage
		{
			Slot data;
			State state = State::Free;
			std::unique_ptr<uint8_t[]> yuv;
			std::unique_ptr<uint8_t[]> rgbaMem;
		};

		void FreeSlotLocked(int slot) { m_slots[slot].state = State::Free; }

		mutable std::mutex m_mutex;
		FrameFormat m_format;
		std::unique_ptr<SlotStorage[]> m_slots;
		std::unique_ptr<int[]> m_readyQueue; // circular, slotCount entries
		uint32_t m_slotCount = 0;
		uint32_t m_readyHead = 0;
		uint32_t m_readyCount = 0;
		int m_current = -1;
		uint64_t m_epoch = 1;
	};
}
