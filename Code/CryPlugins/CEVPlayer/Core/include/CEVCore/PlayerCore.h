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
#include "CEVCore/FrameRing.h"
#include "CEVCore/IByteSource.h"
#include "CEVCore/IClock.h"
#include "CEVCore/Resampler.h"
#include "CEVCore/Stats.h"
#include "CEVCore/Status.h"
#include "CEVCore/VideoDecoderBackend.h"
#include "CEVCore/WebmDemuxer.h"

namespace cev
{
	enum class LoadState : uint8_t { Unloaded, Loading, Ready, Failed };
	enum class PlayerState : uint8_t { Stopped, Playing, Paused, Ended };
	// What the video worker does for a player whose picture nobody sees (PlayerCore::SetVisible).
	//   Visible:    normal decoding into the frame ring.
	//   DecodeOnly: hidden; packets are decoded up to the clock (decoder state stays current) but no ring slot is
	//               taken, nothing is copied, converted or presented.
	//   Stopped:    hidden longer than the stop threshold; no video packet is decoded at all.
	// The clock and the audio are not affected by any of them.
	enum class VideoVisibility : uint8_t { Visible, DecodeOnly, Stopped };

	struct PlayerConfig
	{
		uint32_t ringSlots = 4;              // frame ring size (>= 3 recommended: 1 held by the consumer)
		// Convert to premultiplied RGBA on the worker (initial value of SetOutputRgba()). Streams that are not packable
		// (MediaInfo::packedLayout == false) always convert, whatever this says.
		bool outputRgba = true;
		bool decodeAudio = true;
		// false: the video track (if any) is ignored entirely: no video decoder, no FrameRing / frame buffers, no
		// video decode work, GetInfo().hasVideo == false (MediaInfo::videoSkipped tells that a track was ignored),
		// Update() presents nothing and GetFrame() returns false. The player is audio only: GetTime() / the end of
		// playback follow the clock and the container duration. Meant for external audio track files. A file
		// without a playable stream after skipping (video only, or decodeAudio == false as well) fails to load
		// with Unsupported / StatusDetail::NoSupportedTrack.
		bool decodeVideo = true;
		uint32_t videoThreads = 0;           // libvpx threads, 0 = auto from the resolution
		// M9: decoder backend (VideoDecoderBackend.h), decided at Load(). Cpu = libvpx (default; the editor preview
		// always uses it). Gpu / Auto try Media Foundation and fall back to libvpx with a reason
		// (GetDecodeBackend()). Auto uses the GPU only for videos at least autoMinHeight tall.
		DecodeMode decodeMode = DecodeMode::Cpu;
		uint32_t autoMinHeight = 1080;
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
		// M8: the frame ring uses the packed layout (4:2:0, even width): FrameView::packed is valid for every frame.
		bool packedLayout = false;
		ChromaSiting chromaSiting = ChromaSiting::Left; // from the container (ResolveChromaSiting)
	};

	// The frame returned by GetFrame(): valid until the next Update() that returns true (or Unload()).
	struct FrameView
	{
		const uint8_t* plane[4] = {nullptr, nullptr, nullptr, nullptr}; // Y, U, V, A (A null without alpha)
		int stride[4] = {0, 0, 0, 0};
		const uint8_t* rgba = nullptr;       // premultiplied RGBA8, tightly packed rows; null when this frame was not
		int rgbaStride = 0;                  // converted (SetOutputRgba(false), or committed before it was switched on)
		// M8 packed layout (MediaInfo::packedLayout): one buffer of packedWidth x packedHeight bytes, tight rows, and
		// the plane rectangles inside it (rectA empty without alpha). packed == nullptr when the ring is not packed.
		const uint8_t* packed = nullptr;
		int packedWidth = 0;
		int packedHeight = 0;
		PlaneRect rectY, rectU, rectV, rectA;
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

		// ---- visibility (consumer thread) ----
		// Call once per frame before Update(). `dtSeconds` is the caller's time since the previous call; the stop
		// threshold is accumulated from it (no wall clock is read). Hidden: DecodeOnly at once, Stopped after
		// SetHiddenStopSeconds() of continuous hiding (<= 0: never). Visible again: from DecodeOnly the output simply
		// resumes; from Stopped the video decoder catches up in skip mode from the keyframe before the clock time
		// (or from where it stopped, when that is later) and the last presented frame stays current meanwhile
		// (StatsSnapshot::lastCatchUpMs / lastCatchUpFrames). While not Visible the end of a non looping video and
		// the loop wrap follow the clock and the duration only. Survives Load() / Unload() (the state is reset to
		// Visible by Unload()).
		void SetVisible(bool visible, double dtSeconds);
		void SetHiddenStopSeconds(double seconds); // default 2.0
		VideoVisibility GetVisibility() const;

		// ---- presenting (consumer thread) ----
		// Asks the clock which frame is due. Returns true when a new frame became current (upload it).
		bool Update();
		bool GetFrame(FrameView& out) const; // false before the first frame was presented
		void NoteUpload(uint64_t bytes);     // for Stats::uploadBytes

		// ---- RGBA output on demand (consumer thread, M8) ----
		// Whether the worker converts frames to RGBA. Takes effect at frame boundaries without reloading or touching the
		// clock / audio: on -> the current frame is converted at once on the calling thread (one conversion; its buffer
		// may be allocated here) and every frame committed from now on carries RGBA (frames already queued may not:
		// check FrameView::rgba); off -> no conversion any more, the RGBA buffers are freed (those of queued frames when
		// their slot is reused). Allocation happens only around a switch, never on the steady per-frame path.
		// Before Ready the value is remembered for the next Load(). A loaded stream that is not packable always
		// converts: off then returns Unsupported (the value stays on).
		Status SetOutputRgba(bool enabled);
		bool GetOutputRgba() const;
		// Slots that own an RGBA buffer right now (diagnostics / tests).
		uint32_t RgbaBufferCount() const;

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
		void ResetStatsWindow();             // only the avg / max / p99 window, counters stay
		bool VideoBufferedFrame() const;     // a decoded frame is queued and not yet presented
		bool DecoderAtEnd() const;           // all packets of all streams were decoded
		bool VideoDecoderAtEnd() const;      // all video packets decoded (or no video)
		uint32_t DecodeErrors() const;
		// M9: the backend in use (consumer thread). Valid once Ready; may change once during playback when Media
		// Foundation fails (device removed) and the player continues on libvpx (reason set).
		DecodeBackendInfo GetDecodeBackend() const;

		struct Impl;

	private:
		std::unique_ptr<Impl> m_impl;
	};
}
