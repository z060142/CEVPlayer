#include "CEVCore/IClock.h"

namespace cev
{
	double InternalClock::NowLocked() const
	{
		if (!m_running)
			return m_baseMedia;
		const std::chrono::duration<double> elapsed = SteadyClock::now() - m_baseReal;
		return m_baseMedia + elapsed.count() * m_rate;
	}

	double InternalClock::NowSeconds()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return NowLocked();
	}

	void InternalClock::Start()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_baseMedia = 0.0;
		m_baseReal = SteadyClock::now();
		m_running = true;
	}

	void InternalClock::Pause()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_running)
		{
			m_baseMedia = NowLocked();
			m_running = false;
		}
	}

	void InternalClock::Resume()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (!m_running)
		{
			m_baseReal = SteadyClock::now();
			m_running = true;
		}
	}

	void InternalClock::Seek(double seconds)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_baseMedia = seconds;
		m_baseReal = SteadyClock::now();
	}

	void InternalClock::SetRate(double rate)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (rate <= 0.0)
			return;
		m_baseMedia = NowLocked();
		m_baseReal = SteadyClock::now();
		m_rate = rate;
	}

	double InternalClock::Rate() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_rate;
	}

	bool InternalClock::IsRunning() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_running;
	}
}
