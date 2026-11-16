// Vorbis (libvorbis packet API, ogg_packet built from the CodecPrivate Xiph lacing) and Opus (libopus)
// decoder. Output is float interleaved PCM. Start trimming (Opus pre-skip / CodecDelay), seek pre-roll
// discarding and end trimming (DiscardPadding) are handled here so that the first returned sample is the
// sample that belongs to the packet's timestamp.
#pragma once

#include <cstdint>
#include <memory>

#include "CEVCore/Status.h"
#include "CEVCore/WebmDemuxer.h"

namespace cev
{
	class AudioDecoder
	{
	public:
		static Status Create(const AudioStreamInfo& stream, std::unique_ptr<AudioDecoder>& out,
			StatusDetail* detail = nullptr);

		~AudioDecoder();
		AudioDecoder(const AudioDecoder&) = delete;
		AudioDecoder& operator=(const AudioDecoder&) = delete;

		// Output format (Opus is always 48000 Hz).
		uint32_t SampleRate() const;
		uint32_t Channels() const;
		// Upper bound of frames (samples per channel) returned by one Decode() call.
		uint32_t MaxFramesPerPacket() const;
		// How many packets before a seek target decoding must start to get correct output (Vorbis overlap,
		// Opus pre-roll is expressed in time by the container).
		uint32_t ExtraPrerollPackets() const;
		// Opus pre-skip in output frames (0 for Vorbis).
		uint32_t PreSkipFrames() const;

		// Flush codec state. streamStart = true when the next packet is the first packet of the stream: the
		// Opus pre-skip is then discarded from its output.
		void Reset(bool streamStart);
		// Discard the next `frames` output frames (seek pre-roll / alignment), on top of pending pre-skip.
		void DiscardFrames(uint32_t frames);

		// Decodes one packet. *pcm points into an internal buffer valid until the next call; *frames may be 0
		// (Vorbis start-up, discarded pre-roll). trimEndFrames: frames to drop from the end of this packet.
		Status Decode(const uint8_t* data, uint32_t size, uint32_t trimEndFrames, const float** pcm, uint32_t* frames);

		struct Impl;

		explicit AudioDecoder(std::unique_ptr<Impl> impl);

	private:
		std::unique_ptr<Impl> m_impl;
	};
}
