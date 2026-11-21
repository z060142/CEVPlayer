// CEVPlayerOffline - offline media tools for the editor (CRYENGINE independent, not linked by the runtime
// plugin). Everything operates on WebM files (VP8 / VP9 video, Vorbis / Opus audio) and is built on top of
// CEVPlayerCore (demuxer, audio decoder, resampler), libwebm (mkvparser / mkvmuxer), libogg and libvorbis.
//
// Rules for every function in this header:
//   * No exception leaves it. Failures are reported as Result{status, message}; message is a human readable
//     English sentence for the log.
//   * Paths are UTF-8.
//   * Output is written to "<dst>.tmp" and renamed over <dst> on success. On failure or cancellation the
//     partial file is deleted and <dst> is never touched (an existing <dst> survives a failed run).
//   * Control::cancel (optional) is polled between packets, Control::progress (optional) receives a fraction
//     0..1 (monotonic, 1.0 on success) on the calling thread.
//   * audioTrackIndex counts the audio tracks of the file in Matroska track order (the order of
//     MediaProbe::audioTracks), not the Matroska track number.
//
// Timeline convention (same as PlayerCore, see Core/src/PlayerCore.cpp ApplyAudioSeek): the container time
// axis is absolute, the first KEPT sample of an audio track sits at
//     startFrames = llround((pts0 - CodecDelay) * rate) + preSkipFrames         (rate = decoder rate)
// where pts0 is the block time of the track's first packet and preSkipFrames is the Opus pre-skip that the
// decoder discards (0 for Vorbis). WAV / OGG exports place that sample at output index startFrames, so output
// sample 0 is video t = 0 (silence is prepended for startFrames > 0, samples are dropped for < 0).
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "CEVCore/Status.h"
#include "CEVCore/WebmDemuxer.h" // VideoCodec, AudioCodec

namespace cev
{
	namespace offline
	{
		struct Result
		{
			Status status = Status::Ok;
			std::string message;

			bool Ok() const { return status == Status::Ok; }
			explicit operator bool() const { return status == Status::Ok; }
		};

		struct Control
		{
			const std::atomic<bool>* cancel = nullptr;
			std::function<void(double)> progress;
		};

		struct AudioTrackProbe
		{
			uint32_t index = 0;                  // among the audio tracks of the file
			uint32_t trackNumber = 0;            // Matroska track number
			AudioCodec codec = AudioCodec::None; // None: a codec CEVPlayer cannot decode (codecId tells which)
			std::string codecId;                 // e.g. "A_OPUS", "A_VORBIS"
			uint32_t sampleRate = 0;             // container rate
			uint32_t decodedSampleRate = 0;      // rate of the decoder output (48000 for Opus), 0 when unsupported
			uint32_t channels = 0;
			uint32_t packetCount = 0;
			double firstPacketPts = 0.0;         // seconds, block time of the first packet
			double lastPacketPts = 0.0;
			double codecDelaySeconds = 0.0;      // Matroska CodecDelay (Opus pre-skip)
			double seekPreRollSeconds = 0.0;
			uint32_t preSkipFrames = 0;          // decoder pre-skip in decoded frames (Opus: CodecDelay / OpusHead)
			int64_t startFrames = 0;             // see the header comment; frames at decodedSampleRate
			double startOffsetSeconds = 0.0;     // startFrames / decodedSampleRate: where the audio begins on the video timeline
			double durationSeconds = 0.0;        // end of the audio on the video timeline (last block time + its length)
			std::string language;                // Matroska Language as written in the file, empty when absent
			std::string name;                    // track Name, empty when absent
		};

		struct MediaProbe
		{
			bool hasVideo = false;
			VideoCodec videoCodec = VideoCodec::None;
			int width = 0;
			int height = 0;
			double frameRate = 0.0;
			bool hasAlpha = false;
			uint32_t videoFrameCount = 0;
			double videoFirstPts = 0.0;
			double maxKeyframeIntervalSeconds = 0.0; // longest keyframe distance (last one to the end), 0 = unknown
			double durationSeconds = 0.0;        // container duration
			uint64_t fileSize = 0;
			std::vector<AudioTrackProbe> audioTracks;
		};

		// What an audio export produced.
		struct AudioExportInfo
		{
			uint32_t sampleRate = 0;
			uint32_t channels = 0;
			uint64_t frames = 0;                 // frames per channel in the output
			int64_t startFrames = 0;             // alignment applied (see the header comment)
			bool remuxed = false;                // ExportOggVorbis: lossless remux, no re-encode
			bool downmixed = false;              // ExportWav: more than 2 source channels were mixed to stereo
		};

		Result ProbeMedia(const std::string& path, MediaProbe& out, const Control& ctl = Control());

		// Audio-only WebM: the packets of the track are copied unchanged (no re-encode); CodecPrivate,
		// CodecDelay, SeekPreRoll, DiscardPadding, language, name and every block time are preserved, so the
		// file shares the time axis of the source and decodes to the same samples as the embedded track.
		Result ExtractAudioWebm(const std::string& src, uint32_t audioTrackIndex, const std::string& dst,
			const Control& ctl = Control());

		// Video-only WebM (all audio tracks removed): video packets, alpha (BlockAdditional id 1), colour
		// description and block times are preserved.
		Result StripAudio(const std::string& src, const std::string& dst, const Control& ctl = Control());

		// 16-bit PCM WAV at 48 kHz, aligned so that sample 0 is video t = 0. Channels: 1 -> mono, 2 -> stereo,
		// more than 2 -> mixed down to stereo (Core Resampler downmix, ITU-R BS.775 coefficients; info->downmixed).
		// The length is the length of the audio (no padding to the video duration).
		Result ExportWav(const std::string& src, uint32_t audioTrackIndex, const std::string& dst,
			const Control& ctl = Control(), AudioExportInfo* info = nullptr);

		// Ogg Vorbis. Vorbis source whose first kept sample is at 0 (startFrames == 0): lossless remux of the
		// packets into Ogg pages (granule positions from vorbis_packet_blocksize, the container's DiscardPadding
		// trims the end). Everything else (Opus, or Vorbis with a non-zero start): decode, align as above and
		// encode with vorbisenc (VBR, `quality` -0.1 .. 1.0, default 0.4) at the decoded rate (48 kHz for Opus,
		// the source rate for Vorbis) with the source channel count (no resampling, no downmix).
		Result ExportOggVorbis(const std::string& src, uint32_t audioTrackIndex, const std::string& dst,
			float quality = 0.4f, const Control& ctl = Control(), AudioExportInfo* info = nullptr);
	}
}
