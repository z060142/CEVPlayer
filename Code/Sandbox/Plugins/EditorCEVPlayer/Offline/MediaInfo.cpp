#include "StdAfx.h"
#include "Offline/MediaInfo.h"

#include <memory>

#include "CEVCore/IByteSource.h"
#include "CEVCore/Status.h"
#include "CEVCore/WebmDemuxer.h"

namespace CEVEditor
{
	bool ReadBasicMediaInfo(const std::string& absUtf8Path, SMediaInfo& out, std::string& error, const std::atomic<bool>* cancel)
	{
		out = SMediaInfo();
		std::unique_ptr<cev::IByteSource> source;
		cev::Status status = cev::FileByteSource::Open(absUtf8Path.c_str(), false, source);
		if (cev::Failed(status))
		{
			error = std::string("cannot open ") + absUtf8Path + ": " + cev::StatusToString(status);
			return false;
		}
		std::unique_ptr<cev::WebmDemuxer> demuxer;
		cev::StatusDetail detail = cev::StatusDetail::None;
		status = cev::WebmDemuxer::Open(std::move(source), demuxer, &detail, cancel);
		if (cev::Failed(status) || !demuxer)
		{
			error = std::string("not a usable WebM file: ") + cev::StatusToString(status) + " / " + cev::StatusDetailToString(detail);
			return false;
		}
		if (demuxer->HasVideo())
		{
			const cev::VideoStreamInfo& v = demuxer->Video();
			out.videoCodec = v.codec == cev::VideoCodec::VP9 ? "vp9" : "vp8";
			out.width = v.width;
			out.height = v.height;
			out.fps = v.frameRate;
			out.maxKeyframeIntervalSec = demuxer->MaxKeyframeIntervalSeconds();
			out.hasAlpha = v.hasAlpha;
		}
		out.durationSec = demuxer->DurationSeconds();
		if (demuxer->HasAudio())
		{
			const cev::AudioStreamInfo& a = demuxer->Audio();
			SAudioTrackInfo track;
			track.codec = a.codec == cev::AudioCodec::Opus ? "opus" : "vorbis";
			track.sampleRate = static_cast<int>(a.sampleRate);
			track.channels = static_cast<int>(a.channels);
			track.codecDelaySec = static_cast<double>(a.codecDelayNs) * 1e-9;
			track.startOffsetSec = a.firstPts - track.codecDelaySec;
			out.audioTracks.push_back(track);
		}
		out.valid = demuxer->HasVideo();
		if (!out.valid)
			error = "the file has no video track";
		return out.valid;
	}
}

namespace CEVEditor
{
	bool ProbeMediaFile(const std::string& absUtf8Path, SMediaInfo& out, std::string& error, const std::atomic<bool>* cancel)
	{
		IOfflineOps& ops = GetOfflineOps();
		if (ops.IsAvailable())
		{
			SOpContext ctx;
			ctx.cancel = cancel;
			SMediaInfo probed;
			const SOpResult r = ops.ProbeMedia(absUtf8Path, probed, ctx);
			if (r.Ok() && probed.valid)
			{
				out = probed;
				return true;
			}
		}
		return ReadBasicMediaInfo(absUtf8Path, out, error, cancel);
	}
}
