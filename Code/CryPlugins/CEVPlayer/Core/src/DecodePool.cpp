#include "CEVCore/DecodePool.h"

#include <algorithm>
#include <new>

namespace cev
{
	namespace
	{
		std::mutex g_sharedMutex;
		// The process wide pool lives until ShutdownShared(). The holder sits in static storage and is intentionally
		// never destroyed: a static destructor would join the worker threads during process / DLL teardown (loader
		// lock). Once ShutdownShared() ran it is an empty shared_ptr, i.e. nothing is left allocated.
		alignas(std::shared_ptr<DecodePool>) unsigned char g_sharedStorage[sizeof(std::shared_ptr<DecodePool>)];
		std::shared_ptr<DecodePool>* g_shared = nullptr;
	}

	std::shared_ptr<DecodePool> DecodePool::AcquireShared(uint32_t workerCount)
	{
		try
		{
			std::lock_guard<std::mutex> lock(g_sharedMutex);
			if (g_shared != nullptr && *g_shared)
				return *g_shared;
			if (workerCount == 0)
			{
				const uint32_t hw = std::max(1u, std::thread::hardware_concurrency());
				workerCount = std::min(4u, std::max(2u, hw / 2));
			}
			std::shared_ptr<DecodePool> pool = std::make_shared<DecodePool>(workerCount);
			if (pool->WorkerCount() == 0)
				return nullptr; // could not start a single thread; not cached, the next call retries
			if (g_shared == nullptr)
				g_shared = new (g_sharedStorage) std::shared_ptr<DecodePool>();
			*g_shared = pool;
			return pool;
		}
		catch (...)
		{
			return nullptr;
		}
	}

	void DecodePool::ShutdownShared()
	{
		std::shared_ptr<DecodePool> victim;
		{
			std::lock_guard<std::mutex> lock(g_sharedMutex);
			if (g_shared != nullptr)
				victim = std::move(*g_shared);
		}
		// `victim` dies here, outside the lock: joins the workers when no player still holds the pool.
	}

	DecodePool::DecodePool(uint32_t workerCount)
	{
		workerCount = std::max(1u, std::min(workerCount, 32u));
		try
		{
			m_entries.reserve(64);
			m_threads.reserve(workerCount);
			for (uint32_t i = 0; i < workerCount; ++i)
				m_threads.emplace_back([this] { WorkerMain(); });
		}
		catch (...)
		{
			// Out of threads / memory: keep the workers that did start (possibly none, WorkerCount() == 0 and
			// Register() then fails with OutOfMemory). Never let std::system_error escape with joinable threads.
		}
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
		if (m_threads.empty())
			return Status::OutOfMemory; // no worker could be started
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
		try
		{
			m_entries.push_back(e);
		}
		catch (...)
		{
			return Status::OutOfMemory;
		}
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
			bool more = false;
			try
			{
				more = task->RunStep();
			}
			catch (...)
			{
				// A throwing task (bad_alloc, system_error) must not take the process down: it is parked until
				// its next Wake(). PlayerCore's own RunStep wrapper already turns this into a failed player.
				more = false;
			}
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
