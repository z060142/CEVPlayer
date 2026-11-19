// Audio output of the video preview: Qt Multimedia's QAudioOutput in pull mode. The device pulls PCM from a QIODevice
// whose readData() calls cev::PlayerCore::ReadAudioResampled() (stereo 16 bit at the device rate) and reports every
// block to the cev::AudioClock that drives the player - the same synchronisation contract as the runtime module's SDL
// mixer (CEVCore/AudioClock.h). Nothing of the engine's audio system is involved.
//
// Threads: created, started, suspended and destroyed on the main thread. readData() runs wherever the Qt audio backend
// calls it; it only touches the PlayerCore audio path and the AudioClock (both safe for one audio thread). Stop() (or
// the destructor) must run before the PlayerCore / AudioClock it reads from are destroyed.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <QIODevice>

class QAudioOutput;

namespace cev
{
	class AudioClock;
	class PlayerCore;
}

namespace CEVEditor
{
	//! Output format the preview asks the default device for.
	struct SPreviewAudioFormat
	{
		uint32_t sampleRate = 0; //!< 0 = no usable device
		std::string error;       //!< why there is none
	};

	//! Checks the default output device (stereo, 16 bit signed, 48000 Hz or the device's nearest rate).
	SPreviewAudioFormat ProbePreviewAudioDevice();

	class CPreviewAudioSource final : public QIODevice
	{
	public:
		CPreviewAudioSource(cev::PlayerCore* pSource, cev::AudioClock* pClock);
		bool isSequential() const override { return true; }
		qint64 bytesAvailable() const override { return (1 << 20) + QIODevice::bytesAvailable(); }

		//! readData() calls so far (all sources of the process; for the self test).
		static uint64_t TotalCallbacks() { return s_callbacks.load(std::memory_order_relaxed); }

	protected:
		qint64 readData(char* data, qint64 maxSize) override;
		qint64 writeData(const char*, qint64) override { return -1; }

	private:
		cev::PlayerCore* const m_pSource;
		cev::AudioClock* const m_pClock;
		std::vector<float> m_scratch; // sized once, never grown inside readData
		static std::atomic<uint64_t> s_callbacks;
	};

	class CPreviewAudioOutput
	{
	public:
		CPreviewAudioOutput() = default;
		~CPreviewAudioOutput();
		CPreviewAudioOutput(const CPreviewAudioOutput&) = delete;
		CPreviewAudioOutput& operator=(const CPreviewAudioOutput&) = delete;

		//! Opens the default device at `sampleRate` and starts pulling from `pSource`. False (and `error`) when the
		//! device refuses; the caller then plays without sound.
		bool Start(uint32_t sampleRate, cev::PlayerCore* pSource, cev::AudioClock* pClock, float volume, std::string& error);
		void Stop();
		void Suspend(bool suspend);
		void SetVolume(float volume);
		bool IsRunning() const { return m_pOutput != nullptr; }

	private:
		QAudioOutput* m_pOutput = nullptr;
		std::unique_ptr<CPreviewAudioSource> m_pDevice;
		bool m_suspended = false;
	};
}
