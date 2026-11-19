// WebM demuxer. Headers / tracks come from libwebm's mkvparser; all blocks of the first video and the first
// audio track are indexed once at open (offset, size, pts, key flag, BlockAdditional) so that playback and
// seeking afterwards need no parsing and no allocation: packets are read straight from the byte source into
// caller supplied buffers.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "CEVCore/IByteSource.h"
#include "CEVCore/Status.h"

namespace cev
{
	enum class VideoCodec : uint8_t { None, VP8, VP9 };
	enum class AudioCodec : uint8_t { None, Vorbis, Opus };

	struct VideoStreamInfo
	{
		VideoCodec codec = VideoCodec::None;
		uint32_t trackNumber = 0;
		int width = 0;
		int height = 0;
		double frameRate = 0.0;           // from DefaultDuration, else measured from the pts
		bool hasAlpha = false;            // AlphaMode == 1 (alpha as BlockAdditional id 1)
		int matrixCoefficients = -1;      // Matroska Colour/MatrixCoefficients, -1 = absent
		int colorRange = -1;              // Matroska Colour/Range, -1 = absent
		uint32_t packetCount = 0;
		uint32_t maxPacketSize = 0;
		uint32_t maxAdditionalSize = 0;
		double firstPts = 0.0;            // seconds
		double lastPts = 0.0;
	};

	struct AudioStreamInfo
	{
		AudioCodec codec = AudioCodec::None;
		uint32_t trackNumber = 0;
		uint32_t sampleRate = 0;          // container rate (Opus decodes at 48000 regardless)
		uint32_t channels = 0;
		std::vector<uint8_t> codecPrivate;
		uint64_t codecDelayNs = 0;        // Opus pre-skip
		uint64_t seekPreRollNs = 0;
		uint32_t packetCount = 0;
		uint32_t maxPacketSize = 0;
		double firstPts = 0.0;
		double lastPts = 0.0;
	};

	// One indexed packet.
	struct PacketRef
	{
		uint64_t offset = 0;              // payload position in the file
		uint64_t additionalOffset = 0;    // BlockAdditional id 1 payload, 0 = none
		uint32_t size = 0;
		uint32_t additionalSize = 0;
		int64_t ptsNs = 0;
		int32_t discardPaddingNs = 0;     // Matroska DiscardPadding (end trimming), 0 = none
		bool keyframe = false;
	};

	class WebmDemuxer
	{
	public:
		// Takes ownership of the source. `cancel` (optional) is polled while indexing. Never throws (allocation
		// failure -> OutOfMemory). Every indexed packet lies inside the source and is capped (64 MB video /
		// BlockAdditional, 1 MB audio, 4M packets per stream); pts are clamped and non-decreasing per stream.
		static Status Open(std::unique_ptr<IByteSource> source, std::unique_ptr<WebmDemuxer>& out,
			StatusDetail* detail = nullptr, const std::atomic<bool>* cancel = nullptr);

		WebmDemuxer() = default; // use Open()
		~WebmDemuxer();
		WebmDemuxer(const WebmDemuxer&) = delete;
		WebmDemuxer& operator=(const WebmDemuxer&) = delete;

		bool HasVideo() const { return m_video.codec != VideoCodec::None; }
		bool HasAudio() const { return m_audio.codec != AudioCodec::None; }
		const VideoStreamInfo& Video() const { return m_video; }
		const AudioStreamInfo& Audio() const { return m_audio; }
		// Container duration in seconds (Segment/Info, else derived from the last packets).
		double DurationSeconds() const { return m_duration; }

		uint32_t VideoPacketCount() const { return static_cast<uint32_t>(m_videoPackets.size()); }
		uint32_t AudioPacketCount() const { return static_cast<uint32_t>(m_audioPackets.size()); }
		const PacketRef& VideoPacket(uint32_t index) const { return m_videoPackets[index]; }
		const PacketRef& AudioPacket(uint32_t index) const { return m_audioPackets[index]; }
		double VideoPts(uint32_t index) const { return static_cast<double>(m_videoPackets[index].ptsNs) * 1e-9; }
		double AudioPts(uint32_t index) const { return static_cast<double>(m_audioPackets[index].ptsNs) * 1e-9; }

		// Reads packet payloads into `dst` (capacity must be >= PacketRef::size / additionalSize; the max
		// sizes are in the stream infos).
		Status ReadVideoPacket(uint32_t index, uint8_t* dst, uint32_t capacity);
		Status ReadVideoAdditional(uint32_t index, uint8_t* dst, uint32_t capacity);
		Status ReadAudioPacket(uint32_t index, uint8_t* dst, uint32_t capacity);

		// Index of the last keyframe packet with pts <= seconds (0 when none / before the start).
		uint32_t FindVideoKeyframe(double seconds) const;
		// Index of the last audio packet with pts <= seconds (0 when none).
		uint32_t FindAudioPacket(double seconds) const;
		// Longest distance between two keyframes of the video track, the last one measured to the end of the stream
		// (the worst catch-up after a hidden player becomes visible again, M7). 0 without video.
		double MaxKeyframeIntervalSeconds() const;

	private:
		static Status OpenImpl(std::unique_ptr<IByteSource> source, std::unique_ptr<WebmDemuxer>& out,
			StatusDetail* detail, const std::atomic<bool>* cancel);

		std::unique_ptr<IByteSource> m_source;
		VideoStreamInfo m_video;
		AudioStreamInfo m_audio;
		double m_duration = 0.0;
		std::vector<PacketRef> m_videoPackets;
		std::vector<PacketRef> m_audioPackets;
	};
}
