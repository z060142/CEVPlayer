// Per frame statistics: 240 entry ring, avg / max / p99. Disabled = one branch per record call.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace cev
{
	struct StatSummary
	{
		float avg = 0.0f;
		float max = 0.0f;
		float p99 = 0.0f;
	};

	struct StatsSnapshot
	{
		StatSummary decodeMs;       // libvpx decode (+alpha) per frame, worker thread
		StatSummary convertMs;      // plane copy + YUV->RGBA per frame, worker thread
		StatSummary copyMs;         // measurement (S0): plane copy part of convertMs (WriteFrame)
		StatSummary colorMs;        // measurement (S0): YUV->RGBA part of convertMs (ConvertSlot)
		StatSummary ringOccupancy;  // ready frames in the ring when a frame was committed
		uint32_t samples = 0;       // valid entries in the window (<= Stats::kWindow)
		uint64_t framesDecoded = 0;
		uint64_t framesPresented = 0;
		uint64_t framesDropped = 0;
		uint64_t uploadBytes = 0;   // sum of NoteUpload() calls
		uint64_t allocs = 0;        // allocations seen by the alloc hook since the first record
	};

	class Stats
	{
	public:
		static constexpr uint32_t kWindow = 240;

		// Process wide hook returning a monotonically increasing allocation counter (e.g. the selftest's
		// operator new override). nullptr = no counting.
		using AllocCounterFn = uint64_t (*)();
		static void SetAllocCounter(AllocCounterFn fn);

		Stats() = default;

		void SetEnabled(bool enabled) { m_enabled.store(enabled, std::memory_order_relaxed); }
		bool IsEnabled() const { return m_enabled.load(std::memory_order_relaxed); }

		// Worker side: one entry per decoded frame.
		void RecordDecoded(float decodeMs, float convertMs, uint32_t ringOccupancy);
		// Same, with the plane copy part of convertMs timed separately (measurement, S0).
		void RecordDecoded(float decodeMs, float copyMs, float colorMs, uint32_t ringOccupancy);
		// Consumer side.
		void NotePresented(uint32_t count = 1);
		void NoteDropped(uint32_t count);
		void NoteUpload(uint64_t bytes);

		StatsSnapshot Snapshot() const;
		void Reset();

	private:
		struct Entry
		{
			float decodeMs = 0.0f;
			float convertMs = 0.0f;
			float copyMs = 0.0f;
			float colorMs = 0.0f;
			float ringOccupancy = 0.0f;
		};

		std::atomic<bool> m_enabled{false};
		mutable std::mutex m_mutex;
		Entry m_entries[kWindow];
		uint32_t m_head = 0;
		uint32_t m_count = 0;
		uint64_t m_framesDecoded = 0;
		uint64_t m_allocBase = 0;
		bool m_allocBaseSet = false;
		std::atomic<uint64_t> m_presented{0};
		std::atomic<uint64_t> m_dropped{0};
		std::atomic<uint64_t> m_uploadBytes{0};
	};
}
