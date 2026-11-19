#include "StdAfx.h"
#include "PluginEnv.h"
#include "ApiTest.h"

#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

#include "Bindings/ApiPlayers.h"
#include "Bindings/ApiTable.h"
#include "Player/VideoDescriptor.h"
#include "Player/VideoPlayerImpl.h"
#include "CEVCore/Status.h"

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
				std::string lastSerial;      // payload of the last NewFrame event
				bool serialIsString = true;  // every NewFrame payload was a string of digits

				virtual void OnApiEvent(int, uint32_t event, const SValue* args, uint32_t argCount) override
				{
					if (event < eEvent_Count)
						++events[event];
					if (event == eEvent_NewFrame)
					{
						const bool ok = argCount == 1 && args[0].type == EType::String && !args[0].s.empty()
							&& args[0].s.find_first_not_of("0123456789") == std::string::npos;
						serialIsString = serialIsString && ok;
						if (argCount > 0)
							lastSerial = args[0].s;
					}
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
					const SEntry* const pEntry = FindByName(name); // a missing entry fails the check instead of crashing
					const bool ok = pEntry != nullptr && Call(*pEntry, handle, nullptr, 0, &v);
					if (pOk != nullptr)
						*pOk = ok;
					return v;
				}

				//! Indexed query (A1: AudioTrackName / AudioTrackLanguage); false when the entry does not exist.
				SValue QueryAt(const char* name, int index, bool* pOk = nullptr)
				{
					SValue v;
					const SEntry* const pEntry = FindByName(name);
					const SValue arg[1] = { SValue::FromInt(index) };
					const bool ok = pEntry != nullptr && Call(*pEntry, handle, arg, 1, &v);
					if (pOk != nullptr)
						*pOk = ok;
					return v;
				}

				double Now() const { return gEnv->pTimer != nullptr ? gEnv->pTimer->GetAsyncCurTime() : 0.0; }
			};

			STest s_test;

			//! Surface players through the table: CreateForSurface, the refusals with their codes, SetOutput(Surface),
			//! AttachSurface, Destroy.
			void SurfaceChecks(STest& t)
			{
				SValue made;
				const SValue id[1] = { SValue::FromString("apitest_surface") };
				t.Check(Call(STest::E("CreateForSurface"), 0, id, 1, &made) && made.i != 0, "CreateForSurface(apitest_surface) returns a handle");
				if (made.i == 0)
					return;
				SValue v;
				t.Check(Call(STest::E("SurfaceId"), made.i, nullptr, 0, &v) && v.s == "apitest_surface", "SurfaceId query is <" + v.s + ">");
				const SValue surfaceOut[5] = { SValue::FromInt(static_cast<int>(EOutputMode::Surface)), SValue::FromString(""), SValue::FromInt(0), SValue::FromInt(-1), SValue::FromInt(2) };
				t.Check(Call(STest::E("SetOutput"), made.i, surfaceOut, 5, nullptr), "SetOutput(Surface) on a surface player");
				SValue again;
				const SValue sameId[1] = { SValue::FromString("APITEST_SURFACE") };
				bool refused = !Call(STest::E("CreateForSurface"), 0, sameId, 1, &again) && again.i == 0;
				t.Check(refused && mEnv->pApi->LastErrorCode() == static_cast<int>(cev::Status::InvalidState),
					std::string("CreateForSurface with the same id (other case) is refused, code 2 (") + mEnv->pApi->LastErrorMessage() + ")");
				const SValue badId[1] = { SValue::FromString("bad id!") };
				refused = !Call(STest::E("CreateForSurface"), 0, badId, 1, &again) && again.i == 0;
				t.Check(refused && mEnv->pApi->LastErrorCode() == static_cast<int>(cev::Status::InvalidArgument),
					std::string("CreateForSurface with an invalid id is refused, code 1 (") + mEnv->pApi->LastErrorMessage() + ")");
				t.Check(mEnv->pApi->AttachSurface("ApiTest_Surface") == made.i, "AttachSurface finds the handle of the surface (case-insensitive)");
				t.Check(mEnv->pApi->AttachSurface("no_such_surface") == 0, "AttachSurface of an unknown id is 0");
				t.Check(Call(STest::E("Destroy"), made.i, nullptr, 0, nullptr), "Destroy the surface player");
				t.Check(mEnv->pApi->AttachSurface("apitest_surface") == 0, "the destroyed surface is gone");
			}

			void Finish(STest& t)
			{
				t.step = STest::EStep::Idle;
				t.Check(Call(STest::E("Destroy"), t.handle, nullptr, 0, nullptr), "Destroy");
				SValue ignored;
				t.Check(!Call(STest::E("State"), t.handle, nullptr, 0, &ignored), "State on the destroyed handle is rejected");

				SurfaceChecks(t);

				// script owned players are destroyed by DestroyScriptOwned (game mode ended / level unloaded), component owned stay
				SValue scriptMade;
				t.Check(Call(STest::E("Create"), 0, nullptr, 0, &scriptMade) && scriptMade.i != 0, "Create (script owned)");
				const int componentMade = mEnv->pApi->Create(true);
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
			t.lastSerial.clear();
			t.serialIsString = true;
			LogAlways("cev_apitest: start (%s)", t.path.c_str());

			// the table itself (M6b): entry counts, retired entries gone, new entries present
			t.Check(Controls().size() == 19 && Queries().size() == 25 && Events().size() == eEvent_Count,
				"table has 19 controls, 25 queries, 8 events (" + std::to_string(Controls().size()) + ", " + std::to_string(Queries().size()) + ", " + std::to_string(Events().size()) + ")");
			t.Check(FindByName("TextureName") == nullptr && FindByName("InterfaceVersion") == nullptr && FindByName("Caps") == nullptr, "TextureName, InterfaceVersion and Caps are retired");
			bool allNew = true;
			for (const char* name : { "CreateForSurface", "SetDecode", "UnbindMaterial", "SurfaceId", "Generation", "PixelLayout", "DecodeBackend", "FrameSerial", "IsLoaded", "IsLooping", "HasAudio", "Rate", "Volume", "Path" })
				allNew = allNew && FindByName(name) != nullptr;
			t.Check(allNew, "the 14 entries added in M6b exist");
			t.Check(FindByName("Create")->paramCount == 0, "Create has no TextureName parameter");
			t.Check(FindByName("SetPauseDecodeWhenHidden") != nullptr && FindByName("PauseDecodeWhenHidden") != nullptr, "SetPauseDecodeWhenHidden / PauseDecodeWhenHidden exist (M7)");
			t.Check(Events()[eEvent_NewFrame].params[0].type == EType::String, "NewFrame Serial is a string");
			// A1: audio tracks are tracks; SetLanguage / Language are retired and their GUIDs never come back
			t.Check(FindByName("SetLanguage") == nullptr && FindByName("Language") == nullptr, "SetLanguage and Language are retired (A1)");
			t.Check(FindByName("SetAudioTrack") != nullptr && FindByName("AudioTrack") != nullptr && FindByName("AudioTrackCount") != nullptr
				&& FindByName("AudioTrackName") != nullptr && FindByName("AudioTrackLanguage") != nullptr, "SetAudioTrack, AudioTrack, AudioTrackCount, AudioTrackName, AudioTrackLanguage exist (A1)");
			{
				const SEntry* const pSet = FindByName("SetAudioTrack");
				const SEntry* const pName = FindByName("AudioTrackName");
				const SEntry* const pLang = FindByName("AudioTrackLanguage");
				t.Check(pSet != nullptr && pSet->paramCount == 1 && std::strcmp(pSet->params[0].name, "Track") == 0 && pSet->params[0].type == EType::String, "SetAudioTrack takes one string parameter Track");
				t.Check(pName != nullptr && pName->paramCount == 1 && std::strcmp(pName->params[0].name, "Index") == 0 && pName->params[0].type == EType::Int && pName->returnType == EType::String
					&& pLang != nullptr && pLang->paramCount == 1 && pLang->params[0].type == EType::Int && pLang->returnType == EType::String, "AudioTrackName / AudioTrackLanguage take an int Index and return a string");
				const CryGUID retiredA1[2] = { "EB59C123-4D68-43F3-A979-65C544B6BA1C"_cry_guid, "B7A77A6C-E01F-4051-9B05-CA9596E6E49B"_cry_guid };
				bool reused = false;
				for (const SSpan& span : { Controls(), Queries(), Events() })
					for (const SEntry& e : span)
						reused = reused || e.guid == retiredA1[0] || e.guid == retiredA1[1];
				for (uint32_t i = 0; i < eCryVS_FunctionCount; ++i)
					reused = reused || kCryVSFunctionGuids[i] == retiredA1[0] || kCryVSFunctionGuids[i] == retiredA1[1];
				t.Check(!reused, "the retired GUIDs of SetLanguage / Language are not used by any entry (A1)");
			}

			// creation through the table
			SValue created;
			t.Check(Call(STest::E("Create"), 0, nullptr, 0, &created) && created.i != 0, "Create returns a handle");
			t.handle = created.i;
			if (t.handle == 0)
			{
				LogAlways("cev_apitest: FAIL (%d checks, %d failed)", t.checks, t.failed);
				return;
			}
			mEnv->pApi->Subscribe(t.handle, &t);

			// static queries and bad handles (warnings "invalid player handle" are expected here)
			SValue ignored;
			t.Check(!Call(STest::E("Play"), 987654, nullptr, 0, &ignored), "Play on an invalid handle is rejected without a crash");
			t.Check(!Call(STest::E("State"), 0, nullptr, 0, &ignored), "State on handle 0 is rejected");

			// control sequence
			const SValue output[5] = { SValue::FromInt(static_cast<int>(EOutputMode::Screen)), SValue::FromString(""), SValue::FromInt(0), SValue::FromInt(-1), SValue::FromInt(2) };
			const SValue surfaceOut[5] = { SValue::FromInt(static_cast<int>(EOutputMode::Surface)), SValue::FromString(""), SValue::FromInt(0), SValue::FromInt(-1), SValue::FromInt(2) };
			t.Check(!Call(STest::E("SetOutput"), t.handle, surfaceOut, 5, nullptr), "SetOutput(Surface) on a player without surface id is rejected");
			t.Check(Call(STest::E("SetOutput"), t.handle, output, 5, nullptr), "SetOutput(Screen)");
			const SValue gpu[1] = { SValue::FromInt(static_cast<int>(EDecodeMode::Gpu)) };
			t.Check(Call(STest::E("SetDecode"), t.handle, gpu, 1, nullptr) && mEnv->pApi->GetDecodeMode(t.handle) == 1, "SetDecode(GPU) recorded");
			const SValue badDecode[1] = { SValue::FromInt(7) };
			t.Check(!Call(STest::E("SetDecode"), t.handle, badDecode, 1, nullptr) && mEnv->pApi->GetDecodeMode(t.handle) == 1, "SetDecode(7) rejected");
			const SValue cpu[1] = { SValue::FromInt(static_cast<int>(EDecodeMode::Cpu)) };
			t.Check(Call(STest::E("SetDecode"), t.handle, cpu, 1, nullptr) && mEnv->pApi->GetDecodeMode(t.handle) == 2, "SetDecode(CPU) recorded");
			t.Check(Call(STest::E("UnbindMaterial"), t.handle, nullptr, 0, nullptr), "UnbindMaterial accepted");
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
			t.Check(t.Query("TextureId").i != 0, "TextureId " + std::to_string(t.Query("TextureId").i));
			t.Check(t.Query("IsPremultiplied").b, "IsPremultiplied");
			// queries added in M6b
			t.Check(t.Query("SurfaceId").s.empty(), "SurfaceId of a Create player is empty");
			t.Check(t.Query("Generation").i == 0, "Generation of a player without surface id stays 0 (only surface players count, " + std::to_string(t.Query("Generation").i) + ")");
			t.Check(t.Query("PixelLayout").i == 0 && t.Query("DecodeBackend").i == 0, "PixelLayout RGBA8, DecodeBackend CPU (SetDecode(CPU) before Load)");
			const std::string serial = t.Query("FrameSerial").s;
			t.Check(!serial.empty() && serial.find_first_not_of("0123456789") == std::string::npos && std::strtoull(serial.c_str(), nullptr, 10) > 10, "FrameSerial is decimal text > 10 (" + serial + ")");
			t.Check(t.Query("IsLoaded").b && t.Query("IsLooping").b, "IsLoaded and IsLooping");
			t.Check(t.Query("Rate").f == 1.f && t.Query("Volume").f == 0.f, "Rate 1 and Volume 0 as set");
			t.Check(t.Query("Path").s == t.path, "Path is <" + t.Query("Path").s + ">");
			// M7: PauseDecodeWhenHidden, on by default; a player without a surface id is visible whatever it says
			t.Check(t.Query("PauseDecodeWhenHidden").b, "PauseDecodeWhenHidden is on by default");
			{
				const SValue off[1] = { SValue::FromBool(false) };
				const SValue on[1] = { SValue::FromBool(true) };
				t.Check(Call(STest::E("SetPauseDecodeWhenHidden"), t.handle, off, 1, nullptr) && !t.Query("PauseDecodeWhenHidden").b, "SetPauseDecodeWhenHidden(false)");
				t.Check(Call(STest::E("SetPauseDecodeWhenHidden"), t.handle, on, 1, nullptr) && t.Query("PauseDecodeWhenHidden").b, "SetPauseDecodeWhenHidden(true)");
			}
			t.Check(t.serialIsString && !t.lastSerial.empty(), "NewFrame Serial payloads are decimal strings (last " + t.lastSerial + ")");
			LogAlways("cev_apitest: HasAudio=%d", t.Query("HasAudio").b ? 1 : 0);
			// A1: the audio tracks of the loaded media (a plain .webm has none; a .cevvideo lists its <AudioTracks>)
			{
				bool okCount = false;
				const int count = t.Query("AudioTrackCount", &okCount).i;
				const bool descriptor = IsDescriptorPath(t.path.c_str());
				t.Check(okCount && (descriptor ? count > 0 : count == 0), "AudioTrackCount " + std::to_string(count) + (descriptor ? " > 0 for a descriptor" : " == 0 for a plain video"));
				bool listed = count == 0 && t.Query("AudioTrack").s.empty();
				for (int i = 0; i < count; ++i)
				{
					bool okName = false;
					bool okLang = false;
					const std::string name = t.QueryAt("AudioTrackName", i, &okName).s;
					const std::string language = t.QueryAt("AudioTrackLanguage", i, &okLang).s;
					LogAlways("cev_apitest: audio track %d name='%s' language='%s'", i, name.c_str(), language.c_str());
					t.Check(okName && okLang && !name.empty(), "AudioTrackName(" + std::to_string(i) + ") is not empty");
					listed = listed || stricmp(name.c_str(), t.Query("AudioTrack").s.c_str()) == 0;
				}
				t.Check(listed, "AudioTrack <" + t.Query("AudioTrack").s + "> is one of the listed tracks (or empty without tracks)");
				bool okOut = false;
				t.Check(t.QueryAt("AudioTrackName", count, &okOut).s.empty() && okOut && t.QueryAt("AudioTrackLanguage", -1).s.empty(), "AudioTrackName / AudioTrackLanguage out of range return an empty string");
			}
			LogAlways("cev_apitest: HasAlpha=%d AudioTrack='%s' Variant=%d events: ready=%u started=%u newFrame=%u looped=%u ended=%u error=%u",
				t.Query("HasAlpha").b ? 1 : 0, t.Query("AudioTrack").s.c_str(), t.Query("Variant").i,
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
