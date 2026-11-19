#include "CEVCore/Stats.h"

#include <algorithm>

namespace cev
{
	namespace
	{
		std::atomic<Stats::AllocCounterFn> g_allocCounter{nullptr};

		StatSummary SummarizeValues(float* values, uint32_t count)
		{
			StatSummary s;
			if (count == 0)
				return s;
			double sum = 0.0;
			float mx = values[0];
			for (uint32_t i = 0; i < count; ++i)
			{
				sum += values[i];
				mx = std::max(mx, values[i]);
			}
			s.avg = static_cast<float>(sum / count);
			s.max = mx;
			// p99 by nth_element on the caller's scratch copy (no allocation).
			uint32_t k = static_cast<uint32_t>((count * 99u + 99u) / 100u); // ceil(0.99 * count)
			k = k == 0 ? 0 : k - 1;
			if (k >= count)
				k = count - 1;
			std::nth_element(values, values + k, values + count);
			s.p99 = values[k];
			return s;
		}
	}

	void Stats::SetAllocCounter(AllocCounterFn fn)
	{
		g_allocCounter.store(fn, std::memory_order_release);
	}

	void Stats::RecordDecoded(float decodeMs, float convertMs, uint32_t ringOccupancy)
	{
		RecordDecoded(decodeMs, 0.0f, convertMs, ringOccupancy);
	}

	void Stats::RecordDecoded(float decodeMs, float copyMs, float colorMs, uint32_t ringOccupancy)
	{
		const float convertMs = copyMs + colorMs;
		if (!m_enabled.load(std::memory_order_relaxed))
			return;
		const AllocCounterFn fn = g_allocCounter.load(std::memory_order_acquire);
		std::lock_guard<std::mutex> lock(m_mutex);
		if (fn != nullptr && !m_allocBaseSet)
		{
			m_allocBase = fn();
			m_allocBaseSet = true;
		}
		Entry& e = m_entries[m_head];
		e.decodeMs = decodeMs;
		e.convertMs = convertMs;
		e.copyMs = copyMs;
		e.colorMs = colorMs;
		e.ringOccupancy = static_cast<float>(ringOccupancy);
		m_head = (m_head + 1) % kWindow;
		if (m_count < kWindow)
			++m_count;
		++m_framesDecoded;
	}

	void Stats::NotePresented(uint32_t count)
	{
		if (m_enabled.load(std::memory_order_relaxed))
			m_presented.fetch_add(count, std::memory_order_relaxed);
	}

	void Stats::NoteDropped(uint32_t count)
	{
		if (m_enabled.load(std::memory_order_relaxed))
			m_dropped.fetch_add(count, std::memory_order_relaxed);
	}

	void Stats::NoteUpload(uint64_t bytes)
	{
		if (m_enabled.load(std::memory_order_relaxed))
			m_uploadBytes.fetch_add(bytes, std::memory_order_relaxed);
	}

	StatsSnapshot Stats::Snapshot() const
	{
		StatsSnapshot out;
		float scratch[kWindow];
		Entry copy[kWindow];
		uint32_t count;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			count = m_count;
			for (uint32_t i = 0; i < count; ++i)
				copy[i] = m_entries[i]; // order irrelevant for avg / max / percentile
			out.framesDecoded = m_framesDecoded;
			const AllocCounterFn fn = g_allocCounter.load(std::memory_order_acquire);
			if (fn != nullptr && m_allocBaseSet)
				out.allocs = fn() - m_allocBase;
		}
		out.samples = count;
		for (uint32_t i = 0; i < count; ++i) scratch[i] = copy[i].decodeMs;
		out.decodeMs = SummarizeValues(scratch, count);
		for (uint32_t i = 0; i < count; ++i) scratch[i] = copy[i].convertMs;
		out.convertMs = SummarizeValues(scratch, count);
		for (uint32_t i = 0; i < count; ++i) scratch[i] = copy[i].copyMs;
		out.copyMs = SummarizeValues(scratch, count);
		for (uint32_t i = 0; i < count; ++i) scratch[i] = copy[i].colorMs;
		out.colorMs = SummarizeValues(scratch, count);
		for (uint32_t i = 0; i < count; ++i) scratch[i] = copy[i].ringOccupancy;
		out.ringOccupancy = SummarizeValues(scratch, count);
		out.framesPresented = m_presented.load(std::memory_order_relaxed);
		out.framesDropped = m_dropped.load(std::memory_order_relaxed);
		out.uploadBytes = m_uploadBytes.load(std::memory_order_relaxed);
		return out;
	}

	void Stats::Reset()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_head = 0;
		m_count = 0;
		m_framesDecoded = 0;
		m_allocBaseSet = false;
		m_presented.store(0, std::memory_order_relaxed);
		m_dropped.store(0, std::memory_order_relaxed);
		m_uploadBytes.store(0, std::memory_order_relaxed);
	}
}
