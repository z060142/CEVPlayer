// M8 measurement probe: what the process RAM of one playing video consists of (docs/design/M6-ui-source.md section 10:
// the engine measured 110..160 MB per 1080p video while the frame ring alone accounts for 44..51 MB).
//
//   ram_probe <clip.webm> [frames=90] [ringRgba 0|1 = 1]
//
// Part 1 builds the pieces of PlayerCore one after the other and prints the private bytes (commit charge) after each:
// whole-file source (the engine's CCryPakByteSource also reads the whole file), demuxer index, libvpx decoder(s)
// after decoding `frames` packets (reference frames, frame buffer pool, thread / row-MT buffers), the frame ring as
// PlayerCore sizes it. Part 2 plays the same clip through a real PlayerCore (InternalClock, 3 s, audio decoded) and
// prints its total, which should match the sum of part 1 plus the audio.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include "CEVCore/FrameRing.h"
#include "CEVCore/IByteSource.h"
#include "CEVCore/PlayerCore.h"
#include "CEVCore/VideoDecoder.h"
#include "CEVCore/WebmDemuxer.h"

using namespace cev;

namespace
{
	double PrivateMb()
	{
		PROCESS_MEMORY_COUNTERS_EX pmc = {};
		pmc.cb = sizeof(pmc);
		GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
		return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
	}

	double WorkingSetMb()
	{
		PROCESS_MEMORY_COUNTERS_EX pmc = {};
		pmc.cb = sizeof(pmc);
		GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
		return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
	}

	struct Meter
	{
		double last = PrivateMb();
		double base = last;
		void Step(const char* what)
		{
			const double now = PrivateMb();
			std::printf("  %-44s +%7.1f MB  (total +%7.1f MB, working set %7.1f MB)\n", what, now - last, now - base, WorkingSetMb());
			last = now;
		}
	};
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: ram_probe <clip.webm> [frames=90] [ringRgba 0|1]\n");
		return 2;
	}
	const int frames = argc > 2 ? std::atoi(argv[2]) : 90;
	const bool ringRgba = argc > 3 ? std::atoi(argv[3]) != 0 : true;
	std::printf("clip %s\n part 1: components (private bytes)\n", argv[1]);

	{
		Meter m;
		std::unique_ptr<IByteSource> src;
		if (FileByteSource::Open(argv[1], true, src) != Status::Ok) { std::fprintf(stderr, "open failed\n"); return 1; }
		char what[96];
		std::snprintf(what, sizeof(what), "whole-file source (%.1f MB file)", static_cast<double>(src->Length()) / 1048576.0);
		m.Step(what);

		std::unique_ptr<WebmDemuxer> dmx;
		if (WebmDemuxer::Open(std::move(src), dmx) != Status::Ok || !dmx->HasVideo()) { std::fprintf(stderr, "demux failed\n"); return 1; }
		const VideoStreamInfo& v = dmx->Video();
		std::snprintf(what, sizeof(what), "demuxer index (%u video, %u audio packets)", dmx->VideoPacketCount(), dmx->AudioPacketCount());
		m.Step(what);

		uint32_t threads = v.height >= 1000 ? 4u : (v.height >= 700 ? 2u : 1u);
		threads = std::min(threads, std::max(1u, std::thread::hardware_concurrency()));
		std::unique_ptr<VideoDecoder> dec;
		if (VideoDecoder::Create(v, threads, dec) != Status::Ok) { std::fprintf(stderr, "decoder failed\n"); return 1; }
		std::snprintf(what, sizeof(what), "libvpx init (%u threads%s)", threads, v.hasAlpha ? ", + alpha decoder" : "");
		m.Step(what);

		std::vector<uint8_t> pkt(v.maxPacketSize + 64), add(v.maxAdditionalSize + 64);
		m.Step("packet buffers");
		int decoded = 0;
		bool first = true;
		FrameFormat fmt;
		for (uint32_t i = 0; i < dmx->VideoPacketCount() && decoded < frames; ++i)
		{
			const PacketRef& p = dmx->VideoPacket(i);
			if (dmx->ReadVideoPacket(i, pkt.data(), static_cast<uint32_t>(pkt.size())) != Status::Ok) return 1;
			uint32_t addSize = 0;
			if (p.additionalSize != 0 && dmx->ReadVideoAdditional(i, add.data(), static_cast<uint32_t>(add.size())) == Status::Ok)
				addSize = p.additionalSize;
			if (dec->Decode(pkt.data(), p.size, addSize ? add.data() : nullptr, addSize) != Status::Ok) return 1;
			if (!dec->HasFrame())
				continue;
			if (first)
			{
				first = false;
				DecodedImageInfo info;
				dec->GetFrameInfo(info);
				fmt.width = info.width; fmt.height = info.height;
				fmt.chromaShiftX = info.chromaShiftX; fmt.chromaShiftY = info.chromaShiftY;
				fmt.hasAlpha = v.hasAlpha; fmt.withRgba = ringRgba;
				m.Step("libvpx after the first frame");
			}
			++decoded;
		}
		std::snprintf(what, sizeof(what), "libvpx after %d frames", decoded);
		m.Step(what);

		FrameRing ring;
		if (ring.Init(fmt, 4) != Status::Ok) { std::fprintf(stderr, "ring failed\n"); return 1; }
		std::snprintf(what, sizeof(what), "frame ring 4 slots (rgba %d)", ringRgba ? 1 : 0);
		m.Step(what);
		// Touch every slot (the OS commits pages on first write; Init already memsets the planes, the RGBA buffer is
		// zero initialised by make_unique and therefore written as well).
		for (uint32_t s = 0; s < 4; ++s)
			dec->WriteFrame(fmt, ring.GetSlot(s));
		m.Step("frame ring written");
		ring.Shutdown();
		dec.reset();
		dmx.reset();
		m.Step("all freed");
	}

	std::printf(" part 2: PlayerCore, 3 s of playback (InternalClock, audio decoded, ringSlots 4, outputRgba %d)\n", ringRgba ? 1 : 0);
	{
		Meter m;
		PlayerConfig cfg;
		cfg.outputRgba = ringRgba;
		cfg.decodeAudio = true;
		cfg.outputSampleRate = 48000;
		PlayerCore player(cfg);
		m.Step("PlayerCore + decode pool");
		std::unique_ptr<IByteSource> src;
		if (FileByteSource::Open(argv[1], true, src) != Status::Ok) return 1;
		player.Load(std::move(src));
		for (int i = 0; i < 2000 && player.GetLoadState() == LoadState::Loading; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		if (player.GetLoadState() != LoadState::Ready) { std::fprintf(stderr, "load failed\n"); return 1; }
		m.Step("loaded (first frame)");
		player.Play();
		std::vector<float> audio(4096 * 2);
		const auto t0 = std::chrono::steady_clock::now();
		while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3))
		{
			player.Update();
			player.ReadAudioResampled(audio.data(), 240); // 5 ms of 48 kHz stereo
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		const MediaInfo& info = player.GetInfo();
		std::printf("  %dx%d alpha=%d audio=%d (%u Hz %u ch)\n", info.width, info.height, info.hasAlpha ? 1 : 0, info.hasAudio ? 1 : 0, info.audioSampleRate, info.audioChannels);
		m.Step("after 3 s of playback");
		player.Unload();
		m.Step("unloaded");
	}
	return 0;
}
