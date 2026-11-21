// CEVPlayerOffline internals shared by the .cpp files (not part of the public interface).
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <mkvparser/mkvparser.h>

#include "CEVCore/AudioDecoder.h"
#include "CEVCore/IByteSource.h"
#include "CEVCore/WebmDemuxer.h"
#include "CEVOffline/Offline.h"

namespace cev
{
	namespace offline
	{
		namespace detail
		{
			Result MakeResult(Status status, std::string message);
			inline Result Okay() { return Result(); }

			inline bool IsCancelled(const Control& ctl)
			{
				return ctl.cancel != nullptr && ctl.cancel->load(std::memory_order_relaxed);
			}

			// Monotonic progress reporting, [lo, hi] is the slice of 0..1 this stage covers.
			class Progress
			{
			public:
				explicit Progress(const Control& ctl) : m_ctl(ctl) {}
				void Report(double fraction);

			private:
				const Control& m_ctl;
				double m_last = -1.0;
			};

			// "<dst>.tmp" output file; deleted unless Commit() succeeded.
			class OutFile
			{
			public:
				OutFile() = default;
				~OutFile() { Abort(); }
				OutFile(const OutFile&) = delete;
				OutFile& operator=(const OutFile&) = delete;

				Result Open(const std::string& dst);
				std::FILE* Handle() const { return m_file; }
				// Closes and renames over the destination.
				Result Commit();
				void Abort();

			private:
				std::FILE* m_file = nullptr;
				std::string m_dst;
				std::string m_tmp;
			};

			std::FILE* OpenUtf8(const std::string& path, const char* mode);
			void RemoveUtf8(const std::string& path);

			// One packet of an audio track.
			struct AudioPacketRef
			{
				int64_t ptsNs = 0;
				uint64_t pos = 0;
				uint32_t size = 0;
				int64_t discardPaddingNs = 0;
			};

			struct AudioTrackData
			{
				uint32_t index = 0;
				uint32_t trackNumber = 0;
				std::string codecId;
				std::string language;
				std::string name;
				uint64_t defaultDurationNs = 0;
				double containerRate = 0.0;
				AudioStreamInfo info;              // codec None for codecs the decoder does not know
				bool laced = false;                // a block carried more than one frame (not supported)
				std::vector<AudioPacketRef> packets;
			};

			// mkvparser view of one file (all tracks, every block of every audio track).
			class MkvFile
			{
			public:
				static Result Open(const std::string& path, std::unique_ptr<MkvFile>& out);
				~MkvFile();

				mkvparser::Segment* Segment() const { return m_segment.get(); }
				uint64_t FileSize() const { return m_source->Length(); }
				uint64_t TimecodeScale() const { return m_timecodeScale; }
				double DurationSeconds() const { return m_duration; }
				std::vector<AudioTrackData>& Audio() { return m_audio; }
				const std::string& Path() const { return m_path; }

				// Indexes the packets of every audio track.
				Result ScanAudio(const Control& ctl);
				Status ReadAt(uint64_t pos, void* dst, size_t size);

			private:
				struct SegmentDeleter
				{
					void operator()(mkvparser::Segment* s) const { delete s; }
				};
				class Reader;

				std::string m_path;
				std::unique_ptr<IByteSource> m_source;
				std::unique_ptr<Reader> m_reader;
				std::unique_ptr<mkvparser::Segment, SegmentDeleter> m_segment;
				uint64_t m_timecodeScale = 1000000;
				double m_duration = 0.0;
				std::vector<AudioTrackData> m_audio;
			};

			// Opus: decoder rate 48000 | Vorbis: container rate (from the decoder).
			Result CheckAudioTrack(MkvFile& file, uint32_t index, AudioTrackData*& track);

			// First kept sample on the container timeline, in frames at the decoder rate (header comment).
			int64_t ComputeStartFrames(const AudioStreamInfo& info, uint32_t decoderRate, uint32_t preSkipFrames, int64_t firstPtsNs);

			// Decoder + alignment for one track.
			struct AlignedAudio
			{
				AudioTrackData* track = nullptr;
				std::unique_ptr<AudioDecoder> decoder;
				uint32_t rate = 0;
				uint32_t channels = 0;
				int64_t startFrames = 0;
			};
			Result PrepareAligned(MkvFile& file, uint32_t index, AlignedAudio& out);

			// Streams the aligned PCM (float, interleaved, decoder rate / channels): silence for startFrames > 0
			// first, decoded packets (end trimming applied) after. progress: slice [lo, hi].
			using PcmSink = std::function<Result(const float* pcm, uint32_t frames)>;
			Result RunAligned(MkvFile& file, AlignedAudio& audio, const Control& ctl, Progress& progress, double lo, double hi,
				const PcmSink& sink, uint64_t* totalFrames);

			// Matroska Vorbis CodecPrivate (Xiph lacing) -> the three header packets.
			bool SplitVorbisHeaders(const std::vector<uint8_t>& codecPrivate, std::vector<uint8_t> headers[3]);
		}
	}
}
