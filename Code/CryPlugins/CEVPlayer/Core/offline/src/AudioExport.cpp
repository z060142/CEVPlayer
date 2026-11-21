// ExportWav / ExportOggVorbis.
#include "Internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

#include <ogg/ogg.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

#include "CEVCore/Resampler.h"

namespace cev
{
	namespace offline
	{
		using namespace detail;

		namespace
		{
			constexpr uint32_t kWavRate = 48000;
			constexpr uint32_t kOggSerial = 0x43455631u; // "CEV1"

			inline void Put16(uint8_t* p, uint32_t v)
			{
				p[0] = static_cast<uint8_t>(v);
				p[1] = static_cast<uint8_t>(v >> 8);
			}
			inline void Put32(uint8_t* p, uint32_t v)
			{
				p[0] = static_cast<uint8_t>(v);
				p[1] = static_cast<uint8_t>(v >> 8);
				p[2] = static_cast<uint8_t>(v >> 16);
				p[3] = static_cast<uint8_t>(v >> 24);
			}

			void BuildWavHeader(uint8_t h[44], uint32_t channels, uint32_t rate, uint64_t dataBytes)
			{
				std::memcpy(h, "RIFF", 4);
				Put32(h + 4, static_cast<uint32_t>(36 + dataBytes));
				std::memcpy(h + 8, "WAVEfmt ", 8);
				Put32(h + 16, 16);
				Put16(h + 20, 1); // PCM
				Put16(h + 22, channels);
				Put32(h + 24, rate);
				Put32(h + 28, rate * channels * 2);
				Put16(h + 32, channels * 2);
				Put16(h + 34, 16);
				std::memcpy(h + 36, "data", 4);
				Put32(h + 40, static_cast<uint32_t>(dataBytes));
			}

			inline int16_t ToS16(float x)
			{
				if (!(x == x))
					return 0;
				float v = x * 32767.0f;
				if (v > 32767.0f) v = 32767.0f;
				if (v < -32768.0f) v = -32768.0f;
				return static_cast<int16_t>(std::lroundf(v));
			}

			Result WriteAll(std::FILE* f, const void* data, size_t size)
			{
				if (size != 0 && std::fwrite(data, 1, size, f) != size)
					return MakeResult(Status::IoError, "write failed (disk full?)");
				return Okay();
			}

			// Writes ogg pages; returns false on a write error.
			bool WritePage(std::FILE* f, const ogg_page& og)
			{
				return std::fwrite(og.header, 1, static_cast<size_t>(og.header_len), f) == static_cast<size_t>(og.header_len) &&
					std::fwrite(og.body, 1, static_cast<size_t>(og.body_len), f) == static_cast<size_t>(og.body_len);
			}

			struct OggStream
			{
				ogg_stream_state os;
				bool init = false;
				~OggStream()
				{
					if (init)
						ogg_stream_clear(&os);
				}
				bool Init(int serial)
				{
					init = ogg_stream_init(&os, serial) == 0;
					return init;
				}
			};

			// Drains full pages (flush == false) or everything that is pending (flush == true).
			bool DrainPages(OggStream& s, std::FILE* f, bool flush)
			{
				ogg_page og;
				for (;;)
				{
					const int got = flush ? ogg_stream_flush(&s.os, &og) : ogg_stream_pageout(&s.os, &og);
					if (got == 0)
						return true;
					if (!WritePage(f, og))
						return false;
				}
			}

			Result IoFail() { return MakeResult(Status::IoError, "write failed (disk full?)"); }

			// ---------------------------------------------------------------------------------------------------
			// Lossless Vorbis -> Ogg
			// ---------------------------------------------------------------------------------------------------
			Result RemuxVorbisToOgg(MkvFile& file, AudioTrackData& track, OutFile& out, const Control& ctl, Progress& progress,
				AudioExportInfo* info)
			{
				std::vector<uint8_t> hdr[3];
				if (!SplitVorbisHeaders(track.info.codecPrivate, hdr))
					return MakeResult(Status::InvalidData, "the Vorbis CodecPrivate is malformed");

				vorbis_info vi;
				vorbis_comment vc;
				vorbis_info_init(&vi);
				vorbis_comment_init(&vc);
				struct Guard
				{
					vorbis_info* vi;
					vorbis_comment* vc;
					~Guard()
					{
						vorbis_comment_clear(vc);
						vorbis_info_clear(vi);
					}
				} guard{&vi, &vc};

				ogg_packet hp[3];
				for (int i = 0; i < 3; ++i)
				{
					std::memset(&hp[i], 0, sizeof(ogg_packet));
					hp[i].packet = hdr[i].data();
					hp[i].bytes = static_cast<long>(hdr[i].size());
					hp[i].b_o_s = i == 0 ? 1 : 0;
					hp[i].packetno = i;
					if (vorbis_synthesis_headerin(&vi, &vc, &hp[i]) != 0)
						return MakeResult(Status::InvalidData, "invalid Vorbis header packet " + std::to_string(i));
				}

				OggStream os;
				if (!os.Init(static_cast<int>(kOggSerial)))
					return MakeResult(Status::OutOfMemory, "cannot create the Ogg stream");
				std::FILE* f = out.Handle();
				ogg_stream_packetin(&os.os, &hp[0]);
				if (!DrainPages(os, f, true)) // the identification header gets a page of its own
					return IoFail();
				ogg_stream_packetin(&os.os, &hp[1]);
				ogg_stream_packetin(&os.os, &hp[2]);
				if (!DrainPages(os, f, true)) // comment + setup end on a page boundary (spec)
					return IoFail();

				const size_t count = track.packets.size();
				std::vector<uint8_t> buf(std::max<uint32_t>(track.info.maxPacketSize, 1u));
				ogg_int64_t granule = 0;
				long prevBlock = 0;
				for (size_t i = 0; i < count; ++i)
				{
					if (IsCancelled(ctl))
						return MakeResult(Status::Cancelled, "cancelled");
					const AudioPacketRef& p = track.packets[i];
					if (Failed(file.ReadAt(p.pos, buf.data(), p.size)))
						return MakeResult(Status::IoError, "cannot read audio packet " + std::to_string(i));

					ogg_packet op;
					std::memset(&op, 0, sizeof(op));
					op.packet = buf.data();
					op.bytes = static_cast<long>(p.size);
					op.packetno = static_cast<ogg_int64_t>(3 + i);
					const long block = vorbis_packet_blocksize(&vi, &op);
					if (block < 0)
						return MakeResult(Status::InvalidData, "audio packet " + std::to_string(i) + " is not a valid Vorbis audio packet");
					if (i > 0)
						granule += prevBlock / 4 + block / 4;
					prevBlock = block;
					op.granulepos = granule;
					op.e_o_s = i + 1 == count ? 1 : 0;
					if (op.e_o_s != 0 && p.discardPaddingNs > 0)
					{
						const int64_t trim = static_cast<int64_t>(static_cast<uint64_t>(p.discardPaddingNs) * static_cast<uint64_t>(vi.rate) / 1000000000ull);
						op.granulepos = std::max<ogg_int64_t>(0, granule - trim);
					}
					if (i + 1 == count && info != nullptr)
						info->frames = static_cast<uint64_t>(op.granulepos);
					ogg_stream_packetin(&os.os, &op);
					if (!DrainPages(os, f, false))
						return IoFail();
					progress.Report(0.05 + 0.9 * static_cast<double>(i + 1) / static_cast<double>(count));
				}
				if (!DrainPages(os, f, true))
					return IoFail();
				if (info != nullptr)
				{
					info->sampleRate = static_cast<uint32_t>(vi.rate);
					info->channels = static_cast<uint32_t>(vi.channels);
					info->startFrames = 0;
					info->remuxed = true;
				}
				return Okay();
			}

			// ---------------------------------------------------------------------------------------------------
			// Decode -> align -> vorbisenc
			// ---------------------------------------------------------------------------------------------------
			Result EncodeToOgg(MkvFile& file, AlignedAudio& audio, OutFile& out, float quality, const Control& ctl, Progress& progress,
				AudioExportInfo* info)
			{
				vorbis_info vi;
				vorbis_info_init(&vi);
				vorbis_comment vc;
				vorbis_comment_init(&vc);
				vorbis_dsp_state vd;
				vorbis_block vb;
				bool dspInit = false;
				bool blockInit = false;
				struct Guard
				{
					vorbis_info* vi;
					vorbis_comment* vc;
					vorbis_dsp_state* vd;
					vorbis_block* vb;
					bool* dspInit;
					bool* blockInit;
					~Guard()
					{
						if (*blockInit)
							vorbis_block_clear(vb);
						if (*dspInit)
							vorbis_dsp_clear(vd);
						vorbis_comment_clear(vc);
						vorbis_info_clear(vi);
					}
				} guard{&vi, &vc, &vd, &vb, &dspInit, &blockInit};

				const uint32_t channels = audio.channels;
				const uint32_t rate = audio.rate;
				if (!(quality >= -0.1f))
					quality = -0.1f;
				if (quality > 1.0f)
					quality = 1.0f;
				if (vorbis_encode_init_vbr(&vi, static_cast<long>(channels), static_cast<long>(rate), quality) != 0)
					return MakeResult(Status::Unsupported, "vorbisenc does not support " + std::to_string(channels) + " channel(s) at " +
						std::to_string(rate) + " Hz");
				vorbis_comment_add_tag(&vc, "ENCODER", "CEVPlayerOffline");
				if (vorbis_analysis_init(&vd, &vi) != 0)
					return MakeResult(Status::Unsupported, "cannot initialise the Vorbis analysis state");
				dspInit = true;
				if (vorbis_block_init(&vd, &vb) != 0)
					return MakeResult(Status::Unsupported, "cannot initialise the Vorbis block");
				blockInit = true;

				OggStream os;
				if (!os.Init(static_cast<int>(kOggSerial)))
					return MakeResult(Status::OutOfMemory, "cannot create the Ogg stream");
				std::FILE* f = out.Handle();

				ogg_packet h0, h1, h2;
				vorbis_analysis_headerout(&vd, &vc, &h0, &h1, &h2);
				ogg_stream_packetin(&os.os, &h0);
				if (!DrainPages(os, f, true))
					return IoFail();
				ogg_stream_packetin(&os.os, &h1);
				ogg_stream_packetin(&os.os, &h2);
				if (!DrainPages(os, f, true))
					return IoFail();

				Result failure;
				auto drainEncoder = [&]() -> bool {
					while (vorbis_analysis_blockout(&vd, &vb) == 1)
					{
						vorbis_analysis(&vb, nullptr);
						vorbis_bitrate_addblock(&vb);
						ogg_packet op;
						while (vorbis_bitrate_flushpacket(&vd, &op) != 0)
						{
							ogg_stream_packetin(&os.os, &op);
							if (!DrainPages(os, f, false))
							{
								failure = IoFail();
								return false;
							}
						}
					}
					return true;
				};

				constexpr uint32_t kChunk = 4096;
				const PcmSink sink = [&](const float* pcm, uint32_t frames) -> Result {
					uint32_t done = 0;
					while (done < frames)
					{
						const uint32_t n = std::min(kChunk, frames - done);
						float** planes = vorbis_analysis_buffer(&vd, static_cast<int>(n));
						if (planes == nullptr)
							return MakeResult(Status::OutOfMemory, "vorbis_analysis_buffer failed");
						for (uint32_t c = 0; c < channels; ++c)
						{
							const float* s = pcm + static_cast<size_t>(done) * channels + c;
							float* d = planes[c];
							for (uint32_t i = 0; i < n; ++i)
								d[i] = s[static_cast<size_t>(i) * channels];
						}
						if (vorbis_analysis_wrote(&vd, static_cast<int>(n)) != 0)
							return MakeResult(Status::DecodeError, "vorbis_analysis_wrote failed");
						if (!drainEncoder())
							return failure;
						done += n;
					}
					return Okay();
				};

				uint64_t total = 0;
				Result r = RunAligned(file, audio, ctl, progress, 0.05, 0.95, sink, &total);
				if (!r)
					return r;
				vorbis_analysis_wrote(&vd, 0);
				if (!drainEncoder())
					return failure;
				if (!DrainPages(os, f, true))
					return IoFail();
				if (info != nullptr)
				{
					info->sampleRate = rate;
					info->channels = channels;
					info->frames = total;
					info->startFrames = audio.startFrames;
					info->remuxed = false;
				}
				return Okay();
			}
		}

		// -------------------------------------------------------------------------------------------------------
		// WAV
		// -------------------------------------------------------------------------------------------------------
		Result ExportWav(const std::string& src, uint32_t audioTrackIndex, const std::string& dst, const Control& ctl,
			AudioExportInfo* info)
		{
			try
			{
				if (info != nullptr)
					*info = AudioExportInfo();
				Progress progress(ctl);
				progress.Report(0.0);
				std::unique_ptr<MkvFile> file;
				Result r = MkvFile::Open(src, file);
				if (!r)
					return r;
				r = file->ScanAudio(ctl);
				if (!r)
					return r;
				AlignedAudio audio;
				r = PrepareAligned(*file, audioTrackIndex, audio);
				if (!r)
					return r;
				progress.Report(0.05);

				const uint32_t srcRate = audio.rate;
				const uint32_t srcCh = audio.channels;
				const uint32_t dstCh = srcCh == 1 ? 1u : 2u;
				const bool needResample = srcRate != kWavRate || srcCh > 2;

				OutFile out;
				r = out.Open(dst);
				if (!r)
					return r;
				std::FILE* f = out.Handle();
				uint8_t header[44];
				BuildWavHeader(header, dstCh, kWavRate, 0);
				r = WriteAll(f, header, sizeof(header));
				if (!r)
					return r;

				Resampler resampler;
				constexpr uint32_t kMaxIn = 16384;
				constexpr uint32_t kOutCap = 65536;
				std::vector<float> resOut;
				if (needResample)
				{
					const Status st = resampler.Init(srcRate, srcCh, kWavRate, dstCh, kMaxIn);
					if (Failed(st))
						return MakeResult(st, std::string("cannot initialise the resampler (") + StatusToString(st) + ")");
					resOut.assign(static_cast<size_t>(kOutCap) * dstCh, 0.0f);
				}

				// Output length of the resampler: ceil(inFrames * dst / src); anything the filter tail adds is dropped.
				uint64_t inFrames = 0;
				uint64_t written = 0;
				uint64_t limit = ~0ull;
				std::vector<uint8_t> bytes;

				auto writeFloat = [&](const float* pcm, uint64_t frames) -> Result {
					if (frames > limit - written)
						frames = limit - written;
					const size_t samples = static_cast<size_t>(frames) * dstCh;
					bytes.resize(samples * 2);
					for (size_t i = 0; i < samples; ++i)
						Put16(bytes.data() + i * 2, static_cast<uint16_t>(ToS16(pcm[i])));
					written += frames;
					return WriteAll(f, bytes.data(), bytes.size());
				};

				const PcmSink sink = [&](const float* pcm, uint32_t frames) -> Result {
					inFrames += frames;
					if (!needResample)
						return writeFloat(pcm, frames);
					uint32_t consumed = 0;
					while (consumed < frames)
					{
						uint32_t c = 0, p = 0;
						const Status st = resampler.Process(pcm + static_cast<size_t>(consumed) * srcCh, frames - consumed, resOut.data(), kOutCap, &c, &p);
						if (Failed(st))
							return MakeResult(st, "resampler failure");
						if (p > 0)
						{
							Result w = writeFloat(resOut.data(), p);
							if (!w)
								return w;
						}
						consumed += c;
						if (c == 0 && p == 0)
							return MakeResult(Status::InvalidState, "resampler made no progress");
					}
					return Okay();
				};

				uint64_t total = 0;
				r = RunAligned(*file, audio, ctl, progress, 0.05, 0.95, sink, &total);
				if (!r)
					return r;
				if (needResample)
				{
					limit = (inFrames * kWavRate + srcRate - 1) / srcRate;
					resampler.Flush();
					for (;;)
					{
						uint32_t c = 0, p = 0;
						const Status st = resampler.Process(nullptr, 0, resOut.data(), kOutCap, &c, &p);
						if (Failed(st))
							return MakeResult(st, "resampler failure");
						if (p == 0)
							break;
						Result w = writeFloat(resOut.data(), p);
						if (!w)
							return w;
					}
				}

				const uint64_t dataBytes = written * dstCh * 2;
				if (dataBytes > 0xFFFFFFFFull - 36)
					return MakeResult(Status::Unsupported, "the audio is too long for a WAV file (4 GiB limit)");
				BuildWavHeader(header, dstCh, kWavRate, dataBytes);
				if (std::fseek(f, 0, SEEK_SET) != 0)
					return IoFail();
				r = WriteAll(f, header, sizeof(header));
				if (!r)
					return r;
				r = out.Commit();
				if (!r)
					return r;
				if (info != nullptr)
				{
					info->sampleRate = kWavRate;
					info->channels = dstCh;
					info->frames = written;
					info->startFrames = audio.startFrames;
					info->downmixed = srcCh > 2;
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

		// -------------------------------------------------------------------------------------------------------
		// Ogg Vorbis
		// -------------------------------------------------------------------------------------------------------
		Result ExportOggVorbis(const std::string& src, uint32_t audioTrackIndex, const std::string& dst, float quality,
			const Control& ctl, AudioExportInfo* info)
		{
			try
			{
				if (info != nullptr)
					*info = AudioExportInfo();
				Progress progress(ctl);
				progress.Report(0.0);
				std::unique_ptr<MkvFile> file;
				Result r = MkvFile::Open(src, file);
				if (!r)
					return r;
				r = file->ScanAudio(ctl);
				if (!r)
					return r;
				AlignedAudio audio;
				r = PrepareAligned(*file, audioTrackIndex, audio);
				if (!r)
					return r;
				progress.Report(0.05);

				OutFile out;
				r = out.Open(dst);
				if (!r)
					return r;

				const bool remux = audio.track->info.codec == AudioCodec::Vorbis && audio.startFrames == 0;
				if (remux)
					r = RemuxVorbisToOgg(*file, *audio.track, out, ctl, progress, info);
				else
					r = EncodeToOgg(*file, audio, out, quality, ctl, progress, info);
				if (!r)
					return r;
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
