// PlayerCore: one video / audio player on top of the decode pool.
//
// Threading model
//   * Consumer thread (the game's main thread): every method except ReadAudio(). Update() must be called once
//     per rendered frame; it is the only place frames change.
//   * Audio thread: ReadAudio() (may be called from any single audio callback thread).
//   * Worker threads (DecodePool): Load, demux, decode, convert. They never touch the frame the consumer holds.
//
// Load() is asynchronous: a worker opens and indexes the file, creates the decoders and decodes the first
// frame; only then GetLoadState() becomes Ready. Until then everything else returns NotReady.
#pragma once

#include <cstdint>
#include <memory>

#include "CEVCore/ColorConvert.h"
#include "CEVCore/DecodePool.h"
#include "CEVCore/IByteSource.h"
#include "CEVCore/IClock.h"
#include "CEVCore/Stats.h"
#include "CEVCore/Status.h"
#include "CEVCore/WebmDemuxer.h"

namespace cev
{
	enum class LoadState : uint8_t { Unloaded, Loading, Ready, Failed };
	enum class PlayerState : uint8_t { Stopped, Playing, Paused, Ended };

	struct PlayerConfig
	{
		uint32_t ringSlots = 4;              // frame ring size (>= 3 recommended: 1 held by the consumer)
		bool outputRgba = true;              // convert to premultiplied RGBA on the worker
		bool decodeAudio = true;
		uint32_t videoThreads = 0;           // libvpx threads, 0 = auto from the resolution
		uint32_t audioBufferMs = 500;        // PCM ring size
		ColorStandard colorStandard = ColorStandard::Auto;
		bool enableStats = false;
		IClock* externalClock = nullptr;     // not owned; nullptr = InternalClock
		std::shared_ptr<DecodePool> pool;    // nullptr = DecodePool::AcquireShared()
	};

	struct MediaInfo
	{
		bool hasVideo = false;
		bool hasAudio = false;
		VideoCodec videoCodec = VideoCodec::None;
		AudioCodec audioCodec = AudioCodec::None;
		int width = 0;
		int height = 0;
		double frameRate = 0.0;
		bool hasAlpha = false;
		bool premultiplied = true;           // RGBA output is always premultiplied
		double durationSeconds = 0.0;
		uint32_t audioSampleRate = 0;        // PCM output rate of ReadAudio()
		uint32_t audioChannels = 0;
		uint32_t videoPacketCount = 0;
		ColorStandard colorStandard = ColorStandard::BT601; // resolved
		ColorRange colorRange = ColorRange::Limited;
	};

	// The frame returned by GetFrame(): valid until the next Update() that returns true (or Unload()).
	struct FrameView
	{
		const uint8_t* plane[4] = {nullptr, nullptr, nullptr, nullptr}; // Y, U, V, A (A null without alpha)
		int stride[4] = {0, 0, 0, 0};
		const uint8_t* rgba = nullptr;       // premultiplied RGBA8, tightly packed rows, null if !outputRgba
		int rgbaStride = 0;
		int width = 0;
		int height = 0;
		int chromaShiftX = 1;
		int chromaShiftY = 1;
		double pts = 0.0;
		uint64_t serial = 0;                 // increments with every presented frame
	};

	class PlayerCore
	{
	public:
		explicit PlayerCore(const PlayerConfig& config = PlayerConfig());
		~PlayerCore();
		PlayerCore(const PlayerCore&) = delete;
		PlayerCore& operator=(const PlayerCore&) = delete;

		// ---- loading ----
		// Takes ownership of `source`; returns immediately. Replaces / unloads previous media.
		Status Load(std::unique_ptr<IByteSource> source);
		LoadState GetLoadState() const;
		LoadError GetLoadError() const;
		// Stops everything, joins with the worker task, frees all buffers. Safe to call repeatedly.
		void Unload();
		// Info is valid once GetLoadState() == Ready.
		const MediaInfo& GetInfo() const;

		// ---- control (consumer thread) ----
		Status Play();                       // from Stopped / Paused / Ended
		Status Pause();
		Status Resume();
		Status Stop();                       // back to time 0, Stopped, first frame shown again
		Status Seek(double seconds);
		Status SetLoop(bool loop);
		Status SetRate(double rate);
		PlayerState GetState() const;
		bool IsLooping() const;
		double GetTime() const;              // media clock, clamped to the duration
		uint32_t GetLoopCount() const;       // completed loops

		// ---- presenting (consumer thread) ----
		// Asks the clock which frame is due. Returns true when a new frame became current (upload it).
		bool Update();
		bool GetFrame(FrameView& out) const; // false before the first frame was presented
		void NoteUpload(uint64_t bytes);     // for Stats::uploadBytes

		// ---- audio (audio thread) ----
		// Fills dst with up to `frames` interleaved float frames (GetInfo().audioChannels) and returns the
		// number of decoded frames delivered. The rest of dst is zero filled (silence on underrun, when not
		// playing, or while a seek is pending); before Ready / without audio dst is left untouched and 0 is
		// returned. Does not allocate and never blocks on decoding. Stop the audio thread before Unload().
		uint32_t ReadAudio(float* dst, uint32_t frames);
		// PCM frames ready for ReadAudio() (0 while a seek is pending).
		uint32_t AudioBufferedFrames() const;
		bool AudioEnded() const;             // decoder at the end and PCM ring drained
		bool AudioDecoderAtEnd() const;      // all audio packets decoded (PCM may still be buffered)

		// ---- diagnostics ----
		StatsSnapshot GetStats() const;
		void ResetStats();
		bool VideoBufferedFrame() const;     // a decoded frame is queued and not yet presented
		bool DecoderAtEnd() const;           // all packets of all streams were decoded
		bool VideoDecoderAtEnd() const;      // all video packets decoded (or no video)
		uint32_t DecodeErrors() const;

		struct Impl;

	private:
		std::unique_ptr<Impl> m_impl;
	};
}
