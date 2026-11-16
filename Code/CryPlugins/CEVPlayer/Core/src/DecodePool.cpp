#include "CEVCore/DecodePool.h"

#include <algorithm>

namespace cev
{
	namespace
	{
		std::mutex g_sharedMutex;
		std::weak_ptr<DecodePool> g_shared;

		// Destroys the pool and drops the weak reference's control block as soon as the last player is gone
		// (a plain make_shared block would stay allocated until the next AcquireShared()).
		struct SharedPoolDeleter
		{
			void operator()(DecodePool* pool) const
			{
				std::default_delete<DecodePool>()(pool);
				std::lock_guard<std::mutex> lock(g_sharedMutex);
				if (g_shared.expired()) // not replaced by a newer pool meanwhile
					g_shared.reset();
			}
		};
	}

	std::shared_ptr<DecodePool> DecodePool::AcquireShared(uint32_t workerCount)
	{
		std::lock_guard<std::mutex> lock(g_sharedMutex);
		std::shared_ptr<DecodePool> pool = g_shared.lock();
		if (!pool)
		{
			if (workerCount == 0)
			{
				const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
				workerCount = std::min(4u, std::max(2u, hw / 2));
			}
			std::unique_ptr<DecodePool> owned = std::make_unique<DecodePool>(workerCount);
			pool = std::shared_ptr<DecodePool>(std::unique_ptr<DecodePool, SharedPoolDeleter>(owned.release()));
			g_shared = pool;
		}
		return pool;
	}

	DecodePool::DecodePool(uint32_t workerCount)
	{
		workerCount = std::max(1u, std::min(workerCount, 32u));
		m_entries.reserve(64);
		m_threads.reserve(workerCount);
		for (uint32_t i = 0; i < workerCount; ++i)
			m_threads.emplace_back([this] { WorkerMain(); });
	}

	DecodePool::~DecodePool()
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			m_stop = true;
		}
		m_workCv.notify_all();
		for (std::thread& t : m_threads)
			t.join();
	}

	Status DecodePool::Register(IDecodeTask* task, TaskHandle& handle)
	{
		handle = kInvalidHandle;
		if (task == nullptr)
			return Status::InvalidArgument;
		std::lock_guard<std::mutex> lock(m_mutex);
		for (uint32_t i = 0; i < m_entries.size(); ++i)
		{
			if (!m_entries[i].used)
			{
				m_entries[i] = Entry();
				m_entries[i].task = task;
				m_entries[i].used = true;
				handle = i;
				return Status::Ok;
			}
		}
		Entry e;
		e.task = task;
		e.used = true;
		m_entries.push_back(e);
		handle = static_cast<TaskHandle>(m_entries.size() - 1);
		return Status::Ok;
	}

	void DecodePool::Unregister(TaskHandle handle)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		if (handle >= m_entries.size() || !m_entries[handle].used)
			return;
		m_entries[handle].pending = false;
		m_idleCv.wait(lock, [&] { return !m_entries[handle].running; });
		m_entries[handle] = Entry();
	}

	void DecodePool::Wake(TaskHandle handle)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			if (handle >= m_entries.size() || !m_entries[handle].used)
				return;
			m_entries[handle].pending = true;
		}
		m_workCv.notify_one();
	}

	int DecodePool::FindRunnableLocked()
	{
		const uint32_t n = static_cast<uint32_t>(m_entries.size());
		for (uint32_t k = 0; k < n; ++k)
		{
			const uint32_t i = (m_nextScan + k) % n;
			const Entry& e = m_entries[i];
			if (e.used && e.pending && !e.running)
				return static_cast<int>(i);
		}
		return -1;
	}

	void DecodePool::WorkerMain()
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		for (;;)
		{
			int idx = -1;
			m_workCv.wait(lock, [&] { idx = FindRunnableLocked(); return m_stop || idx >= 0; });
			if (m_stop)
				return;

			Entry& e = m_entries[static_cast<size_t>(idx)];
			e.pending = false;
			e.running = true;
			IDecodeTask* task = e.task;
			m_nextScan = static_cast<uint32_t>(idx) + 1;

			lock.unlock();
			const bool more = task->RunStep();
			lock.lock();

			// m_entries never shrinks, and an entry is only cleared by Unregister() after running == false.
			Entry& done = m_entries[static_cast<size_t>(idx)];
			done.running = false;
			if (more)
				done.pending = true;
			m_idleCv.notify_all();
			if (more || done.pending)
				m_workCv.notify_one(); // let an idle worker pick up a task that became runnable meanwhile
		}
	}
}
