#include "CEVCore/WebmDemuxer.h"

#include <algorithm>
#include <cstring>

#include <mkvparser/mkvparser.h>

namespace cev
{
	namespace
	{
		// Matroska element ids used by the block scanner.
		constexpr uint32_t kIdSegmentInfo = 0x1549A966;
		constexpr uint32_t kIdTracks = 0x1654AE6B;
		constexpr uint32_t kIdCluster = 0x1F43B675;
		constexpr uint32_t kIdCues = 0x1C53BB6B;
		constexpr uint32_t kIdSeekHead = 0x114D9B74;
		constexpr uint32_t kIdTags = 0x1254C367;
		constexpr uint32_t kIdChapters = 0x1043A770;
		constexpr uint32_t kIdAttachments = 0x1941A469;
		constexpr uint32_t kIdTrackEntry = 0xAE;
		constexpr uint32_t kIdTrackNumber = 0xD7;
		constexpr uint32_t kIdTrackVideo = 0xE0;
		constexpr uint32_t kIdAlphaMode = 0x53C0;
		constexpr uint32_t kIdTimecode = 0xE7;
		constexpr uint32_t kIdSimpleBlock = 0xA3;
		constexpr uint32_t kIdBlockGroup = 0xA0;
		constexpr uint32_t kIdBlock = 0xA1;
		constexpr uint32_t kIdBlockAdditions = 0x75A1;
		constexpr uint32_t kIdBlockMore = 0xA6;
		constexpr uint32_t kIdBlockAddId = 0xEE;
		constexpr uint32_t kIdBlockAdditional = 0xA5;
		constexpr uint32_t kIdReferenceBlock = 0xFB;
		constexpr uint32_t kIdDiscardPadding = 0x75A2;
		constexpr uint32_t kIdSilentTracks = 0x5854;
		constexpr uint32_t kIdPosition = 0xA7;
		constexpr uint32_t kIdPrevSize = 0xAB;
		constexpr uint32_t kIdEncryptedBlock = 0xAF;

		constexpr uint64_t kUnknownSize = ~0ull;

		struct SegmentDeleter
		{
			void operator()(mkvparser::Segment* s) const { delete s; } // libwebm owns its objects with plain new
		};

		class MkvReaderAdapter final : public mkvparser::IMkvReader
		{
		public:
			explicit MkvReaderAdapter(IByteSource& source) : m_source(source) {}
			~MkvReaderAdapter() override = default;

			int Read(long long pos, long len, unsigned char* buf) override
			{
				if (pos < 0 || len < 0)
					return -1;
				return Succeeded(m_source.Read(static_cast<uint64_t>(pos), buf, static_cast<size_t>(len))) ? 0 : -1;
			}

			int Length(long long* total, long long* available) override
			{
				const long long len = static_cast<long long>(m_source.Length());
				if (total != nullptr) *total = len;
				if (available != nullptr) *available = len;
				return 0;
			}

		private:
			IByteSource& m_source;
		};

		struct Element
		{
			uint32_t id = 0;
			uint64_t dataStart = 0;
			uint64_t dataSize = 0; // kUnknownSize when unknown
			uint64_t End() const { return dataSize == kUnknownSize ? kUnknownSize : dataStart + dataSize; }
		};

		// Small buffered EBML reader on top of the byte source.
		class Scanner
		{
		public:
			explicit Scanner(IByteSource& source) : m_source(source), m_length(source.Length()) {}

			uint64_t Length() const { return m_length; }

			// Copies n (<= 16) bytes at pos.
			bool Peek(uint64_t pos, uint8_t* out, size_t n)
			{
				if (pos > m_length || n > m_length - pos)
					return false;
				if (pos < m_bufStart || pos + n > m_bufStart + m_bufLen)
				{
					const uint64_t want = std::min<uint64_t>(sizeof(m_buf), m_length - pos);
					if (want < n || Failed(m_source.Read(pos, m_buf, static_cast<size_t>(want))))
						return false;
					m_bufStart = pos;
					m_bufLen = static_cast<size_t>(want);
				}
				std::memcpy(out, m_buf + (pos - m_bufStart), n);
				return true;
			}

			// Reads an element header (id + size) at pos.
			bool ReadElement(uint64_t pos, Element& e)
			{
				uint8_t b[12];
				const size_t avail = static_cast<size_t>(std::min<uint64_t>(sizeof(b), m_length > pos ? m_length - pos : 0));
				if (avail < 2 || !Peek(pos, b, avail))
					return false;

				const unsigned idLen = VintLength(b[0]);
				if (idLen == 0 || idLen > 4 || idLen + 1 > avail)
					return false;
				uint32_t id = 0;
				for (unsigned i = 0; i < idLen; ++i)
					id = (id << 8) | b[i];

				const unsigned sizeLen = VintLength(b[idLen]);
				if (sizeLen == 0 || idLen + sizeLen > avail)
					return false;
				uint64_t size = b[idLen] & (0xFFu >> sizeLen);
				bool allOnes = size == (0xFFu >> sizeLen);
				for (unsigned i = 1; i < sizeLen; ++i)
				{
					size = (size << 8) | b[idLen + i];
					allOnes = allOnes && b[idLen + i] == 0xFF;
				}

				e.id = id;
				e.dataStart = pos + idLen + sizeLen;
				e.dataSize = allOnes ? kUnknownSize : size;
				return true;
			}

			bool ReadUInt(const Element& e, uint64_t& value)
			{
				if (e.dataSize == 0 || e.dataSize > 8)
					return false;
				uint8_t b[8];
				if (!Peek(e.dataStart, b, static_cast<size_t>(e.dataSize)))
					return false;
				value = 0;
				for (uint64_t i = 0; i < e.dataSize; ++i)
					value = (value << 8) | b[i];
				return true;
			}

			bool ReadSInt(const Element& e, int64_t& value)
			{
				uint64_t u = 0;
				if (!ReadUInt(e, u))
					return false;
				const unsigned bits = static_cast<unsigned>(e.dataSize * 8);
				if (bits < 64 && (u & (1ull << (bits - 1))))
					u |= ~0ull << bits;
				value = static_cast<int64_t>(u);
				return true;
			}

			// Reads a Matroska vint (size style, marker removed) at pos. Returns its length in `len`.
			bool ReadVint(uint64_t pos, uint64_t& value, unsigned& len)
			{
				uint8_t b[8];
				if (!Peek(pos, b, 1))
					return false;
				len = VintLength(b[0]);
				if (len == 0 || !Peek(pos, b, len))
					return false;
				value = b[0] & (0xFFu >> len);
				for (unsigned i = 1; i < len; ++i)
					value = (value << 8) | b[i];
				return true;
			}

			static unsigned VintLength(uint8_t first)
			{
				if (first == 0)
					return 0;
				unsigned len = 1;
				uint8_t mask = 0x80;
				while ((first & mask) == 0)
				{
					mask >>= 1;
					++len;
				}
				return len;
			}

		private:
			IByteSource& m_source;
			uint64_t m_length;
			uint8_t m_buf[4096];
			uint64_t m_bufStart = 0;
			size_t m_bufLen = 0;
		};

		bool IsTopLevelId(uint32_t id)
		{
			return id == kIdCluster || id == kIdCues || id == kIdSeekHead || id == kIdTags || id == kIdChapters ||
				id == kIdAttachments || id == kIdSegmentInfo || id == kIdTracks;
		}

		struct BlockExtras
		{
			uint64_t additionalOffset = 0;
			uint32_t additionalSize = 0;
			int32_t discardPaddingNs = 0;
			bool hasReference = false;
		};

		struct IndexContext
		{
			Scanner* scanner = nullptr;
			uint32_t videoTrack = 0;
			uint32_t audioTrack = 0;
			int64_t timecodeScale = 1000000;
			int64_t audioFrameNs = 0; // for laced audio blocks
			std::vector<PacketRef>* video = nullptr;
			std::vector<PacketRef>* audio = nullptr;
			const std::atomic<bool>* cancel = nullptr;
			uint32_t videoAlphaTrackNumber = 0;
		};

		// Adds the frames of one Block / SimpleBlock payload.
		Status AddBlock(IndexContext& ctx, uint64_t dataStart, uint64_t dataSize, int64_t clusterTimecode,
			bool simpleBlock, const BlockExtras& extras)
		{
			Scanner& sc = *ctx.scanner;
			uint64_t track = 0;
			unsigned tnLen = 0;
			if (!sc.ReadVint(dataStart, track, tnLen))
				return Status::InvalidData;
			if (dataSize < tnLen + 3)
				return Status::InvalidData;

			std::vector<PacketRef>* dst = nullptr;
			if (ctx.videoTrack != 0 && track == ctx.videoTrack)
				dst = ctx.video;
			else if (ctx.audioTrack != 0 && track == ctx.audioTrack)
				dst = ctx.audio;
			if (dst == nullptr)
				return Status::Ok;

			uint8_t hdr[3];
			if (!sc.Peek(dataStart + tnLen, hdr, 3))
				return Status::InvalidData;
			const int16_t rel = static_cast<int16_t>((static_cast<uint16_t>(hdr[0]) << 8) | hdr[1]);
			const uint8_t flags = hdr[2];
			const unsigned lacing = (flags >> 1) & 3u;
			const bool key = simpleBlock ? (flags & 0x80) != 0 : !extras.hasReference;
			const int64_t ptsNs = (clusterTimecode + rel) * ctx.timecodeScale;

			uint64_t pos = dataStart + tnLen + 3;
			const uint64_t end = dataStart + dataSize;

			PacketRef ref;
			ref.keyframe = key;
			ref.additionalOffset = extras.additionalOffset;
			ref.additionalSize = extras.additionalSize;
			ref.discardPaddingNs = extras.discardPaddingNs;

			if (lacing == 0)
			{
				ref.offset = pos;
				ref.size = static_cast<uint32_t>(end - pos);
				ref.ptsNs = ptsNs;
				dst->push_back(ref);
				return Status::Ok;
			}

			// Laced block: Xiph (1), fixed size (2), EBML (3).
			uint8_t countByte = 0;
			if (!sc.Peek(pos, &countByte, 1))
				return Status::InvalidData;
			++pos;
			const unsigned frameCount = static_cast<unsigned>(countByte) + 1;
			std::vector<uint32_t> sizes(frameCount, 0);
			uint64_t total = 0;
			if (lacing == 2)
			{
				const uint64_t each = (end - pos) / frameCount;
				for (unsigned i = 0; i < frameCount; ++i)
					sizes[i] = static_cast<uint32_t>(each);
				total = each * frameCount;
			}
			else if (lacing == 1)
			{
				for (unsigned i = 0; i + 1 < frameCount; ++i)
				{
					uint32_t sz = 0;
					uint8_t b = 255;
					while (b == 255)
					{
						if (!sc.Peek(pos, &b, 1))
							return Status::InvalidData;
						++pos;
						sz += b;
					}
					sizes[i] = sz;
					total += sz;
				}
			}
			else
			{
				uint64_t first = 0;
				unsigned len = 0;
				if (!sc.ReadVint(pos, first, len))
					return Status::InvalidData;
				pos += len;
				sizes[0] = static_cast<uint32_t>(first);
				total = first;
				int64_t prev = static_cast<int64_t>(first);
				for (unsigned i = 1; i + 1 < frameCount; ++i)
				{
					uint64_t raw = 0;
					if (!sc.ReadVint(pos, raw, len))
						return Status::InvalidData;
					pos += len;
					const int64_t bias = (1ll << (7 * len - 1)) - 1;
					prev += static_cast<int64_t>(raw) - bias;
					if (prev < 0)
						return Status::InvalidData;
					sizes[i] = static_cast<uint32_t>(prev);
					total += static_cast<uint64_t>(prev);
				}
			}
			if (pos + total > end)
				return Status::InvalidData;
			if (lacing != 2)
				sizes[frameCount - 1] = static_cast<uint32_t>(end - pos - total);

			for (unsigned i = 0; i < frameCount; ++i)
			{
				ref.offset = pos;
				ref.size = sizes[i];
				ref.ptsNs = ptsNs + static_cast<int64_t>(i) * ctx.audioFrameNs;
				ref.additionalOffset = 0;
				ref.additionalSize = 0;
				ref.discardPaddingNs = (i + 1 == frameCount) ? extras.discardPaddingNs : 0;
				dst->push_back(ref);
				pos += sizes[i];
			}
			return Status::Ok;
		}

		Status ScanBlockGroup(IndexContext& ctx, const Element& group, int64_t clusterTimecode)
		{
			Scanner& sc = *ctx.scanner;
			uint64_t blockStart = 0, blockSize = 0;
			bool haveBlock = false;
			BlockExtras extras;

			uint64_t pos = group.dataStart;
			const uint64_t end = group.End();
			Element e;
			while (pos < end && sc.ReadElement(pos, e))
			{
				if (e.dataSize == kUnknownSize)
					return Status::InvalidData;
				switch (e.id)
				{
				case kIdBlock:
					blockStart = e.dataStart;
					blockSize = e.dataSize;
					haveBlock = true;
					break;
				case kIdReferenceBlock:
					extras.hasReference = true;
					break;
				case kIdDiscardPadding:
				{
					int64_t v = 0;
					if (sc.ReadSInt(e, v))
						extras.discardPaddingNs = static_cast<int32_t>(std::max<int64_t>(INT32_MIN, std::min<int64_t>(INT32_MAX, v)));
					break;
				}
				case kIdBlockAdditions:
				{
					uint64_t mp = e.dataStart;
					const uint64_t mend = e.End();
					Element more;
					while (mp < mend && sc.ReadElement(mp, more))
					{
						if (more.dataSize == kUnknownSize)
							return Status::InvalidData;
						if (more.id == kIdBlockMore)
						{
							uint64_t addId = 1; // default per spec
							uint64_t addOff = 0, addSize = 0;
							uint64_t ip = more.dataStart;
							const uint64_t iend = more.End();
							Element item;
							while (ip < iend && sc.ReadElement(ip, item))
							{
								if (item.dataSize == kUnknownSize)
									return Status::InvalidData;
								if (item.id == kIdBlockAddId)
									sc.ReadUInt(item, addId);
								else if (item.id == kIdBlockAdditional)
								{
									addOff = item.dataStart;
									addSize = item.dataSize;
								}
								ip = item.End();
							}
							if (addId == 1 && addSize > 0 && extras.additionalOffset == 0)
							{
								extras.additionalOffset = addOff;
								extras.additionalSize = static_cast<uint32_t>(addSize);
							}
						}
						mp = more.End();
					}
					break;
				}
				default:
					break;
				}
				pos = e.End();
			}
			if (!haveBlock)
				return Status::Ok;
			return AddBlock(ctx, blockStart, blockSize, clusterTimecode, false, extras);
		}

		// Scans the children of one cluster. nextPos = position after the cluster.
		Status ScanCluster(IndexContext& ctx, const Element& cluster, uint64_t& nextPos)
		{
			Scanner& sc = *ctx.scanner;
			int64_t timecode = 0;
			uint64_t pos = cluster.dataStart;
			const bool unknown = cluster.dataSize == kUnknownSize;
			const uint64_t end = unknown ? sc.Length() : cluster.End();

			Element e;
			while (pos < end)
			{
				if (!sc.ReadElement(pos, e))
				{
					if (unknown)
						break; // truncated tail of a live stream
					return Status::InvalidData;
				}
				if (unknown && IsTopLevelId(e.id))
					break;
				if (e.dataSize == kUnknownSize)
					return Status::InvalidData;
				if (e.End() > end)
				{
					if (unknown)
						break;
					return Status::InvalidData;
				}

				Status st = Status::Ok;
				switch (e.id)
				{
				case kIdTimecode:
				{
					uint64_t v = 0;
					if (sc.ReadUInt(e, v))
						timecode = static_cast<int64_t>(v);
					break;
				}
				case kIdSimpleBlock:
					st = AddBlock(ctx, e.dataStart, e.dataSize, timecode, true, BlockExtras());
					break;
				case kIdBlockGroup:
					st = ScanBlockGroup(ctx, e, timecode);
					break;
				case kIdSilentTracks:
				case kIdPosition:
				case kIdPrevSize:
				case kIdEncryptedBlock:
				default:
					break;
				}
				if (Failed(st))
					return st;
				pos = e.End();
			}
			nextPos = pos;
			return Status::Ok;
		}

		// Reads AlphaMode of every video TrackEntry; returns the track number that has AlphaMode == 1 (0 = none).
		void ScanTracksForAlpha(Scanner& sc, const Element& tracks, uint32_t& alphaTrack)
		{
			alphaTrack = 0;
			uint64_t pos = tracks.dataStart;
			const uint64_t end = tracks.End();
			Element entry;
			while (pos < end && sc.ReadElement(pos, entry))
			{
				if (entry.dataSize == kUnknownSize)
					return;
				if (entry.id == kIdTrackEntry)
				{
					uint64_t number = 0;
					uint64_t alpha = 0;
					uint64_t p = entry.dataStart;
					const uint64_t eend = entry.End();
					Element child;
					while (p < eend && sc.ReadElement(p, child))
					{
						if (child.dataSize == kUnknownSize)
							break;
						if (child.id == kIdTrackNumber)
							sc.ReadUInt(child, number);
						else if (child.id == kIdTrackVideo)
						{
							uint64_t vp = child.dataStart;
							const uint64_t vend = child.End();
							Element v;
							while (vp < vend && sc.ReadElement(vp, v))
							{
								if (v.dataSize == kUnknownSize)
									break;
								if (v.id == kIdAlphaMode)
									sc.ReadUInt(v, alpha);
								vp = v.End();
							}
						}
						p = child.End();
					}
					if (alpha == 1 && alphaTrack == 0)
						alphaTrack = static_cast<uint32_t>(number);
				}
				pos = entry.End();
			}
		}
	}

	Status WebmDemuxer::Open(std::unique_ptr<IByteSource> source, std::unique_ptr<WebmDemuxer>& out,
		StatusDetail* detail, const std::atomic<bool>* cancel)
	{
		out.reset();
		StatusDetail dummy = StatusDetail::None;
		StatusDetail& det = detail != nullptr ? *detail : dummy;
		det = StatusDetail::None;
		if (!source)
			return Status::InvalidArgument;

		std::unique_ptr<WebmDemuxer> dmx = std::make_unique<WebmDemuxer>();
		dmx->m_source = std::move(source);
		IByteSource& src = *dmx->m_source;

		// ---- headers, tracks via mkvparser ----
		MkvReaderAdapter reader(src);
		mkvparser::EBMLHeader ebml;
		long long pos = 0;
		if (ebml.Parse(&reader, pos) != 0)
		{
			det = StatusDetail::EbmlHeader;
			return Status::InvalidData;
		}

		mkvparser::Segment* rawSegment = nullptr;
		if (mkvparser::Segment::CreateInstance(&reader, pos, rawSegment) != 0 || rawSegment == nullptr)
		{
			det = StatusDetail::SegmentParse;
			return Status::InvalidData;
		}
		std::unique_ptr<mkvparser::Segment, SegmentDeleter> segment(rawSegment);
		if (segment->ParseHeaders() < 0)
		{
			det = StatusDetail::SegmentParse;
			return Status::InvalidData;
		}

		const mkvparser::SegmentInfo* info = segment->GetInfo();
		const mkvparser::Tracks* tracks = segment->GetTracks();
		if (info == nullptr || tracks == nullptr)
		{
			det = StatusDetail::TracksParse;
			return Status::InvalidData;
		}

		IndexContext ctx;
		ctx.timecodeScale = info->GetTimeCodeScale() > 0 ? info->GetTimeCodeScale() : 1000000;
		ctx.cancel = cancel;

		const mkvparser::VideoTrack* vTrack = nullptr;
		const mkvparser::AudioTrack* aTrack = nullptr;
		for (unsigned long i = 0; i < tracks->GetTracksCount(); ++i)
		{
			const mkvparser::Track* t = tracks->GetTrackByIndex(i);
			if (t == nullptr || t->GetCodecId() == nullptr)
				continue;
			if (t->GetContentEncodingCount() > 0)
			{
				det = StatusDetail::ContentEncoding;
				return Status::Unsupported;
			}
			if (t->GetType() == 1 && vTrack == nullptr)
			{
				if (std::strcmp(t->GetCodecId(), "V_VP8") == 0 || std::strcmp(t->GetCodecId(), "V_VP9") == 0)
					vTrack = static_cast<const mkvparser::VideoTrack*>(t);
			}
			else if (t->GetType() == 2 && aTrack == nullptr)
			{
				if (std::strcmp(t->GetCodecId(), "A_VORBIS") == 0 || std::strcmp(t->GetCodecId(), "A_OPUS") == 0)
					aTrack = static_cast<const mkvparser::AudioTrack*>(t);
			}
		}
		if (vTrack == nullptr && aTrack == nullptr)
		{
			det = StatusDetail::NoSupportedTrack;
			return Status::Unsupported;
		}

		if (vTrack != nullptr)
		{
			VideoStreamInfo& v = dmx->m_video;
			v.codec = std::strcmp(vTrack->GetCodecId(), "V_VP9") == 0 ? VideoCodec::VP9 : VideoCodec::VP8;
			v.trackNumber = static_cast<uint32_t>(vTrack->GetNumber());
			v.width = static_cast<int>(vTrack->GetWidth());
			v.height = static_cast<int>(vTrack->GetHeight());
			if (vTrack->GetDefaultDuration() > 0)
				v.frameRate = 1e9 / static_cast<double>(vTrack->GetDefaultDuration());
			else if (vTrack->GetFrameRate() > 0.0)
				v.frameRate = vTrack->GetFrameRate();
			if (const mkvparser::Colour* colour = vTrack->GetColour())
			{
				v.matrixCoefficients = colour->matrix_coefficients == mkvparser::Colour::kValueNotPresent
					? -1 : static_cast<int>(colour->matrix_coefficients);
				v.colorRange = colour->range == mkvparser::Colour::kValueNotPresent
					? -1 : static_cast<int>(colour->range);
			}
			ctx.videoTrack = v.trackNumber;
		}
		if (aTrack != nullptr)
		{
			AudioStreamInfo& a = dmx->m_audio;
			a.codec = std::strcmp(aTrack->GetCodecId(), "A_OPUS") == 0 ? AudioCodec::Opus : AudioCodec::Vorbis;
			a.trackNumber = static_cast<uint32_t>(aTrack->GetNumber());
			a.sampleRate = static_cast<uint32_t>(aTrack->GetSamplingRate());
			a.channels = static_cast<uint32_t>(aTrack->GetChannels());
			size_t cpSize = 0;
			const unsigned char* cp = aTrack->GetCodecPrivate(cpSize);
			if (cp != nullptr && cpSize > 0)
				a.codecPrivate.assign(cp, cp + cpSize);
			a.codecDelayNs = aTrack->GetCodecDelay();
			a.seekPreRollNs = aTrack->GetSeekPreRoll();
			ctx.audioTrack = a.trackNumber;
			ctx.audioFrameNs = aTrack->GetDefaultDuration() > 0 ? static_cast<int64_t>(aTrack->GetDefaultDuration()) : 0;
		}
		const double infoDuration = info->GetDuration() > 0 ? static_cast<double>(info->GetDuration()) * 1e-9 : 0.0;

		// ---- full block index (own EBML scan: handles BlockAdditions and unknown size clusters) ----
		Scanner scanner(src);
		ctx.scanner = &scanner;
		ctx.video = &dmx->m_videoPackets;
		ctx.audio = &dmx->m_audioPackets;

		const uint64_t segmentStart = static_cast<uint64_t>(segment->m_start);
		uint64_t segmentEnd = segment->m_size < 0 ? scanner.Length() : segmentStart + static_cast<uint64_t>(segment->m_size);
		segmentEnd = std::min<uint64_t>(segmentEnd, scanner.Length());

		uint64_t p = segmentStart;
		while (p < segmentEnd)
		{
			if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
				return Status::Cancelled;
			Element e;
			if (!scanner.ReadElement(p, e))
				break; // trailing garbage / truncated file: keep what we have
			if (e.id == kIdCluster)
			{
				uint64_t next = 0;
				const Status st = ScanCluster(ctx, e, next);
				if (Failed(st))
				{
					// A damaged cluster in the middle of the file ends the index but keeps earlier packets.
					if (dmx->m_videoPackets.empty() && dmx->m_audioPackets.empty())
					{
						det = StatusDetail::ClusterScan;
						return st;
					}
					break;
				}
				p = next;
				continue;
			}
			if (e.id == kIdTracks)
				ScanTracksForAlpha(scanner, e, ctx.videoAlphaTrackNumber);
			if (e.dataSize == kUnknownSize)
				break;
			p = e.End();
		}

		if (dmx->HasVideo())
		{
			VideoStreamInfo& v = dmx->m_video;
			v.hasAlpha = ctx.videoAlphaTrackNumber != 0 && ctx.videoAlphaTrackNumber == v.trackNumber;
			if (dmx->m_videoPackets.empty())
			{
				det = StatusDetail::NoSupportedTrack;
				return Status::InvalidData;
			}
			v.packetCount = static_cast<uint32_t>(dmx->m_videoPackets.size());
			for (const PacketRef& r : dmx->m_videoPackets)
			{
				v.maxPacketSize = std::max(v.maxPacketSize, r.size);
				v.maxAdditionalSize = std::max(v.maxAdditionalSize, r.additionalSize);
			}
			v.firstPts = dmx->VideoPts(0);
			v.lastPts = dmx->VideoPts(v.packetCount - 1);
			if (v.frameRate <= 0.0)
				v.frameRate = v.packetCount > 1 && v.lastPts > v.firstPts
					? (v.packetCount - 1) / (v.lastPts - v.firstPts) : 30.0;
			if (v.width <= 0 || v.height <= 0)
			{
				det = StatusDetail::TracksParse;
				return Status::InvalidData;
			}
		}
		if (dmx->HasAudio())
		{
			AudioStreamInfo& a = dmx->m_audio;
			if (dmx->m_audioPackets.empty())
			{
				// Audio track without packets: ignore the audio, keep the video.
				dmx->m_audio = AudioStreamInfo();
			}
			else
			{
				a.packetCount = static_cast<uint32_t>(dmx->m_audioPackets.size());
				for (const PacketRef& r : dmx->m_audioPackets)
					a.maxPacketSize = std::max(a.maxPacketSize, r.size);
				a.firstPts = dmx->AudioPts(0);
				a.lastPts = dmx->AudioPts(a.packetCount - 1);
			}
		}
		if (!dmx->HasVideo() && !dmx->HasAudio())
		{
			det = StatusDetail::NoSupportedTrack;
			return Status::InvalidData;
		}

		double derived = 0.0;
		if (dmx->HasVideo())
			derived = std::max(derived, dmx->m_video.lastPts + 1.0 / dmx->m_video.frameRate);
		if (dmx->HasAudio())
			derived = std::max(derived, dmx->m_audio.lastPts + 0.02);
		dmx->m_duration = infoDuration > 0.0 ? infoDuration : derived;

		out = std::move(dmx);
		return Status::Ok;
	}

	WebmDemuxer::~WebmDemuxer() = default;

	Status WebmDemuxer::ReadVideoPacket(uint32_t index, uint8_t* dst, uint32_t capacity)
	{
		if (index >= m_videoPackets.size())
			return Status::OutOfRange;
		const PacketRef& r = m_videoPackets[index];
		if (capacity < r.size || (dst == nullptr && r.size > 0))
			return Status::InvalidArgument;
		return m_source->Read(r.offset, dst, r.size);
	}

	Status WebmDemuxer::ReadVideoAdditional(uint32_t index, uint8_t* dst, uint32_t capacity)
	{
		if (index >= m_videoPackets.size())
			return Status::OutOfRange;
		const PacketRef& r = m_videoPackets[index];
		if (r.additionalSize == 0)
			return Status::Ok;
		if (capacity < r.additionalSize || dst == nullptr)
			return Status::InvalidArgument;
		return m_source->Read(r.additionalOffset, dst, r.additionalSize);
	}

	Status WebmDemuxer::ReadAudioPacket(uint32_t index, uint8_t* dst, uint32_t capacity)
	{
		if (index >= m_audioPackets.size())
			return Status::OutOfRange;
		const PacketRef& r = m_audioPackets[index];
		if (capacity < r.size || (dst == nullptr && r.size > 0))
			return Status::InvalidArgument;
		return m_source->Read(r.offset, dst, r.size);
	}

	uint32_t WebmDemuxer::FindVideoKeyframe(double seconds) const
	{
		const int64_t ns = static_cast<int64_t>(seconds * 1e9);
		// last packet with pts <= ns
		uint32_t lo = 0, hi = static_cast<uint32_t>(m_videoPackets.size());
		while (lo < hi)
		{
			const uint32_t mid = lo + (hi - lo) / 2;
			if (m_videoPackets[mid].ptsNs <= ns)
				lo = mid + 1;
			else
				hi = mid;
		}
		uint32_t idx = lo == 0 ? 0 : lo - 1;
		while (idx > 0 && !m_videoPackets[idx].keyframe)
			--idx;
		return idx;
	}

	uint32_t WebmDemuxer::FindAudioPacket(double seconds) const
	{
		const int64_t ns = static_cast<int64_t>(seconds * 1e9);
		uint32_t lo = 0, hi = static_cast<uint32_t>(m_audioPackets.size());
		while (lo < hi)
		{
			const uint32_t mid = lo + (hi - lo) / 2;
			if (m_audioPackets[mid].ptsNs <= ns)
				lo = mid + 1;
			else
				hi = mid;
		}
		return lo == 0 ? 0 : lo - 1;
	}
}
