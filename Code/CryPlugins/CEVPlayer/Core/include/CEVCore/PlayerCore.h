// PlayerCore: one video / audio player on top of the decode pool.
//
// Threading model
//   * Consumer thread (the game's main thread): every method except ReadAudio(). Update() must be called once
//     per rendered frame; it is the only place frames change.
//   * Audio thread: ReadAudio() (may be called from any single audio callback thread). It only touches atomics
//     and the mutex protected PCM ring, so it may run concurrently with Load() / Unload() on the consumer thread
//     (it returns 0 while no audio is loaded). The one thing it cannot survive is the destructor: stop the audio
//     callback before destroying the PlayerCore.
//   * Worker threads (DecodePool): Load, demux, decode, convert. They never touch the frame the consumer holds.
//
// Load() is asynchronous: a worker opens and indexes the file, creates the decoders and decodes the first
// frame; only then GetLoadState() becomes Ready. Until then everything else returns NotReady.
// A Ready player turns Failed (GetLoadError(): Unsupported / StreamChanged) when the stream switches resolution,
// chroma format or bit depth mid-stream; Update() and the audio then stop, Unload() resets it.
// No function throws: allocation failures end up as Status::OutOfMemory / a Failed load state.
#pragma once

#include <cstdint>
#include <memory>

#include "CEVCore/ColorConvert.h"
#include "CEVCore/DecodePool.h"
#include "CEVCore/IByteSource.h"
#include "CEVCore/IClock.h"
#include "CEVCore/Resampler.h"
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
		// false: the video track (if any) is ignored entirely: no video decoder, no FrameRing / frame buffers, no
		// video decode work, GetInfo().hasVideo == false (MediaInfo::videoSkipped tells that a track was ignored),
		// Update() presents nothing and GetFrame() returns false. The player is audio only: GetTime() / the end of
		// playback follow the clock and the container duration. Meant for external language audio files. A file
		// without a playable stream after skipping (video only, or decodeAudio == false as well) fails to load
		// with Unsupported / StatusDetail::NoSupportedTrack.
		bool decodeVideo = true;
		uint32_t videoThreads = 0;           // libvpx threads, 0 = auto from the resolution
		uint32_t audioBufferMs = 500;        // PCM ring size
		ColorStandard colorStandard = ColorStandard::Auto;
		bool enableStats = false;
		IClock* externalClock = nullptr;     // not owned; nullptr = InternalClock (e.g. an AudioClock, see AudioClock.h)
		std::shared_ptr<DecodePool> pool;    // nullptr = DecodePool::AcquireShared()
		// Device rate for ReadAudioResampled(): 0 = disabled, otherwise the audio is converted to this rate and
		// to stereo (the engine's SDL device uses 48000). ReadAudio() is not affected.
		uint32_t outputSampleRate = 0;
		// Initial value of SetAvOffset() (seconds, clamped to +-kMaxAvOffsetSeconds).
		double avOffsetSeconds = 0.0;
	};

	struct MediaInfo
	{
		bool hasVideo = false;
		bool hasAudio = false;
		bool videoSkipped = false;           // the file has a video track but PlayerConfig::decodeVideo == false
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
		uint32_t outputAudioSampleRate = 0;  // ReadAudioResampled() rate (0 = resampling disabled / not available)
		uint32_t outputAudioChannels = 0;    // 2 when outputAudioSampleRate != 0
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
		static constexpr double kMaxAvOffsetSeconds = 2.0;
		// Playback rate range of SetRate(): the range the audio resampler supports (Resampler::kMinRate / kMaxRate).
		static constexpr double kMinRate = Resampler::kMinRate;
		static constexpr double kMaxRate = Resampler::kMaxRate;

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
		// rate > 0 (NaN / <= 0 -> InvalidArgument, value unchanged); values outside kMinRate .. kMaxRate are CLAMPED
		// into that range (and return Ok) so that the clock, the audio resampler and the video all run at the same
		// speed: GetRate() returns the rate that is in effect.
		Status SetRate(double rate);
		double GetRate() const;              // effective (clamped) playback rate, 1.0 by default
		// A/V offset, seconds, consumer thread, applied in Update() only (the audio path is not touched).
		// Update() looks the picture up at   clockTime - offset:
		//   offset > 0: the picture waits `offset` longer, i.e. it is shown that much later relative to the clock
		//               (use it when the audio output has `offset` of latency the clock does not account for: the
		//               sound is heard offset later than the clock says, so the picture must wait as well);
		//   offset < 0: the picture is shown earlier.
		// Clamped to +-kMaxAvOffsetSeconds; NaN / infinity -> InvalidArgument (value unchanged). Works before Load()
		// and survives Load() / Unload(). GetTime() and the clock are not shifted; playback ends when the clock
		// reaches the duration and the queued frames (up to offset late) have been presented.
		Status SetAvOffset(double seconds);
		double GetAvOffset() const;
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
		// returned. Does not allocate and never blocks on decoding. Safe against concurrent Load() / Unload()
		// (see the threading model); the destination must hold `frames * channels` floats for the channel count
		// of the media currently loaded, so after loading other media re-read GetInfo().audioChannels once the
		// player is Ready again before sizing the next callback buffer.
		uint32_t ReadAudio(float* dst, uint32_t frames);
		// Like ReadAudio(), but the PCM goes through the Resampler: dst receives `frames` interleaved STEREO
		// frames at PlayerConfig::outputSampleRate (GetInfo().outputAudioSampleRate), at the current playback
		// rate (SetRate(): varispeed, 0.25 .. 4.0). Returns the number of frames that came from decoded audio;
		// the rest of dst is zero filled. Requires PlayerConfig::outputSampleRate != 0 and media with audio:
		// otherwise dst is left untouched and 0 is returned (as for ReadAudio() before Ready). Same thread
		// contract as ReadAudio(); the resampler is reset automatically after a seek / loop / stop (no stale
		// audio). Never allocates; if Load() / Unload() holds the resampler it outputs silence for that call.
		// Do not mix ReadAudio() and ReadAudioResampled() on one player.
		// With an AudioClock pass the return value to AudioClock::OnFramesConsumed(frames, returned): the clock holds
		// while the callback delivers silence after a seek / loop / start / underrun (see AudioClock.h).
		uint32_t ReadAudioResampled(float* dst, uint32_t frames);
		// PCM frames ready for ReadAudio() (0 while a seek is pending).
		uint32_t AudioBufferedFrames() const;
		bool AudioEnded() const;             // decoder at the end and PCM ring drained
		bool AudioDecoderAtEnd() const;      // all audio packets decoded (PCM may still be buffered)

		// ---- diagnostics ----
		void SetStatsEnabled(bool enabled);  // runtime toggle (PlayerConfig::enableStats is the initial value)
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
