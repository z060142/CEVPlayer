// VP8 / VP9 decoder (libvpx). Alpha: a second decoder instance decodes BlockAdditional id 1 and its luma plane
// becomes the A plane. Output is copied into FrameRing slots (no allocation, no crop, odd sizes supported).
#pragma once

#include <cstdint>
#include <memory>

#include "CEVCore/ColorConvert.h"
#include "CEVCore/FrameRing.h"
#include "CEVCore/Status.h"
#include "CEVCore/WebmDemuxer.h"

namespace cev
{
	struct DecodedImageInfo
	{
		int width = 0;
		int height = 0;
		int chromaShiftX = 1;
		int chromaShiftY = 1;
		bool hasAlpha = false;
	};

	class VideoDecoder
	{
	public:
		// threads: libvpx worker threads (>= 1).
		static Status Create(const VideoStreamInfo& stream, uint32_t threads, std::unique_ptr<VideoDecoder>& out);

		~VideoDecoder();
		VideoDecoder(const VideoDecoder&) = delete;
		VideoDecoder& operator=(const VideoDecoder&) = delete;

		// Decodes one packet (+ optional alpha packet). Returns Ok when the packet was consumed (check
		// HasFrame()), DecodeError when libvpx rejected the main packet. A failing alpha packet is not an
		// error: the frame is then opaque.
		Status Decode(const uint8_t* data, uint32_t size, const uint8_t* alphaData, uint32_t alphaSize);

		// A visible frame came out of the last Decode(). Valid until the next Decode().
		bool HasFrame() const;
		Status GetFrameInfo(DecodedImageInfo& info) const;
		// The last decoded frame as libvpx holds it (planes, strides, size, subsampling; A = the alpha decoder's luma or
		// null). Valid until the next Decode(). Tests compare ring slots against it.
		Status GetFrameView(YuvaImage& out) const;

		// Copies the last decoded frame into the slot (planes and A). Slot memory was sized for `format`;
		// returns InvalidData if the frame does not match it.
		Status WriteFrame(const FrameFormat& format, FrameRing::Slot& slot) const;

		struct Impl;

		explicit VideoDecoder(std::unique_ptr<Impl> impl);

	private:
		std::unique_ptr<Impl> m_impl;
	};
}
