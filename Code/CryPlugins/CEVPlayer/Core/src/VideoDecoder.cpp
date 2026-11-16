#include "CEVCore/VideoDecoder.h"

#include <algorithm>
#include <cstring>

#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>

namespace cev
{
	struct VideoDecoder::Impl
	{
		vpx_codec_ctx_t main;
		vpx_codec_ctx_t alpha;
		bool mainInit = false;
		bool alphaInit = false;
		const vpx_image_t* image = nullptr;
		const vpx_image_t* alphaImage = nullptr;

		Impl()
		{
			std::memset(&main, 0, sizeof(main));
			std::memset(&alpha, 0, sizeof(alpha));
		}

		~Impl()
		{
			if (alphaInit)
				vpx_codec_destroy(&alpha);
			if (mainInit)
				vpx_codec_destroy(&main);
		}
	};

	VideoDecoder::VideoDecoder(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
	VideoDecoder::~VideoDecoder() = default;

	Status VideoDecoder::Create(const VideoStreamInfo& stream, uint32_t threads, std::unique_ptr<VideoDecoder>& out)
	{
		out.reset();
		if (stream.codec != VideoCodec::VP8 && stream.codec != VideoCodec::VP9)
			return Status::Unsupported;
		if (threads == 0)
			threads = 1;
		threads = std::min<uint32_t>(threads, 16);

		std::unique_ptr<Impl> impl = std::make_unique<Impl>();
		vpx_codec_iface_t* iface = stream.codec == VideoCodec::VP9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx();

		vpx_codec_dec_cfg_t cfg;
		cfg.threads = threads;
		cfg.w = static_cast<unsigned int>(stream.width);
		cfg.h = static_cast<unsigned int>(stream.height);
		if (vpx_codec_dec_init(&impl->main, iface, &cfg, 0) != VPX_CODEC_OK)
			return Status::Unsupported;
		impl->mainInit = true;
		if (stream.codec == VideoCodec::VP9 && threads > 1)
			vpx_codec_control(&impl->main, VP9D_SET_ROW_MT, 1); // optional, ignored when unsupported

		if (stream.hasAlpha)
		{
			vpx_codec_dec_cfg_t acfg = cfg;
			acfg.threads = std::max<uint32_t>(1, threads / 2);
			if (vpx_codec_dec_init(&impl->alpha, iface, &acfg, 0) != VPX_CODEC_OK)
				return Status::Unsupported;
			impl->alphaInit = true;
			if (stream.codec == VideoCodec::VP9 && acfg.threads > 1)
				vpx_codec_control(&impl->alpha, VP9D_SET_ROW_MT, 1);
		}

		out = std::make_unique<VideoDecoder>(std::move(impl));
		return Status::Ok;
	}

	Status VideoDecoder::Decode(const uint8_t* data, uint32_t size, const uint8_t* alphaData, uint32_t alphaSize)
	{
		Impl& d = *m_impl;
		d.image = nullptr;
		d.alphaImage = nullptr;
		if (data == nullptr || size == 0)
			return Status::InvalidArgument;

		// The alpha decoder must see every alpha packet (inter frames reference the previous ones), also when
		// the main packet fails or produces no visible frame.
		if (d.alphaInit && alphaData != nullptr && alphaSize > 0)
		{
			if (vpx_codec_decode(&d.alpha, alphaData, alphaSize, nullptr, 0) == VPX_CODEC_OK)
			{
				vpx_codec_iter_t it = nullptr;
				d.alphaImage = vpx_codec_get_frame(&d.alpha, &it);
			}
		}

		if (vpx_codec_decode(&d.main, data, size, nullptr, 0) != VPX_CODEC_OK)
		{
			d.alphaImage = nullptr;
			return Status::DecodeError;
		}
		vpx_codec_iter_t it = nullptr;
		d.image = vpx_codec_get_frame(&d.main, &it);
		if (d.image != nullptr && ((d.image->fmt & VPX_IMG_FMT_HIGHBITDEPTH) != 0 || d.image->bit_depth != 8))
		{
			d.image = nullptr;
			return Status::Unsupported;
		}
		if (d.image == nullptr)
			d.alphaImage = nullptr;
		return Status::Ok;
	}

	bool VideoDecoder::HasFrame() const
	{
		return m_impl->image != nullptr;
	}

	Status VideoDecoder::GetFrameInfo(DecodedImageInfo& info) const
	{
		const vpx_image_t* img = m_impl->image;
		if (img == nullptr)
			return Status::InvalidState;
		info.width = static_cast<int>(img->d_w);
		info.height = static_cast<int>(img->d_h);
		info.chromaShiftX = static_cast<int>(img->x_chroma_shift);
		info.chromaShiftY = static_cast<int>(img->y_chroma_shift);
		info.hasAlpha = m_impl->alphaInit;
		return Status::Ok;
	}

	Status VideoDecoder::WriteFrame(const FrameFormat& format, FrameRing::Slot& slot) const
	{
		const vpx_image_t* img = m_impl->image;
		if (img == nullptr)
			return Status::InvalidState;
		if (static_cast<int>(img->d_w) != format.width || static_cast<int>(img->d_h) != format.height ||
			static_cast<int>(img->x_chroma_shift) != format.chromaShiftX ||
			static_cast<int>(img->y_chroma_shift) != format.chromaShiftY)
			return Status::InvalidData;
		if (img->planes[0] == nullptr || img->planes[1] == nullptr || img->planes[2] == nullptr)
			return Status::InvalidData;

		const int w = format.width;
		const int h = format.height;
		for (int r = 0; r < h; ++r)
			std::memcpy(slot.plane[0] + static_cast<ptrdiff_t>(r) * slot.stride[0],
				img->planes[0] + static_cast<ptrdiff_t>(r) * img->stride[0], static_cast<size_t>(w));

		const int cw = format.ChromaWidth();
		const int ch = format.ChromaHeight();
		for (int p = 1; p <= 2; ++p)
			for (int r = 0; r < ch; ++r)
				std::memcpy(slot.plane[p] + static_cast<ptrdiff_t>(r) * slot.stride[p],
					img->planes[p] + static_cast<ptrdiff_t>(r) * img->stride[p], static_cast<size_t>(cw));

		if (format.hasAlpha && slot.plane[3] != nullptr)
		{
			const vpx_image_t* a = m_impl->alphaImage;
			if (a != nullptr && a->planes[0] != nullptr && static_cast<int>(a->d_w) == w && static_cast<int>(a->d_h) == h)
			{
				for (int r = 0; r < h; ++r)
					std::memcpy(slot.plane[3] + static_cast<ptrdiff_t>(r) * slot.stride[3],
						a->planes[0] + static_cast<ptrdiff_t>(r) * a->stride[0], static_cast<size_t>(w));
			}
			else
			{
				for (int r = 0; r < h; ++r)
					std::memset(slot.plane[3] + static_cast<ptrdiff_t>(r) * slot.stride[3], 255, static_cast<size_t>(w));
			}
		}
		return Status::Ok;
	}
}
