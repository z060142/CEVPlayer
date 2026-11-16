// Fixed set of decode worker threads shared by all players. Workers sleep on a condition variable and only
// wake when a registered task has been Wake()d. One task never runs on two workers at once, and the pool
// never holds its lock while a task runs.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "CEVCore/Status.h"

namespace cev
{
	class IDecodeTask
	{
	public:
		virtual ~IDecodeTask() = default;
		// Does a bounded amount of work (about one frame). Returns true when more work is immediately
		// available (the pool then schedules the task again, round robin with the other tasks), false when the
		// task is idle until the next Wake().
		virtual bool RunStep() = 0;
	};

	class DecodePool
	{
	public:
		using TaskHandle = uint32_t;
		static constexpr TaskHandle kInvalidHandle = 0xFFFFFFFFu;

		// Process wide shared pool: created on first use and kept alive (idle workers just sleep) until
		// ShutdownShared(). Returns nullptr when no worker thread could be started (PlayerCore::Load then fails
		// with OutOfMemory). workerCount == 0 -> default (min(4, max(2, hardware_concurrency / 2))), only used
		// when it creates the pool.
		static std::shared_ptr<DecodePool> AcquireShared(uint32_t workerCount = 0);
		// Drops the process wide pool's own reference. The workers are joined as soon as the last PlayerCore
		// holding it is gone (immediately when none is). Call it at plugin / module shutdown, after the players
		// were destroyed or unloaded, so no worker thread outlives the module. A later AcquireShared() creates a
		// new pool.
		static void ShutdownShared();

		explicit DecodePool(uint32_t workerCount);
		~DecodePool();
		DecodePool(const DecodePool&) = delete;
		DecodePool& operator=(const DecodePool&) = delete;

		uint32_t WorkerCount() const { return static_cast<uint32_t>(m_threads.size()); }

		// Registration allocates; do it at load time, not during playback.
		Status Register(IDecodeTask* task, TaskHandle& handle);
		// Blocks until the task is not running any more, then removes it. After return no worker touches it.
		void Unregister(TaskHandle handle);
		// Marks the task as having work and wakes a worker. Cheap, any thread.
		void Wake(TaskHandle handle);

	private:
		struct Entry
		{
			IDecodeTask* task = nullptr;
			bool used = false;
			bool pending = false;
			bool running = false;
		};

		void WorkerMain();
		int FindRunnableLocked();

		std::mutex m_mutex;
		std::condition_variable m_workCv;
		std::condition_variable m_idleCv;
		std::vector<Entry> m_entries;
		std::vector<std::thread> m_threads;
		uint32_t m_nextScan = 0;
		bool m_stop = false;
	};
}
