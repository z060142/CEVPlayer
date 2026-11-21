#include "Internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace cev
{
	namespace offline
	{
		namespace detail
		{
			namespace
			{
				namespace fs = std::filesystem;

				constexpr uint32_t kMaxAudioPayload = 16u << 20;
				constexpr uint32_t kCancelPollBlocks = 128;

				fs::path ToPath(const std::string& utf8)
				{
					return fs::u8path(utf8);
				}
			}

			Result MakeResult(Status status, std::string message)
			{
				Result r;
				r.status = status;
				r.message = std::move(message);
				return r;
			}

			void Progress::Report(double fraction)
			{
				if (!m_ctl.progress)
					return;
				if (!(fraction > 0.0))
					fraction = 0.0;
				if (fraction > 1.0)
					fraction = 1.0;
				if (fraction <= m_last)
					return;
				m_last = fraction;
				m_ctl.progress(fraction);
			}

			std::FILE* OpenUtf8(const std::string& path, const char* mode)
			{
				if (path.empty())
					return nullptr;
				const fs::path p = ToPath(path);
#if defined(_WIN32)
				wchar_t wmode[8] = {};
				for (size_t i = 0; mode[i] != 0 && i + 1 < sizeof(wmode) / sizeof(wmode[0]); ++i)
					wmode[i] = static_cast<wchar_t>(mode[i]);
				return _wfopen(p.c_str(), wmode);
#else
				return std::fopen(p.c_str(), mode);
#endif
			}

			void RemoveUtf8(const std::string& path)
			{
				std::error_code ec;
				fs::remove(ToPath(path), ec);
			}

			// ---------------------------------------------------------------------------------------------------
			// OutFile
			// ---------------------------------------------------------------------------------------------------
			Result OutFile::Open(const std::string& dst)
			{
				Abort();
				if (dst.empty())
					return MakeResult(Status::InvalidArgument, "destination path is empty");
				m_dst = dst;
				m_tmp = dst + ".tmp";
				m_file = OpenUtf8(m_tmp, "wb");
				if (m_file == nullptr)
					return MakeResult(Status::IoError, "cannot create output file '" + m_tmp + "'");
				return Okay();
			}

			Result OutFile::Commit()
			{
				if (m_file == nullptr)
					return MakeResult(Status::InvalidState, "no output file is open");
				const bool flushed = std::fflush(m_file) == 0 && std::ferror(m_file) == 0;
				const bool closed = std::fclose(m_file) == 0;
				m_file = nullptr;
				if (!flushed || !closed)
				{
					RemoveUtf8(m_tmp);
					return MakeResult(Status::IoError, "writing '" + m_tmp + "' failed (disk full?)");
				}
				std::error_code ec;
				fs::rename(ToPath(m_tmp), ToPath(m_dst), ec);
				if (ec)
				{
					std::error_code ec2;
					fs::remove(ToPath(m_dst), ec2);
					ec.clear();
					fs::rename(ToPath(m_tmp), ToPath(m_dst), ec);
				}
				if (ec)
				{
					RemoveUtf8(m_tmp);
					return MakeResult(Status::IoError, "cannot move the output to '" + m_dst + "': " + ec.message());
				}
				m_tmp.clear();
				return Okay();
			}

			void OutFile::Abort()
			{
				if (m_file != nullptr)
				{
					std::fclose(m_file);
					m_file = nullptr;
				}
				if (!m_tmp.empty())
				{
					RemoveUtf8(m_tmp);
					m_tmp.clear();
				}
			}

			// ---------------------------------------------------------------------------------------------------
			// MkvFile
			// ---------------------------------------------------------------------------------------------------
			// mkvparser reads a few bytes at a time; a 64 KB window keeps that off the disk.
			class MkvFile::Reader final : public mkvparser::IMkvReader
			{
			public:
				explicit Reader(IByteSource& source) : m_source(source), m_length(source.Length()) {}

				int Read(long long pos, long len, unsigned char* buf) override
				{
					if (pos < 0 || len < 0)
						return -1;
					if (len == 0)
						return 0;
					const uint64_t upos = static_cast<uint64_t>(pos);
					const uint64_t ulen = static_cast<uint64_t>(len);
					if (upos > m_length || ulen > m_length - upos)
						return -1;
					if (ulen > kWindow)
						return Succeeded(m_source.Read(upos, buf, static_cast<size_t>(ulen))) ? 0 : -1;
					if (upos < m_winStart || upos + ulen > m_winStart + m_winLen)
					{
						const uint64_t want = std::min<uint64_t>(kWindow, m_length - upos);
						if (Failed(m_source.Read(upos, m_win, static_cast<size_t>(want))))
							return -1;
						m_winStart = upos;
						m_winLen = want;
					}
					std::memcpy(buf, m_win + (upos - m_winStart), static_cast<size_t>(ulen));
					return 0;
				}

				int Length(long long* total, long long* available) override
				{
					if (total != nullptr)
						*total = static_cast<long long>(m_length);
					if (available != nullptr)
						*available = static_cast<long long>(m_length);
					return 0;
				}

			private:
				static constexpr uint64_t kWindow = 64 * 1024;
				IByteSource& m_source;
				uint64_t m_length;
				uint8_t m_win[kWindow];
				uint64_t m_winStart = 0;
				uint64_t m_winLen = 0;
			};

			MkvFile::~MkvFile() = default;

			Status MkvFile::ReadAt(uint64_t pos, void* dst, size_t size)
			{
				return m_source->Read(pos, dst, size);
			}

			Result MkvFile::Open(const std::string& path, std::unique_ptr<MkvFile>& out)
			{
				out.reset();
				std::unique_ptr<MkvFile> f(new MkvFile());
				f->m_path = path;
				const Status st = FileByteSource::Open(path.c_str(), false, f->m_source);
				if (Failed(st) || f->m_source == nullptr)
					return MakeResult(Status::IoError, "cannot open '" + path + "'");
				if (f->m_source->Length() < 16)
					return MakeResult(Status::InvalidData, "'" + path + "' is too small to be a WebM file");
				f->m_reader.reset(new Reader(*f->m_source));

				mkvparser::EBMLHeader header;
				long long pos = 0;
				if (header.Parse(f->m_reader.get(), pos) < 0)
					return MakeResult(Status::InvalidData, "'" + path + "' is not a WebM / Matroska file (no EBML header)");

				mkvparser::Segment* seg = nullptr;
				if (mkvparser::Segment::CreateInstance(f->m_reader.get(), pos, seg) != 0 || seg == nullptr)
					return MakeResult(Status::InvalidData, "'" + path + "' has no readable Segment");
				f->m_segment.reset(seg);
				if (seg->Load() < 0)
					return MakeResult(Status::InvalidData, "'" + path + "': the Segment headers / clusters are corrupt");
				if (seg->GetTracks() == nullptr)
					return MakeResult(Status::InvalidData, "'" + path + "' has no Tracks element");

				const mkvparser::SegmentInfo* info = seg->GetInfo();
				if (info != nullptr)
				{
					const long long scale = info->GetTimeCodeScale();
					if (scale > 0)
						f->m_timecodeScale = static_cast<uint64_t>(scale);
					const long long dur = info->GetDuration();
					if (dur > 0)
						f->m_duration = static_cast<double>(dur) * 1e-9;
				}

				const mkvparser::Tracks* tracks = seg->GetTracks();
				uint32_t audioIndex = 0;
				for (unsigned long i = 0; i < tracks->GetTracksCount(); ++i)
				{
					const mkvparser::Track* t = tracks->GetTrackByIndex(i);
					if (t == nullptr || t->GetType() != mkvparser::Track::kAudio)
						continue;
					const mkvparser::AudioTrack* at = static_cast<const mkvparser::AudioTrack*>(t);

					AudioTrackData a;
					a.index = audioIndex++;
					a.trackNumber = static_cast<uint32_t>(t->GetNumber());
					a.codecId = t->GetCodecId() != nullptr ? t->GetCodecId() : "";
					a.language = t->GetLanguage() != nullptr ? t->GetLanguage() : "";
					a.name = t->GetNameAsUTF8() != nullptr ? t->GetNameAsUTF8() : "";
					a.defaultDurationNs = t->GetDefaultDuration();
					a.containerRate = at->GetSamplingRate();

					AudioStreamInfo& s = a.info;
					if (a.codecId == "A_OPUS")
						s.codec = AudioCodec::Opus;
					else if (a.codecId == "A_VORBIS")
						s.codec = AudioCodec::Vorbis;
					s.trackNumber = a.trackNumber;
					s.sampleRate = a.containerRate > 0.0 ? static_cast<uint32_t>(std::llround(a.containerRate)) : 0u;
					const long long ch = at->GetChannels();
					s.channels = ch > 0 && ch < 256 ? static_cast<uint32_t>(ch) : 0u;
					size_t cpSize = 0;
					const unsigned char* cp = t->GetCodecPrivate(cpSize);
					if (cp != nullptr && cpSize > 0 && cpSize <= (1u << 20))
						s.codecPrivate.assign(cp, cp + cpSize);
					s.codecDelayNs = t->GetCodecDelay();
					s.seekPreRollNs = t->GetSeekPreRoll();
					f->m_audio.push_back(std::move(a));
				}

				out = std::move(f);
				return Okay();
			}

			Result MkvFile::ScanAudio(const Control& ctl)
			{
				mkvparser::Segment* seg = m_segment.get();
				for (AudioTrackData& a : m_audio)
				{
					a.packets.clear();
					a.laced = false;
				}
				if (m_audio.empty())
					return Okay();

				uint32_t blocks = 0;
				const mkvparser::Cluster* cluster = seg->GetFirst();
				while (cluster != nullptr && !cluster->EOS())
				{
					const mkvparser::BlockEntry* entry = nullptr;
					if (cluster->GetFirst(entry) < 0)
						return MakeResult(Status::InvalidData, "corrupt cluster while indexing audio");
					while (entry != nullptr && !entry->EOS())
					{
						if ((++blocks % kCancelPollBlocks) == 0 && IsCancelled(ctl))
							return MakeResult(Status::Cancelled, "cancelled");

						const mkvparser::Block* block = entry->GetBlock();
						if (block != nullptr)
						{
							const long long number = block->GetTrackNumber();
							for (AudioTrackData& a : m_audio)
							{
								if (static_cast<long long>(a.trackNumber) != number)
									continue;
								if (block->GetFrameCount() != 1)
								{
									a.laced = true;
									break;
								}
								const mkvparser::Block::Frame& fr = block->GetFrame(0);
								if (fr.len <= 0 || static_cast<uint32_t>(fr.len) > kMaxAudioPayload || fr.pos < 0)
								{
									a.laced = true; // treated like an unsupported block layout
									break;
								}
								AudioPacketRef p;
								p.ptsNs = block->GetTime(cluster);
								p.pos = static_cast<uint64_t>(fr.pos);
								p.size = static_cast<uint32_t>(fr.len);
								p.discardPaddingNs = block->GetDiscardPadding();
								a.packets.push_back(p);
								break;
							}
						}
						const mkvparser::BlockEntry* next = nullptr;
						if (cluster->GetNext(entry, next) < 0)
							return MakeResult(Status::InvalidData, "corrupt block while indexing audio");
						entry = next;
					}
					cluster = seg->GetNext(cluster);
				}

				for (AudioTrackData& a : m_audio)
				{
					AudioStreamInfo& s = a.info;
					s.packetCount = static_cast<uint32_t>(a.packets.size());
					for (const AudioPacketRef& p : a.packets)
						s.maxPacketSize = std::max(s.maxPacketSize, p.size);
					if (!a.packets.empty())
					{
						s.firstPts = static_cast<double>(a.packets.front().ptsNs) * 1e-9;
						s.lastPts = static_cast<double>(a.packets.back().ptsNs) * 1e-9;
					}
				}
				return Okay();
			}

			Result CheckAudioTrack(MkvFile& file, uint32_t index, AudioTrackData*& track)
			{
				track = nullptr;
				std::vector<AudioTrackData>& audio = file.Audio();
				if (audio.empty())
					return MakeResult(Status::InvalidData, "the file has no audio track");
				if (index >= audio.size())
					return MakeResult(Status::OutOfRange, "audio track index " + std::to_string(index) + " is out of range (the file has " +
						std::to_string(audio.size()) + " audio track(s))");
				AudioTrackData& a = audio[index];
				if (a.info.codec == AudioCodec::None)
					return MakeResult(Status::Unsupported, "audio track " + std::to_string(index) + " uses codec '" + a.codecId +
						"', only Opus and Vorbis are supported");
				if (a.laced)
					return MakeResult(Status::Unsupported, "audio track " + std::to_string(index) + " uses laced blocks, which are not supported");
				if (a.packets.empty())
					return MakeResult(Status::InvalidData, "audio track " + std::to_string(index) + " has no packets");
				for (const AudioPacketRef& p : a.packets)
				{
					if (p.ptsNs < 0)
						return MakeResult(Status::Unsupported, "audio track " + std::to_string(index) + " has negative block times");
				}
				track = &a;
				return Okay();
			}

			int64_t ComputeStartFrames(const AudioStreamInfo& info, uint32_t decoderRate, uint32_t preSkipFrames, int64_t firstPtsNs)
			{
				// Identical arithmetic to PlayerCore::ApplyAudioSeek for t = 0 starting at packet 0.
				const double startSec = static_cast<double>(firstPtsNs) * 1e-9 - static_cast<double>(info.codecDelayNs) * 1e-9;
				int64_t frames = std::llround(startSec * decoderRate);
				frames += preSkipFrames;
				return frames;
			}

			Result PrepareAligned(MkvFile& file, uint32_t index, AlignedAudio& out)
			{
				AudioTrackData* track = nullptr;
				Result r = CheckAudioTrack(file, index, track);
				if (!r)
					return r;
				StatusDetail detail = StatusDetail::None;
				const Status st = AudioDecoder::Create(track->info, out.decoder, &detail);
				if (Failed(st))
					return MakeResult(st, std::string("cannot create the audio decoder for track ") + std::to_string(index) + ": " +
						StatusToString(st) + " / " + StatusDetailToString(detail));
				out.track = track;
				out.rate = out.decoder->SampleRate();
				out.channels = out.decoder->Channels();
				out.startFrames = ComputeStartFrames(track->info, out.rate, out.decoder->PreSkipFrames(), track->packets.front().ptsNs);
				return Okay();
			}

			Result RunAligned(MkvFile& file, AlignedAudio& audio, const Control& ctl, Progress& progress, double lo, double hi,
				const PcmSink& sink, uint64_t* totalFrames)
			{
				uint64_t total = 0;
				AudioDecoder& dec = *audio.decoder;
				dec.Reset(true);

				if (audio.startFrames > 0)
				{
					std::vector<float> silence(static_cast<size_t>(8192) * audio.channels, 0.0f);
					uint64_t left = static_cast<uint64_t>(audio.startFrames);
					while (left > 0)
					{
						if (IsCancelled(ctl))
							return MakeResult(Status::Cancelled, "cancelled");
						const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(left, 8192));
						Result r = sink(silence.data(), n);
						if (!r)
							return r;
						left -= n;
						total += n;
					}
				}
				else if (audio.startFrames < 0)
				{
					dec.DiscardFrames(static_cast<uint32_t>(std::min<int64_t>(-audio.startFrames, 0x7FFFFFFF)));
				}

				const AudioTrackData& t = *audio.track;
				std::vector<uint8_t> buf(std::max<uint32_t>(t.info.maxPacketSize, 1u));
				const size_t count = t.packets.size();
				for (size_t i = 0; i < count; ++i)
				{
					if (IsCancelled(ctl))
						return MakeResult(Status::Cancelled, "cancelled");
					const AudioPacketRef& p = t.packets[i];
					if (Failed(file.ReadAt(p.pos, buf.data(), p.size)))
						return MakeResult(Status::IoError, "cannot read audio packet " + std::to_string(i));
					uint32_t trim = 0;
					if (p.discardPaddingNs > 0)
						trim = static_cast<uint32_t>(std::min<uint64_t>(static_cast<uint64_t>(p.discardPaddingNs) * audio.rate / 1000000000ull, 0xFFFFFFFFull));
					const float* pcm = nullptr;
					uint32_t frames = 0;
					const Status st = dec.Decode(buf.data(), p.size, trim, &pcm, &frames);
					if (Failed(st))
						return MakeResult(st, "audio packet " + std::to_string(i) + " failed to decode (" + StatusToString(st) + ")");
					if (frames > 0)
					{
						Result r = sink(pcm, frames);
						if (!r)
							return r;
						total += frames;
					}
					progress.Report(lo + (hi - lo) * static_cast<double>(i + 1) / static_cast<double>(count));
				}
				if (totalFrames != nullptr)
					*totalFrames = total;
				return Okay();
			}

			bool SplitVorbisHeaders(const std::vector<uint8_t>& cp, std::vector<uint8_t> headers[3])
			{
				if (cp.size() < 3 || cp[0] != 2)
					return false;
				size_t pos = 1;
				size_t sizes[3] = {0, 0, 0};
				for (int i = 0; i < 2; ++i)
				{
					for (;;)
					{
						if (pos >= cp.size())
							return false;
						const uint8_t b = cp[pos++];
						sizes[i] += b;
						if (b != 255)
							break;
					}
				}
				if (pos + sizes[0] + sizes[1] >= cp.size())
					return false;
				sizes[2] = cp.size() - pos - sizes[0] - sizes[1];
				size_t offset = pos;
				for (int i = 0; i < 3; ++i)
				{
					headers[i].assign(cp.begin() + static_cast<std::ptrdiff_t>(offset), cp.begin() + static_cast<std::ptrdiff_t>(offset + sizes[i]));
					offset += sizes[i];
				}
				return true;
			}
		}
	}
}
