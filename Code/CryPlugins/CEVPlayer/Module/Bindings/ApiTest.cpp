#include "StdAfx.h"
#include "PluginEnv.h"
#include "ApiTest.h"

#include <string>

#include "Bindings/ApiPlayers.h"
#include "Bindings/ApiTable.h"
#include "Player/VideoPlayerImpl.h"

namespace CEVPlayer
{
	namespace Api
	{
		namespace
		{
			struct STest final : public IEventSink
			{
				enum class EStep { Idle, Waiting, SkipPlay, SkipVerify };
				EStep step = EStep::Idle;
				int handle = 0;
				double startTime = 0.0;
				int checks = 0;
				int failed = 0;
				uint32_t events[eEvent_Count] = {};
				std::string path;

				virtual void OnApiEvent(int, uint32_t event, const SValue*, uint32_t) override
				{
					if (event < eEvent_Count)
						++events[event];
				}

				void Check(bool ok, const std::string& what)
				{
					++checks;
					if (!ok)
						++failed;
					LogAlways("cev_apitest: %s %s", ok ? "ok  " : "FAIL", what.c_str());
				}

				static const SEntry& E(const char* name) { return *FindByName(name); }

				SValue Query(const char* name, bool* pOk = nullptr)
				{
					SValue v;
					const bool ok = Call(E(name), handle, nullptr, 0, &v);
					if (pOk != nullptr)
						*pOk = ok;
					return v;
				}

				double Now() const { return gEnv->pTimer != nullptr ? gEnv->pTimer->GetAsyncCurTime() : 0.0; }
			};

			STest s_test;

			void Finish(STest& t)
			{
				t.step = STest::EStep::Idle;
				t.Check(Call(STest::E("Destroy"), t.handle, nullptr, 0, nullptr), "Destroy");
				SValue ignored;
				t.Check(!Call(STest::E("State"), t.handle, nullptr, 0, &ignored), "State on the destroyed handle is rejected");

				// script owned players are destroyed by DestroyScriptOwned (game mode ended / level unloaded), component owned stay
				SValue scriptMade;
				const SValue noName[1] = { SValue::FromString("") };
				t.Check(Call(STest::E("Create"), 0, noName, 1, &scriptMade) && scriptMade.i != 0, "Create (script owned)");
				const int componentMade = mEnv->pApi->Create(nullptr, true);
				t.Check(componentMade != 0, "Create (component owned)");
				const uint32_t destroyed = mEnv->pApi->DestroyScriptOwned();
				t.Check(destroyed >= 1 && mEnv->pApi->Find(scriptMade.i, false) == nullptr, "DestroyScriptOwned destroyed the script owned player (" + std::to_string(destroyed) + ")");
				t.Check(mEnv->pApi->Find(componentMade, false) != nullptr, "DestroyScriptOwned left the component owned player");
				if (componentMade != 0)
					mEnv->pApi->Destroy(componentMade);

				LogAlways("cev_apitest: %s (%d checks, %d failed)", t.failed == 0 ? "PASS" : "FAIL", t.checks, t.failed);
			}

			//! Stop hides a fullscreen player until Play (CVideoPlayerImpl::FullscreenHidden).
			bool FullscreenHidden(STest& t)
			{
				IVideoPlayer* const pPlayer = mEnv->pApi->Find(t.handle, false);
				return pPlayer != nullptr && static_cast<CVideoPlayerImpl*>(pPlayer)->FullscreenHidden();
			}
		}

		void StartApiTest(const char* path)
		{
			STest& t = s_test;
			if (t.step != STest::EStep::Idle)
			{
				LogWarning("cev_apitest is already running");
				return;
			}
			if (mEnv == nullptr || mEnv->pApi == nullptr)
				return;
			t.handle = 0;
			t.checks = 0;
			t.failed = 0;
			for (uint32_t& n : t.events)
				n = 0;
			t.path = path != nullptr && path[0] != '\0' ? path : "Videos/vp9_alpha.webm";
			LogAlways("cev_apitest: start (%s)", t.path.c_str());

			// creation through the table
			SValue created;
			const SValue textureName[1] = { SValue::FromString("") };
			t.Check(Call(STest::E("Create"), 0, textureName, 1, &created) && created.i != 0, "Create returns a handle");
			t.handle = created.i;
			if (t.handle == 0)
			{
				LogAlways("cev_apitest: FAIL (%d checks, %d failed)", t.checks, t.failed);
				return;
			}
			mEnv->pApi->Subscribe(t.handle, &t);

			// static queries and bad handles (warnings "invalid player handle" are expected here)
			t.Check(t.Query("InterfaceVersion").i == static_cast<int>(kVideoPlayerInterfaceVersion), "InterfaceVersion matches the header");
			t.Check((t.Query("Caps").i & eVideoCap_Variants) != 0, "Caps has the Variants bit");
			SValue ignored;
			t.Check(!Call(STest::E("Play"), 987654, nullptr, 0, &ignored), "Play on an invalid handle is rejected without a crash");
			t.Check(!Call(STest::E("State"), 0, nullptr, 0, &ignored), "State on handle 0 is rejected");

			// control sequence
			const SValue output[5] = { SValue::FromInt(static_cast<int>(EOutputMode::Screen)), SValue::FromString(""), SValue::FromInt(0), SValue::FromInt(-1), SValue::FromInt(2) };
			t.Check(Call(STest::E("SetOutput"), t.handle, output, 5, nullptr), "SetOutput(Screen)");
			const SValue loop[1] = { SValue::FromBool(true) };
			t.Check(Call(STest::E("SetLoop"), t.handle, loop, 1, nullptr), "SetLoop(true)");
			const SValue volume[1] = { SValue::FromFloat(0.f) };
			t.Check(Call(STest::E("SetVolume"), t.handle, volume, 1, nullptr), "SetVolume(0)");
			const SValue load[1] = { SValue::FromString(t.path.c_str()) };
			t.Check(Call(STest::E("Load"), t.handle, load, 1, nullptr), "Load accepted");
			t.Check(Call(STest::E("Play"), t.handle, nullptr, 0, nullptr), "Play accepted (queued while loading)");
			t.startTime = t.Now();
			t.step = STest::EStep::Waiting;
		}

		void TickApiTest()
		{
			STest& t = s_test;
			if (t.step == STest::EStep::SkipPlay && t.Now() - t.startTime >= 0.7)
			{
				t.Check(t.Query("State").i == static_cast<int>(EVideoPlayerState::Playing), "Playing before the key press");
				if (gEnv->pInput != nullptr)
				{
					SInputEvent key;
					key.deviceType = eIDT_Keyboard;
					key.state = eIS_Pressed;
					key.keyId = eKI_Space;
					key.keyName = "space";
					key.value = 1.f;
					gEnv->pInput->PostInputEvent(key, true);
				}
				t.startTime = t.Now();
				t.step = STest::EStep::SkipVerify;
				return;
			}
			if (t.step == STest::EStep::SkipVerify && t.Now() - t.startTime >= 0.7)
			{
				t.Check(t.Query("State").i == static_cast<int>(EVideoPlayerState::Stopped), "key press stopped the skippable video");
				t.Check(t.events[eEvent_Skipped] == 1, "Skipped event arrived (" + std::to_string(t.events[eEvent_Skipped]) + ")");
				t.Check(FullscreenHidden(t), "the skipped video is no longer drawn on the screen");
				Finish(t);
				return;
			}
			if (t.step != STest::EStep::Waiting || t.Now() - t.startTime < 2.0)
				return;

			bool ok = false;
			const int state = t.Query("State", &ok).i;
			t.Check(ok && state == static_cast<int>(EVideoPlayerState::Playing), std::string("State after 2 s is Playing (is ") + StateName(state) + ")");
			const float time = t.Query("Time").f;
			t.Check(time > 0.5f, "Time after 2 s advanced (" + std::to_string(time) + " s)");
			const int width = t.Query("Width").i;
			const int height = t.Query("Height").i;
			t.Check(width > 0 && height > 0, "Width x Height known (" + std::to_string(width) + "x" + std::to_string(height) + ")");
			t.Check(t.Query("Duration").f > 0.f, "Duration known");
			t.Check(t.Query("TextureId").i != 0, "TextureId " + std::to_string(t.Query("TextureId").i) + ", name " + t.Query("TextureName").s);
			t.Check(t.Query("IsPremultiplied").b, "IsPremultiplied");
			LogAlways("cev_apitest: HasAlpha=%d Language='%s' Variant=%d events: ready=%u started=%u newFrame=%u looped=%u ended=%u error=%u",
				t.Query("HasAlpha").b ? 1 : 0, t.Query("Language").s.c_str(), t.Query("Variant").i,
				t.events[eEvent_Ready], t.events[eEvent_Started], t.events[eEvent_NewFrame], t.events[eEvent_Looped], t.events[eEvent_Ended], t.events[eEvent_Error]);
			t.Check(t.events[eEvent_Ready] == 1 && t.events[eEvent_Started] >= 1 && t.events[eEvent_NewFrame] > 10, "Ready, Started and NewFrame events arrived");

			t.Check(Call(STest::E("Stop"), t.handle, nullptr, 0, nullptr), "Stop");
			t.Check(t.Query("State").i == static_cast<int>(EVideoPlayerState::Stopped), "State after Stop is Stopped");
			t.Check(FullscreenHidden(t), "Stop hides the fullscreen output");

			// skippable: play again, post a key press, the player must stop and send Skipped
			const SValue skippable[1] = { SValue::FromBool(true) };
			t.Check(Call(STest::E("SetSkippable"), t.handle, skippable, 1, nullptr), "SetSkippable(true)");
			t.Check(Call(STest::E("Play"), t.handle, nullptr, 0, nullptr), "Play again");
			t.Check(!FullscreenHidden(t), "Play shows the fullscreen output again");
			t.startTime = t.Now();
			t.step = STest::EStep::SkipPlay;
		}


		void ShutdownApiTest()
		{
			s_test.step = STest::EStep::Idle;
			s_test.handle = 0;
		}
	}
}
