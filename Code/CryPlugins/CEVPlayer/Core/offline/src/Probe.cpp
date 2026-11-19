#include "Internal.h"

#include <new>

namespace cev
{
	namespace offline
	{
		using namespace detail;

		Result ProbeMedia(const std::string& path, MediaProbe& out, const Control& ctl)
		{
			try
			{
				out = MediaProbe();
				Progress progress(ctl);
				progress.Report(0.0);

				std::unique_ptr<MkvFile> file;
				Result r = MkvFile::Open(path, file);
				if (!r)
					return r;
				if (IsCancelled(ctl))
					return MakeResult(Status::Cancelled, "cancelled");
				progress.Report(0.2);
				r = file->ScanAudio(ctl);
				if (!r)
					return r;
				progress.Report(0.7);

				// Video + container facts: Core's demuxer (it also knows the alpha mode).
				std::unique_ptr<IByteSource> src;
				if (Failed(FileByteSource::Open(path.c_str(), false, src)))
					return MakeResult(Status::IoError, "cannot open '" + path + "'");
				std::unique_ptr<WebmDemuxer> demux;
				StatusDetail detail = StatusDetail::None;
				const Status st = WebmDemuxer::Open(std::move(src), demux, &detail, ctl.cancel);
				if (st == Status::Cancelled)
					return MakeResult(Status::Cancelled, "cancelled");
				if (Failed(st))
					return MakeResult(st, std::string("'") + path + "' has no playable VP8 / VP9 / Vorbis / Opus stream (" + StatusToString(st) +
						" / " + StatusDetailToString(detail) + ")");

				out.fileSize = file->FileSize();
				out.durationSeconds = demux->DurationSeconds();
				if (demux->HasVideo())
				{
					const VideoStreamInfo& v = demux->Video();
					out.hasVideo = true;
					out.videoCodec = v.codec;
					out.width = v.width;
					out.height = v.height;
					out.frameRate = v.frameRate;
					out.hasAlpha = v.hasAlpha;
					out.videoFrameCount = v.packetCount;
					out.videoFirstPts = v.firstPts;
					out.maxKeyframeIntervalSeconds = demux->MaxKeyframeIntervalSeconds();
				}

				for (AudioTrackData& a : file->Audio())
				{
					AudioTrackProbe p;
					p.index = a.index;
					p.trackNumber = a.trackNumber;
					p.codec = a.info.codec;
					p.codecId = a.codecId;
					p.sampleRate = a.info.sampleRate;
					p.channels = a.info.channels;
					p.packetCount = a.info.packetCount;
					p.firstPacketPts = a.info.firstPts;
					p.lastPacketPts = a.info.lastPts;
					p.codecDelaySeconds = static_cast<double>(a.info.codecDelayNs) * 1e-9;
					p.seekPreRollSeconds = static_cast<double>(a.info.seekPreRollNs) * 1e-9;
					p.language = a.language;
					p.name = a.name;

					if (a.info.codec != AudioCodec::None && !a.laced && !a.packets.empty())
					{
						std::unique_ptr<AudioDecoder> dec;
						if (Succeeded(AudioDecoder::Create(a.info, dec, nullptr)))
						{
							p.decodedSampleRate = dec->SampleRate();
							p.channels = dec->Channels();
							p.preSkipFrames = dec->PreSkipFrames();
							p.startFrames = ComputeStartFrames(a.info, p.decodedSampleRate, p.preSkipFrames, a.packets.front().ptsNs);
							p.startOffsetSeconds = static_cast<double>(p.startFrames) / p.decodedSampleRate;
						}
					}
					if (!a.packets.empty())
					{
						double spacing = 0.0;
						if (a.packets.size() > 1)
							spacing = (a.info.lastPts - a.info.firstPts) / static_cast<double>(a.packets.size() - 1);
						else if (a.defaultDurationNs > 0)
							spacing = static_cast<double>(a.defaultDurationNs) * 1e-9;
						p.durationSeconds = a.info.lastPts + spacing;
					}
					out.audioTracks.push_back(std::move(p));
				}

				progress.Report(1.0);
				return Okay();
			}
			catch (const std::bad_alloc&)
			{
				return MakeResult(Status::OutOfMemory, "out of memory");
			}
			catch (...)
			{
				return MakeResult(Status::InvalidState, "internal error (unexpected exception)");
			}
		}
	}
}
