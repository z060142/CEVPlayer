#include "StdAfx.h"
#include "PluginEnv.h"
#include "Contract/ContractTest.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <CryCore/Platform/CryWindows.h>
#include <Cry3DEngine/I3DEngine.h>
#include <Cry3DEngine/IMaterial.h>
#include <CryRenderer/ITexture.h>

#include "Contract/TexSource.h"
#include "Player/PlayerManager.h"
#include "Player/VideoPlayerImpl.h"
#include "Render/FullscreenOutput.h"

namespace CEVPlayer
{
	namespace Contract
	{
		namespace
		{
			//! Real seconds since the first call (steady_clock). The engine's async time follows the smoothed / clamped
			//! frame time and runs 5-17% slow, which the wall-clock checks must not see.
			double SteadySeconds()
			{
				static const std::chrono::steady_clock::time_point s_start = std::chrono::steady_clock::now();
				return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
			}

			constexpr const char* kSurface = "CevSelfTest";
			constexpr uint64_t kConsumerA = 0x1111;
			constexpr uint64_t kConsumerB = 0x2222;
			constexpr uint64_t kConsumerC = 0x3333;
			constexpr uint64_t kConsumerD = 0x4444;

			struct STest
			{
				enum class EStep { Idle, WaitReadyA, Playing, PlayAdvance, PausedSettle, PausedHold, PausedSeek, Evict, WaitReadyB, M7 };
				EStep step = EStep::Idle;
				const CryTexSourceApi* api = nullptr;
				IVideoPlayer* player = nullptr;
				std::string pathA, pathB;
				double mark = 0.0;
				bool ready = false;
				int frames = 0;
				int checks = 0;
				int failed = 0;
				uint64_t serial = 0;
				uint32_t generation = 0;
				int width = 0;
				std::string textureName;

				void Check(bool ok, const std::string& what)
				{
					++checks;
					if (!ok)
						++failed;
					LogAlways("cev_contract_selftest: %s %s", ok ? "ok  " : "FAIL", what.c_str());
				}
				double Now() const { return SteadySeconds(); }
				bool Query(const char* name, CryTexSourceInfo& out)
				{
					std::memset(&out, 0, sizeof(out));
					out.size = sizeof(out);
					return api->query(name, &out) == 1;
				}
				void Hint(uint64_t consumer, uint32_t flags, int w, int h, uint32_t accept, uint32_t size = sizeof(CryTexSourceHint), const char* name = kSurface)
				{
					CryTexSourceHint hint = {};
					hint.size = size;
					hint.flags = flags;
					hint.pixelWidth = w;
					hint.pixelHeight = h;
					hint.consumer = consumer;
					hint.acceptLayouts = accept;
					api->hint(name, &hint);
				}
				SHintSummary Summary()
				{
					SHintSummary s;
					GetHintSummary(kSurface, s);
					return s;
				}
			};

			STest s_test;

			std::string Num(long long v) { return std::to_string(v); }

			// ======================================================================================== M7 part
			// Runs after the M6 part with the same STest: a list of steps, each called every frame until it returns
			// true. Consumers in `sustain` send their hint every frame (rule 5: live consumers hint every frame).
			constexpr const char* kM7 = "CevM7";          // level logic surface, 2 s keyframes
			constexpr const char* kM7End = "CevM7End";    // level logic surface, end / loop / markers while hidden
			constexpr const char* kM7Var = "CevM7Var";    // level logic surface, variants
			constexpr const char* kM7Open = "CevM7Open";  // made by open
			constexpr const char* kM7Missing = "CevM7Missing";
			constexpr const char* kM7Ctl = "CevM7Ctl";

			struct SRecorder final : public IVideoPlayerListener
			{
				int ended = 0, looped = 0, markers = 0, started = 0, errors = 0;
				double endedPlayerTime = -1.0;
				void OnEnded(IVideoPlayer& p) override { ++ended; endedPlayerTime = p.GetTime(); }
				void OnLooped(IVideoPlayer&) override { ++looped; }
				void OnMarker(IVideoPlayer&, const char*, double) override { ++markers; }
				void OnStarted(IVideoPlayer&) override { ++started; }
				void OnError(IVideoPlayer&, int, const char*) override { ++errors; }
			};

			struct SSustain { std::string name; uint64_t consumer; uint32_t flags; int h; uint32_t accept; };

			struct SM7
			{
				std::vector<std::function<bool(STest&, double)>> steps;
				size_t index = 0;
				double stepStart = 0.0;
				bool stepEntered = false;
				std::vector<SSustain> sustain;
				SRecorder rec;
				IVideoPlayer* p = nullptr;      // kM7
				IVideoPlayer* pEnd = nullptr;
				IVideoPlayer* pVar = nullptr;
				std::string pathKey, pathEnd, pathVar, pathSmall, pathOther;
				cev::StatsSnapshot s0;
				uint64_t serial = 0;
				uint64_t uploads = 0;
				uint64_t catchUps = 0;
				double wallMark = 0.0;
				double playerTimeMark = 0.0;
				uint32_t generation = 0;
				int width = 0;
				int savedStats = 0;
				float savedStop = 2.f;
				const void* openPlayer = nullptr;
			};
			SM7 s_m7;

			CVideoPlayerImpl* Impl(IVideoPlayer* p) { return static_cast<CVideoPlayerImpl*>(p); }

			// ======================================================================================== cev_hidden_bench
			// Measurement (M7): one surface player, a consumer hint toggled between hidden and visible; per cycle the
			// catch-up time and frame count of the visible request. With cev_stats_file the per-second rows carry the
			// visibility state, so the decode / copy / colour / upload costs of the three states can be read there.
			struct SBench
			{
				enum class EPhase { Idle, Loading, Warm, Hidden, WaitFrame, Visible };
				EPhase phase = EPhase::Idle;
				IVideoPlayer* p = nullptr;
				int cycles = 10, cycle = 0;
				double hiddenSec = 4.0, visibleSec = 2.0, mark = 0.0;
				uint64_t serial = 0, catchUps = 0;
				std::vector<float> ms;
				std::vector<uint32_t> frames;
			};
			SBench s_bench;
			constexpr const char* kBench = "CevBench";
			constexpr uint64_t kBenchConsumer = 0xBE7C;

			double BenchNow() { return SteadySeconds(); }

			void EndBench(const char* why)
			{
				SBench& b = s_bench;
				if (b.p != nullptr && mEnv != nullptr && mEnv->pManager != nullptr)
					mEnv->pManager->DestroyPlayer(b.p);
				b.p = nullptr;
				b.phase = SBench::EPhase::Idle;
				std::vector<float> sorted = b.ms;
				std::sort(sorted.begin(), sorted.end());
				double sum = 0.0;
				uint32_t maxFrames = 0;
				for (size_t i = 0; i < b.ms.size(); ++i)
				{
					sum += b.ms[i];
					maxFrames = std::max(maxFrames, b.frames[i]);
				}
				if (!sorted.empty())
					LogAlways("cev_hidden_bench: done (%s): %u catch-ups, ms min %.2f median %.2f avg %.2f max %.2f, frames max %u", why, static_cast<unsigned>(sorted.size()),
						sorted.front(), sorted[sorted.size() / 2], sum / sorted.size(), sorted.back(), maxFrames);
				else
					LogAlways("cev_hidden_bench: done (%s): no catch-up measured", why);
			}

			void TickBench()
			{
				SBench& b = s_bench;
				if (b.phase == SBench::EPhase::Idle || mEnv == nullptr || mEnv->pManager == nullptr || b.p == nullptr)
					return;
				const double now = BenchNow();
				const bool hidden = b.phase == SBench::EPhase::Hidden;
				if (b.phase != SBench::EPhase::Loading && b.phase != SBench::EPhase::Warm)
				{
					CryTexSourceHint hint = {};
					hint.size = sizeof(hint);
					hint.flags = hidden ? 0u : 1u;
					hint.pixelWidth = 1920;
					hint.pixelHeight = 1080;
					hint.consumer = kBenchConsumer;
					CryTexSource_v1()->hint(kBench, &hint);
				}
				CVideoPlayerImpl* const p = static_cast<CVideoPlayerImpl*>(b.p);
				CryTexSourceInfo info = {};
				info.size = sizeof(info);
				CryTexSource_v1()->query(kBench, &info);
				switch (b.phase)
				{
				case SBench::EPhase::Loading:
					if (p->GetState() == EVideoPlayerState::Error)
						return EndBench("load failed");
					if (p->IsLoaded())
					{
						b.phase = SBench::EPhase::Warm;
						b.mark = now;
					}
					return;
				case SBench::EPhase::Warm:
					if (now - b.mark < b.visibleSec)
						return;
					LogAlways("cev_hidden_bench: phase visible -> hidden (cycle %d)", b.cycle + 1);
					b.phase = SBench::EPhase::Hidden;
					b.mark = now;
					return;
				case SBench::EPhase::Hidden:
					if (now - b.mark < b.hiddenSec)
						return;
					b.serial = info.contentSerial;
					b.catchUps = p->Core().GetStats().catchUps;
					LogAlways("cev_hidden_bench: phase hidden -> visible (cycle %d, state was %s, player time %.3f)", b.cycle + 1, p->VisibilityName(), p->GetTime());
					b.phase = SBench::EPhase::WaitFrame;
					b.mark = now;
					return;
				case SBench::EPhase::WaitFrame:
				{
					const cev::StatsSnapshot s = p->Core().GetStats();
					if (info.contentSerial == b.serial || s.catchUps == b.catchUps)
					{
						if (now - b.mark > 10.0)
							return EndBench("no frame after a visible request within 10 s");
						return;
					}
					double off = 0.0;
					p->GetAvOffsetMs(off);
					b.ms.push_back(s.lastCatchUpMs);
					b.frames.push_back(s.lastCatchUpFrames);
					LogAlways("cev_hidden_bench: cycle %d catch-up %.2f ms, %u frames, first frame %.1f ms behind the clock, %.1f ms of wall time until it was presented",
						b.cycle + 1, static_cast<double>(s.lastCatchUpMs), s.lastCatchUpFrames, off, (now - b.mark) * 1000.0);
					b.phase = SBench::EPhase::Visible;
					b.mark = now;
					return;
				}
				case SBench::EPhase::Visible:
					if (now - b.mark < b.visibleSec)
						return;
					if (++b.cycle >= b.cycles)
						return EndBench("all cycles");
					LogAlways("cev_hidden_bench: phase visible -> hidden (cycle %d)", b.cycle + 1);
					b.phase = SBench::EPhase::Hidden;
					b.mark = now;
					return;
				default:
					return;
				}
			}

			void Sustain(const char* name, uint64_t consumer, uint32_t flags, int h, uint32_t accept = 0u)
			{
				for (SSustain& s : s_m7.sustain)
				{
					if (s.consumer == consumer && stricmp(s.name.c_str(), name) == 0)
					{
						s.flags = flags;
						s.h = h;
						s.accept = accept;
						return;
					}
				}
				s_m7.sustain.push_back(SSustain{ name, consumer, flags, h, accept });
			}

			void Unsustain(const char* name, uint64_t consumer, bool release)
			{
				for (size_t i = 0; i < s_m7.sustain.size(); ++i)
				{
					if (s_m7.sustain[i].consumer == consumer && stricmp(s_m7.sustain[i].name.c_str(), name) == 0)
					{
						s_m7.sustain.erase(s_m7.sustain.begin() + static_cast<std::ptrdiff_t>(i));
						break;
					}
				}
				if (release)
					s_test.Hint(consumer, 2u, 0, 0, 0u, sizeof(CryTexSourceHint), name);
			}

			uint64_t Serial(STest& t, const char* name)
			{
				CryTexSourceInfo info = {};
				return t.Query(name, info) ? info.contentSerial : 0;
			}

			double Elapsed(double now) { return now - s_m7.stepStart; }

			bool LoadedFor(STest& t, IVideoPlayer* p, double now, double seconds, const char* what)
			{
				if (p == nullptr)
					return true;
				if (!p->IsLoaded())
				{
					if (Elapsed(now) > 15.0)
					{
						t.Check(false, std::string(what) + " did not become ready within 15 s");
						return true;
					}
					s_m7.wallMark = now;
					return false;
				}
				return now - s_m7.wallMark >= seconds;
			}


			// ======================================================================================== M8 part
			// Layout negotiation (docs/design/M6-ui-source.md section 3 rules 5-8, section 5.2): packed planes only while
			// every live consumer accepts them, the stream is 4:2:0 with an even width, there is no fullscreen / material
			// output and cev_allowPlanar is on. The test plays the role of the consumer and checks the uploaded bytes.
			constexpr const char* kM8 = "CevM8";       // vp8_vorbis.webm 640x360 (audio: the clock is the audio clock)
			constexpr const char* kM8A = "CevM8A";     // vp9_alpha.webm 640x360 alpha
			constexpr const char* kM8X = "CevM8X";     // vp9_444.webm / vp9_odd_321x181.webm: never packed
			constexpr uint64_t kM8ConsumerA = 0x8A;    // accepts packed YUV420 + YUVA420
			constexpr uint64_t kM8ConsumerB = 0x8B;    // RGBA8 only
			constexpr uint32_t kAcceptAll = 0x7u;      // RGBA8 | PACKED_YUV420 | PACKED_YUVA420
			constexpr uint32_t kAccept420 = 0x3u;      // RGBA8 | PACKED_YUV420

			struct SM8
			{
				IVideoPlayer* p = nullptr;
				IVideoPlayer* pA = nullptr;
				IVideoPlayer* pX = nullptr;
				uint32_t generation = 0;
				uint64_t serial = 0;
				uint64_t switchTick = 0;      // player tick of the last observed switch
				uint64_t loads = 0;
				double wallMark = 0.0;        // wall clock / player time when the step began (audio continuity)
				double timeMark = 0.0;
				int savedAllowPlanar = 1;
				int savedStats = 0;
				bool active = false;
				IMaterial* pMaterial = nullptr;
			};
			SM8 s_m8;

			uint64_t Fnv(uint64_t h, const uint8_t* p, size_t n)
			{
				for (size_t i = 0; i < n; ++i)
					h = (h ^ p[i]) * 0x100000001B3ull;
				return h;
			}

			//! The packed picture rebuilt from the core's planes at the rectangles the snapshot reports (independent of the
			//! module's upload code), hashed like CVideoPlayerImpl hashes what it uploads.
			bool PackedHashFromCore(CVideoPlayerImpl* p, const CryTexSourceInfo& info, uint64_t& hash, uint64_t& serial)
			{
				cev::FrameView v;
				if (!p->Core().GetFrame(v) || info.planeY.w != v.width || info.planeY.h != v.height)
					return false;
				const int texW = info.planeY.w;
				const int texH = info.planeU.y + info.planeU.h + (info.planeA.w > 0 ? info.planeA.h : 0);
				std::vector<uint8_t> img(static_cast<size_t>(texW) * texH, 0);
				const CryTexSourceRect rects[4] = { info.planeY, info.planeU, info.planeV, info.planeA };
				for (int plane = 0; plane < 4; ++plane)
				{
					const CryTexSourceRect& r = rects[plane];
					if (r.w <= 0 || v.plane[plane] == nullptr)
						continue;
					for (int y = 0; y < r.h; ++y)
						std::memcpy(img.data() + static_cast<size_t>(r.y + y) * texW + r.x, v.plane[plane] + static_cast<ptrdiff_t>(y) * v.stride[plane], static_cast<size_t>(r.w));
				}
				hash = Fnv(0xCBF29CE484222325ull, img.data(), img.size());
				serial = v.serial;
				return true;
			}

			ITexture* Tex(IVideoPlayer* p) { return p != nullptr ? p->GetTexture() : nullptr; }

			//! Waits (up to `timeout` s of the step) until the snapshot's generation differs from s_m8.generation.
			bool WaitGeneration(STest& t, const char* name, double now, double timeout, CryTexSourceInfo& info, const char* what)
			{
				t.Query(name, info);
				if (info.generation != s_m8.generation)
					return true;
				if (Elapsed(now) > timeout)
				{
					t.Check(false, std::string(what) + ": no texture switch within " + std::to_string(timeout) + " s");
					return true;
				}
				return false;
			}

			void CheckPacked(STest& t, IVideoPlayer* p, const CryTexSourceInfo& info, bool alpha, const char* what)
			{
				const int w = p->GetWidth(), h = p->GetHeight(), ch = (h + 1) / 2;
				const uint32_t layout = alpha ? CTS_LAYOUT_PACKED_YUVA420 : CTS_LAYOUT_PACKED_YUV420;
				t.Check(info.layout == layout && static_cast<uint32_t>(p->GetPixelLayout()) == layout, std::string(what) + ": layout " + Num(info.layout) + " (player " + Num(static_cast<long long>(p->GetPixelLayout())) + ")");
				t.Check(info.planeY.x == 0 && info.planeY.y == 0 && info.planeY.w == w && info.planeY.h == h
					&& info.planeU.x == 0 && info.planeU.y == h && info.planeU.w == w / 2 && info.planeU.h == ch
					&& info.planeV.x == w / 2 && info.planeV.y == h && info.planeV.w == w / 2 && info.planeV.h == ch
					&& (alpha ? (info.planeA.x == 0 && info.planeA.y == h + ch && info.planeA.w == w && info.planeA.h == h) : (info.planeA.w == 0 && info.planeA.h == 0)),
					std::string(what) + ": plane rects Y(" + Num(info.planeY.w) + "x" + Num(info.planeY.h) + ") U(" + Num(info.planeU.x) + "," + Num(info.planeU.y) + ") V(" + Num(info.planeV.x) + "," + Num(info.planeV.y) + ") A(" + Num(info.planeA.y) + "," + Num(info.planeA.h) + ")");
				ITexture* const pTex = Tex(p);
				const int texH = h + ch + (alpha ? h : 0);
				t.Check(pTex != nullptr && info.textureId == pTex->GetTextureID() && pTex->GetWidth() == w && pTex->GetHeight() == texH && pTex->GetTextureDstFormat() == eTF_R8 && pTex->GetNumMips() == 1,
					std::string(what) + ": texture R8 " + Num(w) + "x" + Num(texH) + " (got " + (pTex ? Num(pTex->GetWidth()) + "x" + Num(pTex->GetHeight()) + " format " + Num(static_cast<long long>(pTex->GetTextureDstFormat())) : std::string("none")) + ")");
				t.Check((info.flags & 1u) == 0 && (info.flags & 8u) == 0 && ((info.flags & 2u) != 0) == alpha && info.width == w && info.height == h, std::string(what) + ": flags 0x" + Num(info.flags) + ", width / height = picture");
				t.Check(info.colorMatrix == 0 && info.colorRange == 0 && info.chromaSiting == CTS_SITING_LEFT && info.textureId2 == 0, std::string(what) + ": BT.601 limited, siting left (" + Num(info.colorMatrix) + "/" + Num(info.colorRange) + "/" + Num(info.chromaSiting) + ")");
			}

			void CheckRgba(STest& t, IVideoPlayer* p, const CryTexSourceInfo& info, const char* what)
			{
				ITexture* const pTex = Tex(p);
				t.Check(info.layout == CTS_LAYOUT_RGBA8 && p->GetPixelLayout() == EVideoPixelLayout::RGBA8 && (info.flags & 1u) != 0 && info.planeY.w == 0,
					std::string(what) + ": RGBA8, premultiplied, no plane rects (layout " + Num(info.layout) + ")");
				t.Check(pTex != nullptr && info.textureId == pTex->GetTextureID() && pTex->GetWidth() == p->GetWidth() && pTex->GetHeight() == p->GetHeight() && pTex->GetTextureDstFormat() == eTF_R8G8B8A8,
					std::string(what) + ": texture RGBA8 at the picture size");
			}

			//! The texture switch itself: generation +1, content in the same tick, no reload, the clock continuous.
			void CheckSwitch(STest& t, IVideoPlayer* p, const CryTexSourceInfo& info, double now, const char* what, bool checkHysteresis)
			{
				CVideoPlayerImpl* const impl = Impl(p);
				t.Check(info.generation == s_m8.generation + 1u, std::string(what) + ": generation +1 (" + Num(s_m8.generation) + " -> " + Num(info.generation) + ")");
				t.Check(info.contentSerial > s_m8.serial, std::string(what) + ": contentSerial increased");
				t.Check(impl->FirstUploadTick() == impl->TextureTick() && impl->TextureTick() != 0, std::string(what) + ": first upload in the tick the texture was made (tick " + Num(static_cast<long long>(impl->TextureTick())) + ", upload " + Num(static_cast<long long>(impl->FirstUploadTick())) + ")");
				t.Check(impl->LoadCount() == s_m8.loads, std::string(what) + ": no reload (loads " + Num(static_cast<long long>(impl->LoadCount())) + ")");
				const double wall = now - s_m8.wallMark;
				const double media = info.timeSeconds - s_m8.timeMark;
				t.Check(std::fabs(media - wall) <= 0.05 || media < 0.0 /* loop wrap */, std::string(what) + ": clock continuous (media " + std::to_string(media) + " s, wall " + std::to_string(wall) + " s, clock " + impl->ClockSourceName() + ")");
				if (checkHysteresis)
				{
					const uint64_t ticks = impl->TextureTick() - s_m8.switchTick;
					t.Check(ticks >= 30, std::string(what) + ": hysteresis, " + Num(static_cast<long long>(ticks)) + " ticks after the previous switch (>= 30)");
				}
				s_m8.generation = info.generation;
				s_m8.serial = info.contentSerial;
				s_m8.switchTick = impl->TextureTick();
				s_m8.wallMark = now;
				s_m8.timeMark = info.timeSeconds;
			}

			//! Marks the state before a condition change.
			void Mark(STest& t, const char* name, double now)
			{
				CryTexSourceInfo info = {};
				t.Query(name, info);
				s_m8.generation = info.generation;
				s_m8.serial = info.contentSerial;
				s_m8.wallMark = now;
				s_m8.timeMark = info.timeSeconds;
			}

			void AddM8Steps(std::vector<std::function<bool(STest&, double)>>& steps)
			{
				auto add = [&](std::function<bool(STest&, double)> s) { steps.push_back(s); };

				add([](STest& t, double) {
					SM8& m = s_m8;
					m = SM8();
					m.savedAllowPlanar = mEnv->m_cevAllowPlanar;
					m.savedStats = mEnv->m_cevStats;
					m.active = true;
					mEnv->m_cevAllowPlanar = 1;
					mEnv->m_cevStats = 1; // the core counts converted bytes only with stats on
					m.p = mEnv->pManager->CreatePlayerForSurface(kM8);
					t.Check(m.p != nullptr, "M8: CreatePlayerForSurface CevM8");
					if (m.p == nullptr)
						return true;
					m.p->SetVolume(0.f);
					m.p->SetLoop(true);
					t.Check(m.p->Load("Videos/vp8_vorbis.webm"), "M8: Load vp8_vorbis.webm");
					m.p->Play();
					return true;
				});
				add([](STest& t, double now) {
					if (!LoadedFor(t, s_m8.p, now, 0.5, "M8 video"))
						return false;
					CryTexSourceInfo info = {};
					t.Query(kM8, info);
					CheckRgba(t, s_m8.p, info, "no consumer");
					s_m8.loads = Impl(s_m8.p)->LoadCount();
					Impl(s_m8.p)->SetUploadChecksum(true);
					Mark(t, kM8, now);
					s_m8.switchTick = 0;
					Sustain(kM8, kM8ConsumerA, 1u, 360, kAcceptAll);
					return true;
				});
				// one consumer that accepts packed planes -> packed, at once (no switch for longer than the hold)
				add([](STest& t, double now) {
					CryTexSourceInfo info = {};
					if (!WaitGeneration(t, kM8, now, 2.0, info, "accepting consumer"))
						return false;
					CheckSwitch(t, s_m8.p, info, now, "RGBA8 -> packed", false);
					CheckPacked(t, s_m8.p, info, false, "accepting consumer");
					// the same tick the consumer must not see RGBA: anonymous consumer arrives right now (hysteresis test)
					Sustain(kM8, 0, 1u, 360, 0u);
					return true;
				});
				add([](STest& t, double now) {
					CryTexSourceInfo info = {};
					if (!WaitGeneration(t, kM8, now, 3.0, info, "anonymous consumer"))
						return false;
					CheckSwitch(t, s_m8.p, info, now, "packed -> RGBA8 (anonymous consumer)", true);
					CheckRgba(t, s_m8.p, info, "anonymous consumer");
					Unsustain(kM8, 0, true); // release
					return true;
				});
				add([](STest& t, double now) {
					CryTexSourceInfo info = {};
					if (!WaitGeneration(t, kM8, now, 3.0, info, "anonymous released"))
						return false;
					CheckSwitch(t, s_m8.p, info, now, "RGBA8 -> packed (anonymous released)", true);
					CheckPacked(t, s_m8.p, info, false, "anonymous released");
					return true;
				});
				// pixel check: the bytes uploaded into the R8 texture are the core's planes at the reported rects
				add([](STest& t, double now) {
					if (Elapsed(now) < 0.3)
						return false;
					s_m8.p->Pause();
					return true;
				});
				add([](STest& t, double now) {
					if (Elapsed(now) < 0.3)
						return false;
					CVideoPlayerImpl* const impl = Impl(s_m8.p);
					CryTexSourceInfo info = {};
					t.Query(kM8, info);
					uint64_t hash = 0, serial = 0;
					const bool ok = PackedHashFromCore(impl, info, hash, serial);
					t.Check(ok && serial == impl->LastUploadSerial() && hash == impl->LastUploadChecksum() && impl->LastUploadBytes() == static_cast<uint64_t>(info.planeY.w) * (info.planeU.y + info.planeU.h),
						"packed upload = core planes at the snapshot rects (serial " + Num(static_cast<long long>(serial)) + "/" + Num(static_cast<long long>(impl->LastUploadSerial())) + ", bytes " + Num(static_cast<long long>(impl->LastUploadBytes())) + ")");
					t.Check(impl->Core().RgbaBufferCount() == 0, "packed: the core holds no RGBA buffer (" + Num(impl->Core().RgbaBufferCount()) + ")");
					const cev::StatsSnapshot st = impl->Core().GetStats();
					s_m8.serial = st.colorBytes; // reused as a mark
					s_m8.p->Resume();
					s_m8.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					if (Elapsed(now) < 0.5)
						return false;
					CVideoPlayerImpl* const impl = Impl(s_m8.p);
					t.Check(impl->Core().GetStats().colorBytes == s_m8.serial, "packed: no colour conversion while playing");
					Mark(t, kM8, now);
					Sustain(kM8, kM8ConsumerB, 1u, 360, 1u); // a v2 consumer that only takes RGBA8
					return true;
				});
				add([](STest& t, double now) {
					CryTexSourceInfo info = {};
					if (!WaitGeneration(t, kM8, now, 3.0, info, "RGBA8-only consumer"))
						return false;
					CheckSwitch(t, s_m8.p, info, now, "packed -> RGBA8 (consumer B)", true);
					CheckRgba(t, s_m8.p, info, "consumer B");
					Unsustain(kM8, kM8ConsumerB, false); // stops hinting: expires after 120 updates
					return true;
				});
				add([](STest& t, double now) {
					CryTexSourceInfo info = {};
					if (!WaitGeneration(t, kM8, now, 10.0, info, "consumer B expired"))
						return false;
					t.Check(Impl(s_m8.p)->HintConsumers() == 1, "B expired after 120 updates (" + Num(Impl(s_m8.p)->HintConsumers()) + " consumers)");
					CheckSwitch(t, s_m8.p, info, now, "RGBA8 -> packed (B expired)", true);
					CheckPacked(t, s_m8.p, info, false, "B expired");
					mEnv->m_cevAllowPlanar = 0;
					return true;
				});
				add([](STest& t, double now) {
					CryTexSourceInfo info = {};
					if (!WaitGeneration(t, kM8, now, 3.0, info, "cev_allowPlanar 0"))
						return false;
					CheckSwitch(t, s_m8.p, info, now, "cev_allowPlanar 0", false);
					CheckRgba(t, s_m8.p, info, "cev_allowPlanar 0");
					mEnv->m_cevAllowPlanar = 1;
					return true;
				});
				add([](STest& t, double now) {
					CryTexSourceInfo info = {};
					if (!WaitGeneration(t, kM8, now, 3.0, info, "cev_allowPlanar 1"))
						return false;
					CheckSwitch(t, s_m8.p, info, now, "cev_allowPlanar 1", true);
					CheckPacked(t, s_m8.p, info, false, "cev_allowPlanar 1");
					// material output: RGBA8 while bound (a warning about the empty test material's slot is expected)
					IMaterialManager* const pMm = gEnv->p3DEngine != nullptr ? gEnv->p3DEngine->GetMaterialManager() : nullptr;
					s_m8.pMaterial = pMm != nullptr ? pMm->CreateMaterial("cev_m8_selftest") : nullptr;
					if (s_m8.pMaterial != nullptr)
						s_m8.pMaterial->AddRef();
					t.Check(s_m8.pMaterial != nullptr && s_m8.p->BindMaterial(s_m8.pMaterial, -1, 0), "BindMaterial (test material)");
					return true;
				});
				add([](STest& t, double now) {
					CryTexSourceInfo info = {};
					if (!WaitGeneration(t, kM8, now, 3.0, info, "material output"))
						return false;
					CheckSwitch(t, s_m8.p, info, now, "material output", false);
					CheckRgba(t, s_m8.p, info, "material output");
					s_m8.p->UnbindMaterial(s_m8.pMaterial);
					return true;
				});
				add([](STest& t, double now) {
					CryTexSourceInfo info = {};
					if (!WaitGeneration(t, kM8, now, 3.0, info, "material unbound"))
						return false;
					CheckSwitch(t, s_m8.p, info, now, "material unbound", true);
					CheckPacked(t, s_m8.p, info, false, "material unbound");
					s_m8.p->SetFullscreen(true, EVideoFit::Letterbox);
					return true;
				});
				add([](STest& t, double now) {
					CryTexSourceInfo info = {};
					if (FullscreenOutput::SupportsPlanar())
					{
						// L4: the fullscreen output draws packed layouts, the consumer accepts them: nothing changes (7 checks,
						// as many as the RGBA8 branch below)
						t.Query(kM8, info);
						if (info.generation == s_m8.generation && Elapsed(now) < 3.0)
							return false;
						t.Check(info.generation == s_m8.generation, "fullscreen (planar output): no texture switch within 3 s, generation unchanged (" + Num(s_m8.generation) + " -> " + Num(info.generation) + ")");
						t.Check(Impl(s_m8.p)->LoadCount() == s_m8.loads, "fullscreen (planar output): no reload (loads " + Num(static_cast<long long>(Impl(s_m8.p)->LoadCount())) + ")");
						CheckPacked(t, s_m8.p, info, false, "fullscreen (planar output)");
					}
					else
					{
						if (!WaitGeneration(t, kM8, now, 3.0, info, "fullscreen"))
							return false;
						CheckSwitch(t, s_m8.p, info, now, "fullscreen", false);
						CheckRgba(t, s_m8.p, info, "fullscreen");
					}
					s_m8.p->SetFullscreen(false, EVideoFit::Letterbox);
					Unsustain(kM8, kM8ConsumerA, true);
					return true;
				});
				add([](STest& t, double now) {
					if (Elapsed(now) < 1.0)
						return false;
					CryTexSourceInfo info = {};
					t.Query(kM8, info);
					CheckRgba(t, s_m8.p, info, "fullscreen off, last consumer released");
					mEnv->pManager->DestroyPlayer(s_m8.p);
					s_m8.p = nullptr;
					// alpha: a consumer that takes PACKED_YUV420 but not YUVA420 keeps RGBA8
					s_m8.pA = mEnv->pManager->CreatePlayerForSurface(kM8A);
					t.Check(s_m8.pA != nullptr && s_m8.pA->Load("Videos/vp9_alpha.webm"), "M8: alpha surface");
					if (s_m8.pA != nullptr)
					{
						s_m8.pA->SetLoop(true);
						s_m8.pA->Play();
					}
					Sustain(kM8A, kM8ConsumerA, 1u, 360, kAccept420);
					return true;
				});
				add([](STest& t, double now) {
					if (!LoadedFor(t, s_m8.pA, now, 1.0, "M8 alpha video"))
						return false;
					CryTexSourceInfo info = {};
					t.Query(kM8A, info);
					CheckRgba(t, s_m8.pA, info, "alpha, consumer without YUVA420");
					t.Check(info.generation == 1u, "alpha: no switch (generation " + Num(info.generation) + ")");
					Impl(s_m8.pA)->SetUploadChecksum(true);
					s_m8.loads = Impl(s_m8.pA)->LoadCount();
					Mark(t, kM8A, now);
					s_m8.switchTick = 0;
					Sustain(kM8A, kM8ConsumerA, 1u, 360, kAcceptAll);
					return true;
				});
				add([](STest& t, double now) {
					CryTexSourceInfo info = {};
					if (!WaitGeneration(t, kM8A, now, 3.0, info, "alpha, consumer with YUVA420"))
						return false;
					CheckSwitch(t, s_m8.pA, info, now, "alpha RGBA8 -> PACKED_YUVA420", false);
					CheckPacked(t, s_m8.pA, info, true, "alpha");
					s_m8.pA->Pause();
					return true;
				});
				add([](STest& t, double now) {
					if (Elapsed(now) < 0.3)
						return false;
					CVideoPlayerImpl* const impl = Impl(s_m8.pA);
					CryTexSourceInfo info = {};
					t.Query(kM8A, info);
					uint64_t hash = 0, serial = 0;
					const bool ok = PackedHashFromCore(impl, info, hash, serial);
					t.Check(ok && serial == impl->LastUploadSerial() && hash == impl->LastUploadChecksum(), "alpha: packed upload = core planes incl. A at the snapshot rects");
					Unsustain(kM8A, kM8ConsumerA, true);
					mEnv->pManager->DestroyPlayer(s_m8.pA);
					s_m8.pA = nullptr;
					// 4:4:4 and odd width: never packed, even when the consumer accepts everything
					s_m8.pX = mEnv->pManager->CreatePlayerForSurface(kM8X);
					t.Check(s_m8.pX != nullptr && s_m8.pX->Load("Videos/m8_444.webm"), "M8: 4:4:4 surface");
					if (s_m8.pX != nullptr)
						s_m8.pX->Play();
					Sustain(kM8X, kM8ConsumerA, 1u, 180, kAcceptAll);
					return true;
				});
				add([](STest& t, double now) {
					if (!LoadedFor(t, s_m8.pX, now, 1.0, "M8 4:4:4 video"))
						return false;
					CryTexSourceInfo info = {};
					t.Query(kM8X, info);
					CheckRgba(t, s_m8.pX, info, "4:4:4 with an accepting consumer");
					t.Check(info.generation == 1u && Impl(s_m8.pX)->Core().GetOutputRgba(), "4:4:4: no switch, the core converts");
					t.Check(s_m8.pX->Load("Videos/vp9_odd_321x181.webm"), "M8: odd width on the same surface");
					s_m8.pX->Play();
					return true;
				});
				add([](STest& t, double now) {
					if (!LoadedFor(t, s_m8.pX, now, 1.0, "M8 odd width video"))
						return false;
					CryTexSourceInfo info = {};
					t.Query(kM8X, info);
					CheckRgba(t, s_m8.pX, info, "odd width with an accepting consumer");
					Unsustain(kM8X, kM8ConsumerA, true);
					mEnv->pManager->DestroyPlayer(s_m8.pX);
					s_m8.pX = nullptr;
					if (s_m8.pMaterial != nullptr)
						s_m8.pMaterial->Release();
					s_m8.pMaterial = nullptr;
					mEnv->m_cevAllowPlanar = s_m8.savedAllowPlanar;
					mEnv->m_cevStats = s_m8.savedStats;
					s_m8.active = false;
					return true;
				});
			}

			void CleanupM8()
			{
				SM8& m = s_m8;
				if (mEnv != nullptr && mEnv->pManager != nullptr)
				{
					for (IVideoPlayer** pp : { &m.p, &m.pA, &m.pX })
					{
						if (*pp != nullptr)
							mEnv->pManager->DestroyPlayer(*pp);
						*pp = nullptr;
					}
					if (m.active)
					{
						mEnv->m_cevAllowPlanar = m.savedAllowPlanar;
						mEnv->m_cevStats = m.savedStats;
					}
				}
				m.active = false;
				if (m.pMaterial != nullptr)
					m.pMaterial->Release();
				m.pMaterial = nullptr;
			}

			void BuildM7Steps(STest& t)
			{
				SM7& m = s_m7;
				m.steps.clear();
				using Step = std::function<bool(STest&, double)>;
				auto add = [&](Step s) { m.steps.push_back(s); };

				// ---- setup: a looping 2 s keyframe video, nobody hints yet
				add([](STest& t, double) {
					SM7& m = s_m7;
					m.savedStats = mEnv->m_cevStats;
					m.savedStop = mEnv->m_cevHiddenStopSeconds;
					mEnv->m_cevStats = 1;               // counters of the core (copy / colour bytes)
					mEnv->m_cevHiddenStopSeconds = 1.f; // shorter than the default 2 s for the test
					m.p = mEnv->pManager->CreatePlayerForSurface(kM7);
					t.Check(m.p != nullptr, "M7: CreatePlayerForSurface CevM7");
					if (m.p == nullptr)
						return true;
					m.p->SetVolume(0.f);
					m.p->SetLoop(true);
					t.Check(m.p->Load(m.pathKey.c_str()), "M7: Load " + m.pathKey);
					m.p->Play();
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (!LoadedFor(t, m.p, now, 1.0, "M7 video"))
						return false;
					m.serial = Serial(t, kM7);
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (now - m.wallMark < 0.5)
						return false;
					CVideoPlayerImpl* const p = Impl(m.p);
					t.Check(p->HintConsumers() == 0 && std::strcmp(p->VisibilityName(), "visible") == 0, std::string("no hint: visible (") + p->VisibilityName() + ")");
					t.Check(Serial(t, kM7) > m.serial, "no hint: contentSerial increases as before");
					// ---- one consumer, visible = 0
					Sustain(kM7, kConsumerA, 0u, 720);
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (now - m.wallMark < 0.3)
						return false;
					CVideoPlayerImpl* const p = Impl(m.p);
					t.Check(std::strcmp(p->VisibilityName(), "decode-only") == 0, std::string("hidden: decode-only (") + p->VisibilityName() + ")");
					m.s0 = p->Core().GetStats();
					m.serial = Serial(t, kM7);
					m.uploads = p->UploadCount();
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (now - m.wallMark < 0.5)
						return false;
					CVideoPlayerImpl* const p = Impl(m.p);
					const cev::StatsSnapshot s = p->Core().GetStats();
					t.Check(Serial(t, kM7) == m.serial, "decode-only: contentSerial does not move");
					t.Check(s.copyBytes == m.s0.copyBytes && s.colorBytes == m.s0.colorBytes && s.uploadBytes == m.s0.uploadBytes && p->UploadCount() == m.uploads,
						"decode-only: copy / colour / upload bytes 0 (+" + Num(static_cast<long long>(s.copyBytes - m.s0.copyBytes)) + " / +" + Num(static_cast<long long>(s.colorBytes - m.s0.colorBytes)) + " / +" + Num(static_cast<long long>(s.uploadBytes - m.s0.uploadBytes)) + ")");
					t.Check(s.framesDecodeOnly > m.s0.framesDecodeOnly + 5, "decode-only: still decoding (+" + Num(static_cast<long long>(s.framesDecodeOnly - m.s0.framesDecodeOnly)) + " frames in 0.5 s)");
					t.Check(p->GetTime() > 0.0 && p->GetState() == EVideoPlayerState::Playing, "decode-only: the clock goes on");
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					CVideoPlayerImpl* const p = Impl(m.p);
					if (std::strcmp(p->VisibilityName(), "stopped") != 0)
					{
						if (Elapsed(now) > 2.0)
						{
							t.Check(false, "hidden: not stopped 2 s after the 1 s threshold");
							return true;
						}
						return false;
					}
					t.Check(true, "hidden longer than cev_hiddenStopSeconds: stopped");
					m.s0 = p->Core().GetStats();
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (now - m.wallMark < 0.5)
						return false;
					CVideoPlayerImpl* const p = Impl(m.p);
					const cev::StatsSnapshot s = p->Core().GetStats();
					t.Check(s.framesDecodeOnly == m.s0.framesDecodeOnly && s.framesDecoded == m.s0.framesDecoded, "stopped: no decoding (decode-only +" + Num(static_cast<long long>(s.framesDecodeOnly - m.s0.framesDecodeOnly)) + ")");
					m.serial = Serial(t, kM7);
					m.catchUps = s.catchUps;
					Sustain(kM7, kConsumerA, 1u, 720); // visible again
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					CVideoPlayerImpl* const p = Impl(m.p);
					if (Serial(t, kM7) == m.serial)
					{
						if (Elapsed(now) > 3.0)
						{
							t.Check(false, "visible again: no new frame within 3 s");
							return true;
						}
						return false;
					}
					const cev::StatsSnapshot s = p->Core().GetStats();
					const double fps = 30.0;
					t.Check(s.catchUps == m.catchUps + 1 && s.lastCatchUpMs > 0.f, "visible again: catch-up recorded (" + std::to_string(s.lastCatchUpMs) + " ms, " + Num(s.lastCatchUpFrames) + " frames)");
					t.Check(s.lastCatchUpFrames <= static_cast<uint32_t>(2.0 * fps), "catch-up frames <= keyframe interval (" + Num(s.lastCatchUpFrames) + " <= 60)");
					double ms = 0.0;
					t.Check(p->GetAvOffsetMs(ms) && std::fabs(ms) <= 2000.0 / fps, "picture after the catch-up matches the clock within 2 frames (" + std::to_string(ms) + " ms)");
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (now - m.wallMark < 0.3)
						return false;
					const uint64_t a = Serial(t, kM7);
					t.Check(a > m.serial + 3, "visible again: contentSerial increases");
					// ---- two consumers: one visible, one not
					Sustain(kM7, kConsumerB, 0u, 360);
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (now - m.wallMark < 0.2)
						return false;
					CVideoPlayerImpl* const p = Impl(m.p);
					t.Check(p->HintConsumers() == 2 && std::strcmp(p->VisibilityName(), "visible") == 0, std::string("A visible + B hidden: visible (") + p->VisibilityName() + ")");
					t.Check(p->HintPixelHeight() == 720, "aggregated pixel height = max (" + Num(p->HintPixelHeight()) + ")");
					Unsustain(kM7, kConsumerA, true); // A releases
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (now - m.wallMark < 0.2)
						return false;
					CVideoPlayerImpl* const p = Impl(m.p);
					t.Check(p->HintConsumers() == 1 && std::strcmp(p->VisibilityName(), "visible") != 0, std::string("A released, B hidden: hidden (") + p->VisibilityName() + ")");
					// A comes back visible, then stops sending: after 120 updates it expires and B alone hides the surface
					Sustain(kM7, kConsumerA, 1u, 720);
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (now - m.wallMark < 0.2)
						return false;
					t.Check(std::strcmp(Impl(m.p)->VisibilityName(), "visible") == 0, "A visible again: visible");
					Unsustain(kM7, kConsumerA, false); // silent: expires after 120 of our updates
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					CVideoPlayerImpl* const p = Impl(m.p);
					if (p->HintConsumers() != 1)
					{
						if (Elapsed(now) > 10.0)
						{
							t.Check(false, "A did not expire within 10 s");
							return true;
						}
						return false;
					}
					t.Check(std::strcmp(p->VisibilityName(), "visible") != 0, std::string("A expired after 120 updates: hidden (") + p->VisibilityName() + ")");
					// ---- PauseDecodeWhenHidden = off: hidden but decoding and uploading as usual
					p->SetPauseDecodeWhenHidden(false);
					m.serial = Serial(t, kM7);
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (now - m.wallMark < 0.5)
						return false;
					CVideoPlayerImpl* const p = Impl(m.p);
					t.Check(std::strcmp(p->VisibilityName(), "visible") == 0 && Serial(t, kM7) > m.serial + 5, "PauseDecodeWhenHidden off: decoding and uploading while hidden");
					p->SetPauseDecodeWhenHidden(true);
					Unsustain(kM7, kConsumerB, true);
					mEnv->pManager->DestroyPlayer(m.p);
					m.p = nullptr;
					return true;
				});

				// ---- end / loop / markers while hidden (non looping first)
				add([](STest& t, double now) {
					SM7& m = s_m7;
					m.pEnd = mEnv->pManager->CreatePlayerForSurface(kM7End);
					t.Check(m.pEnd != nullptr, "M7: CreatePlayerForSurface CevM7End");
					if (m.pEnd == nullptr)
						return true;
					m.rec = SRecorder();
					m.pEnd->AddListener(&m.rec);
					m.pEnd->SetVolume(0.f);
					Sustain(kM7End, kConsumerC, 0u, 100); // hidden from the start
					t.Check(m.pEnd->Load(m.pathEnd.c_str()), "M7: Load " + m.pathEnd);
					m.pEnd->Play();
					m.wallMark = -1.0;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (m.pEnd == nullptr)
						return true;
					if (m.wallMark < 0.0)
					{
						if (m.pEnd->GetState() == EVideoPlayerState::Playing)
							m.wallMark = now; // started
						else if (Elapsed(now) > 15.0)
						{
							t.Check(false, "end test video did not start");
							return true;
						}
						return false;
					}
					const double duration = m.pEnd->GetDuration();
					if (m.rec.ended == 0 && now - m.wallMark < duration + 3.0)
						return false;
					const double wall = now - m.wallMark;
					CryTexSourceInfo info = {};
					t.Query(kM7End, info);
					t.Check(m.rec.ended == 1 && info.state == 5, "hidden: Ended once, state 5 (ended " + Num(m.rec.ended) + ", state " + Num(info.state) + ")");
					t.Check(std::fabs(wall - duration) < 0.25, "hidden: Ended after " + std::to_string(wall) + " s of wall time, duration " + std::to_string(duration));
					t.Check(std::fabs(m.rec.endedPlayerTime - duration) < 0.05, "hidden: player time at Ended " + std::to_string(m.rec.endedPlayerTime));
					t.Check(m.rec.markers == 2, "hidden: both markers fired (" + Num(m.rec.markers) + ")");
					t.Check(std::strcmp(Impl(m.pEnd)->VisibilityName(), "stopped") == 0, std::string("hidden for the whole video: stopped (") + Impl(m.pEnd)->VisibilityName() + ")");
					// looping run, still hidden
					m.rec = SRecorder();
					m.pEnd->SetLoop(true);
					m.pEnd->Play();
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (m.pEnd == nullptr)
						return true;
					const double duration = m.pEnd->GetDuration();
					if (m.rec.looped == 0 && now - m.wallMark < duration + 3.0)
						return false;
					const double wall = now - m.wallMark;
					CryTexSourceInfo info = {};
					t.Query(kM7End, info);
					t.Check(m.rec.looped == 1 && info.loopCount == 1 && m.rec.ended == 0, "hidden loop: Looped once, loopCount 1, no Ended (looped " + Num(m.rec.looped) + ", loopCount " + Num(info.loopCount) + ")");
					t.Check(std::fabs(wall - duration) < 0.25, "hidden loop: wrap after " + std::to_string(wall) + " s, duration " + std::to_string(duration));
					t.Check(m.rec.markers == 2 && info.state == 3 && (info.flags & 4u) != 0, "hidden loop: markers fired, state 3, flags playing (" + Num(m.rec.markers) + ")");
					Unsustain(kM7End, kConsumerC, true);
					m.pEnd->RemoveListener(&m.rec);
					mEnv->pManager->DestroyPlayer(m.pEnd);
					m.pEnd = nullptr;
					return true;
				});

				// ---- variant by the aggregated pixel height at the start
				add([](STest& t, double) {
					SM7& m = s_m7;
					m.pVar = mEnv->pManager->CreatePlayerForSurface(kM7Var);
					t.Check(m.pVar != nullptr, "M7: CreatePlayerForSurface CevM7Var");
					if (m.pVar == nullptr)
						return true;
					m.pVar->SetVolume(0.f);
					t.Hint(kConsumerD, 1u, 533, 300, 0u, sizeof(CryTexSourceHint), kM7Var);
					Sustain(kM7Var, kConsumerD, 1u, 300);
					t.Check(m.pVar->Load(m.pathVar.c_str()), "M7: Load " + m.pathVar);
					m.pVar->Play();
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (!LoadedFor(t, m.pVar, now, 0.3, "variant descriptor"))
						return false;
					t.Check(m.pVar->GetVariant() == 1 && m.pVar->GetHeight() == 360, "hint height 300 -> variant 1 (360) (" + Num(m.pVar->GetVariant()) + ", " + Num(m.pVar->GetHeight()) + ")");
					m.pVar->Stop();
					Sustain(kM7Var, kConsumerD, 1u, 700);
					m.wallMark = -1.0;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (m.wallMark < 0.0)
					{
						m.wallMark = now; // the 700 hint is in the table after this frame's tick
						return false;
					}
					if (now - m.wallMark < 0.1)
						return false;
					m.rec = SRecorder();
					m.pVar->AddListener(&m.rec);
					m.pVar->Play(); // from Stopped: picks again -> reload with the 720 variant
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (!(m.pVar->IsLoaded() && m.pVar->GetState() == EVideoPlayerState::Playing && m.pVar->GetVariant() == 2) && Elapsed(now) < 10.0)
						return false;
					t.Check(m.pVar->GetVariant() == 2 && m.pVar->GetHeight() == 720 && m.pVar->GetState() == EVideoPlayerState::Playing, "Play from Stopped with hint 700 -> variant 2 (720), playing (" + Num(m.pVar->GetVariant()) + ")");
					t.Check(m.rec.started == 1, "Started sent once for the reloaded start (" + Num(m.rec.started) + ")");
					m.pVar->RemoveListener(&m.rec);
					m.pVar->Stop();
					Unsustain(kM7Var, kConsumerD, true); // nobody hints: the main video
					m.wallMark = -1.0;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (m.wallMark < 0.0)
					{
						m.wallMark = now;
						return false;
					}
					if (now - m.wallMark < 0.1)
						return false;
					m.pVar->Play();
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (!(m.pVar->IsLoaded() && m.pVar->GetVariant() == 0) && Elapsed(now) < 10.0)
						return false;
					t.Check(m.pVar->GetVariant() == 0 && m.pVar->GetHeight() == 1080, "no hint -> main video (" + Num(m.pVar->GetVariant()) + ", height " + Num(m.pVar->GetHeight()) + ")");
					Sustain(kM7Var, kConsumerD, 1u, 300);
					m.pVar->SetVariant(3); // forced: wins over the height
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (!(m.pVar->IsLoaded() && m.pVar->GetVariant() == 3) && Elapsed(now) < 10.0)
						return false;
					t.Check(m.pVar->GetVariant() == 3, "SetVariant(3) wins over hint 300 (" + Num(m.pVar->GetVariant()) + ")");
					m.pVar->Stop();
					m.pVar->Play(); // forced: no reload at the start
					t.Check(m.pVar->GetVariant() == 3 && m.pVar->IsLoaded(), "forced variant stays at Play");
					Unsustain(kM7Var, kConsumerD, true);
					mEnv->pManager->DestroyPlayer(m.pVar);
					m.pVar = nullptr;
					return true;
				});

				// ---- open / close
				add([](STest& t, double) {
					SM7& m = s_m7;
					CryTexSourceOpen o = {};
					o.size = sizeof(o);
					o.flags = 1u; // autoplay
					o.volume = 0.f;
					LARGE_INTEGER f, a, b;
					::QueryPerformanceFrequency(&f);
					::QueryPerformanceCounter(&a);
					const int r = t.api->open(kM7Open, m.pathSmall.c_str(), &o);
					::QueryPerformanceCounter(&b);
					const double ms = static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
					t.Check(r == 1 && ms < 1.0, "open returns 1 at once (" + Num(r) + ", " + std::to_string(ms) + " ms)");
					CryTexSourceInfo info = {};
					const bool queried = t.Query(kM7Open, info);
					t.Check(queried && info.state == 1, "after open: query 1, state 1 (loading) (" + Num(info.state) + ")");
					t.Check(t.api->open("CevM7Bad", "Videos/x.mp4", &o) == 0 && t.api->open("CevM7Bad", "Videos/x", &o) == 0, "open with another extension -> 0");
					t.Check(t.api->open("bad id", m.pathSmall.c_str(), &o) == 0, "open with an invalid id -> 0");
					t.Check(t.api->open(kSurface, m.pathSmall.c_str(), &o) == 2, "open of a surface made by level logic -> 2");
					t.api->close(kSurface); // not ours: nothing happens
					t.Check(t.api->open(kM7Missing, "Videos/no_such_file.webm", nullptr) == 1, "open of a missing file -> 1 (the error comes later)");
					t.Check(HasPendingCommands(), "M10: HasPendingCommands after open is true");
					return true;
				});
				add([](STest& t, double) {
					// one manager tick ran since the previous step
					t.Check(!HasPendingCommands(), "M10: HasPendingCommands after the next tick is false");
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					CryTexSourceInfo info = {};
					t.Query(kM7Open, info);
					if (info.state != 3 && Elapsed(now) < 10.0)
						return false;
					t.Check(info.state == 3 && (info.flags & 4u) != 0 && info.textureId > 0, "open: loading -> playing (state " + Num(info.state) + ")");
					t.Check(mEnv->pManager->FindPlayerBySurface(kSurface) != nullptr, "close of a level logic surface does not destroy it");
					CryTexSourceInfo miss = {};
					const bool missQueried = t.Query(kM7Missing, miss);
					t.Check(missQueried && miss.state == 6 && miss.errorCode != 0, "missing file: state 6, errorCode " + Num(miss.errorCode));
					t.api->close(kM7Missing);
					m.openPlayer = mEnv->pManager->FindPlayerBySurface(kM7Open);
					m.generation = info.generation;
					m.width = info.width;
					m.playerTimeMark = info.timeSeconds;
					CryTexSourceOpen o = {};
					o.size = sizeof(o);
					o.flags = 1u;
					t.Check(t.api->open(kM7Open, m.pathSmall.c_str(), &o) == 1, "open again with the same src -> 1");
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (now - m.wallMark < 0.3)
						return false;
					CryTexSourceInfo info = {};
					t.Query(kM7Open, info);
					t.Check(mEnv->pManager->FindPlayerBySurface(kM7Open) == m.openPlayer && info.generation == m.generation && info.timeSeconds > m.playerTimeMark,
						"same src: not reopened (time " + std::to_string(m.playerTimeMark) + " -> " + std::to_string(info.timeSeconds) + ")");
					t.Check(!t.Query(kM7Missing, info), "closed missing surface: query 0");
					CryTexSourceOpen o = {};
					o.size = sizeof(o);
					o.flags = 1u;
					t.Check(t.api->open(kM7Open, m.pathOther.c_str(), &o) == 1, "open with another src -> 1");
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					CryTexSourceInfo info = {};
					t.Query(kM7Open, info);
					if (!(info.state == 3 && info.width != m.width) && Elapsed(now) < 10.0)
						return false;
					t.Check(info.generation == m.generation + 1u && info.width != m.width, "another src: media switched, generation +1 (" + Num(m.generation) + " -> " + Num(info.generation) + ", width " + Num(info.width) + ")");
					t.api->close(kM7Open);
					t.Check(!t.Query(kM7Open, info), "close: query 0 at once");
					m.wallMark = now;
					return true;
				});
				add([](STest& t, double now) {
					SM7& m = s_m7;
					if (now - m.wallMark < 0.2)
						return false;
					CryTexSourceInfo info = {};
					t.Check(!t.Query(kM7Open, info) && mEnv->pManager->FindPlayerBySurface(kM7Open) == nullptr, "after close: the player is gone");
					// ---- control
					t.Check(t.api->open(kM7Ctl, m.pathSmall.c_str(), nullptr) == 1, "open without options (no autoplay)");
					t.Check(t.api->control(kM7Ctl, CTS_VERB_SEEK, 2.0) == 1, "SEEK while loading accepted");
					t.Check(t.api->control(kM7Ctl, CTS_VERB_RATE, 10.0) == 0 && t.api->control(kM7Ctl, CTS_VERB_RATE, 0.1) == 0
						&& t.api->control(kM7Ctl, CTS_VERB_RATE, std::nan("")) == 0, "RATE outside the core's range / NaN -> 0");
					{
						const double inf = std::numeric_limits<double>::infinity();
						const int neg = t.api->control(kM7Ctl, CTS_VERB_SEEK, -1.0);
						const int big = t.api->control(kM7Ctl, CTS_VERB_SEEK, inf);
						t.Check(neg == 0 && big == 0, "SEEK negative / infinite -> 0 (" + Num(neg) + ", " + Num(big) + ", inf=" + std::to_string(inf) + ")");
					}
					t.Check(t.api->control(kM7Ctl, CTS_VERB_VOLUME, 2.0) == 0 && t.api->control(kM7Ctl, CTS_VERB_LOOP, 0.5) == 0 && t.api->control(kM7Ctl, 99u, 0.0) == 0, "VOLUME 2, LOOP 0.5, unknown verb -> 0");
					t.Check(t.api->control("no_such_surface", CTS_VERB_PLAY, 0.0) == 0, "control of an unknown name -> 0");
					return true;
				});
				add([](STest& t, double now) {
					CryTexSourceInfo info = {};
					t.Query(kM7Ctl, info);
					if (info.state != 2 && Elapsed(now) < 10.0)
						return false;
					t.Check(info.state == 2 && std::fabs(info.timeSeconds - 2.0) < 0.05, "loaded: state 2 (ready) at the remembered SEEK 2.0 (" + Num(info.state) + ", t=" + std::to_string(info.timeSeconds) + ")");
					t.Check(t.api->control(kM7Ctl, CTS_VERB_PLAY, 0.0) == 1 && t.api->control(kM7Ctl, CTS_VERB_VOLUME, 0.0) == 1 && t.api->control(kM7Ctl, CTS_VERB_MUTE, 1.0) == 1
						&& t.api->control(kM7Ctl, CTS_VERB_RATE, 2.0) == 1, "PLAY, VOLUME 0, MUTE 1, RATE 2 accepted");
					return true;
				});
				add([](STest& t, double now) {
					if (Elapsed(now) < 0.3)
						return false;
					CryTexSourceInfo info = {};
					t.Query(kM7Ctl, info);
					CVideoPlayerImpl* const p = Impl(mEnv->pManager->FindPlayerBySurface(kM7Ctl));
					t.Check(info.state == 3 && (info.flags & 4u) != 0, "PLAY -> state 3, flags playing");
					t.Check(p != nullptr && p->IsMuted() && p->GetVolume() == 0.f && p->GetRate() == 2.f, "MUTE / VOLUME / RATE applied");
					t.Check(t.api->control(kM7Ctl, CTS_VERB_PAUSE, 0.0) == 1, "PAUSE accepted");
					return true;
				});
				add([](STest& t, double now) {
					if (Elapsed(now) < 0.2)
						return false;
					CryTexSourceInfo info = {};
					t.Query(kM7Ctl, info);
					t.Check(info.state == 4 && (info.flags & 4u) == 0, "PAUSE -> state 4, playing bit clear (" + Num(info.state) + ")");
					t.Check(t.api->control(kM7Ctl, CTS_VERB_SEEK, 1.0) == 1, "SEEK 1.0 accepted");
					return true;
				});
				add([](STest& t, double now) {
					if (Elapsed(now) < 0.2)
						return false;
					CryTexSourceInfo info = {};
					t.Query(kM7Ctl, info);
					t.Check(std::fabs(info.timeSeconds - 1.0) < 0.05 && info.state == 4, "SEEK -> timeSeconds " + std::to_string(info.timeSeconds));
					t.Check(t.api->control(kM7Ctl, CTS_VERB_LOOP, 1.0) == 1 && t.api->control(kM7Ctl, CTS_VERB_SEEK, info.durationSeconds - 0.3) == 1
						&& t.api->control(kM7Ctl, CTS_VERB_RATE, 1.0) == 1 && t.api->control(kM7Ctl, CTS_VERB_PLAY, 0.0) == 1, "LOOP 1, SEEK near the end, PLAY accepted");
					return true;
				});
				add([](STest& t, double now) {
					CryTexSourceInfo info = {};
					t.Query(kM7Ctl, info);
					if (info.loopCount == 0 && Elapsed(now) < 3.0)
						return false;
					t.Check(info.loopCount == 1 && info.state == 3 && info.timeSeconds < 1.0, "LOOP: wrap -> loopCount 1, still playing (" + Num(info.loopCount) + ")");
					CVideoPlayerImpl* const p = Impl(mEnv->pManager->FindPlayerBySurface(kM7Ctl));
					t.Check(p != nullptr && p->IsLooping() && p->OwnedByOpen(), "LOOP applied, player owned by open");
					t.api->close(kM7Ctl);
					return true;
				});
				add([](STest& t, double now) {
					if (Elapsed(now) < 0.2)
						return false;
					CryTexSourceInfo info = {};
					t.Check(!t.Query(kM7Ctl, info) && mEnv->pManager->FindPlayerBySurface(kM7Ctl) == nullptr, "close: control surface gone");
					t.Check(t.api->control(kM7Ctl, CTS_VERB_PLAY, 0.0) == 0, "control after close -> 0");
					mEnv->m_cevStats = s_m7.savedStats;
					mEnv->m_cevHiddenStopSeconds = s_m7.savedStop;
					return true;
				});
				AddM8Steps(m.steps); // M8: layout negotiation
			}

			//! false while steps remain.
			bool TickM7(STest& t, double now)
			{
				SM7& m = s_m7;
				for (const SSustain& s : m.sustain)
					t.Hint(s.consumer, s.flags, s.h * 16 / 9, s.h, s.accept, sizeof(CryTexSourceHint), s.name.c_str());
				while (m.index < m.steps.size())
				{
					if (!m.stepEntered)
					{
						m.stepEntered = true;
						m.stepStart = now;
					}
					if (!m.steps[m.index](t, now))
						return false;
					++m.index;
					m.stepEntered = false;
					return false; // one step per frame: the manager ticks in between
				}
				return true;
			}

			void CleanupM7()
			{
				SM7& m = s_m7;
				if (mEnv != nullptr && mEnv->pManager != nullptr)
				{
					for (IVideoPlayer** pp : { &m.p, &m.pEnd, &m.pVar })
					{
						if (*pp != nullptr)
						{
							(*pp)->RemoveListener(&m.rec);
							mEnv->pManager->DestroyPlayer(*pp);
						}
						*pp = nullptr;
					}
					for (const char* name : { kM7Open, kM7Ctl, kM7Missing })
					{
						if (IVideoPlayer* const q = mEnv->pManager->FindPlayerBySurface(name))
							mEnv->pManager->DestroyPlayer(q);
					}
				}
				CleanupM8();
				m.sustain.clear();
				m.steps.clear();
				m.index = 0;
				m.stepEntered = false;
			}

			void Fail(STest& t, const char* why)
			{
				t.Check(false, why);
				CleanupM7();
				if (t.player != nullptr && mEnv->pManager != nullptr)
					mEnv->pManager->DestroyPlayer(t.player);
				t.player = nullptr;
				t.step = STest::EStep::Idle;
				LogAlways("cev_contract_selftest: FAIL (%d checks, %d failed)", t.checks, t.failed);
			}

			void Finish(STest& t)
			{
				CPlayerManager& m = *mEnv->pManager;
				m.DestroyPlayer(t.player);
				t.player = nullptr;
				CryTexSourceInfo info = {};
				t.Check(!t.Query(kSurface, info), "query after DestroyPlayer returns 0");
				t.Check(m.FindPlayerBySurface(kSurface) == nullptr, "FindPlayerBySurface after DestroyPlayer is nullptr");
				t.Hint(kConsumerA, 1u, 10, 10, 0); // a hint for a name that is gone: ignored, no crash
				IVideoPlayer* const again = m.CreatePlayerForSurface("cevselftest");
				t.Check(again != nullptr, "the id can be used again after the player was destroyed");
				if (again != nullptr)
					m.DestroyPlayer(again);
				t.step = STest::EStep::Idle;
				LogAlways("cev_contract_selftest: %s (%d checks, %d failed)", t.failed == 0 ? "PASS" : "FAIL", t.checks, t.failed);
			}
		}

		void StartSelfTest(const char* pathA, const char* pathB)
		{
			STest& t = s_test;
			if (t.step != STest::EStep::Idle)
			{
				LogWarning("cev_contract_selftest is already running");
				return;
			}
			if (mEnv == nullptr || mEnv->pManager == nullptr)
				return;
			CPlayerManager& m = *mEnv->pManager;
			t = STest();
			t.pathA = pathA != nullptr && pathA[0] != '\0' ? pathA : "Videos/vp9_alpha.webm";
			t.pathB = pathB != nullptr && pathB[0] != '\0' ? pathB : "Videos/vp9_odd_321x181.webm";
			CleanupM7();
			s_m7.pathKey = "Videos/m7_key2s.webm";        // looping, keyframe every 2 s
			s_m7.pathEnd = "Videos/m7_markers.cevvideo";  // vp9_odd_321x181 (5 s) with markers at 1.0 and 2.5
			s_m7.pathVar = "Videos/m7_variants.cevvideo"; // main 1080 + variants 360 / 720 / 1080
			s_m7.pathSmall = "Videos/vp9_odd_321x181.webm";
			s_m7.pathOther = "Videos/vp9_alpha.webm";
			LogAlways("cev_contract_selftest: start (%s, %s)", t.pathA.c_str(), t.pathB.c_str());

			// The export of this module, found the way a consumer finds it (no internal shortcut).
			HMODULE self = nullptr;
			::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&StartSelfTest), &self);
			using PFN_Source = const CryTexSourceApi* (*)(void);
			const PFN_Source fn = self != nullptr ? reinterpret_cast<PFN_Source>(::GetProcAddress(self, "CryTexSource_v1")) : nullptr;
			t.Check(fn != nullptr, "GetProcAddress(CryTexSource_v1)");
			t.api = fn != nullptr ? fn() : nullptr;
			if (t.api == nullptr)
			{
				Fail(t, "no contract table");
				return;
			}
			t.Check(t.api->version == 2u, "version is 2 (" + Num(t.api->version) + ")");
			t.Check(t.api->query && t.api->hint && t.api->open && t.api->close && t.api->control, "all five function pointers exist");
			CryTexSourceInfo info = {};
			t.Check(!t.Query("no_such_surface", info), "query of an unknown name returns 0");
			t.Check(t.api->open("no_such_surface", "Videos/x.avi", nullptr) == 0 && t.api->control("no_such_surface", CTS_VERB_PLAY, 0.0) == 0, "open of a non-webm src / control of an unknown name return 0");
			t.api->close("no_such_surface");

			// registry
			t.player = m.CreatePlayerForSurface(kSurface);
			t.Check(t.player != nullptr, "CreatePlayerForSurface");
			if (t.player == nullptr)
			{
				Fail(t, "no player");
				return;
			}
			t.Check(std::strcmp(t.player->GetTextureName(), "$cev:CevSelfTest") == 0, std::string("texture name is $cev:<id> (") + t.player->GetTextureName() + ")");
			t.Check(std::strcmp(t.player->GetSurfaceId(), kSurface) == 0, "GetSurfaceId");
			t.Check(t.player->GetPixelLayout() == EVideoPixelLayout::RGBA8 && t.player->GetDecodeBackend() == EVideoDecodeBackend::Cpu, "GetPixelLayout RGBA8, GetDecodeBackend Cpu");
			t.Check(m.CreatePlayerForSurface("CEVSELFTEST") == nullptr, "a duplicate id (other case) is refused (error expected in the log)");
			t.Check(m.CreatePlayerForSurface("bad id") == nullptr && m.CreatePlayerForSurface("") == nullptr
				&& m.CreatePlayerForSurface(std::string(64, 'a').c_str()) == nullptr, "invalid ids are refused (errors expected in the log)");
			t.Check(m.CreatePlayerForSurface(std::string(63, 'a').c_str()) != nullptr, "a 63 character id is accepted");
			if (IVideoPlayer* const p63 = m.FindPlayerBySurface(std::string(63, 'A').c_str()))
				m.DestroyPlayer(p63);
			t.Check(m.FindPlayerBySurface("cevSELFtest") == t.player, "FindPlayerBySurface is case-insensitive");
			t.Check(m.FindPlayerBySurface("nope") == nullptr, "FindPlayerBySurface of an unknown id is nullptr");
			t.Check(t.Query("cevselftest", info), "query (other case) returns 1 right after creation");
			t.Check(info.state == 0 && info.textureId == 0 && info.width == 0 && info.generation == 0 && info.layout == CTS_LAYOUT_RGBA8,
				"empty snapshot: state 0, textureId 0, width 0, generation 0, layout RGBA8");

			// hints
			t.Hint(kConsumerA, 1u, 640, 360, 7u);
			t.Hint(kConsumerB, 0u, 1280, 720, 3u);
			t.Hint(0, 1u, 320, 200, 0u, static_cast<uint32_t>(offsetof(CryTexSourceHint, consumer))); // v1 consumer
			SHintSummary s = t.Summary();
			t.Check(s.consumers == 3 && s.visible && s.pixelWidth == 1280 && s.pixelHeight == 720 && s.acceptLayouts == 1u,
				"hints aggregate: 3 consumers, visible, 1280x720, accept RGBA8 only (anonymous consumer) - got " + Num(s.consumers) + " " + Num(s.visible) + " " + Num(s.pixelWidth) + "x" + Num(s.pixelHeight) + " 0x" + Num(s.acceptLayouts));
			t.Hint(kConsumerA, 2u, 0, 0, 0u); // release
			t.Hint(0, 2u, 0, 0, 0u);          // release of the anonymous consumer
			s = t.Summary();
			t.Check(s.consumers == 1 && !s.visible && s.pixelWidth == 1280 && s.acceptLayouts == 3u, "release removes consumers (left: B, invisible, accept 0x3)");
			t.Hint(kConsumerB, 2u, 0, 0, 0u); // M7: a hidden consumer would pause the decoding of the M6 part
			t.Check(t.Summary().consumers == 0, "B released: nobody hints, the surface counts as visible");
			SHintSummary unknown;
			t.Hint(kConsumerA, 1u, 1, 1, 0u, sizeof(CryTexSourceHint), "no_such_surface");
			t.Check(!GetHintSummary("no_such_surface", unknown), "a hint for an unknown name is not recorded");

			t.player->SetVolume(0.f);
			t.Check(t.player->Load(t.pathA.c_str()), "Load A accepted");
			t.player->Play();
			t.mark = t.Now();
			t.step = STest::EStep::WaitReadyA;
		}

		void StartHiddenBench(const char* path, int cycles, float hiddenSeconds, float visibleSeconds)
		{
			SBench& b = s_bench;
			if (b.phase != SBench::EPhase::Idle || mEnv == nullptr || mEnv->pManager == nullptr)
			{
				LogWarning("cev_hidden_bench: already running or no manager");
				return;
			}
			b = SBench();
			b.cycles = cycles > 0 ? cycles : 10;
			b.hiddenSec = hiddenSeconds > 0.f ? hiddenSeconds : 4.0;
			b.visibleSec = visibleSeconds > 0.f ? visibleSeconds : 2.0;
			b.p = mEnv->pManager->CreatePlayerForSurface(kBench);
			if (b.p == nullptr)
				return;
			b.p->SetVolume(0.f);
			b.p->SetLoop(true);
			if (!b.p->Load(path))
				return EndBench("load refused");
			b.p->Play();
			b.phase = SBench::EPhase::Loading;
			LogAlways("cev_hidden_bench: start <%s> %d cycles, hidden %.1f s, visible %.1f s, cev_hiddenStopSeconds %.2f", path, b.cycles, b.hiddenSec, b.visibleSec,
				static_cast<double>(mEnv->m_cevHiddenStopSeconds));
		}

		void TickSelfTest()
		{
			TickBench();
			STest& t = s_test;
			if (t.step == STest::EStep::Idle)
				return;
			if (mEnv == nullptr || mEnv->pManager == nullptr || t.player == nullptr)
			{
				t.step = STest::EStep::Idle;
				return;
			}
			CryTexSourceInfo info = {};
			const double now = t.Now();
			switch (t.step)
			{
			case STest::EStep::WaitReadyA:
			case STest::EStep::WaitReadyB:
				if (!t.ready)
				{
					if (t.player->IsLoaded())
					{
						t.ready = true;
						t.mark = now;
					}
					else if (now - t.mark > 15.0)
						Fail(t, "the video did not become ready within 15 s");
					return;
				}
				if (now - t.mark < 1.0)
					return;
				t.ready = false;
				if (t.step == STest::EStep::WaitReadyA)
				{
					t.Check(t.Query(kSurface, info), "query while playing");
					t.Check(info.state == 3 && (info.flags & 4u) != 0 && (info.flags & 1u) != 0, "state 3 (playing), flags playing + premultiplied (state " + Num(info.state) + ", flags 0x" + Num(info.flags) + ")");
					t.Check(info.textureId > 0 && info.textureId == t.player->GetTextureId(), "textureId " + Num(info.textureId) + " = player texture");
					t.Check(info.width == t.player->GetWidth() && info.height == t.player->GetHeight() && info.width > 0, "size " + Num(info.width) + "x" + Num(info.height));
					t.Check(((info.flags & 2u) != 0) == t.player->HasAlpha(), "flags bit1 = HasAlpha");
					t.Check(info.generation == 1u && info.generation == t.player->GetGeneration(), "generation 1 after the first texture (" + Num(info.generation) + ")");
					t.Check(info.contentSerial > 1u, "contentSerial advanced (" + Num(static_cast<long long>(info.contentSerial)) + ")");
					t.Check(info.layout == CTS_LAYOUT_RGBA8 && info.textureId2 == 0 && info.planeY.w == 0 && info.errorCode == 0, "layout RGBA8, plane fields 0, errorCode 0");
					t.Check(info.timeSeconds > 0.3 && info.durationSeconds > 0.0, "time / duration");

					// a v1 sized struct: only the v1 fields are written, the bytes after them stay as they were
					unsigned char raw[sizeof(CryTexSourceInfo)];
					std::memset(raw, 0xAB, sizeof(raw));
					CryTexSourceInfo* const v1 = reinterpret_cast<CryTexSourceInfo*>(raw);
					const uint32_t v1Size = static_cast<uint32_t>(offsetof(CryTexSourceInfo, generation));
					v1->size = v1Size;
					t.Check(t.api->query(kSurface, v1) == 1, "query with the v1 size");
					bool untouched = true;
					for (size_t i = v1Size; i < sizeof(raw); ++i)
						untouched = untouched && raw[i] == 0xAB;
					t.Check(untouched && v1->size == v1Size, "v1 size: nothing beyond the v1 fields is written, size kept");
					t.Check(v1->textureId == info.textureId && v1->width == info.width && v1->contentSerial >= info.contentSerial, "v1 size: v1 fields filled");

					t.serial = info.contentSerial;
					t.mark = now;
					t.step = STest::EStep::PlayAdvance;
				}
				else
				{
					t.Check(t.Query(kSurface, info), "query after loading B");
					t.Check(info.generation == t.generation + 1u, "generation +1 after the size change (" + Num(t.generation) + " -> " + Num(info.generation) + ")");
					t.Check(info.contentSerial > t.serial, "contentSerial increased");
					t.Check(t.textureName == t.player->GetTextureName(), "engine texture name unchanged (" + std::string(t.player->GetTextureName()) + ")");
					t.Check(info.width != t.width && info.width == t.player->GetWidth() && info.textureId > 0, "new size " + Num(info.width) + "x" + Num(info.height) + " (was width " + Num(t.width) + ")");
					BuildM7Steps(t);
					t.step = STest::EStep::M7;
				}
				return;

			case STest::EStep::PlayAdvance:
				if (now - t.mark < 0.5)
					return;
				t.Query(kSurface, info);
				t.Check(info.contentSerial > t.serial, "contentSerial increases while playing (" + Num(static_cast<long long>(t.serial)) + " -> " + Num(static_cast<long long>(info.contentSerial)) + ")");
				t.player->Pause();
				t.mark = now;
				t.step = STest::EStep::PausedSettle;
				return;

			case STest::EStep::PausedSettle:
				if (now - t.mark < 0.2)
					return;
				t.Query(kSurface, info);
				t.Check(info.state == 4 && (info.flags & 4u) == 0, "state 4 (paused), playing bit clear");
				t.serial = info.contentSerial;
				t.mark = now;
				t.step = STest::EStep::PausedHold;
				return;

			case STest::EStep::PausedHold:
				if (now - t.mark < 0.5)
					return;
				t.Query(kSurface, info);
				t.Check(info.contentSerial == t.serial, "contentSerial does not move while paused");
				// M10b: one seek while paused replaces the picture once (the target frame), not twice
				t.serial = info.contentSerial;
				t.player->Seek(1.0);
				t.mark = now;
				t.step = STest::EStep::PausedSeek;
				return;

			case STest::EStep::PausedSeek:
				if (now - t.mark < 0.5)
					return;
				t.Query(kSurface, info);
				t.Check(info.contentSerial == t.serial + 1u, "a seek while paused increases contentSerial by exactly 1 (" + Num(static_cast<long long>(t.serial)) + " -> " + Num(static_cast<long long>(info.contentSerial)) + ")");
				t.Hint(kConsumerC, 1u, 100, 100, 1u);
				t.Hint(kConsumerD, 1u, 50, 50, 1u);
				t.frames = 0;
				t.step = STest::EStep::Evict;
				return;

			case STest::EStep::Evict:
			{
				++t.frames;
				t.Hint(kConsumerD, 1u, 50, 50, 1u); // D keeps sending every update, C does not
				if (t.frames == 60)
				{
					const SHintSummary s = t.Summary();
					t.Check(s.consumers == 2, "after 60 updates C and D are alive, B expired long ago (" + Num(s.consumers) + ")");
				}
				if (t.frames < 125)
					return;
				const SHintSummary s = t.Summary();
				t.Check(s.consumers == 1 && s.pixelWidth == 50, "after 120 updates without a hint C expired, D stays (" + Num(s.consumers) + ", " + Num(s.pixelWidth) + ")");
				t.Query(kSurface, info);
				t.generation = info.generation;
				t.serial = info.contentSerial;
				t.width = info.width;
				t.textureName = t.player->GetTextureName();
				t.Check(t.player->Load(t.pathB.c_str()), "Load B accepted");
				t.player->Play();
				t.mark = now;
				t.step = STest::EStep::WaitReadyB;
				return;
			}
			case STest::EStep::M7:
				if (TickM7(t, now))
				{
					CleanupM7();
					Finish(t);
				}
				return;
			default:
				return;
			}
		}

		void ShutdownSelfTest()
		{
			s_test.step = STest::EStep::Idle;
			s_test.player = nullptr;
			s_m7 = SM7(); // the manager destroys the players itself
			s_bench = SBench();
		}
	}
}
