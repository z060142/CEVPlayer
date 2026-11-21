// IOfflineOps over CEVPlayerOffline (Core/offline): the real implementation of the offline hooks.
// Runs on job threads only (the job runner); CEVPlayerOffline never touches the engine.
#include "StdAfx.h"
#include "Offline/IOfflineOps.h"
#include "Offline/OfflineOpsReal.h"

#include "CEVOffline/Offline.h"

namespace CEVEditor
{
	namespace
	{
		SOpResult ToResult(const cev::offline::Result& r)
		{
			SOpResult out;
			out.message = r.message;
			if (r.Ok())
				out.status = EOpStatus::Ok;
			else if (r.status == cev::Status::Cancelled)
				out.status = EOpStatus::Cancelled;
			else
				out.status = EOpStatus::Failed;
			return out;
		}

		cev::offline::Control ToControl(const SOpContext& ctx)
		{
			cev::offline::Control c;
			c.cancel = ctx.cancel;
			if (ctx.progress)
			{
				const auto progress = ctx.progress;
				c.progress = [progress](double f) { progress(static_cast<float>(f)); };
			}
			return c;
		}

		class CRealOfflineOps final : public IOfflineOps
		{
		public:
			const char* Name() const override { return "CEVPlayerOffline"; }
			bool IsAvailable() const override { return true; }

			SOpResult ProbeMedia(const std::string& absSrc, SMediaInfo& out, const SOpContext& ctx) override
			{
				out = SMediaInfo();
				cev::offline::MediaProbe probe;
				const SOpResult r = ToResult(cev::offline::ProbeMedia(absSrc, probe, ToControl(ctx)));
				if (!r.Ok())
					return r;
				out.valid = probe.hasVideo;
				out.videoCodec = probe.videoCodec == cev::VideoCodec::VP9 ? "vp9" : "vp8";
				out.width = probe.width;
				out.height = probe.height;
				out.fps = probe.frameRate;
				out.maxKeyframeIntervalSec = probe.maxKeyframeIntervalSeconds;
				out.durationSec = probe.durationSeconds;
				out.hasAlpha = probe.hasAlpha;
				for (const cev::offline::AudioTrackProbe& a : probe.audioTracks)
				{
					SAudioTrackInfo t;
					t.codec = a.codec == cev::AudioCodec::Opus ? "opus" : a.codec == cev::AudioCodec::Vorbis ? "vorbis" : a.codecId;
					t.sampleRate = static_cast<int>(a.sampleRate);
					t.channels = static_cast<int>(a.channels);
					t.startOffsetSec = a.startOffsetSeconds;
					t.codecDelaySec = a.codecDelaySeconds;
					out.audioTracks.push_back(t);
				}
				return r;
			}

			SOpResult ExtractAudioWebm(const std::string& absSrc, int trackIndex, const std::string& absDst, const SOpContext& ctx) override
			{
				return ToResult(cev::offline::ExtractAudioWebm(absSrc, static_cast<uint32_t>(trackIndex), absDst, ToControl(ctx)));
			}

			SOpResult ExportWav(const std::string& absSrc, int trackIndex, const std::string& absDst, const SOpContext& ctx) override
			{
				cev::offline::AudioExportInfo info;
				SOpResult r = ToResult(cev::offline::ExportWav(absSrc, static_cast<uint32_t>(trackIndex), absDst, ToControl(ctx), &info));
				if (r.Ok())
				{
					ctx.Log("WAV: " + std::to_string(info.frames) + " frames, " + std::to_string(info.sampleRate) + " Hz, " + std::to_string(info.channels)
						+ " ch, start " + std::to_string(info.startFrames) + (info.downmixed ? " (mixed down to stereo)" : ""));
				}
				return r;
			}

			SOpResult ExportOggVorbis(const std::string& absSrc, int trackIndex, const std::string& absDst, float quality, const SOpContext& ctx) override
			{
				cev::offline::AudioExportInfo info;
				SOpResult r = ToResult(cev::offline::ExportOggVorbis(absSrc, static_cast<uint32_t>(trackIndex), absDst, quality, ToControl(ctx), &info));
				if (r.Ok())
					ctx.Log(std::string("Ogg: ") + std::to_string(info.frames) + " frames, " + (info.remuxed ? "lossless remux" : "re-encoded"));
				return r;
			}

			SOpResult StripAudio(const std::string& absSrc, const std::string& absDst, const SOpContext& ctx) override
			{
				return ToResult(cev::offline::StripAudio(absSrc, absDst, ToControl(ctx)));
			}
		};

		CRealOfflineOps g_real;
	}

	IOfflineOps* GetRealOfflineOps()
	{
		return &g_real;
	}
}
