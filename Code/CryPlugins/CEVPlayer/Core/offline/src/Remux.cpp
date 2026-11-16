// ExtractAudioWebm / StripAudio: packet copy through libwebm's mkvmuxer (no re-encode).
#include "Internal.h"

#include <algorithm>
#include <cmath>
#include <new>

#include <mkvmuxer/mkvmuxer.h>
#include <mkvmuxer/mkvwriter.h>

namespace cev
{
	namespace offline
	{
		using namespace detail;

		namespace
		{
			constexpr uint64_t kMaxClusterDurationNs = 5000000000ull;

			void ConfigureSegment(mkvmuxer::Segment& seg, uint64_t timecodeScale, double durationSeconds)
			{
				seg.set_mode(mkvmuxer::Segment::kFile);
				seg.set_max_cluster_duration(kMaxClusterDurationNs);
				mkvmuxer::SegmentInfo* info = seg.GetSegmentInfo();
				info->set_timecode_scale(timecodeScale);
				info->set_writing_app("CEVPlayerOffline");
				info->set_muxing_app("CEVPlayerOffline");
				if (durationSeconds > 0.0)
					seg.set_duration(durationSeconds * 1e9 / static_cast<double>(timecodeScale));
			}
		}

		Result ExtractAudioWebm(const std::string& src, uint32_t audioTrackIndex, const std::string& dst, const Control& ctl)
		{
			try
			{
				Progress progress(ctl);
				progress.Report(0.0);
				std::unique_ptr<MkvFile> file;
				Result r = MkvFile::Open(src, file);
				if (!r)
					return r;
				r = file->ScanAudio(ctl);
				if (!r)
					return r;
				AudioTrackData* track = nullptr;
				r = CheckAudioTrack(*file, audioTrackIndex, track);
				if (!r)
					return r;
				progress.Report(0.1);

				OutFile out;
				r = out.Open(dst);
				if (!r)
					return r;
				mkvmuxer::MkvWriter writer(out.Handle());
				mkvmuxer::Segment seg;
				if (!seg.Init(&writer))
					return MakeResult(Status::IoError, "cannot initialise the WebM writer");
				ConfigureSegment(seg, file->TimecodeScale(), file->DurationSeconds());

				const AudioStreamInfo& info = track->info;
				const uint64_t number = seg.AddAudioTrack(static_cast<int32_t>(info.sampleRate), static_cast<int32_t>(info.channels), 1);
				mkvmuxer::AudioTrack* at = number != 0 ? static_cast<mkvmuxer::AudioTrack*>(seg.GetTrackByNumber(number)) : nullptr;
				if (at == nullptr)
					return MakeResult(Status::InvalidData, "cannot create the output audio track");
				at->set_codec_id(info.codec == AudioCodec::Opus ? mkvmuxer::Tracks::kOpusCodecId : mkvmuxer::Tracks::kVorbisCodecId);
				at->set_sample_rate(track->containerRate > 0.0 ? track->containerRate : static_cast<double>(info.sampleRate));
				at->set_channels(info.channels);
				if (!info.codecPrivate.empty() && !at->SetCodecPrivate(info.codecPrivate.data(), info.codecPrivate.size()))
					return MakeResult(Status::OutOfMemory, "cannot store the CodecPrivate");
				if (info.codecDelayNs != 0)
					at->set_codec_delay(info.codecDelayNs);
				if (info.seekPreRollNs != 0)
					at->set_seek_pre_roll(info.seekPreRollNs);
				if (track->defaultDurationNs != 0)
					at->set_default_duration(track->defaultDurationNs);
				if (!track->language.empty())
					at->set_language(track->language.c_str());
				if (!track->name.empty())
					at->set_name(track->name.c_str());
				seg.CuesTrack(number);

				std::vector<uint8_t> buf(std::max<uint32_t>(info.maxPacketSize, 1u));
				const size_t count = track->packets.size();
				for (size_t i = 0; i < count; ++i)
				{
					if (IsCancelled(ctl))
						return MakeResult(Status::Cancelled, "cancelled");
					const AudioPacketRef& p = track->packets[i];
					if (Failed(file->ReadAt(p.pos, buf.data(), p.size)))
						return MakeResult(Status::IoError, "cannot read audio packet " + std::to_string(i));
					const bool ok = p.discardPaddingNs != 0
						? seg.AddFrameWithDiscardPadding(buf.data(), p.size, p.discardPaddingNs, number, static_cast<uint64_t>(p.ptsNs), true)
						: seg.AddFrame(buf.data(), p.size, number, static_cast<uint64_t>(p.ptsNs), true);
					if (!ok)
						return MakeResult(Status::IoError, "the WebM writer rejected audio packet " + std::to_string(i));
					progress.Report(0.1 + 0.85 * static_cast<double>(i + 1) / static_cast<double>(count));
				}
				if (!seg.Finalize())
					return MakeResult(Status::IoError, "cannot finalize the WebM output");
				r = out.Commit();
				if (!r)
					return r;
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

		Result StripAudio(const std::string& src, const std::string& dst, const Control& ctl)
		{
			try
			{
				Progress progress(ctl);
				progress.Report(0.0);
				std::unique_ptr<MkvFile> file;
				Result r = MkvFile::Open(src, file);
				if (!r)
					return r;

				std::unique_ptr<IByteSource> source;
				if (Failed(FileByteSource::Open(src.c_str(), false, source)))
					return MakeResult(Status::IoError, "cannot open '" + src + "'");
				std::unique_ptr<WebmDemuxer> demux;
				StatusDetail detail = StatusDetail::None;
				const Status st = WebmDemuxer::Open(std::move(source), demux, &detail, ctl.cancel);
				if (st == Status::Cancelled)
					return MakeResult(Status::Cancelled, "cancelled");
				if (Failed(st))
					return MakeResult(st, std::string("cannot index '") + src + "' (" + StatusToString(st) + " / " + StatusDetailToString(detail) + ")");
				if (!demux->HasVideo())
					return MakeResult(Status::InvalidData, "the file has no (supported) video track");
				const VideoStreamInfo& vi = demux->Video();

				const mkvparser::Tracks* tracks = file->Segment()->GetTracks();
				const mkvparser::Track* t = tracks->GetTrackByNumber(static_cast<long>(vi.trackNumber));
				if (t == nullptr || t->GetType() != mkvparser::Track::kVideo)
					return MakeResult(Status::InvalidData, "video track not found in the track list");
				const mkvparser::VideoTrack* vt = static_cast<const mkvparser::VideoTrack*>(t);
				progress.Report(0.05);

				OutFile out;
				r = out.Open(dst);
				if (!r)
					return r;
				mkvmuxer::MkvWriter writer(out.Handle());
				mkvmuxer::Segment seg;
				if (!seg.Init(&writer))
					return MakeResult(Status::IoError, "cannot initialise the WebM writer");
				ConfigureSegment(seg, file->TimecodeScale(), demux->DurationSeconds());

				const uint64_t number = seg.AddVideoTrack(vi.width, vi.height, 1);
				mkvmuxer::VideoTrack* ot = number != 0 ? static_cast<mkvmuxer::VideoTrack*>(seg.GetTrackByNumber(number)) : nullptr;
				if (ot == nullptr)
					return MakeResult(Status::InvalidData, "cannot create the output video track");
				const char* codecId = t->GetCodecId();
				ot->set_codec_id(codecId != nullptr && codecId[0] != 0 ? codecId
					: (vi.codec == VideoCodec::VP8 ? mkvmuxer::Tracks::kVp8CodecId : mkvmuxer::Tracks::kVp9CodecId));
				size_t cpSize = 0;
				const unsigned char* cp = t->GetCodecPrivate(cpSize);
				if (cp != nullptr && cpSize > 0 && !ot->SetCodecPrivate(cp, cpSize))
					return MakeResult(Status::OutOfMemory, "cannot store the CodecPrivate");
				if (t->GetDefaultDuration() != 0)
					ot->set_default_duration(t->GetDefaultDuration());
				if (t->GetNameAsUTF8() != nullptr && t->GetNameAsUTF8()[0] != 0)
					ot->set_name(t->GetNameAsUTF8());
				if (vt->GetDisplayWidth() > 0)
					ot->set_display_width(static_cast<uint64_t>(vt->GetDisplayWidth()));
				if (vt->GetDisplayHeight() > 0)
					ot->set_display_height(static_cast<uint64_t>(vt->GetDisplayHeight()));
				if (vt->GetFrameRate() > 0.0)
					ot->set_frame_rate(vt->GetFrameRate());
				if (vi.hasAlpha)
				{
					ot->SetAlphaMode(mkvmuxer::VideoTrack::kAlpha);
					ot->set_max_block_additional_id(1);
				}
				if (const mkvparser::Colour* c = vt->GetColour())
				{
					mkvmuxer::Colour colour;
					const long long none = mkvparser::Colour::kValueNotPresent;
					if (c->matrix_coefficients != none) colour.set_matrix_coefficients(static_cast<uint64_t>(c->matrix_coefficients));
					if (c->bits_per_channel != none) colour.set_bits_per_channel(static_cast<uint64_t>(c->bits_per_channel));
					if (c->chroma_subsampling_horz != none) colour.set_chroma_subsampling_horz(static_cast<uint64_t>(c->chroma_subsampling_horz));
					if (c->chroma_subsampling_vert != none) colour.set_chroma_subsampling_vert(static_cast<uint64_t>(c->chroma_subsampling_vert));
					if (c->cb_subsampling_horz != none) colour.set_cb_subsampling_horz(static_cast<uint64_t>(c->cb_subsampling_horz));
					if (c->cb_subsampling_vert != none) colour.set_cb_subsampling_vert(static_cast<uint64_t>(c->cb_subsampling_vert));
					if (c->chroma_siting_horz != none) colour.set_chroma_siting_horz(static_cast<uint64_t>(c->chroma_siting_horz));
					if (c->chroma_siting_vert != none) colour.set_chroma_siting_vert(static_cast<uint64_t>(c->chroma_siting_vert));
					if (c->range != none) colour.set_range(static_cast<uint64_t>(c->range));
					if (c->transfer_characteristics != none) colour.set_transfer_characteristics(static_cast<uint64_t>(c->transfer_characteristics));
					if (c->primaries != none) colour.set_primaries(static_cast<uint64_t>(c->primaries));
					if (c->max_cll != none) colour.set_max_cll(static_cast<uint64_t>(c->max_cll));
					if (c->max_fall != none) colour.set_max_fall(static_cast<uint64_t>(c->max_fall));
					ot->SetColour(colour);
				}
				seg.CuesTrack(number);

				std::vector<uint8_t> frame(std::max<uint32_t>(vi.maxPacketSize, 1u));
				std::vector<uint8_t> additional(std::max<uint32_t>(vi.maxAdditionalSize, 1u));
				const uint32_t count = demux->VideoPacketCount();
				for (uint32_t i = 0; i < count; ++i)
				{
					if (IsCancelled(ctl))
						return MakeResult(Status::Cancelled, "cancelled");
					const PacketRef& p = demux->VideoPacket(i);
					if (p.ptsNs < 0)
						return MakeResult(Status::Unsupported, "negative video block times are not supported");
					Status rs = demux->ReadVideoPacket(i, frame.data(), static_cast<uint32_t>(frame.size()));
					bool ok = false;
					if (Succeeded(rs) && p.additionalSize > 0)
					{
						rs = demux->ReadVideoAdditional(i, additional.data(), static_cast<uint32_t>(additional.size()));
						if (Succeeded(rs))
							ok = seg.AddFrameWithAdditional(frame.data(), p.size, additional.data(), p.additionalSize, 1, number,
								static_cast<uint64_t>(p.ptsNs), p.keyframe);
					}
					else if (Succeeded(rs))
					{
						ok = seg.AddFrame(frame.data(), p.size, number, static_cast<uint64_t>(p.ptsNs), p.keyframe);
					}
					if (Failed(rs))
						return MakeResult(rs, "cannot read video packet " + std::to_string(i));
					if (!ok)
						return MakeResult(Status::IoError, "the WebM writer rejected video packet " + std::to_string(i));
					progress.Report(0.05 + 0.9 * static_cast<double>(i + 1) / static_cast<double>(count));
				}
				if (!seg.Finalize())
					return MakeResult(Status::IoError, "cannot finalize the WebM output");
				r = out.Commit();
				if (!r)
					return r;
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
