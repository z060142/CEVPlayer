#include "StdAfx.h"
#include "PluginEnv.h"
#include "Contract/ContractTest.h"

#include <cstddef>
#include <cstring>
#include <string>

#include <CryCore/Platform/CryWindows.h>

#include "Contract/TexSource.h"
#include "Player/PlayerManager.h"

namespace CEVPlayer
{
	namespace Contract
	{
		namespace
		{
			constexpr const char* kSurface = "CevSelfTest";
			constexpr uint64_t kConsumerA = 0x1111;
			constexpr uint64_t kConsumerB = 0x2222;
			constexpr uint64_t kConsumerC = 0x3333;
			constexpr uint64_t kConsumerD = 0x4444;

			struct STest
			{
				enum class EStep { Idle, WaitReadyA, Playing, PlayAdvance, PausedSettle, PausedHold, Evict, WaitReadyB };
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
				double Now() const { return gEnv->pTimer != nullptr ? gEnv->pTimer->GetAsyncCurTime() : 0.0; }
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

			void Fail(STest& t, const char* why)
			{
				t.Check(false, why);
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
				CryTexSourceInfo info;
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
			CryTexSourceInfo info;
			t.Check(!t.Query("no_such_surface", info), "query of an unknown name returns 0");
			t.Check(t.api->open("no_such_surface", "Videos/x.webm", nullptr) == 0 && t.api->control("no_such_surface", CTS_VERB_PLAY, 0.0) == 0, "open / control return 0 (M7)");
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
			SHintSummary unknown;
			t.Hint(kConsumerA, 1u, 1, 1, 0u, sizeof(CryTexSourceHint), "no_such_surface");
			t.Check(!GetHintSummary("no_such_surface", unknown), "a hint for an unknown name is not recorded");

			t.player->SetVolume(0.f);
			t.Check(t.player->Load(t.pathA.c_str()), "Load A accepted");
			t.player->Play();
			t.mark = t.Now();
			t.step = STest::EStep::WaitReadyA;
		}

		void TickSelfTest()
		{
			STest& t = s_test;
			if (t.step == STest::EStep::Idle)
				return;
			if (mEnv == nullptr || mEnv->pManager == nullptr || t.player == nullptr)
			{
				t.step = STest::EStep::Idle;
				return;
			}
			CryTexSourceInfo info;
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
					Finish(t);
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
			default:
				return;
			}
		}

		void ShutdownSelfTest()
		{
			s_test.step = STest::EStep::Idle;
			s_test.player = nullptr;
		}
	}
}
