#include "CEVCore/PlayerCore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
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
#include "CEVCore/VideoDecoderBackend.h"

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

		constexpr uint8_t kModeVisible = static_cast<uint8_t>(VideoVisibility::Visible);
		constexpr uint8_t kModeDecodeOnly = static_cast<uint8_t>(VideoVisibility::DecodeOnly);
		constexpr uint8_t kModeStopped = static_cast<uint8_t>(VideoVisibility::Stopped);

		inline int64_t SteadyNs()
		{
			return std::chrono::duration_cast<std::chrono::nanoseconds>(Steady::now().time_since_epoch()).count();
		}
	}

	struct PlayerCore::Impl final : public IDecodeTask
	{
		explicit Impl(const PlayerConfig& config) : cfg(config)
		{
			clock = cfg.externalClock != nullptr ? cfg.externalClock : &ownClock;
			avOffset = ClampOffset(cfg.avOffsetSeconds);
			requestedRgba.store(cfg.outputRgba, std::memory_order_relaxed);
			rgbaWanted.store(cfg.outputRgba, std::memory_order_relaxed);
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
		std::unique_ptr<IVideoDecoderBackend> vdec; // M9: libvpx or Media Foundation
		uint32_t vThreads = 1;                      // libvpx threads (also for a fallback during playback)
		mutable std::mutex backendMutex;
		DecodeBackendInfo backendInfo;              // under backendMutex (GetDecodeBackend)
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
		uint32_t vInFlight = 0;                 // tickets with a slot submitted to vdec and not received yet
		bool vDrained = false;                  // vdec->Drain() sent at the end of the packets
		uint32_t vSkipUntil = 0;                // skip-decode packets before this index (backend fallback resume)
		double lastCommittedPts = -1.0;
		int abandoned[16];                      // Flush() output
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
		bool audioEndNotified = false;          // consumer thread: IClock::NotifyAudioEnded() sent since the last seek
		double avOffset = 0.0;                 // seconds, see PlayerCore::SetAvOffset()
		uint32_t loopCount = 0;
		uint64_t serial = 0;

		// ---------------------------------------------------------------- RGBA on demand (M8)
		std::atomic<bool> requestedRgba{true};  // SetOutputRgba() value (also before / between loads)
		std::atomic<bool> rgbaWanted{true};     // in effect: requested, or forced for a ring that is not packed

		// ---------------------------------------------------------------- visibility (M7)
		// consumer -> worker
		std::atomic<uint8_t> vMode{kModeVisible};       // VideoVisibility
		std::atomic<double> decodeUntil{0.0};           // picture time the clock has reached (Update / seek)
		std::atomic<uint32_t> resyncGen{0};             // +1 per Stopped -> Visible: the worker catches up
		std::atomic<bool> catchUpPending{false};        // set with resyncGen, cleared by the worker: no presenting meanwhile
		std::atomic<int64_t> catchUpRequestNs{0};       // steady time of the visible request (catch-up timing only)
		// worker
		uint32_t appliedResyncGen = 0;
		bool vCatchUp = false;
		uint32_t catchUpFrames = 0;
		// consumer
		double hiddenFor = 0.0;                         // seconds hidden in a row (sum of the caller's dt)
		double hiddenStopSeconds = 2.0;
		uint64_t decodeOnlyAtHide = 0;

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
			if (resyncGen.load(std::memory_order_acquire) != appliedResyncGen)
				ApplyResync();

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

		// Converts the slot to RGBA when RGBA is wanted (allocating the slot's buffer the first time after a switch),
		// otherwise frees its buffer. Called by the owner of the slot (worker: Writing, consumer: Held). Returns true
		// when the slot now holds this frame's RGBA.
		bool ConvertSlot(uint32_t index)
		{
			FrameRing::Slot& slot = ring.GetSlot(index);
			if (!rgbaWanted.load(std::memory_order_acquire))
			{
				if (slot.rgba != nullptr)
					ring.ReleaseRgba(index);
				slot.rgbaValid = false;
				return false;
			}
			if (!ring.EnsureRgba(index))
				return false; // out of memory: the frame has no RGBA (FrameView::rgba null)
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
			slot.rgbaValid = true;
			return true;
		}

		// Ring format / slot count shared by both backends (wantRgba as before M8: forced for non packable streams).
		Status InitRing(int width, int height, int csx, int csy, bool alpha, uint32_t extraSlots)
		{
			format = FrameFormat();
			format.width = width;
			format.height = height;
			format.chromaShiftX = csx;
			format.chromaShiftY = csy;
			format.hasAlpha = alpha;
			const bool wantRgba = requestedRgba.load(std::memory_order_acquire) || !format.Packable();
			rgbaWanted.store(wantRgba, std::memory_order_release);
			format.withRgba = wantRgba;
			info.width = width;
			info.height = height;
			return ring.Init(format, std::max(3u, cfg.ringSlots) + extraSlots);
		}

		// libvpx: decode until the first visible picture to learn the real format, then build the ring. `idx` = next
		// packet. Returns Ok, or the status for Fail() (InvalidData: no picture, IoError, Cancelled, ring errors).
		Status LoadFirstFrameLibvpx(const VideoStreamInfo& v, uint32_t& idx)
		{
			const uint32_t count = demux->VideoPacketCount();
			const uint32_t limit = std::min<uint32_t>(count, 64);
			bool got = false;
			for (idx = 0; idx < limit && !got; ++idx)
			{
				if (cancel.load(std::memory_order_relaxed))
					return Status::Cancelled;
				const PacketRef& ref = demux->VideoPacket(idx);
				if (Failed(demux->ReadVideoPacket(idx, videoBuf.data(), static_cast<uint32_t>(videoBuf.size()))))
					return Status::IoError;
				const bool alpha = v.hasAlpha && ref.additionalSize > 0;
				if (alpha && Failed(demux->ReadVideoAdditional(idx, additionalBuf.data(), static_cast<uint32_t>(additionalBuf.size()))))
					return Status::IoError;
				const DecodePacket cp{videoBuf.data(), ref.size, ref.keyframe};
				const DecodePacket ap{additionalBuf.data(), alpha ? ref.additionalSize : 0u, ref.keyframe};
				bool picture = false;
				if (Succeeded(LibvpxDecodeProbe(*vdec, cp, alpha ? &ap : nullptr, picture)) && picture)
				{
					got = true;
					break;
				}
			}
			if (!got)
				return Status::InvalidData;
			DecodedImageInfo di;
			if (!vdec->LastFrameInfo(di))
				return Status::InvalidData;
			Status st = InitRing(di.width, di.height, di.chromaShiftX, di.chromaShiftY, v.hasAlpha, 0);
			if (Failed(st))
				return st; // Unsupported (too large) / OutOfMemory (budget)
			vdec->SetOutputFormat(format);
			uint64_t epoch = 0;
			const int slot = ring.AcquireWrite(epoch);
			if (slot < 0)
				return Status::InvalidState;
			if (Failed(LibvpxWriteLastFrame(*vdec, format, ring.GetSlot(static_cast<uint32_t>(slot)))))
			{
				ring.Abandon(slot);
				return Status::InvalidData;
			}
			ConvertSlot(static_cast<uint32_t>(slot));
			ring.Commit(slot, epoch, demux->VideoPts(idx));
			lastCommittedPts = demux->VideoPts(idx);
			++idx;
			return Status::Ok;
		}

		static void WakeThunk(void* context) { static_cast<Impl*>(context)->WakeWorker(); }

		// Media Foundation: the ring is built from the container size (4:2:0) before decoding; packets are submitted
		// until the first picture came back. Any failure: everything is released (`got` false, `chosen.reason` says
		// why) and the caller continues with libvpx from packet 0. Returns Cancelled only when Unload() interrupted.
		Status LoadFirstFrameMf(const VideoStreamInfo& v, DecodeBackendInfo& chosen, uint32_t& idx, bool& got)
		{
			got = false;
			idx = 0;
			MediaFoundationConfig mc;
			mc.stream = v;
			mc.maxPacketSize = v.maxPacketSize;
			mc.maxAdditionalSize = v.maxAdditionalSize;
			mc.alphaThreads = std::max(1u, vThreads / 2);
			mc.wake = &WakeThunk;
			mc.wakeContext = this;
			if (Failed(CreateMediaFoundationBackend(mc, vdec, chosen.reason, sizeof(chosen.reason))))
			{
				vdec.reset();
				return Status::Ok;
			}
			const auto giveUp = [&](const char* why) {
				const DecodeBackendInfo bi = vdec->Info();
				std::snprintf(chosen.reason, sizeof(chosen.reason), "%s", bi.reason[0] != 0 ? bi.reason : why);
				const uint32_t n = vdec->Flush(abandoned, 16);
				for (uint32_t i = 0; i < n; ++i)
					ring.Abandon(abandoned[i]);
				vdec.reset();
				ring.Shutdown();
				vInFlight = 0;
				vDrained = false;
				idx = 0;
			};
			if (Failed(InitRing(v.width, v.height, 1, 1, v.hasAlpha, vdec->MaxInFlight() - 1)))
			{
				giveUp("Media Foundation: ring init failed");
				return Status::Ok;
			}
			vdec->SetOutputFormat(format);
			const uint32_t count = demux->VideoPacketCount();
			const Steady::time_point deadline = Steady::now() + std::chrono::seconds(5);
			for (;;)
			{
				if (cancel.load(std::memory_order_relaxed))
				{
					giveUp("cancelled");
					return Status::Cancelled;
				}
				bool progressed = false;
				DecodeTicket t;
				Status r = Status::Ok;
				while (!got && vdec->Receive(t, r))
				{
					progressed = true;
					--vInFlight;
					if (r == Status::Ok)
					{
						ConvertSlot(static_cast<uint32_t>(t.slotIndex));
						ring.Commit(t.slotIndex, t.epoch, t.pts);
						lastCommittedPts = t.pts;
						got = true;
					}
					else
					{
						ring.Abandon(t.slotIndex);
						if (r != Status::Cancelled)
						{
							giveUp(r == Status::InvalidData ? "Media Foundation: picture size differs from the container" : "Media Foundation: first picture failed");
							return Status::Ok;
						}
					}
				}
				if (got)
					break;
				if (idx < count && vInFlight < vdec->MaxInFlight() && idx < 64)
				{
					uint64_t epoch = 0;
					const int slot = ring.AcquireWrite(epoch);
					if (slot >= 0)
					{
						const PacketRef& ref = demux->VideoPacket(idx);
						const bool alpha = v.hasAlpha && ref.additionalSize > 0;
						if (Failed(demux->ReadVideoPacket(idx, videoBuf.data(), static_cast<uint32_t>(videoBuf.size()))) ||
							(alpha && Failed(demux->ReadVideoAdditional(idx, additionalBuf.data(), static_cast<uint32_t>(additionalBuf.size())))))
						{
							ring.Abandon(slot);
							giveUp("read error");
							return Status::Ok;
						}
						DecodeTicket nt;
						nt.epoch = epoch;
						nt.pts = demux->VideoPts(idx);
						nt.slotIndex = slot;
						nt.slot = &ring.GetSlot(static_cast<uint32_t>(slot));
						const DecodePacket cp{videoBuf.data(), ref.size, ref.keyframe};
						const DecodePacket ap{additionalBuf.data(), alpha ? ref.additionalSize : 0u, ref.keyframe};
						const Status st = vdec->Submit(cp, alpha ? &ap : nullptr, nt);
						if (st == Status::Ok)
						{
							++vInFlight;
							++idx;
							progressed = true;
						}
						else
						{
							ring.Abandon(slot);
							if (st != Status::Busy)
							{
								giveUp("Media Foundation: submit failed");
								return Status::Ok;
							}
						}
					}
				}
				else if (vInFlight > 0 && !vDrained && (idx >= count || idx >= 64))
				{
					vdec->Drain();
					vDrained = true;
				}
				else if (vInFlight == 0)
				{
					giveUp("Media Foundation: no picture in the first packets");
					return Status::Ok;
				}
				if (Steady::now() > deadline)
				{
					giveUp("Media Foundation: first picture timed out (5 s)");
					return Status::Ok;
				}
				if (!progressed)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			vDrained = false;
			return Status::Ok;
		}

		// Media Foundation stopped working during playback (device removed, ...): continue on libvpx from the frame
		// after the last committed one (or the pending seek target), warn once through the backend reason.
		bool SwitchToLibvpx()
		{
			const VideoStreamInfo& v = demux->Video();
			DecodeBackendInfo lb;
			{
				const DecodeBackendInfo bi = vdec->Info();
				std::snprintf(lb.reason, sizeof(lb.reason), "%s", bi.reason[0] != 0 ? bi.reason : "Media Foundation failed during playback");
			}
			const uint32_t n = vdec->Flush(abandoned, 16);
			for (uint32_t i = 0; i < n; ++i)
				ring.Abandon(abandoned[i]);
			vInFlight = 0;
			vDrained = false;
			vdec.reset();
			if (Failed(CreateLibvpxBackend(v, vThreads, vdec)))
			{
				vdec.reset();
				FailStream(Status::Unsupported, StatusDetail::VideoDecoderInit);
				return false;
			}
			vdec->SetOutputFormat(format);
			{
				std::lock_guard<std::mutex> lock(backendMutex);
				backendInfo = lb;
			}
			const uint32_t count = demux->VideoPacketCount();
			if (vSeekActive)
				vPos = demux->FindVideoKeyframe(vSeekTarget);
			else
			{
				uint32_t next = 0;
				while (next < count && demux->VideoPts(next) <= lastCommittedPts + 1e-9)
					++next;
				vSkipUntil = next;
				vPos = demux->FindVideoKeyframe(next < count ? demux->VideoPts(next) : lastCommittedPts);
				if (vPos > next)
					vPos = next;
			}
			vEof.store(vPos >= count, std::memory_order_relaxed);
			return true;
		}

		// Commits the completed tickets in submission order (stale epochs are freed by Commit). Returns true when a
		// ticket came back; `failover` when the backend asks to be replaced.
		bool ReceiveCompleted(bool& failover)
		{
			bool any = false;
			DecodeTicket t;
			Status r = Status::Ok;
			const bool timing = stats.IsEnabled();
			while (vdec->Receive(t, r))
			{
				any = true;
				if (vInFlight > 0)
					--vInFlight;
				if (r == Status::Ok)
				{
					Steady::time_point c0, c1;
					if (timing)
						c0 = Steady::now();
					const bool converted = ConvertSlot(static_cast<uint32_t>(t.slotIndex));
					if (timing)
					{
						c1 = Steady::now();
						const uint64_t luma = static_cast<uint64_t>(format.width) * static_cast<uint64_t>(format.height);
						const uint64_t chroma = 2ull * static_cast<uint64_t>(format.ChromaWidth()) * static_cast<uint64_t>(format.ChromaHeight());
						stats.NoteCopy(luma + chroma + (format.hasAlpha ? luma : 0ull), converted ? luma * 4ull : 0ull);
					}
					if (ring.Commit(t.slotIndex, t.epoch, t.pts))
					{
						lastCommittedPts = t.pts;
						if (timing)
							stats.RecordDecoded(t.decodeMs, t.copyMs, MsBetween(c0, c1), ring.InFlightCount(), t.readbackMs);
					}
					continue;
				}
				ring.Abandon(t.slotIndex);
				if (r == Status::BackendFailed)
					failover = true;
				else if (r == Status::DecodeError)
					decodeErrors.fetch_add(1, std::memory_order_relaxed);
				else if (r == Status::InvalidData)
				{
					decodeErrors.fetch_add(1, std::memory_order_relaxed);
					// Resolution / chroma format switched mid-stream: the ring is sized for the first frame, so stop
					// cleanly instead of freezing on the last frame.
					DecodedImageInfo di;
					if (vdec->LastFrameInfo(di) && (di.width != format.width || di.height != format.height ||
						di.chromaShiftX != format.chromaShiftX || di.chromaShiftY != format.chromaShiftY))
					{
						FailStream(Status::Unsupported, StatusDetail::StreamChanged);
						return any;
					}
				}
			}
			return any;
		}

		void FlushBackend()
		{
			if (!vdec)
				return;
			const uint32_t n = vdec->Flush(abandoned, 16);
			for (uint32_t i = 0; i < n; ++i)
				ring.Abandon(abandoned[i]);
			vInFlight = 0;
			vDrained = false;
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
				vThreads = threads;

				videoBuf.assign(std::max<uint32_t>(v.maxPacketSize, 1u), 0);
				additionalBuf.assign(std::max<uint32_t>(v.maxAdditionalSize, 1u), 0);
				firstVideoPts = v.firstPts;
				info.frameRate = v.frameRate;
				info.hasAlpha = v.hasAlpha;
				info.videoPacketCount = v.packetCount;
				info.colorStandard = ResolveColorStandard(cfg.colorStandard, v.matrixCoefficients, v.width, v.height);
				info.colorRange = ResolveColorRange(v.colorRange);
				info.chromaSiting = ResolveChromaSiting(v.chromaSitingHorz, v.chromaSitingVert);

				// M9: backend choice. Gpu / Auto try Media Foundation; every failure falls back to libvpx with a reason.
				DecodeBackendInfo chosen;
				bool tryMf = false;
				if (cfg.decodeMode != DecodeMode::Cpu)
				{
					if (v.codec != VideoCodec::VP9)
						std::snprintf(chosen.reason, sizeof(chosen.reason), "%s", "VP8 has no hardware decode: libvpx");
					else if (cfg.decodeMode == DecodeMode::Gpu || static_cast<uint32_t>(std::max(0, v.height)) >= cfg.autoMinHeight)
						tryMf = true;
				}
				uint32_t idx = 0;
				bool got = false;
				if (tryMf)
				{
					const Status mf = LoadFirstFrameMf(v, chosen, idx, got);
					if (mf == Status::Cancelled)
						return Fail(Status::Cancelled, StatusDetail::FirstFrame);
					if (got)
						chosen = vdec->Info();
				}
				if (!got)
				{
					st = CreateLibvpxBackend(v, threads, vdec);
					if (Failed(st))
						return Fail(st, StatusDetail::VideoDecoderInit);
					const Status lv = LoadFirstFrameLibvpx(v, idx);
					if (lv != Status::Ok)
						return Fail(lv, lv == Status::InvalidState ? StatusDetail::RingInit : (lv == Status::Unsupported || lv == Status::OutOfMemory ? StatusDetail::RingInit : StatusDetail::FirstFrame));
					DecodeBackendInfo lb = vdec->Info();
					std::snprintf(lb.reason, sizeof(lb.reason), "%s", chosen.reason);
					chosen = lb;
				}
				{
					std::lock_guard<std::mutex> lock(backendMutex);
					backendInfo = chosen;
				}
				info.packedLayout = ring.IsPacked();
				vPos = idx;
				vEof.store(vPos >= demux->VideoPacketCount() && vInFlight == 0, std::memory_order_relaxed);
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
				FlushBackend();
				vSkipUntil = 0;
				vPos = demux->FindVideoKeyframe(t);
				vSeekActive = true;
				vSeekTarget = t;
				vEof.store(false, std::memory_order_relaxed);
			}
			if (hasAudio)
				ApplyAudioSeek(t);
			appliedSeekGen.store(gen, std::memory_order_release);
		}

		// Stopped -> Visible: continue from where decoding stopped when the keyframe before the clock is not later
		// (the decoder state is still valid), otherwise jump to that keyframe; then skip-decode up to the clock.
		void ApplyResync()
		{
			appliedResyncGen = resyncGen.load(std::memory_order_acquire);
			vCatchUp = false;
			if (hasVideo)
			{
				const uint32_t k = demux->FindVideoKeyframe(decodeUntil.load(std::memory_order_relaxed));
				if (k > vPos)
				{
					FlushBackend();
					vSkipUntil = 0;
					vPos = k;
					vEof.store(false, std::memory_order_relaxed);
					ring.FlushReady(); // frames before the jump are stale (the consumer does not present meanwhile)
				}
				vCatchUp = !vEof.load(std::memory_order_relaxed);
				catchUpFrames = 0;
			}
			if (!vCatchUp)
				catchUpPending.store(false, std::memory_order_release);
		}

		// ================================================================ decode steps (worker)
		bool VideoStep()
		{
			// (1) completed pictures first, in submission order
			bool failover = false;
			bool more = ReceiveCompleted(failover);
			if (faulted.load(std::memory_order_relaxed))
				return false;
			if (failover)
			{
				if (!SwitchToLibvpx())
					return false;
				more = true;
			}
			if (vEof.load(std::memory_order_relaxed))
				return false;
			const uint32_t count = demux->VideoPacketCount();
			if (vPos >= count)
			{
				if (vInFlight > 0)
				{
					if (!vDrained)
					{
						vdec->Drain();
						vDrained = true;
					}
					return more; // the backend wakes us when the rest comes back
				}
				vEof.store(true, std::memory_order_release);
				return false;
			}

			const uint32_t idx = vPos;
			const uint8_t mode = vMode.load(std::memory_order_acquire);
			if (mode == kModeStopped)
				return more; // hidden for long: no video decoding at all (SetVisible wakes us)
			const bool hasNext = idx + 1 < count;
			const double until = decodeUntil.load(std::memory_order_relaxed);
			// Hidden: decode only the packets the clock has passed (the covering frame is left for the output when
			// the picture becomes visible again), no ring slot. Catch-up: the same up to the moving clock.
			const bool hiddenSkip = mode == kModeDecodeOnly;
			if (hiddenSkip && (!hasNext || demux->VideoPts(idx + 1) > until + 1e-9))
				return more; // caught up with the clock: Update() wakes us every frame
			const bool catchSkip = !hiddenSkip && vCatchUp && hasNext && demux->VideoPts(idx + 1) <= until + 1e-9;
			const bool skipping = hiddenSkip || catchSkip || idx < vSkipUntil ||
				(vSeekActive && hasNext && demux->VideoPts(idx + 1) <= vSeekTarget + 1e-9);
			if (!skipping && vCatchUp)
			{
				// The catch-up reached the clock: the next frame is for output. Frames still queued from before are
				// older than it when packets were skipped (drop them, the ring may be full of them); without skipping
				// they are the valid continuation and the consumer presents them first.
				vCatchUp = false;
				if (catchUpFrames > 0)
					ring.FlushReady();
				const int64_t ns = SteadyNs() - catchUpRequestNs.load(std::memory_order_relaxed);
				stats.NoteCatchUp(static_cast<float>(static_cast<double>(std::max<int64_t>(ns, 1)) * 1e-6), catchUpFrames);
				catchUpPending.store(false, std::memory_order_release); // the consumer presents again
			}

			// (2) the next packet: a slot unless skipping (ring full or too many in flight: wait)
			int slot = -1;
			uint64_t epoch = 0;
			if (!skipping)
			{
				if (vInFlight >= vdec->MaxInFlight())
					return more;
				slot = ring.AcquireWrite(epoch);
				if (slot < 0)
					return more; // ring full: idle until the consumer frees a slot and wakes us
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
			Steady::time_point t0, t1;
			if (timing)
				t0 = Steady::now();
			if (Succeeded(st))
			{
				DecodeTicket ticket;
				ticket.epoch = epoch;
				ticket.pts = demux->VideoPts(idx);
				ticket.slotIndex = slot;
				ticket.slot = slot >= 0 ? &ring.GetSlot(static_cast<uint32_t>(slot)) : nullptr;
				const DecodePacket cp{videoBuf.data(), ref.size, ref.keyframe};
				const DecodePacket ap{additionalBuf.data(), alpha ? ref.additionalSize : 0u, ref.keyframe};
				st = vdec->Submit(cp, alpha ? &ap : nullptr, ticket);
			}
			if (timing)
				t1 = Steady::now();
			if (st == Status::Busy)
			{
				if (slot >= 0)
					ring.Abandon(slot);
				return more; // the backend wakes us when it has room
			}
			if (st == Status::BackendFailed)
			{
				if (slot >= 0)
					ring.Abandon(slot);
				return SwitchToLibvpx();
			}
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
				if (hiddenSkip || catchSkip)
				{
					stats.NoteDecodeOnly();
					if (catchSkip)
						++catchUpFrames;
					if (timing && hiddenSkip)
						stats.RecordDecoded(MsBetween(t0, t1), 0.0f, 0.0f, ring.InFlightCount());
				}
				++vPos;
				return true;
			}
			vSeekActive = false;

			if (Succeeded(st))
				++vInFlight; // comes back through Receive (libvpx: at once)
			else
				ring.Abandon(slot);

			++vPos;
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
			decodeUntil.store(std::max(t - avOffset, firstVideoPts), std::memory_order_relaxed);
			if (hasVideo)
				ring.FlushReady();
			clock->Seek(t);
			audioEndNotified = false;
			if (!hasAudio)
			{
				clock->NotifyAudioEnded(); // nothing to wait for
				audioEndNotified = true;
			}
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
			// The backend may call WakeWorker() from its own thread until it is gone (Media Foundation: joins it).
			vdec.reset();
			handle = DecodePool::kInvalidHandle;
			pool.reset();
			{
				// Waits for a ReadAudioResampled() call that is still inside the resampler.
				std::lock_guard<std::mutex> lock(resMutex);
				resReady = false;
				resampler.Shutdown();
				std::vector<float>().swap(resIn);
			}

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
			vInFlight = 0;
			vDrained = false;
			vSkipUntil = 0;
			lastCommittedPts = -1.0;
			{
				std::lock_guard<std::mutex> lock(backendMutex);
				backendInfo = DecodeBackendInfo();
			}
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
			audioEndNotified = false;
			serial = 0;
			vMode.store(kModeVisible);
			decodeUntil.store(0.0);
			resyncGen.store(0);
			appliedResyncGen = 0;
			catchUpPending.store(false);
			vCatchUp = false;
			catchUpFrames = 0;
			hiddenFor = 0.0;
			decodeOnlyAtHide = 0;
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
		if (!(rate > 0.0))
			return Status::InvalidArgument; // also NaN
		// The resampler clamps to its own range; clamp here as well so the clock and the video use the same rate.
		rate = std::min(std::max(rate, kMinRate), kMaxRate);
		m_impl->audioRate.store(rate, std::memory_order_relaxed);
		m_impl->clock->SetRate(rate);
		return Status::Ok;
	}

	double PlayerCore::GetRate() const { return m_impl->clock->Rate(); }

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

		const uint8_t mode = d.vMode.load(std::memory_order_relaxed);
		// hidden or catching up: nothing is presented, the end follows the clock only
		const bool outputOff = mode != kModeVisible || d.catchUpPending.load(std::memory_order_acquire);
		d.decodeUntil.store(std::max(tVideo, d.firstVideoPts), std::memory_order_relaxed);
		if (d.hasVideo && !outputOff)
		{
			uint32_t dropped = 0;
			const int slot = d.ring.PopForTime(std::max(tVideo, d.firstVideoPts), kPresentEpsilon, dropped);
			if (dropped > 0)
				d.stats.NoteDropped(dropped);
			if (slot >= 0)
			{
				// A frame committed just before RGBA was switched on: convert it here (Held, this thread owns it).
				if (d.rgbaWanted.load(std::memory_order_acquire) && !d.ring.GetSlot(static_cast<uint32_t>(slot)).rgbaValid)
					d.ConvertSlot(static_cast<uint32_t>(slot));
				newFrame = true;
				++d.serial;
				d.stats.NotePresented();
			}
			if ((slot >= 0 || dropped > 0) && !d.vEof.load(std::memory_order_relaxed))
				d.WakeWorker();
		}
		else if (d.hasVideo && mode == kModeDecodeOnly)
		{
			d.WakeWorker(); // the hidden decoder follows the clock
		}
		if (d.hasAudio && !d.aEof.load(std::memory_order_relaxed) && d.aring.FreeFrames() >= d.aring.Capacity() / 4)
			d.WakeWorker();
		if (!d.audioEndNotified && d.AudioAtEnd() && (!d.hasAudio || d.aring.Buffered() == 0))
		{
			// Audio is over (shorter than the video, or none): a clock that holds for audio must stop holding.
			d.clock->NotifyAudioEnded();
			d.audioEndNotified = true;
		}

		if (d.State() == PlayerState::Playing && t >= d.info.durationSeconds - kEndEpsilon)
		{
			// Hidden (or catching up): the clock alone decides, the decoder may be stopped. Visible: the last
			// frame must have been presented as well.
			bool videoDone = !d.hasVideo || outputOff || (d.VideoAtEnd() && d.ring.ReadyCount() == 0);
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
		out.rgba = s.rgbaValid ? s.rgba : nullptr;
		out.rgbaStride = s.rgbaValid ? s.rgbaStride : 0;
		out.packed = s.packed;
		out.packedWidth = d.ring.PackedWidth();
		out.packedHeight = d.ring.PackedHeight();
		out.rectY = d.ring.PackedPlane(0);
		out.rectU = d.ring.PackedPlane(1);
		out.rectV = d.ring.PackedPlane(2);
		out.rectA = d.ring.PackedPlane(3);
		out.width = d.format.width;
		out.height = d.format.height;
		out.chromaShiftX = d.format.chromaShiftX;
		out.chromaShiftY = d.format.chromaShiftY;
		out.pts = s.pts;
		out.serial = d.serial;
		return true;
	}

	void PlayerCore::NoteUpload(uint64_t bytes) { m_impl->stats.NoteUpload(bytes); }

	Status PlayerCore::SetOutputRgba(bool enabled)
	{
		Impl& d = *m_impl;
		d.requestedRgba.store(enabled, std::memory_order_release);
		if (d.loadState.load(std::memory_order_acquire) != LoadState::Ready || !d.hasVideo)
			return Status::Ok; // remembered: the load step reads it
		if (!d.ring.IsPacked())
			return enabled ? Status::Ok : Status::Unsupported; // separate planes: RGBA is the only output
		if (d.rgbaWanted.load(std::memory_order_acquire) == enabled)
			return Status::Ok;
		d.rgbaWanted.store(enabled, std::memory_order_release);
		const int cur = d.ring.Current(); // Held: only this thread touches it
		if (enabled)
		{
			// The frame on screen gets its RGBA now (one conversion on this thread), so a consumer that needs RGBA8
			// never waits for the next frame.
			if (cur >= 0 && !d.ring.GetSlot(static_cast<uint32_t>(cur)).rgbaValid && !d.ConvertSlot(static_cast<uint32_t>(cur)))
				return Status::OutOfMemory;
		}
		else
		{
			if (cur >= 0)
				d.ring.ReleaseRgba(static_cast<uint32_t>(cur));
			d.ring.ReleaseFreeRgba(); // Ready / Writing slots: freed by the worker when it reuses them
		}
		return Status::Ok;
	}

	bool PlayerCore::GetOutputRgba() const
	{
		const Impl& d = *m_impl;
		if (d.loadState.load(std::memory_order_acquire) == LoadState::Ready && d.hasVideo)
			return d.rgbaWanted.load(std::memory_order_acquire);
		return d.requestedRgba.load(std::memory_order_acquire);
	}

	uint32_t PlayerCore::RgbaBufferCount() const
	{
		return m_impl->ring.IsInitialized() ? m_impl->ring.RgbaBufferCount() : 0u;
	}

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
	void PlayerCore::ResetStatsWindow() { m_impl->stats.ResetWindow(); }

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

	DecodeBackendInfo PlayerCore::GetDecodeBackend() const
	{
		std::lock_guard<std::mutex> lock(m_impl->backendMutex);
		return m_impl->backendInfo;
	}

	bool PlayerCore::VideoDecoderAtEnd() const
	{
		const Impl& d = *m_impl;
		return d.loadState.load(std::memory_order_acquire) == LoadState::Ready && d.VideoAtEnd();
	}

	void PlayerCore::SetVisible(bool visible, double dtSeconds)
	{
		Impl& d = *m_impl;
		const uint8_t mode = d.vMode.load(std::memory_order_relaxed);
		if (visible)
		{
			d.hiddenFor = 0.0;
			if (mode == kModeDecodeOnly)
			{
				// Frames still queued from before hiding are stale once the hidden decoder moved on; otherwise
				// (e.g. paused while hidden) they are exactly what is due.
				if (d.stats.DecodeOnlyCount() != d.decodeOnlyAtHide)
					d.ring.FlushReady();
				d.vMode.store(kModeVisible, std::memory_order_release);
				d.WakeWorker();
			}
			else if (mode == kModeStopped)
			{
				d.catchUpRequestNs.store(SteadyNs(), std::memory_order_relaxed);
				d.catchUpPending.store(true, std::memory_order_release);
				d.resyncGen.fetch_add(1, std::memory_order_acq_rel);
				d.vMode.store(kModeVisible, std::memory_order_release);
				d.WakeWorker();
			}
			return;
		}
		if (std::isfinite(dtSeconds) && dtSeconds > 0.0)
			d.hiddenFor += dtSeconds;
		if (mode == kModeVisible)
		{
			d.decodeOnlyAtHide = d.stats.DecodeOnlyCount();
			d.vMode.store(kModeDecodeOnly, std::memory_order_release);
			d.WakeWorker();
		}
		if (d.hiddenStopSeconds > 0.0 && d.hiddenFor >= d.hiddenStopSeconds - 1e-9)
			d.vMode.store(kModeStopped, std::memory_order_release);
	}

	void PlayerCore::SetHiddenStopSeconds(double seconds)
	{
		m_impl->hiddenStopSeconds = std::isfinite(seconds) && seconds > 0.0 ? seconds : 0.0;
	}

	VideoVisibility PlayerCore::GetVisibility() const
	{
		return static_cast<VideoVisibility>(m_impl->vMode.load(std::memory_order_relaxed));
	}

	uint32_t PlayerCore::DecodeErrors() const { return m_impl->decodeErrors.load(std::memory_order_relaxed); }
}
