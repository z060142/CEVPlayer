// S0 measurement probe: per-frame cost of CEVPlayerCore's video path without the engine.
//
//   core_bench <clip.webm> [threads (0 = PlayerCore auto rule)] [maxFrames]
//
// For every video packet: decode (libvpx main + alpha), copy into a FrameRing slot (VideoDecoder::WriteFrame),
// convert the slot to premultiplied RGBA (ConvertYuvaToRgbaPremultiplied). Prints mean / p99 / max ms, bytes per
// frame and the RAM of a 4 slot ring as PlayerCore allocates it. Also reports the colour of alpha = 0 pixels
// (straight vs premultiplied colour planes). Single threaded loop: no pacing, no audio.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "CEVCore/ColorConvert.h"
#include "CEVCore/FrameRing.h"
#include "CEVCore/IByteSource.h"
#include "CEVCore/VideoDecoder.h"
#include "CEVCore/WebmDemuxer.h"

using namespace cev;
using Clock = std::chrono::steady_clock;

namespace
{
	double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

	struct Series
	{
		std::vector<double> v;
		void Print(const char* name) const
		{
			if (v.empty()) { std::printf("  %-8s n=0\n", name); return; }
			std::vector<double> s = v;
			std::sort(s.begin(), s.end());
			double sum = 0; for (double x : s) sum += x;
			const size_t i99 = std::min(s.size() - 1, static_cast<size_t>(s.size() * 0.99));
			std::printf("  %-8s mean %7.3f ms  p99 %7.3f ms  max %7.3f ms  (n=%zu)\n", name, sum / s.size(), s[i99], s.back(), s.size());
		}
	};
}

int main(int argc, char** argv)
{
	if (argc < 2) { std::fprintf(stderr, "usage: core_bench <clip.webm> [threads] [maxFrames]\n"); return 2; }
	const int threadArg = argc > 2 ? std::atoi(argv[2]) : 0;
	const int maxFrames = argc > 3 ? std::atoi(argv[3]) : 1 << 30;

	std::unique_ptr<IByteSource> src;
	if (FileByteSource::Open(argv[1], true, src) != Status::Ok) { std::fprintf(stderr, "open failed\n"); return 1; }
	std::unique_ptr<WebmDemuxer> dmx;
	if (WebmDemuxer::Open(std::move(src), dmx) != Status::Ok || !dmx->HasVideo()) { std::fprintf(stderr, "demux failed\n"); return 1; }
	const VideoStreamInfo& v = dmx->Video();

	// Same rule as PlayerCore.cpp (cfg.videoThreads == 0).
	uint32_t threads = static_cast<uint32_t>(threadArg);
	if (threads == 0)
	{
		threads = v.height >= 1000 ? 4u : (v.height >= 700 ? 2u : 1u);
		threads = std::min(threads, std::max(1u, std::thread::hardware_concurrency()));
	}
	std::unique_ptr<VideoDecoder> dec;
	if (VideoDecoder::Create(v, threads, dec) != Status::Ok) { std::fprintf(stderr, "decoder failed\n"); return 1; }

	std::vector<uint8_t> pkt(v.maxPacketSize + 64), add(v.maxAdditionalSize + 64);
	FrameRing ring;
	FrameFormat fmt;
	bool ringReady = false;
	Series tDecode, tCopy, tConvert;
	size_t yuvBytes = 0, rgbaBytes = 0;
	double alphaZeroY = 0, alphaZeroU = 0, alphaZeroV = 0; size_t alphaZeroN = 0;
	int frames = 0;
	const ColorStandard cs = ResolveColorStandard(ColorStandard::Auto, v.matrixCoefficients, v.width, v.height);
	const ColorRange cr = ResolveColorRange(v.colorRange);

	for (uint32_t i = 0; i < dmx->VideoPacketCount() && frames < maxFrames; ++i)
	{
		const PacketRef& p = dmx->VideoPacket(i);
		if (dmx->ReadVideoPacket(i, pkt.data(), static_cast<uint32_t>(pkt.size())) != Status::Ok) return 1;
		uint32_t addSize = 0;
		if (p.additionalSize != 0 && dmx->ReadVideoAdditional(i, add.data(), static_cast<uint32_t>(add.size())) == Status::Ok)
			addSize = p.additionalSize;

		const auto t0 = Clock::now();
		if (dec->Decode(pkt.data(), p.size, addSize ? add.data() : nullptr, addSize) != Status::Ok) { std::fprintf(stderr, "decode error at %u\n", i); return 1; }
		const auto t1 = Clock::now();
		if (!dec->HasFrame()) continue;
		if (!ringReady)
		{
			DecodedImageInfo info;
			dec->GetFrameInfo(info);
			fmt.width = info.width; fmt.height = info.height;
			fmt.chromaShiftX = info.chromaShiftX; fmt.chromaShiftY = info.chromaShiftY;
			fmt.hasAlpha = info.hasAlpha || v.hasAlpha; fmt.withRgba = true;
			if (ring.Init(fmt, 4) != Status::Ok) return 1;
			ringReady = true;
			const size_t cw = static_cast<size_t>(fmt.ChromaWidth()), ch = static_cast<size_t>(fmt.ChromaHeight());
			yuvBytes = static_cast<size_t>(fmt.width) * fmt.height * (fmt.hasAlpha ? 2 : 1) + 2 * cw * ch;
			rgbaBytes = static_cast<size_t>(fmt.width) * fmt.height * 4;
		}
		FrameRing::Slot& slot = ring.GetSlot(static_cast<uint32_t>(frames % 4));
		if (dec->WriteFrame(fmt, slot) != Status::Ok) { std::fprintf(stderr, "WriteFrame failed\n"); return 1; }
		const auto t2 = Clock::now();
		YuvaImage img;
		for (int k = 0; k < 4; ++k) { img.plane[k] = slot.plane[k]; img.stride[k] = slot.stride[k]; }
		img.width = fmt.width; img.height = fmt.height; img.chromaShiftX = fmt.chromaShiftX; img.chromaShiftY = fmt.chromaShiftY;
		ConvertYuvaToRgbaPremultiplied(img, cs, cr, slot.rgba, slot.rgbaStride);
		const auto t3 = Clock::now();
		tDecode.v.push_back(Ms(t0, t1)); tCopy.v.push_back(Ms(t1, t2)); tConvert.v.push_back(Ms(t2, t3));

		if (fmt.hasAlpha && slot.plane[3] != nullptr && frames % 30 == 0)
		{
			for (int y = 0; y < fmt.height; y += 2)
				for (int x = 0; x < fmt.width; x += 2)
					if (slot.plane[3][static_cast<ptrdiff_t>(y) * slot.stride[3] + x] == 0)
					{
						alphaZeroY += slot.plane[0][static_cast<ptrdiff_t>(y) * slot.stride[0] + x];
						alphaZeroU += slot.plane[1][static_cast<ptrdiff_t>(y >> 1) * slot.stride[1] + (x >> 1)];
						alphaZeroV += slot.plane[2][static_cast<ptrdiff_t>(y >> 1) * slot.stride[2] + (x >> 1)];
						++alphaZeroN;
					}
		}
		++frames;
	}

	const size_t cw = static_cast<size_t>(fmt.ChromaWidth()), ch = static_cast<size_t>(fmt.ChromaHeight());
	const size_t packed = static_cast<size_t>(fmt.width) * (fmt.height + ch + (fmt.hasAlpha ? fmt.height : 0));
	std::printf("clip %s\n  %dx%d %.2f fps alpha=%d codec=%s threads=%u frames=%d matrix=%d range=%d -> %s %s\n", argv[1], fmt.width, fmt.height,
		v.frameRate, fmt.hasAlpha ? 1 : 0, v.codec == VideoCodec::VP9 ? "VP9" : "VP8", threads, frames, v.matrixCoefficients, v.colorRange,
		cs == ColorStandard::BT709 ? "BT709" : "BT601", cr == ColorRange::Full ? "full" : "limited");
	tDecode.Print("decode"); tCopy.Print("copy"); tConvert.Print("convert");
	std::printf("  bytes/frame: planes %zu  rgba %zu  packed-layout %zu (%dx%zu R8)\n", yuvBytes, rgbaBytes, packed, fmt.width,
		static_cast<size_t>(fmt.height) + ch + (fmt.hasAlpha ? fmt.height : 0));
	std::printf("  ring 4 slots: %.1f MB (planes+rgba), %.1f MB without rgba\n", 4.0 * (yuvBytes + rgbaBytes) / 1048576.0, 4.0 * yuvBytes / 1048576.0);
	(void)cw;
	if (alphaZeroN)
		std::printf("  alpha==0 pixels: n=%zu mean Y %.1f U %.1f V %.1f (premultiplied limited black would be Y16 U128 V128)\n", alphaZeroN,
			alphaZeroY / alphaZeroN, alphaZeroU / alphaZeroN, alphaZeroV / alphaZeroN);
	return 0;
}
