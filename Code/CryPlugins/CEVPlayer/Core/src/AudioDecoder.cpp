#include "CEVCore/AudioDecoder.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <ogg/ogg.h>
#include <opus/opus.h>
#include <vorbis/codec.h>

namespace cev
{
	namespace
	{
		constexpr uint32_t kOpusMaxFrames = 5760;   // 120 ms at 48 kHz
		constexpr uint32_t kVorbisMaxFrames = 16384;

		struct OpusDeleter
		{
			void operator()(OpusDecoder* d) const { opus_decoder_destroy(d); }
		};
	}

	struct AudioDecoder::Impl
	{
		AudioCodec codec = AudioCodec::None;
		uint32_t channels = 0;
		uint32_t sampleRate = 0;
		uint32_t maxFrames = 0;
		uint64_t pendingDiscard = 0;
		uint32_t preSkipFrames = 0;
		std::vector<float> buffer;

		// Opus
		std::unique_ptr<OpusDecoder, OpusDeleter> opus;

		// Vorbis
		vorbis_info vi;
		vorbis_comment vc;
		vorbis_dsp_state vd;
		vorbis_block vb;
		bool infoInit = false;
		bool dspInit = false;
		bool blockInit = false;
		ogg_int64_t packetNo = 3;

		Impl()
		{
			std::memset(&vi, 0, sizeof(vi));
			std::memset(&vc, 0, sizeof(vc));
			std::memset(&vd, 0, sizeof(vd));
			std::memset(&vb, 0, sizeof(vb));
		}

		~Impl()
		{
			if (blockInit)
				vorbis_block_clear(&vb);
			if (dspInit)
				vorbis_dsp_clear(&vd);
			if (infoInit)
			{
				vorbis_comment_clear(&vc);
				vorbis_info_clear(&vi);
			}
		}

		Status InitOpus(const AudioStreamInfo& stream, StatusDetail& detail)
		{
			uint32_t channels_ = stream.channels;
			uint32_t preSkip = static_cast<uint32_t>((stream.codecDelayNs * 48000ull + 500000000ull) / 1000000000ull);
			int gainQ8 = 0;
			const std::vector<uint8_t>& cp = stream.codecPrivate;
			if (cp.size() >= 19 && std::memcmp(cp.data(), "OpusHead", 8) == 0)
			{
				channels_ = cp[9];
				const uint32_t headerPreSkip = static_cast<uint32_t>(cp[10]) | (static_cast<uint32_t>(cp[11]) << 8);
				if (preSkip == 0)
					preSkip = headerPreSkip;
				gainQ8 = static_cast<int16_t>(static_cast<uint16_t>(cp[16]) | (static_cast<uint16_t>(cp[17]) << 8));
				if (cp[18] != 0)
				{
					detail = StatusDetail::BadCodecPrivate; // multichannel mapping families are not supported
					return Status::Unsupported;
				}
			}
			else if (!cp.empty())
			{
				detail = StatusDetail::BadCodecPrivate;
				return Status::InvalidData;
			}
			if (channels_ < 1 || channels_ > 2)
			{
				detail = StatusDetail::BadCodecPrivate;
				return Status::Unsupported;
			}

			int err = 0;
			OpusDecoder* dec = opus_decoder_create(48000, static_cast<int>(channels_), &err);
			if (dec == nullptr || err != OPUS_OK)
			{
				detail = StatusDetail::AudioDecoderInit;
				return Status::Unsupported;
			}
			opus.reset(dec);
			if (gainQ8 != 0)
				opus_decoder_ctl(dec, OPUS_SET_GAIN(gainQ8));

			codec = AudioCodec::Opus;
			channels = channels_;
			sampleRate = 48000;
			maxFrames = kOpusMaxFrames;
			preSkipFrames = preSkip;
			pendingDiscard = preSkip;
			buffer.assign(static_cast<size_t>(maxFrames) * channels, 0.0f);
			return Status::Ok;
		}

		Status InitVorbis(const AudioStreamInfo& stream, StatusDetail& detail)
		{
			const std::vector<uint8_t>& cp = stream.codecPrivate;
			if (cp.size() < 3 || cp[0] != 2)
			{
				detail = StatusDetail::BadCodecPrivate;
				return Status::InvalidData;
			}
			size_t pos = 1;
			size_t sizes[3] = {0, 0, 0};
			for (int i = 0; i < 2; ++i)
			{
				for (;;)
				{
					if (pos >= cp.size())
					{
						detail = StatusDetail::BadCodecPrivate;
						return Status::InvalidData;
					}
					const uint8_t b = cp[pos++];
					sizes[i] += b;
					if (b != 255)
						break;
				}
			}
			if (pos + sizes[0] + sizes[1] >= cp.size())
			{
				detail = StatusDetail::BadCodecPrivate;
				return Status::InvalidData;
			}
			sizes[2] = cp.size() - pos - sizes[0] - sizes[1];

			vorbis_info_init(&vi);
			vorbis_comment_init(&vc);
			infoInit = true;

			size_t offset = pos;
			for (int i = 0; i < 3; ++i)
			{
				ogg_packet op;
				std::memset(&op, 0, sizeof(op));
				op.packet = const_cast<unsigned char*>(cp.data() + offset);
				op.bytes = static_cast<long>(sizes[i]);
				op.b_o_s = i == 0 ? 1 : 0;
				op.e_o_s = 0;
				op.granulepos = 0;
				op.packetno = i;
				if (vorbis_synthesis_headerin(&vi, &vc, &op) != 0)
				{
					detail = StatusDetail::BadCodecPrivate;
					return Status::InvalidData;
				}
				offset += sizes[i];
			}

			if (vi.channels < 1 || vi.channels > 8 || vi.rate < 8000)
			{
				detail = StatusDetail::BadCodecPrivate;
				return Status::Unsupported;
			}
			if (vorbis_synthesis_init(&vd, &vi) != 0)
			{
				detail = StatusDetail::AudioDecoderInit;
				return Status::Unsupported;
			}
			dspInit = true;
			if (vorbis_block_init(&vd, &vb) != 0)
			{
				detail = StatusDetail::AudioDecoderInit;
				return Status::Unsupported;
			}
			blockInit = true;

			codec = AudioCodec::Vorbis;
			channels = static_cast<uint32_t>(vi.channels);
			sampleRate = static_cast<uint32_t>(vi.rate);
			maxFrames = kVorbisMaxFrames;
			buffer.assign(static_cast<size_t>(maxFrames) * channels, 0.0f);
			return Status::Ok;
		}
	};

	AudioDecoder::AudioDecoder(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
	AudioDecoder::~AudioDecoder() = default;

	Status AudioDecoder::Create(const AudioStreamInfo& stream, std::unique_ptr<AudioDecoder>& out, StatusDetail* detail)
	{
		out.reset();
		StatusDetail dummy = StatusDetail::None;
		StatusDetail& det = detail != nullptr ? *detail : dummy;

		std::unique_ptr<Impl> impl = std::make_unique<Impl>();
		Status st = Status::Unsupported;
		if (stream.codec == AudioCodec::Opus)
			st = impl->InitOpus(stream, det);
		else if (stream.codec == AudioCodec::Vorbis)
			st = impl->InitVorbis(stream, det);
		if (Failed(st))
			return st;
		out = std::make_unique<AudioDecoder>(std::move(impl));
		return Status::Ok;
	}

	uint32_t AudioDecoder::SampleRate() const { return m_impl->sampleRate; }
	uint32_t AudioDecoder::Channels() const { return m_impl->channels; }
	uint32_t AudioDecoder::MaxFramesPerPacket() const { return m_impl->maxFrames; }
	uint32_t AudioDecoder::PreSkipFrames() const { return m_impl->preSkipFrames; }
	uint32_t AudioDecoder::ExtraPrerollPackets() const { return m_impl->codec == AudioCodec::Vorbis ? 2u : 0u; }

	void AudioDecoder::Reset(bool streamStart)
	{
		Impl& d = *m_impl;
		if (d.codec == AudioCodec::Opus)
		{
			opus_decoder_ctl(d.opus.get(), OPUS_RESET_STATE);
			d.pendingDiscard = streamStart ? d.preSkipFrames : 0;
		}
		else
		{
			vorbis_synthesis_restart(&d.vd);
			d.pendingDiscard = 0;
		}
	}

	void AudioDecoder::DiscardFrames(uint32_t frames)
	{
		m_impl->pendingDiscard += frames;
	}

	Status AudioDecoder::Decode(const uint8_t* data, uint32_t size, uint32_t trimEndFrames, const float** pcm, uint32_t* frames)
	{
		Impl& d = *m_impl;
		*pcm = d.buffer.data();
		*frames = 0;
		if (data == nullptr || size == 0)
			return Status::InvalidArgument;

		uint32_t got = 0;
		if (d.codec == AudioCodec::Opus)
		{
			const int n = opus_decode_float(d.opus.get(), data, static_cast<opus_int32>(size), d.buffer.data(),
				static_cast<int>(d.maxFrames), 0);
			if (n < 0)
				return Status::DecodeError;
			got = static_cast<uint32_t>(n);
		}
		else
		{
			ogg_packet op;
			std::memset(&op, 0, sizeof(op));
			op.packet = const_cast<unsigned char*>(data);
			op.bytes = static_cast<long>(size);
			op.packetno = d.packetNo++;
			op.granulepos = -1;
			if (vorbis_synthesis(&d.vb, &op) != 0)
				return Status::DecodeError;
			if (vorbis_synthesis_blockin(&d.vd, &d.vb) != 0)
				return Status::DecodeError;

			float** planes = nullptr;
			int n = 0;
			while ((n = vorbis_synthesis_pcmout(&d.vd, &planes)) > 0)
			{
				const uint32_t room = d.maxFrames - got;
				const uint32_t take = std::min<uint32_t>(static_cast<uint32_t>(n), room);
				for (uint32_t c = 0; c < d.channels; ++c)
				{
					float* dst = d.buffer.data() + static_cast<size_t>(got) * d.channels + c;
					const float* srcPlane = planes[c];
					for (uint32_t i = 0; i < take; ++i)
						dst[static_cast<size_t>(i) * d.channels] = srcPlane[i];
				}
				got += take;
				vorbis_synthesis_read(&d.vd, n); // also drops what did not fit
			}
		}

		// start / pre-roll discard, end trimming
		const uint32_t start = static_cast<uint32_t>(std::min<uint64_t>(d.pendingDiscard, got));
		d.pendingDiscard -= start;
		const uint32_t end = got > trimEndFrames ? got - trimEndFrames : 0;
		if (end > start)
		{
			*pcm = d.buffer.data() + static_cast<size_t>(start) * d.channels;
			*frames = end - start;
		}
		return Status::Ok;
	}
}
