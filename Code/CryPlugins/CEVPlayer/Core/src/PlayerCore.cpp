#include "CEVCore/PlayerCore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#include "AudioRing.h"
#include "CEVCore/AudioDecoder.h"
#include "CEVCore/FrameRing.h"
#include "CEVCore/Resampler.h"
#include "CEVCore/VideoDecoder.h"

namespace cev
{
	namespace
	{
		using Steady = std::chrono::steady_clock;

		inline float MsBetween(Steady::time_point a, Steady::time_point b)
		{
			return std::chrono::duration<float, std::milli>(b - a).count();
		}

		constexpr double kPresentEpsilon = 1e-4; // frames due within 0.1 ms count as due
		constexpr double kEndEpsilon = 1e-3;
		constexpr double kEndGraceMin = 0.25;     // seconds past the duration after which remaining frames are discarded
		constexpr uint32_t kMaxAudioBufferMs = 4000;
		constexpr uint32_t kResampleBlock = 1024; // input frames moved from the PCM ring to the resampler at a time
	}

	struct PlayerCore::Impl final : public IDecodeTask
	{
		explicit Impl(const PlayerConfig& config) : cfg(config)
		{
			clock = cfg.externalClock != nullptr ? cfg.externalClock : &ownClock;
			avOffset = ClampOffset(cfg.avOffsetSeconds);
			stats.SetEnabled(cfg.enableStats);
		}

		// ---------------------------------------------------------------- configuration / shared
		PlayerConfig cfg;
		std::shared_ptr<DecodePool> pool;
		DecodePool::TaskHandle handle = DecodePool::kInvalidHandle;
		InternalClock ownClock;
		IClock* clock = nullptr;
		Stats stats;

		std::atomic<LoadState> loadState{LoadState::Unloaded};
		LoadError loadError;
		std::atomic<bool> cancel{false};
		std::unique_ptr<IByteSource> pendingSource;

		// ---------------------------------------------------------------- media (built by the load step)
		std::unique_ptr<WebmDemuxer> demux;
		std::unique_ptr<VideoDecoder> vdec;
		std::unique_ptr<AudioDecoder> adec;
		FrameRing ring;
		FrameFormat format;
		AudioRing aring;
		MediaInfo info;
		std::vector<uint8_t> videoBuf;
		std::vector<uint8_t> additionalBuf;
		std::vector<uint8_t> audioBuf;
		double firstVideoPts = 0.0;
		bool hasVideo = false;
		bool hasAudio = false;

		// ---------------------------------------------------------------- worker side state
		uint32_t vPos = 0;
		bool vSeekActive = false;
		double vSeekTarget = 0.0;
		std::atomic<bool> vEof{false};
		uint32_t aPos = 0;
		uint64_t silenceRemaining = 0;
		const float* pendingPcm = nullptr;
		uint32_t pendingFrames = 0;
		std::atomic<bool> aEof{false};
		std::atomic<bool> faulted{false};      // a worker step threw: both streams count as ended
		// What ReadAudio() (audio thread) may touch besides AudioRing and the atomics below: nothing.
		std::atomic<bool> audioActive{false};  // true from the end of the load step until Unload starts
		std::atomic<uint32_t> audioChannels{1};
		std::atomic<uint32_t> appliedSeekGen{0};
		std::atomic<uint32_t> decodeErrors{0};

		// ---------------------------------------------------------------- resampled audio output
		// Owned by the audio thread (ReadAudioResampled). Load / Unload touch it only under resMutex, the audio
		// thread uses try_lock so it never waits for them.
		std::mutex resMutex;
		Resampler resampler;
		std::vector<float> resIn;               // kResampleBlock * source channels
		bool resReady = false;                  // resampler initialised for the loaded media (under resMutex)
		uint32_t resGen = 0;                    // appliedSeekGen the resampler state belongs to
		bool resFlushed = false;                // the tail of the stream was flushed for this generation
		std::atomic<bool> resEnabled{false};    // set at the end of the load step, cleared when Unload starts
		std::atomic<double> audioRate{1.0};     // SetRate() value, applied by the audio thread

		// ---------------------------------------------------------------- seek command (consumer -> worker)
		std::mutex cmdMutex;
		double seekTarget = 0.0;
		std::atomic<uint32_t> seekGen{0};

		// ---------------------------------------------------------------- consumer state
		std::atomic<uint8_t> state{static_cast<uint8_t>(PlayerState::Stopped)};
		bool loop = false;
		double avOffset = 0.0;                 // seconds, see PlayerCore::SetAvOffset()
		uint32_t loopCount = 0;
		uint64_t serial = 0;

		static double ClampOffset(double v)
		{
			if (!(v == v))
				return 0.0;
			return std::min(std::max(v, -PlayerCore::kMaxAvOffsetSeconds), PlayerCore::kMaxAvOffsetSeconds);
		}

		PlayerState State() const { return static_cast<PlayerState>(state.load(std::memory_order_acquire)); }
		void SetState(PlayerState s) { state.store(static_cast<uint8_t>(s), std::memory_order_release); }

		// ================================================================ IDecodeTask
		bool RunStep() override
		{
			// An exception (bad_alloc, system_error) on a worker thread would terminate the process: turn it into
			// a failed player instead.
			try
			{
				return RunStepImpl();
			}
			catch (const std::bad_alloc&)
			{
				return AbortStep(Status::OutOfMemory);
			}
			catch (...)
			{
				return AbortStep(Status::LoadFailed);
			}
		}

		bool AbortStep(Status status)
		{
			if (loadState.load(std::memory_order_acquire) == LoadState::Loading)
				return Fail(status, StatusDetail::InternalError);
			decodeErrors.fetch_add(1, std::memory_order_relaxed);
			faulted.store(true, std::memory_order_release);
			return false;
		}

		// The stream cannot be played any further (format change): Ready -> Failed, surfaced through
		// GetLoadState() / GetLoadError(). Buffers stay valid until Unload().
		bool FailStream(Status status, StatusDetail detail)
		{
			faulted.store(true, std::memory_order_release);
			return Fail(status, detail);
		}

		bool RunStepImpl()
		{
			if (cancel.load(std::memory_order_relaxed) || faulted.load(std::memory_order_relaxed))
				return false;
			const LoadState ls = loadState.load(std::memory_order_acquire);
			if (ls == LoadState::Loading)
				return DoLoad();
			if (ls != LoadState::Ready)
				return false;

			if (seekGen.load(std::memory_order_acquire) != appliedSeekGen.load(std::memory_order_relaxed))
				ApplySeek();

			bool more = false;
			if (hasVideo && VideoStep())
				more = true;
			if (hasAudio && AudioStep())
				more = true;
			return more;
		}

		// ================================================================ load (worker)
		bool Fail(Status status, StatusDetail detail)
		{
			loadError.status = status;
			loadError.detail = detail;
			loadState.store(LoadState::Failed, std::memory_order_release);
			return false;
		}

		bool ConvertSlot(FrameRing::Slot& slot)
		{
			if (!format.withRgba)
				return true;
			YuvaImage img;
			for (int i = 0; i < 4; ++i)
			{
				img.plane[i] = slot.plane[i];
				img.stride[i] = slot.stride[i];
			}
			img.width = format.width;
			img.height = format.height;
			img.chromaShiftX = format.chromaShiftX;
			img.chromaShiftY = format.chromaShiftY;
			ConvertYuvaToRgbaPremultiplied(img, info.colorStandard, info.colorRange, slot.rgba, slot.rgbaStride);
			return true;
		}

		bool DoLoad()
		{
			std::unique_ptr<IByteSource> source = std::move(pendingSource);
			StatusDetail detail = StatusDetail::None;
			Status st = WebmDemuxer::Open(std::move(source), demux, &detail, &cancel);
			if (Failed(st))
				return Fail(st, detail);

			const VideoStreamInfo& v = demux->Video();
			const AudioStreamInfo& a = demux->Audio();
			const bool videoPresent = demux->HasVideo();
			hasVideo = videoPresent && cfg.decodeVideo;
			hasAudio = demux->HasAudio() && cfg.decodeAudio;
			if (!cfg.decodeVideo && !hasAudio)
				return Fail(Status::Unsupported, StatusDetail::NoSupportedTrack); // nothing left to play

			info = MediaInfo();
			info.hasVideo = hasVideo;
			info.videoSkipped = videoPresent && !hasVideo;
			info.videoCodec = hasVideo ? v.codec : VideoCodec::None;
			info.durationSeconds = demux->DurationSeconds();

			if (hasVideo)
			{
				uint32_t threads = cfg.videoThreads;
				if (threads == 0)
				{
					threads = v.height >= 1000 ? 4u : (v.height >= 700 ? 2u : 1u);
					threads = std::min(threads, std::max(1u, std::thread::hardware_concurrency()));
				}
				st = VideoDecoder::Create(v, threads, vdec);
				if (Failed(st))
					return Fail(st, StatusDetail::VideoDecoderInit);

				videoBuf.assign(std::max<uint32_t>(v.maxPacketSize, 1u), 0);
				additionalBuf.assign(std::max<uint32_t>(v.maxAdditionalSize, 1u), 0);
				firstVideoPts = v.firstPts;
				info.frameRate = v.frameRate;
				info.hasAlpha = v.hasAlpha;
				info.videoPacketCount = v.packetCount;
				info.colorStandard = ResolveColorStandard(cfg.colorStandard, v.matrixCoefficients, v.width, v.height);
				info.colorRange = ResolveColorRange(v.colorRange);

				// Decode until the first visible frame to learn the real format, then build the ring.
				const uint32_t count = demux->VideoPacketCount();
				const uint32_t limit = std::min<uint32_t>(count, 64);
				uint32_t idx = 0;
				bool got = false;
				for (; idx < limit && !got; ++idx)
				{
					if (cancel.load(std::memory_order_relaxed))
						return Fail(Status::Cancelled, StatusDetail::FirstFrame);
					const PacketRef& ref = demux->VideoPacket(idx);
					if (Failed(demux->ReadVideoPacket(idx, videoBuf.data(), static_cast<uint32_t>(videoBuf.size()))))
						return Fail(Status::IoError, StatusDetail::FirstFrame);
					const bool alpha = v.hasAlpha && ref.additionalSize > 0;
					if (alpha && Failed(demux->ReadVideoAdditional(idx, additionalBuf.data(), static_cast<uint32_t>(additionalBuf.size()))))
						return Fail(Status::IoError, StatusDetail::FirstFrame);
					if (Succeeded(vdec->Decode(videoBuf.data(), ref.size, alpha ? additionalBuf.data() : nullptr,
						alpha ? ref.additionalSize : 0)) && vdec->HasFrame())
					{
						got = true;
						break;
					}
				}
				if (!got)
					return Fail(Status::InvalidData, StatusDetail::FirstFrame);

				DecodedImageInfo di;
				if (Failed(vdec->GetFrameInfo(di)))
					return Fail(Status::InvalidData, StatusDetail::FirstFrame);
				format = FrameFormat();
				format.width = di.width;
				format.height = di.height;
				format.chromaShiftX = di.chromaShiftX;
				format.chromaShiftY = di.chromaShiftY;
				format.hasAlpha = v.hasAlpha;
				format.withRgba = cfg.outputRgba;
				info.width = di.width;
				info.height = di.height;

				const uint32_t slots = std::max(3u, cfg.ringSlots);
				st = ring.Init(format, slots);
				if (Failed(st))
					return Fail(st, StatusDetail::RingInit); // Unsupported (too large) / OutOfMemory (budget)

				uint64_t epoch = 0;
				const int slot = ring.AcquireWrite(epoch);
				if (slot < 0)
					return Fail(Status::InvalidState, StatusDetail::RingInit);
				if (Failed(vdec->WriteFrame(format, ring.GetSlot(static_cast<uint32_t>(slot)))))
				{
					ring.Abandon(slot);
					return Fail(Status::InvalidData, StatusDetail::FirstFrame);
				}
				ConvertSlot(ring.GetSlot(static_cast<uint32_t>(slot)));
				ring.Commit(slot, epoch, demux->VideoPts(idx));
				vPos = idx + 1;
				vEof.store(vPos >= count, std::memory_order_relaxed);
			}
			else
			{
				vEof.store(true, std::memory_order_relaxed);
			}

			if (hasAudio)
			{
				StatusDetail adet = StatusDetail::None;
				st = AudioDecoder::Create(a, adec, &adet);
				if (Failed(st))
				{
					// Unsupported audio must not kill a playable video: continue without sound.
					adec.reset();
					hasAudio = false;
					if (!hasVideo)
						return Fail(st, adet);
				}
			}
			if (hasAudio)
			{
				audioBuf.assign(std::max<uint32_t>(a.maxPacketSize, 1u), 0);
				info.hasAudio = true;
				info.audioCodec = a.codec;
				info.audioSampleRate = adec->SampleRate();
				info.audioChannels = adec->Channels();
				// PCM ring: 50 ms .. 4 s of audio, computed in 64 bit before anything is allocated.
				const uint64_t rate = info.audioSampleRate;
				const uint32_t bufMs = std::min(std::max(50u, cfg.audioBufferMs), kMaxAudioBufferMs);
				uint64_t cap = std::max<uint64_t>(rate * bufMs / 1000u, adec->MaxFramesPerPacket());
				cap = std::min<uint64_t>(cap, std::max<uint64_t>(rate * (kMaxAudioBufferMs / 1000u), adec->MaxFramesPerPacket()));
				aring.Init(info.audioChannels, static_cast<uint32_t>(cap));
				ApplyAudioSeek(0.0);
				audioChannels.store(std::max(1u, info.audioChannels), std::memory_order_relaxed);
				if (cfg.outputSampleRate != 0)
				{
					std::lock_guard<std::mutex> lock(resMutex);
					if (Succeeded(resampler.Init(info.audioSampleRate, std::max(1u, info.audioChannels), cfg.outputSampleRate,
						2, kResampleBlock)))
					{
						resIn.assign(static_cast<size_t>(kResampleBlock) * std::max(1u, info.audioChannels), 0.0f);
						resampler.SetRate(audioRate.load(std::memory_order_relaxed));
						resGen = appliedSeekGen.load(std::memory_order_relaxed);
						resFlushed = false;
						resReady = true;
						info.outputAudioSampleRate = cfg.outputSampleRate;
						info.outputAudioChannels = 2;
						resEnabled.store(true, std::memory_order_release);
					}
				}
				audioActive.store(true, std::memory_order_release);
			}
			else
			{
				aEof.store(true, std::memory_order_relaxed);
			}

			loadState.store(LoadState::Ready, std::memory_order_release);
			return true;
		}

		// ================================================================ seek (worker)
		void ApplyAudioSeek(double t)
		{
			aring.Clear();
			pendingPcm = nullptr;
			pendingFrames = 0;
			aEof.store(false, std::memory_order_relaxed);

			const AudioStreamInfo& a = demux->Audio();
			const uint32_t rate = adec->SampleRate();
			const double preroll = static_cast<double>(a.seekPreRollNs) * 1e-9;
			uint32_t s = demux->FindAudioPacket(std::max(0.0, t - preroll));
			const uint32_t extra = adec->ExtraPrerollPackets();
			s = s >= extra ? s - extra : 0;
			adec->Reset(s == 0);

			// Matroska: actual time of a packet's first decoded sample = block time - CodecDelay. For packet 0 the
			// Opus pre-skip (== CodecDelay) is discarded, which moves the first kept sample back to the block time.
			const double startSec = demux->AudioPts(s) - static_cast<double>(a.codecDelayNs) * 1e-9;
			int64_t startFrames = std::llround(startSec * rate);
			if (s == 0)
				startFrames += adec->PreSkipFrames();
			const int64_t targetFrames = std::llround(t * rate);
			silenceRemaining = 0;
			if (startFrames > targetFrames)
				silenceRemaining = static_cast<uint64_t>(startFrames - targetFrames);
			else if (targetFrames > startFrames)
				adec->DiscardFrames(static_cast<uint32_t>(std::min<int64_t>(targetFrames - startFrames, 0x7FFFFFFF)));
			aPos = s;
		}

		void ApplySeek()
		{
			uint32_t gen = 0;
			double t = 0.0;
			{
				std::lock_guard<std::mutex> lock(cmdMutex);
				gen = seekGen.load(std::memory_order_relaxed);
				t = seekTarget;
			}
			if (hasVideo)
			{
				vPos = demux->FindVideoKeyframe(t);
				vSeekActive = true;
				vSeekTarget = t;
				vEof.store(false, std::memory_order_relaxed);
			}
			if (hasAudio)
				ApplyAudioSeek(t);
			appliedSeekGen.store(gen, std::memory_order_release);
		}

		// ================================================================ decode steps (worker)
		bool VideoStep()
		{
			if (vEof.load(std::memory_order_relaxed))
				return false;
			const uint32_t count = demux->VideoPacketCount();
			if (vPos >= count)
			{
				vEof.store(true, std::memory_order_release);
				return false;
			}

			const uint32_t idx = vPos;
			const bool skipping = vSeekActive && idx + 1 < count && demux->VideoPts(idx + 1) <= vSeekTarget + 1e-9;

			int slot = -1;
			uint64_t epoch = 0;
			if (!skipping)
			{
				slot = ring.AcquireWrite(epoch);
				if (slot < 0)
					return false; // ring full: idle until the consumer frees a slot and wakes us
				if (seekGen.load(std::memory_order_acquire) != appliedSeekGen.load(std::memory_order_relaxed))
				{
					ring.Abandon(slot);
					return true; // a seek arrived: handle it first
				}
			}

			const bool timing = stats.IsEnabled();
			const PacketRef& ref = demux->VideoPacket(idx);
			Status st = demux->ReadVideoPacket(idx, videoBuf.data(), static_cast<uint32_t>(videoBuf.size()));
			const bool alpha = demux->Video().hasAlpha && ref.additionalSize > 0;
			if (Succeeded(st) && alpha)
			{
				if (Failed(demux->ReadVideoAdditional(idx, additionalBuf.data(), static_cast<uint32_t>(additionalBuf.size()))))
					st = Status::IoError;
			}
			Steady::time_point t0, t1, t2;
			if (timing)
				t0 = Steady::now();
			if (Succeeded(st))
				st = vdec->Decode(videoBuf.data(), ref.size, alpha ? additionalBuf.data() : nullptr, alpha ? ref.additionalSize : 0);
			if (timing)
				t1 = Steady::now();
			if (Failed(st))
				decodeErrors.fetch_add(1, std::memory_order_relaxed);
			if (st == Status::Unsupported) // e.g. the stream switched to high bit depth
			{
				if (slot >= 0)
					ring.Abandon(slot);
				return FailStream(Status::Unsupported, StatusDetail::StreamChanged);
			}

			if (skipping)
			{
				++vPos;
				return true;
			}
			vSeekActive = false;

			if (Succeeded(st) && vdec->HasFrame())
			{
				FrameRing::Slot& s = ring.GetSlot(static_cast<uint32_t>(slot));
				if (Succeeded(vdec->WriteFrame(format, s)))
				{
					ConvertSlot(s);
					if (timing)
						t2 = Steady::now();
					if (ring.Commit(slot, epoch, demux->VideoPts(idx)) && timing)
						stats.RecordDecoded(MsBetween(t0, t1), MsBetween(t1, t2), ring.InFlightCount());
				}
				else
				{
					decodeErrors.fetch_add(1, std::memory_order_relaxed);
					ring.Abandon(slot);
					// Resolution / chroma format switched mid-stream: the ring is sized for the first frame, so
					// stop cleanly instead of freezing on the last frame.
					DecodedImageInfo di;
					if (Succeeded(vdec->GetFrameInfo(di)) && (di.width != format.width || di.height != format.height ||
						di.chromaShiftX != format.chromaShiftX || di.chromaShiftY != format.chromaShiftY))
						return FailStream(Status::Unsupported, StatusDetail::StreamChanged);
				}
			}
			else
			{
				ring.Abandon(slot);
			}

			++vPos;
			if (vPos >= count)
				vEof.store(true, std::memory_order_release);
			return true;
		}

		bool AudioStep()
		{
			if (aEof.load(std::memory_order_relaxed))
				return false;
			const uint32_t channels = info.audioChannels;
			const uint32_t freeFrames = aring.FreeFrames();

			if (pendingFrames > 0)
			{
				const uint32_t n = aring.Write(pendingPcm, pendingFrames);
				pendingPcm += static_cast<size_t>(n) * channels;
				pendingFrames -= n;
				return pendingFrames == 0; // ring full -> idle until ReadAudio frees space
			}
			if (silenceRemaining > 0)
			{
				const uint32_t n = aring.Write(nullptr, static_cast<uint32_t>(std::min<uint64_t>(silenceRemaining, freeFrames)));
				silenceRemaining -= n;
				return n > 0;
			}

			const uint32_t count = demux->AudioPacketCount();
			if (aPos >= count)
			{
				aEof.store(true, std::memory_order_release);
				return false;
			}
			if (freeFrames < info.audioSampleRate / 50) // need ~20 ms of room before decoding another packet
				return false;

			const PacketRef& ref = demux->AudioPacket(aPos);
			const uint32_t index = aPos++;
			if (Failed(demux->ReadAudioPacket(index, audioBuf.data(), static_cast<uint32_t>(audioBuf.size()))))
			{
				decodeErrors.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			uint32_t trim = 0;
			if (ref.discardPaddingNs > 0)
				trim = static_cast<uint32_t>(static_cast<uint64_t>(ref.discardPaddingNs) * info.audioSampleRate / 1000000000ull);
			const float* pcm = nullptr;
			uint32_t frames = 0;
			if (Failed(adec->Decode(audioBuf.data(), ref.size, trim, &pcm, &frames)))
			{
				decodeErrors.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			pendingPcm = pcm;
			pendingFrames = frames;
			const uint32_t n = aring.Write(pendingPcm, pendingFrames);
			pendingPcm += static_cast<size_t>(n) * channels;
			pendingFrames -= n;
			return pendingFrames == 0;
		}

		// ================================================================ consumer helpers
		bool EofValid() const
		{
			return seekGen.load(std::memory_order_acquire) == appliedSeekGen.load(std::memory_order_acquire);
		}

		bool VideoAtEnd() const
		{
			return !hasVideo || faulted.load(std::memory_order_acquire) || (EofValid() && vEof.load(std::memory_order_acquire));
		}

		bool AudioAtEnd() const
		{
			return !hasAudio || faulted.load(std::memory_order_acquire) || (EofValid() && aEof.load(std::memory_order_acquire));
		}

		void WakeWorker()
		{
			if (pool && handle != DecodePool::kInvalidHandle)
				pool->Wake(handle);
		}

		void SeekInternal(double t)
		{
			{
				std::lock_guard<std::mutex> lock(cmdMutex);
				seekTarget = t;
				seekGen.fetch_add(1, std::memory_order_acq_rel);
			}
			if (hasVideo)
				ring.FlushReady();
			clock->Seek(t);
			WakeWorker();
		}

		void UnloadImpl()
		{
			if (loadState.load(std::memory_order_acquire) == LoadState::Unloaded && handle == DecodePool::kInvalidHandle)
				return;
			audioActive.store(false, std::memory_order_release); // ReadAudio() returns 0 from here on
			resEnabled.store(false, std::memory_order_release);
			cancel.store(true, std::memory_order_release);
			if (pool && handle != DecodePool::kInvalidHandle)
				pool->Unregister(handle);
			handle = DecodePool::kInvalidHandle;
			pool.reset();
			{
				// Waits for a ReadAudioResampled() call that is still inside the resampler.
				std::lock_guard<std::mutex> lock(resMutex);
				resReady = false;
				resampler.Shutdown();
				std::vector<float>().swap(resIn);
			}

			vdec.reset();
			adec.reset();
			demux.reset();
			ring.Shutdown();
			aring.Shutdown();
			pendingSource.reset();
			std::vector<uint8_t>().swap(videoBuf);
			std::vector<uint8_t>().swap(additionalBuf);
			std::vector<uint8_t>().swap(audioBuf);

			info = MediaInfo();
			format = FrameFormat();
			loadError = LoadError();
			hasVideo = hasAudio = false;
			vPos = aPos = 0;
			vSeekActive = false;
			vSeekTarget = 0.0;
			vEof.store(false);
			aEof.store(false);
			faulted.store(false);
			silenceRemaining = 0;
			pendingPcm = nullptr;
			pendingFrames = 0;
			seekGen.store(0);
			appliedSeekGen.store(0);
			seekTarget = 0.0;
			decodeErrors.store(0);
			loopCount = 0;
			serial = 0;
			firstVideoPts = 0.0;
			SetState(PlayerState::Stopped);
			stats.Reset();
			clock->Pause();
			clock->Seek(0.0);
			loadState.store(LoadState::Unloaded, std::memory_order_release);
			cancel.store(false, std::memory_order_release);
		}
	};

	// ====================================================================== public API
	PlayerCore::PlayerCore(const PlayerConfig& config) : m_impl(std::make_unique<Impl>(config)) {}

	PlayerCore::~PlayerCore()
	{
		m_impl->UnloadImpl();
	}

	Status PlayerCore::Load(std::unique_ptr<IByteSource> source)
	{
		Impl& d = *m_impl;
		if (!source)
			return Status::InvalidArgument;
		try
		{
			d.UnloadImpl();

			d.clock->Pause();
			d.clock->Seek(0.0);
			d.pool = d.cfg.pool ? d.cfg.pool : DecodePool::AcquireShared();
			d.pendingSource = std::move(source);
			if (!d.pool)
			{
				d.pendingSource.reset();
				d.loadError = LoadError{Status::OutOfMemory, StatusDetail::PoolRegister};
				d.loadState.store(LoadState::Failed, std::memory_order_release);
				return Status::OutOfMemory;
			}
			d.loadState.store(LoadState::Loading, std::memory_order_release);
			if (Failed(d.pool->Register(&d, d.handle)))
			{
				d.loadError = LoadError{Status::OutOfMemory, StatusDetail::PoolRegister};
				d.loadState.store(LoadState::Failed, std::memory_order_release);
				d.pool.reset();
				d.pendingSource.reset();
				return Status::OutOfMemory;
			}
			d.pool->Wake(d.handle);
			return Status::Ok;
		}
		catch (...)
		{
			// Nothing may throw out of the API. Release whatever was set up and report a failed load.
			try { d.UnloadImpl(); } catch (...) {}
			d.loadError = LoadError{Status::OutOfMemory, StatusDetail::InternalError};
			d.loadState.store(LoadState::Failed, std::memory_order_release);
			return Status::OutOfMemory;
		}
	}

	LoadState PlayerCore::GetLoadState() const { return m_impl->loadState.load(std::memory_order_acquire); }

	LoadError PlayerCore::GetLoadError() const
	{
		return m_impl->loadState.load(std::memory_order_acquire) == LoadState::Failed ? m_impl->loadError : LoadError();
	}

	void PlayerCore::Unload() { m_impl->UnloadImpl(); }

	const MediaInfo& PlayerCore::GetInfo() const { return m_impl->info; }

	Status PlayerCore::Play()
	{
		Impl& d = *m_impl;
		if (d.loadState.load(std::memory_order_acquire) != LoadState::Ready)
			return Status::NotReady;
		const PlayerState s = d.State();
		if (s == PlayerState::Playing)
			return Status::Ok;
		if (s == PlayerState::Ended)
			d.SeekInternal(0.0);
		d.clock->Resume();
		d.SetState(PlayerState::Playing);
		return Status::Ok;
	}

	Status PlayerCore::Pause()
	{
		Impl& d = *m_impl;
		if (d.loadState.load(std::memory_order_acquire) != LoadState::Ready)
			return Status::NotReady;
		if (d.State() == PlayerState::Playing)
		{
			d.clock->Pause();
			d.SetState(PlayerState::Paused);
		}
		return Status::Ok;
	}

	Status PlayerCore::Resume()
	{
		Impl& d = *m_impl;
		if (d.loadState.load(std::memory_order_acquire) != LoadState::Ready)
			return Status::NotReady;
		if (d.State() == PlayerState::Paused)
		{
			d.clock->Resume();
			d.SetState(PlayerState::Playing);
		}
		return Status::Ok;
	}

	Status PlayerCore::Stop()
	{
		Impl& d = *m_impl;
		if (d.loadState.load(std::memory_order_acquire) != LoadState::Ready)
			return Status::NotReady;
		d.clock->Pause();
		d.SetState(PlayerState::Stopped);
		d.SeekInternal(0.0);
		return Status::Ok;
	}

	Status PlayerCore::Seek(double seconds)
	{
		Impl& d = *m_impl;
		if (d.loadState.load(std::memory_order_acquire) != LoadState::Ready)
			return Status::NotReady;
		if (!(seconds >= 0.0))
			seconds = 0.0;
		seconds = std::min(seconds, d.info.durationSeconds);
		if (d.State() == PlayerState::Ended)
			d.SetState(PlayerState::Paused);
		d.SeekInternal(seconds);
		return Status::Ok;
	}

	Status PlayerCore::SetLoop(bool loop)
	{
		m_impl->loop = loop;
		return Status::Ok;
	}

	Status PlayerCore::SetRate(double rate)
	{
		if (!(rate > 0.0) || rate > 16.0)
			return Status::InvalidArgument;
		m_impl->audioRate.store(rate, std::memory_order_relaxed);
		m_impl->clock->SetRate(rate);
		return Status::Ok;
	}

	Status PlayerCore::SetAvOffset(double seconds)
	{
		if (!std::isfinite(seconds))
			return Status::InvalidArgument;
		m_impl->avOffset = Impl::ClampOffset(seconds);
		return Status::Ok;
	}

	double PlayerCore::GetAvOffset() const { return m_impl->avOffset; }

	PlayerState PlayerCore::GetState() const { return m_impl->State(); }
	bool PlayerCore::IsLooping() const { return m_impl->loop; }
	uint32_t PlayerCore::GetLoopCount() const { return m_impl->loopCount; }

	double PlayerCore::GetTime() const
	{
		const Impl& d = *m_impl;
		if (d.loadState.load(std::memory_order_acquire) != LoadState::Ready)
			return 0.0;
		return std::min(d.clock->NowSeconds(), d.info.durationSeconds);
	}

	bool PlayerCore::Update()
	{
		Impl& d = *m_impl;
		if (d.loadState.load(std::memory_order_acquire) != LoadState::Ready)
			return false;

		bool newFrame = false;
		const double t = d.clock->NowSeconds();
		const double tVideo = t - d.avOffset; // presentation time lookup (see SetAvOffset)

		if (d.hasVideo)
		{
			uint32_t dropped = 0;
			const int slot = d.ring.PopForTime(std::max(tVideo, d.firstVideoPts), kPresentEpsilon, dropped);
			if (dropped > 0)
				d.stats.NoteDropped(dropped);
			if (slot >= 0)
			{
				newFrame = true;
				++d.serial;
				d.stats.NotePresented();
			}
			if ((slot >= 0 || dropped > 0) && !d.vEof.load(std::memory_order_relaxed))
				d.WakeWorker();
		}
		if (d.hasAudio && !d.aEof.load(std::memory_order_relaxed) && d.aring.FreeFrames() >= d.aring.Capacity() / 4)
			d.WakeWorker();

		if (d.State() == PlayerState::Playing && t >= d.info.durationSeconds - kEndEpsilon)
		{
			bool videoDone = !d.hasVideo || (d.VideoAtEnd() && d.ring.ReadyCount() == 0);
			if (!videoDone && d.VideoAtEnd())
			{
				// Frames that stay queued long after the duration (bogus timestamps) must not hold the end back:
				// once the clock is a grace period past the duration the decoder is done, drop what is left.
				const double period = d.info.frameRate > 0.0 ? 1.0 / d.info.frameRate : 0.0;
				// (measured on the picture time line: a late picture, offset > 0, gets its extra time)
				if (tVideo >= d.info.durationSeconds + std::max(kEndGraceMin, period))
				{
					d.ring.FlushReady();
					videoDone = true;
				}
			}
			if (videoDone)
			{
				if (d.loop)
				{
					++d.loopCount;
					d.SeekInternal(0.0);
				}
				else
				{
					d.clock->Pause();
					d.clock->Seek(d.info.durationSeconds);
					d.SetState(PlayerState::Ended);
				}
			}
		}
		return newFrame;
	}

	bool PlayerCore::GetFrame(FrameView& out) const
	{
		const Impl& d = *m_impl;
		if (d.loadState.load(std::memory_order_acquire) != LoadState::Ready || !d.hasVideo)
			return false;
		const int cur = d.ring.Current();
		if (cur < 0)
			return false;
		const FrameRing::Slot& s = d.ring.GetSlot(static_cast<uint32_t>(cur));
		for (int i = 0; i < 4; ++i)
		{
			out.plane[i] = s.plane[i];
			out.stride[i] = s.stride[i];
		}
		out.rgba = s.rgba;
		out.rgbaStride = s.rgbaStride;
		out.width = d.format.width;
		out.height = d.format.height;
		out.chromaShiftX = d.format.chromaShiftX;
		out.chromaShiftY = d.format.chromaShiftY;
		out.pts = s.pts;
		out.serial = d.serial;
		return true;
	}

	void PlayerCore::NoteUpload(uint64_t bytes) { m_impl->stats.NoteUpload(bytes); }

	uint32_t PlayerCore::ReadAudio(float* dst, uint32_t frames)
	{
		Impl& d = *m_impl;
		if (dst == nullptr || frames == 0)
			return 0;
		// Only atomics and the (mutex protected) AudioRing are touched here, so this is safe while Load() /
		// Unload() run on another thread. Before Ready / after Unload started dst is left untouched.
		if (d.loadState.load(std::memory_order_acquire) != LoadState::Ready || !d.audioActive.load(std::memory_order_acquire))
			return 0;
		const uint32_t channels = d.audioChannels.load(std::memory_order_relaxed);
		if (d.State() != PlayerState::Playing ||
			d.seekGen.load(std::memory_order_acquire) != d.appliedSeekGen.load(std::memory_order_acquire))
		{
			std::memset(dst, 0, static_cast<size_t>(frames) * channels * sizeof(float));
			return 0;
		}
		const uint32_t n = d.aring.Read(dst, frames);
		if (n < frames)
			std::memset(dst + static_cast<size_t>(n) * channels, 0, static_cast<size_t>(frames - n) * channels * sizeof(float));
		// No worker wake-up here: Update() (consumer thread) wakes the worker when a quarter of the ring is free.
		return n;
	}

	uint32_t PlayerCore::ReadAudioResampled(float* dst, uint32_t frames)
	{
		Impl& d = *m_impl;
		if (dst == nullptr || frames == 0)
			return 0;
		if (!d.resEnabled.load(std::memory_order_acquire) || d.loadState.load(std::memory_order_acquire) != LoadState::Ready ||
			!d.audioActive.load(std::memory_order_acquire))
			return 0;
		const size_t silenceBytes = static_cast<size_t>(frames) * 2 * sizeof(float);
		// Load / Unload hold this while (re)building or freeing the resampler: never wait for them.
		std::unique_lock<std::mutex> lock(d.resMutex, std::try_to_lock);
		if (!lock.owns_lock() || !d.resReady)
		{
			std::memset(dst, 0, silenceBytes);
			return 0;
		}
		if (d.State() != PlayerState::Playing ||
			d.seekGen.load(std::memory_order_acquire) != d.appliedSeekGen.load(std::memory_order_acquire))
		{
			std::memset(dst, 0, silenceBytes);
			return 0;
		}
		// A seek / loop / stop was applied since the last call: drop the resampler's history and buffered input.
		const uint32_t gen = d.appliedSeekGen.load(std::memory_order_acquire);
		if (gen != d.resGen)
		{
			d.resampler.Reset();
			d.resGen = gen;
			d.resFlushed = false;
		}
		d.resampler.SetRate(d.audioRate.load(std::memory_order_relaxed));

		uint32_t produced = 0;
		while (produced < frames)
		{
			const bool atEnd = d.AudioAtEnd(); // evaluated before the read: the ring is then really drained
			uint32_t need = d.resampler.InputNeeded(frames - produced);
			need = std::min(std::min(need, d.resampler.InputSpace()), kResampleBlock);
			uint32_t got = 0;
			if (need > 0)
			{
				got = d.aring.Read(d.resIn.data(), need);
				if (got > 0)
					d.resampler.Push(d.resIn.data(), got);
			}
			const uint32_t n = d.resampler.Pull(dst + static_cast<size_t>(produced) * 2, frames - produced);
			produced += n;
			if (n == 0 && got == 0)
			{
				if (need > 0 && atEnd && !d.resFlushed)
				{
					d.resampler.Flush(); // end of stream: let the filter tail out
					d.resFlushed = true;
					continue;
				}
				break; // underrun (or nothing more to do)
			}
		}
		if (produced < frames)
			std::memset(dst + static_cast<size_t>(produced) * 2, 0, static_cast<size_t>(frames - produced) * 2 * sizeof(float));
		return produced;
	}

	uint32_t PlayerCore::AudioBufferedFrames() const
	{
		const Impl& d = *m_impl;
		return (d.loadState.load(std::memory_order_acquire) == LoadState::Ready && d.hasAudio && d.EofValid())
			? d.aring.Buffered() : 0;
	}

	bool PlayerCore::AudioDecoderAtEnd() const
	{
		const Impl& d = *m_impl;
		return d.loadState.load(std::memory_order_acquire) == LoadState::Ready && d.AudioAtEnd();
	}

	bool PlayerCore::AudioEnded() const
	{
		const Impl& d = *m_impl;
		return d.loadState.load(std::memory_order_acquire) == LoadState::Ready && d.AudioAtEnd() && d.aring.Buffered() == 0;
	}

	void PlayerCore::SetStatsEnabled(bool enabled) { m_impl->stats.SetEnabled(enabled); }
	StatsSnapshot PlayerCore::GetStats() const { return m_impl->stats.Snapshot(); }
	void PlayerCore::ResetStats() { m_impl->stats.Reset(); }

	bool PlayerCore::VideoBufferedFrame() const
	{
		const Impl& d = *m_impl;
		return d.loadState.load(std::memory_order_acquire) == LoadState::Ready && d.hasVideo && d.ring.ReadyCount() > 0;
	}

	bool PlayerCore::DecoderAtEnd() const
	{
		const Impl& d = *m_impl;
		return d.loadState.load(std::memory_order_acquire) == LoadState::Ready && d.VideoAtEnd() && d.AudioAtEnd();
	}

	bool PlayerCore::VideoDecoderAtEnd() const
	{
		const Impl& d = *m_impl;
		return d.loadState.load(std::memory_order_acquire) == LoadState::Ready && d.VideoAtEnd();
	}

	uint32_t PlayerCore::DecodeErrors() const { return m_impl->decodeErrors.load(std::memory_order_relaxed); }
}
