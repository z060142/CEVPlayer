#include "StdAfx.h"
#include "Editor/PreviewAudio.h"

#include <algorithm>
#include <cstring>

#include <QtMultimedia/QAudioDeviceInfo>
#include <QtMultimedia/QAudioFormat>
#include <QtMultimedia/QAudioOutput>

#include "CEVCore/AudioClock.h"
#include "CEVCore/PlayerCore.h"

namespace CEVEditor
{
	namespace
	{
		constexpr uint32_t kPreferredRate = 48000;
		constexpr uint32_t kChannels = 2;
		constexpr uint32_t kBytesPerFrame = kChannels * 2;  // stereo, 16 bit
		constexpr uint32_t kScratchFrames = 4096;
		constexpr uint32_t kBufferMs = 100;                 // device buffer: also the latency the clock compensates

		QAudioFormat MakeFormat(int rate)
		{
			QAudioFormat format;
			format.setSampleRate(rate);
			format.setChannelCount(static_cast<int>(kChannels));
			format.setSampleSize(16);
			format.setCodec(QStringLiteral("audio/pcm"));
			format.setByteOrder(QAudioFormat::LittleEndian);
			format.setSampleType(QAudioFormat::SignedInt);
			return format;
		}
	}

	std::atomic<uint64_t> CPreviewAudioSource::s_callbacks{0};

	SPreviewAudioFormat ProbePreviewAudioDevice()
	{
		SPreviewAudioFormat result;
		const QAudioDeviceInfo device = QAudioDeviceInfo::defaultOutputDevice();
		if (device.isNull())
		{
			result.error = "no audio output device";
			return result;
		}
		QAudioFormat format = MakeFormat(static_cast<int>(kPreferredRate));
		if (!device.isFormatSupported(format))
		{
			// keep stereo / 16 bit, take the rate the device offers (the Core resamples to any rate)
			const QAudioFormat nearest = device.nearestFormat(format);
			format = MakeFormat(nearest.sampleRate());
			if (nearest.sampleRate() < 8000 || nearest.sampleRate() > 192000 || !device.isFormatSupported(format))
			{
				result.error = "the audio output device does not accept stereo 16 bit PCM";
				return result;
			}
		}
		result.sampleRate = static_cast<uint32_t>(format.sampleRate());
		return result;
	}

	CPreviewAudioSource::CPreviewAudioSource(cev::PlayerCore* pSource, cev::AudioClock* pClock)
		: m_pSource(pSource)
		, m_pClock(pClock)
		, m_scratch(static_cast<size_t>(kScratchFrames) * kChannels, 0.f)
	{
	}

	qint64 CPreviewAudioSource::readData(char* data, qint64 maxSize)
	{
		s_callbacks.fetch_add(1, std::memory_order_relaxed);
		const qint64 totalFrames = maxSize / kBytesPerFrame;
		int16_t* out = reinterpret_cast<int16_t*>(data);
		qint64 done = 0;
		while (done < totalFrames)
		{
			const uint32_t frames = static_cast<uint32_t>(std::min<qint64>(totalFrames - done, kScratchFrames));
			float* const block = m_scratch.data();
			std::fill(block, block + static_cast<size_t>(frames) * kChannels, 0.f); // the Core leaves dst untouched before Ready
			const uint32_t audible = m_pSource != nullptr ? m_pSource->ReadAudioResampled(block, frames) : 0;
			if (m_pClock != nullptr)
				m_pClock->OnFramesConsumed(frames, audible); // every block, silence included (AudioClock.h)
			for (uint32_t i = 0; i < frames * kChannels; ++i)
			{
				const float v = std::max(-1.f, std::min(1.f, block[i]));
				out[i] = static_cast<int16_t>(v * 32767.f);
			}
			out += frames * kChannels;
			done += frames;
		}
		return totalFrames * kBytesPerFrame;
	}

	CPreviewAudioOutput::~CPreviewAudioOutput()
	{
		Stop();
	}

	bool CPreviewAudioOutput::Start(uint32_t sampleRate, cev::PlayerCore* pSource, cev::AudioClock* pClock, float volume, std::string& error)
	{
		Stop();
		const QAudioDeviceInfo device = QAudioDeviceInfo::defaultOutputDevice();
		if (device.isNull() || sampleRate == 0)
		{
			error = "no audio output device";
			return false;
		}
		m_pDevice = std::make_unique<CPreviewAudioSource>(pSource, pClock);
		m_pDevice->open(QIODevice::ReadOnly);
		m_pOutput = new QAudioOutput(device, MakeFormat(static_cast<int>(sampleRate)));
		m_pOutput->setBufferSize(static_cast<int>(sampleRate * kBytesPerFrame * kBufferMs / 1000));
		m_pOutput->setVolume(static_cast<qreal>(volume));
		m_pOutput->start(m_pDevice.get());
		if (m_pOutput->error() != QAudio::NoError || m_pOutput->state() == QAudio::StoppedState)
		{
			error = "the audio output device could not be started (QAudio error " + std::to_string(static_cast<int>(m_pOutput->error())) + ")";
			Stop();
			return false;
		}
		if (pClock != nullptr)
		{
			pClock->SetOutputSampleRate(sampleRate);
			pClock->SetOutputLatencyFrames(static_cast<uint32_t>(std::max(0, m_pOutput->bufferSize())) / kBytesPerFrame);
		}
		m_suspended = false;
		return true;
	}

	void CPreviewAudioOutput::Stop()
	{
		if (m_pOutput != nullptr)
		{
			m_pOutput->stop(); // no readData() after this returns
			delete m_pOutput;
			m_pOutput = nullptr;
		}
		if (m_pDevice)
		{
			m_pDevice->close();
			m_pDevice.reset();
		}
		m_suspended = false;
	}

	void CPreviewAudioOutput::Suspend(bool suspend)
	{
		if (m_pOutput == nullptr || suspend == m_suspended)
			return;
		m_suspended = suspend;
		if (suspend)
			m_pOutput->suspend();
		else
			m_pOutput->resume();
	}

	void CPreviewAudioOutput::SetVolume(float volume)
	{
		if (m_pOutput != nullptr)
			m_pOutput->setVolume(static_cast<qreal>(std::max(0.f, std::min(1.f, volume))));
	}
}
