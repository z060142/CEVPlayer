// Media clock. M1 ships InternalClock (steady clock x rate). Later: SDL played-sample clock, CryAudio position.
#pragma once

#include <chrono>
#include <mutex>

namespace cev
{
	class IClock
	{
	public:
		virtual ~IClock() = default;

		// Current media time in seconds.
		virtual double NowSeconds() = 0;
		// Reset to 0 and run.
		virtual void Start() = 0;
		virtual void Pause() = 0;
		// Continue from the paused position.
		virtual void Resume() = 0;
		// Jump to `seconds`, keeps running / paused state.
		virtual void Seek(double seconds) = 0;
		virtual void SetRate(double rate) = 0;
		virtual double Rate() const = 0;
		virtual bool IsRunning() const = 0;
		// PlayerCore tells the clock that the current media has no (more) audio to wait for: no audio track, or
		// the audio decoder is at its end and the PCM ring is drained. Called from the consumer thread, again after
		// every Seek() once it applies. Default: ignored. AudioClock uses it to stop holding for audio that will
		// never come.
		virtual void NotifyAudioEnded() {}
	};

	class InternalClock final : public IClock
	{
	public:
		InternalClock() = default;

		double NowSeconds() override;
		void Start() override;
		void Pause() override;
		void Resume() override;
		void Seek(double seconds) override;
		void SetRate(double rate) override;
		double Rate() const override;
		bool IsRunning() const override;

	private:
		using SteadyClock = std::chrono::steady_clock;
		double NowLocked() const;

		mutable std::mutex m_mutex;
		SteadyClock::time_point m_baseReal{};
		double m_baseMedia = 0.0;
		double m_rate = 1.0;
		bool m_running = false;
	};
}
