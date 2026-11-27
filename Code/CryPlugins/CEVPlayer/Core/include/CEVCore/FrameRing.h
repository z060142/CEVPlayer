// Fixed slot frame ring. Producer = decode worker, consumer = main thread.
// All plane memory is allocated once in Init(); steady state never allocates. The RGBA buffers are on demand (M8):
// allocated / freed only when the output switches between RGBA and planar (EnsureRgba / ReleaseRgba).
//
// Packed layout (M8): for 4:2:0 with an even width every slot is ONE
// contiguous buffer, rows of exactly `width` bytes: Y (0, 0, w, h), U (0, h, cw, ch), V (cw, h, cw, ch) and, with
// alpha, A (0, h + ch, w, h). The plane pointers / strides point into it, so the converter and WriteFrame work on
// both layouts. Other subsamplings (4:2:2, 4:4:4, 4:4:0) and odd widths keep separate 16 byte aligned planes.
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
		//! The packed layout applies: 4:2:0 and an even width.
		bool Packable() const { return chromaShiftX == 1 && chromaShiftY == 1 && (width & 1) == 0; }
		//! Rows of the packed buffer: h + ch (+ h with alpha). Its width is `width`.
		int PackedHeight() const { return height + ChromaHeight() + (hasAlpha ? height : 0); }
	};

	//! A rectangle in the packed buffer (texels = bytes; rows of FrameFormat::width bytes).
	struct PlaneRect
	{
		int x = 0;
		int y = 0;
		int w = 0;
		int h = 0;
	};

	class FrameRing
	{
	public:
		struct Slot
		{
			uint8_t* plane[4] = {nullptr, nullptr, nullptr, nullptr}; // Y, U, V, A (A only when hasAlpha)
			int stride[4] = {0, 0, 0, 0};
			uint8_t* packed = nullptr; // the whole packed buffer (= plane[0]) when the ring is packed, else null
			uint8_t* rgba = nullptr;  // width*height*4, premultiplied; null while no RGBA buffer is allocated
			int rgbaStride = 0;
			bool rgbaValid = false;   // rgba holds the conversion of THIS frame (set by whoever wrote the slot)
			double pts = 0.0;         // seconds
		};

		FrameRing() = default;
		~FrameRing() = default;
		FrameRing(const FrameRing&) = delete;
		FrameRing& operator=(const FrameRing&) = delete;

		// Allocates `slotCount` (>= 2) slots (RGBA buffers too when format.withRgba). Must not be called while other
		// threads use the ring. The memory budget always counts the RGBA buffers, so EnsureRgba never exceeds it.
		Status Init(const FrameFormat& format, uint32_t slotCount);
		void Shutdown();
		bool IsInitialized() const { return m_slotCount != 0; }

		const FrameFormat& Format() const { return m_format; }
		uint32_t SlotCount() const { return m_slotCount; }
		//! Packed layout in use (FrameFormat::Packable() at Init).
		bool IsPacked() const { return m_packed; }
		//! Packed buffer geometry (0 when not packed): width x height bytes, and the plane rectangles (0 = Y, 1 = U,
		//! 2 = V, 3 = A; A is empty without alpha).
		int PackedWidth() const { return m_packed ? m_format.width : 0; }
		int PackedHeight() const { return m_packed ? m_format.PackedHeight() : 0; }
		PlaneRect PackedPlane(int plane) const;

		// ---- RGBA buffers on demand (M8) ----
		// Only for a slot the caller owns: Writing (producer) or Held (consumer). EnsureRgba allocates the slot's
		// buffer when it has none (false: out of memory); ReleaseRgba frees it. Both clear rgbaValid.
		bool EnsureRgba(uint32_t slot);
		void ReleaseRgba(uint32_t slot);
		// Frees the RGBA buffers of all Free slots (any thread; the ring lock keeps them Free meanwhile).
		void ReleaseFreeRgba();
		// Slots that currently own an RGBA buffer (diagnostics / tests).
		uint32_t RgbaBufferCount() const;
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
		bool m_packed = false;
		uint32_t m_rgbaCount = 0; // slots owning an RGBA buffer (under m_mutex)
	};
}
