#include "StdAfx.h"
#include "PluginEnv.h"
#include "ApiTable.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "CEVCore/PlayerCore.h"
#include <Cry3DEngine/IMaterial.h>
#include <CryRenderer/IRenderer.h>

#include "ApiPlayers.h"
#include "Player/VideoPlayerImpl.h"

namespace CEVPlayer
{
	namespace Api
	{
		// ============================================================================================ GUIDs
		// FIXED. They are stored in saved flow graphs / Schematyc scripts / CryVS scripts: never change one after the
		// first release. A new entry gets a new GUID, an entry that goes away keeps its GUID reserved.
		const CryGUID kSchematycComponentGuid = "FB8428A3-8C24-41C7-B75D-687E373CCD72"_cry_guid;
		const CryGUID kCryVSComponentGuid     = "D90B2053-502C-4E1B-B635-61FDDE706AD3"_cry_guid;
		const CryGUID kOutputModeTypeGuid     = "B1A59049-6497-4430-8AE7-52035260856D"_cry_guid;
		const CryGUID kVideoFileNameTypeGuid  = "DA1FFCAF-AF64-482B-BF62-B2C6002197F2"_cry_guid;
		const CryGUID kFitTypeGuid            = "4DF842EF-5FF7-4794-B8E8-180FB499CEEA"_cry_guid;
		const CryGUID kDecodeModeTypeGuid     = "14025090-A62F-4DC1-9A6E-47A9F50D5278"_cry_guid;
		// CryVS global functions, in ECryVSFunction order (M6c).
		const CryGUID kCryVSFunctionGuids[eCryVS_FunctionCount] =
		{
			"27AF1B77-C05D-456D-B465-016FB958F9D9"_cry_guid, // CEVPlayer::PlayOnScreen
			"BD64D321-5F54-4C88-BE59-9F89D90883E7"_cry_guid, // CEVPlayer::PlayOnMaterial
			"35BF4E46-7F0D-49AC-A196-B57EAB16D689"_cry_guid, // CEVPlayer::Stop
			"4276BD72-C99B-40B4-95C1-213C80FFEB98"_cry_guid, // CEVPlayer::UI::PlayToSurface
			"4212CE2B-B8C1-44E0-A010-E1B1391AF471"_cry_guid, // CEVPlayer::UI::StopSurface
			"70FA6405-329B-4335-AB42-5334E1F8DA79"_cry_guid, // CEVPlayer::UI::SurfaceInfo
			"30F7ED4B-1FC5-43E9-952A-80A3847F199F"_cry_guid, // CEVPlayer::Advanced::Open
			"4CA6690D-4E60-4EBC-8782-1B0C9F2414A0"_cry_guid, // CEVPlayer::Advanced::Close
			"414E5002-C895-45FD-A347-81D12E7C8465"_cry_guid, // CEVPlayer::Advanced::Play
			"AF12D09D-DD5A-440F-B6F8-D21D5139C43D"_cry_guid, // CEVPlayer::Advanced::Pause
			"46CA5F4B-83E0-4C20-892D-F47737861B40"_cry_guid, // CEVPlayer::Advanced::Stop
			"404EFDA7-98CF-461A-BC27-62B92AEF6787"_cry_guid, // CEVPlayer::Advanced::Seek
			"4ACC3D98-043C-4CA7-A43C-9DB7D96F69BA"_cry_guid, // CEVPlayer::Advanced::SetLoop
			"285B5336-FBD5-4D52-A878-40FD620E7932"_cry_guid, // CEVPlayer::Advanced::SetRate
			"C24FFE00-8215-4B4C-80D5-3996D2650B39"_cry_guid, // CEVPlayer::Advanced::SetVolume
			"888EAF9B-3128-4510-9C50-9B26353DA322"_cry_guid, // CEVPlayer::Advanced::SetLanguage
			"D71E69BE-A424-451D-8840-A7BCFE53ADDD"_cry_guid, // CEVPlayer::Advanced::SetVariant
			"20A1CC28-BE7C-4382-8F5C-FAD6955C25D2"_cry_guid, // CEVPlayer::Advanced::SetSkippable
			"13B9D4C4-43B4-4602-9E42-E8A6F07B4974"_cry_guid, // CEVPlayer::Advanced::Info
			"6A464499-0622-463D-8F43-57E805205A1C"_cry_guid, // CEVPlayer::Advanced::SetPauseDecodeWhenHidden (M7)
		};
		const CryGUID kCryVSResizedSignalGuid = "8431E4A5-BEA5-4461-B110-4B7883761FBC"_cry_guid;
		const CryGUID kCryVSClosedSignalGuid  = "AAE0463C-E424-4A35-8737-00D3F22801F1"_cry_guid;
		// Retired (see ApiTable.h): 6215D8DE-93E8-4236-9DB5-9DDCFD4F1C0D (TextureName), 2819FAC7-8B52-4BA3-9FA7-25BE56E317AD
		// (InterfaceVersion), 23441E42-DE20-4EA3-A312-F6D624D5D83C (Caps).

		std::string SerialText(uint64_t serial)
		{
			char buffer[32];
			std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(serial));
			return buffer;
		}

		// ============================================================================================ SValue
		SValue SValue::As(EType to) const
		{
			if (to == type || to == EType::Void)
				return *this;
			switch (to)
			{
			case EType::Bool:
				return type == EType::String ? FromBool(s == "1" || stricmp(s.c_str(), "true") == 0) : FromBool(type == EType::Float ? f != 0.f : i != 0 || b);
			case EType::Int:
				return type == EType::String ? FromInt(std::atoi(s.c_str())) : (type == EType::Float ? FromInt(static_cast<int>(f)) : FromInt(type == EType::Bool ? (b ? 1 : 0) : i));
			case EType::Float:
				return type == EType::String ? FromFloat(static_cast<float>(std::atof(s.c_str()))) : (type == EType::Float ? *this : FromFloat(type == EType::Bool ? (b ? 1.f : 0.f) : static_cast<float>(i)));
			case EType::String:
			{
				char buffer[64];
				switch (type)
				{
				case EType::Bool: return FromString(b ? "true" : "false");
				case EType::Int: std::snprintf(buffer, sizeof(buffer), "%d", i); return FromString(buffer);
				case EType::Float: std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(f)); return FromString(buffer);
				default: return FromString("");
				}
			}
			default:
				return *this;
			}
		}

		std::string SValue::ToText() const
		{
			switch (type)
			{
			case EType::Bool: return b ? "true" : "false";
			case EType::Int: return std::to_string(i);
			case EType::Float: { char buffer[64]; std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(f)); return buffer; }
			case EType::String: return "\"" + s + "\"";
			default: return "-";
			}
		}

		// ============================================================================================ functions
		// Plain functions on top of IVideoPlayer / CApiPlayers. No reflection, no lookup besides the handle.
		namespace
		{
			bool FnCreate(const SCall& c)
			{
				const int handle = mEnv->pApi->Create();
				*c.pResult = SValue::FromInt(handle);
				return handle != 0;
			}
			bool FnCreateForSurface(const SCall& c)
			{
				const int handle = mEnv->pApi->CreateForSurface(c.args[0].s.c_str());
				*c.pResult = SValue::FromInt(handle);
				return handle != 0;
			}
			bool FnDestroy(const SCall& c) { return mEnv->pApi->Destroy(c.handle); }
			bool FnLoad(const SCall& c) { return c.pPlayer->Load(c.args[0].s.c_str()); }
			bool FnPlay(const SCall& c) { c.pPlayer->Play(); return true; }
			bool FnPause(const SCall& c) { c.pPlayer->Pause(); return true; }
			bool FnResume(const SCall& c) { c.pPlayer->Resume(); return true; }
			bool FnStop(const SCall& c) { c.pPlayer->Stop(); return true; }
			bool FnSeek(const SCall& c) { c.pPlayer->Seek(static_cast<double>(c.args[0].f)); return true; }
			bool FnSetLoop(const SCall& c) { c.pPlayer->SetLoop(c.args[0].b); return true; }
			bool FnSetRate(const SCall& c)
			{
				const float rate = c.args[0].f;
				if (!(rate > 0.f)) // also NaN
				{
					LogWarning("api: SetRate(%g) must be > 0, ignored", static_cast<double>(rate));
					return false;
				}
				const float clamped = static_cast<float>(std::min(cev::PlayerCore::kMaxRate, std::max(cev::PlayerCore::kMinRate, static_cast<double>(rate))));
				if (clamped != rate)
					LogWarning("api: SetRate(%g) is outside [%g, %g], clamped to %g", static_cast<double>(rate), cev::PlayerCore::kMinRate, cev::PlayerCore::kMaxRate, static_cast<double>(clamped));
				c.pPlayer->SetRate(clamped);
				return true;
			}
			bool FnSetLanguage(const SCall& c) { c.pPlayer->SetLanguage(c.args[0].s.c_str()); return true; }
			bool FnSetVolume(const SCall& c) { c.pPlayer->SetVolume(c.args[0].f); return true; }
			bool FnSetVariant(const SCall& c) { c.pPlayer->SetVariant(c.args[0].i); return true; }
			bool FnSetSkippable(const SCall& c) { mEnv->pApi->SetSkippable(c.handle, c.args[0].b); return true; }
			bool FnUnbindMaterial(const SCall& c) { c.pPlayer->UnbindMaterial(nullptr); return true; }
			// every IVideoPlayer of this plug-in is a CVideoPlayerImpl (made by CPlayerManager)
			bool FnSetPauseDecodeWhenHidden(const SCall& c) { static_cast<CVideoPlayerImpl*>(c.pPlayer)->SetPauseDecodeWhenHidden(c.args[0].b); return true; }
			bool FnSetDecode(const SCall& c)
			{
				const int mode = c.args[0].i;
				if (mode < static_cast<int>(EDecodeMode::Auto) || mode > static_cast<int>(EDecodeMode::Cpu))
				{
					LogWarning("api: SetDecode(%d): unknown mode (0 = Auto, 1 = GPU, 2 = CPU), ignored", mode);
					return false;
				}
				mEnv->pApi->SetDecodeMode(c.handle, mode);
				// every IVideoPlayer of this plug-in is a CVideoPlayerImpl; the choice applies at the next Load / Play from stopped
				static_cast<CVideoPlayerImpl*>(c.pPlayer)->SetDecodeRequest(mode);
				return true;
			}

			bool FnSetOutput(const SCall& c)
			{
				IVideoPlayer& player = *c.pPlayer;
				const int mode = c.args[0].i;
				const std::string& name = c.args[1].s;
				const int slot = c.args[2].i;
				const int subMaterial = c.args[3].i;
				int fit = c.args[4].i;
				if (fit < 0 || fit > static_cast<int>(EVideoFit::Letterbox))
					fit = static_cast<int>(EVideoFit::Letterbox);

				// the modes are exclusive: leave the previous one first
				player.SetFullscreen(false);
				player.UnbindMaterial(nullptr);

				switch (static_cast<EOutputMode>(mode))
				{
				case EOutputMode::Surface:
					if (player.GetSurfaceId()[0] == '\0')
					{
						LogWarning("api: SetOutput(Surface) only works for a player made by CreateForSurface (handle %d has no surface id)", c.handle);
						return false;
					}
					return true;
				case EOutputMode::Screen:
					player.SetFullscreen(true, static_cast<EVideoFit>(fit));
					return true;
				case EOutputMode::Material:
				{
					IMaterial* pMaterial = c.pDefaultMaterial;
					if (name.empty() && pMaterial == nullptr)
					{
						LogWarning("api: SetOutput(Material) needs a material name");
						return false;
					}
					if (!name.empty())
					{
						pMaterial = gEnv != nullptr && gEnv->p3DEngine != nullptr ? gEnv->p3DEngine->GetMaterialManager()->LoadMaterial(name.c_str(), false) : nullptr;
						if (pMaterial == nullptr)
						{
							LogWarning("api: SetOutput(Material): material <%s> not found", name.c_str());
							return false;
						}
					}
					if (slot < 0 || slot >= EFTT_MAX)
					{
						LogWarning("api: SetOutput(Material): texture slot %d is outside 0..%d", slot, static_cast<int>(EFTT_MAX) - 1);
						return false;
					}
					return player.BindMaterial(pMaterial, subMaterial, slot);
				}
				default:
					LogWarning("api: SetOutput: unknown mode %d (0 = Surface, 1 = Screen, 2 = Material)", mode);
					return false;
				}
			}

			bool FnState(const SCall& c) { *c.pResult = SValue::FromInt(static_cast<int>(c.pPlayer->GetState())); return true; }
			bool FnTime(const SCall& c) { *c.pResult = SValue::FromFloat(static_cast<float>(c.pPlayer->GetTime())); return true; }
			bool FnDuration(const SCall& c) { *c.pResult = SValue::FromFloat(static_cast<float>(c.pPlayer->GetDuration())); return true; }
			bool FnWidth(const SCall& c) { *c.pResult = SValue::FromInt(c.pPlayer->GetWidth()); return true; }
			bool FnHeight(const SCall& c) { *c.pResult = SValue::FromInt(c.pPlayer->GetHeight()); return true; }
			bool FnTextureId(const SCall& c) { *c.pResult = SValue::FromInt(c.pPlayer->GetTextureId()); return true; }
			bool FnHasAlpha(const SCall& c) { *c.pResult = SValue::FromBool(c.pPlayer->HasAlpha()); return true; }
			bool FnIsPremultiplied(const SCall& c) { *c.pResult = SValue::FromBool(c.pPlayer->IsPremultiplied()); return true; }
			bool FnLanguage(const SCall& c) { *c.pResult = SValue::FromString(c.pPlayer->GetLanguage()); return true; }
			bool FnVariant(const SCall& c) { *c.pResult = SValue::FromInt(c.pPlayer->GetVariant()); return true; }
			bool FnSurfaceId(const SCall& c) { *c.pResult = SValue::FromString(c.pPlayer->GetSurfaceId()); return true; }
			bool FnGeneration(const SCall& c) { *c.pResult = SValue::FromInt(static_cast<int>(c.pPlayer->GetGeneration())); return true; }
			bool FnPixelLayout(const SCall& c) { *c.pResult = SValue::FromInt(static_cast<int>(c.pPlayer->GetPixelLayout())); return true; }
			bool FnDecodeBackend(const SCall& c) { *c.pResult = SValue::FromInt(static_cast<int>(c.pPlayer->GetDecodeBackend())); return true; }
			bool FnFrameSerial(const SCall& c) { *c.pResult = SValue::FromString(SerialText(c.pPlayer->GetFrameSerial()).c_str()); return true; }
			bool FnIsLoaded(const SCall& c) { *c.pResult = SValue::FromBool(c.pPlayer->IsLoaded()); return true; }
			bool FnIsLooping(const SCall& c) { *c.pResult = SValue::FromBool(c.pPlayer->IsLooping()); return true; }
			bool FnHasAudio(const SCall& c) { *c.pResult = SValue::FromBool(c.pPlayer->HasAudio()); return true; }
			// every IVideoPlayer of this plug-in is a CVideoPlayerImpl (made by CPlayerManager)
			bool FnRate(const SCall& c) { *c.pResult = SValue::FromFloat(static_cast<const CVideoPlayerImpl*>(c.pPlayer)->GetRate()); return true; }
			bool FnVolume(const SCall& c) { *c.pResult = SValue::FromFloat(static_cast<const CVideoPlayerImpl*>(c.pPlayer)->GetVolume()); return true; }
			bool FnPath(const SCall& c) { *c.pResult = SValue::FromString(c.pPlayer->GetPath()); return true; }
			bool FnPauseDecodeWhenHidden(const SCall& c) { *c.pResult = SValue::FromBool(static_cast<const CVideoPlayerImpl*>(c.pPlayer)->GetPauseDecodeWhenHidden()); return true; }

			// ======================================================================================== parameters
			const SParam kCreateForSurfaceParams[] = { { EType::String, "SurfaceID", "Surface id: 1 to 63 of A-Z a-z 0-9 _ . : -, compared case-insensitively; UIs find the video by it", 0, "", 'in00' } };
			const SParam kLoadParams[]        = { { EType::String, "Path", "CryPak path of a .webm video or a .cevvideo descriptor, e.g. Videos/intro.webm", 0, "", 'in00' } };
			const SParam kSeekParams[]        = { { EType::Float, "Seconds", "Target time in seconds", 0, nullptr, 'in00' } };
			const SParam kSetLoopParams[]     = { { EType::Bool, "Loop", "Restart at 0 when the end is reached", 0, nullptr, 'in00' } };
			const SParam kSetRateParams[]     = { { EType::Float, "Rate", "Playback speed, 0.25 .. 4 (1 = normal; values outside are clamped)", 1.0, nullptr, 'in00' } };
			const SParam kSetLanguageParams[] = { { EType::String, "Language", "Audio language as named in the .cevvideo descriptor; empty = default choice", 0, "", 'in00' } };
			const SParam kSetVolumeParams[]   = { { EType::Float, "Volume", "Player volume 0..1 (multiplied with the global volume cev_volume)", 1.0, nullptr, 'in00' } };
			const SParam kSetOutputParams[]   =
			{
				{ EType::Int,    "Mode", "0 = Surface only (players made by CreateForSurface), 1 = Screen (fullscreen on the top layer), 2 = Material (bind to a texture slot of a material)", 0, nullptr, 'in00' },
				{ EType::String, "Name", "Mode Material: material name (empty = the entity's own material where there is one); not used by the other modes", 0, "", 'in01' },
				{ EType::Int,    "Slot", "Mode Material: texture slot (EEfResTextures value, 0 = diffuse)", 0, nullptr, 'in02' },
				{ EType::Int,    "SubMaterial", "Mode Material: sub-material index, -1 = the material itself", -1.0, nullptr, 'in03' },
				{ EType::Int,    "Fit", "Mode Screen: 0 = Fill (stretch), 1 = Fit (keep aspect, scene visible in the bars), 2 = Letterbox (keep aspect, black bars)", 2.0, nullptr, 'in04' },
			};
			const SParam kSetVariantParams[]  = { { EType::Int, "Variant", "-2 = follow the cvar cev_variant, -1 = automatic, 0 = main video, n = n-th variant of the .cevvideo descriptor", -2.0, nullptr, 'in00' } };
			const SParam kSetSkippableParams[] = { { EType::Bool, "Skippable", "A key / mouse / gamepad button press stops the video while it plays and sends the Skipped event", 0, nullptr, 'in00' } };
			const SParam kSetDecodeParams[]   = { { EType::Int, "Mode", "0 = Auto (GPU for videos at least cev_decodeAutoMinHeight tall), 1 = GPU (Media Foundation, CPU fallback), 2 = CPU", 0, nullptr, 'in00' } };
			const SParam kSetPauseDecodeWhenHiddenParams[] = { { EType::Bool, "Pause", "true (default) = a surface no UI shows only decodes, and stops decoding after cev_hiddenStopSeconds; false = it keeps decoding and uploading", 1.0, nullptr, 'in00' } };

			const SParam kNewFrameParams[]    = { { EType::String, "Serial", "Number of the uploaded picture as decimal text (counts up, never wraps)", 0, "", 'seri' } };
			const SParam kMarkerParams[]      =
			{
				{ EType::String, "Name", "Marker name from the .cevvideo descriptor", 0, "", 'name' },
				{ EType::Float,  "Time", "Marker time in the media (seconds)", 0, nullptr, 'time' },
			};
			const SParam kErrorParams[]       =
			{
				{ EType::Int,    "Code", "Status code of the failure", 0, nullptr, 'code' },
				{ EType::String, "Message", "Short text, may be empty", 0, "", 'mesg' },
			};

#define CEV_ARRAY(a) a, static_cast<uint32_t>(sizeof(a) / sizeof(a[0]))
#define CEV_NOPARAMS nullptr, 0
			// ======================================================================================== the table
			const SEntry kControls[] =
			{
				{ EKind::Control, "B2A87BA0-054C-4A88-9153-1433F8B9D5A5"_cry_guid, "Create", "Creates a player without a surface id (for the Screen / Material outputs) and returns its handle. Call Load next; Destroy it when done.", CEV_NOPARAMS, EType::Int, "Handle", false, eEntry_NoPlayer | eEntry_Standalone, &FnCreate },
				{ EKind::Control, "89616E06-B1CA-4D3F-BCFE-22A63D3EBA9D"_cry_guid, "CreateForSurface", "Creates a player identified by a surface id (UIs use its texture by that id) and returns its handle; fails when the id is invalid or already in use. Call Load next; Destroy it when done.", CEV_ARRAY(kCreateForSurfaceParams), EType::Int, "Handle", false, eEntry_NoPlayer | eEntry_Standalone, &FnCreateForSurface },
				{ EKind::Control, "21F8A1C1-39AF-4937-BC2A-DA9818DA8555"_cry_guid, "Destroy", "Destroys the player and invalidates its handle.", CEV_NOPARAMS, EType::Void, nullptr, false, eEntry_Standalone, &FnDestroy },
				{ EKind::Control, "AA69D241-E1A4-4424-8139-E55F5E6A2F5D"_cry_guid, "Load", "Starts loading a .webm video or a .cevvideo descriptor. Asynchronous: the Ready event (or Error) follows. Play may be called right away, it starts when the video is ready.", CEV_ARRAY(kLoadParams), EType::Void, nullptr, true, 0, &FnLoad },
				{ EKind::Control, "CC294648-3872-4574-B532-8599A1EF0872"_cry_guid, "Play", "Plays from Stopped / Paused / Ended; queued while the video is loading.", CEV_NOPARAMS, EType::Void, nullptr, false, 0, &FnPlay },
				{ EKind::Control, "9A5627F9-47EE-40F7-A272-1C3E21562BD6"_cry_guid, "Pause", "Pauses playback.", CEV_NOPARAMS, EType::Void, nullptr, false, 0, &FnPause },
				{ EKind::Control, "1750FB12-2102-4635-98D3-361F88B7F73F"_cry_guid, "Resume", "Continues a paused video.", CEV_NOPARAMS, EType::Void, nullptr, false, 0, &FnResume },
				{ EKind::Control, "40BDAADD-C428-4A47-A16C-930B74F4EC42"_cry_guid, "Stop", "Stops and goes back to time 0. A Screen output is hidden until the next Play; a surface / material output keeps showing the first frame.", CEV_NOPARAMS, EType::Void, nullptr, false, 0, &FnStop },
				{ EKind::Control, "C89FA6A6-1EDF-4BDA-A6A6-5C08915A887C"_cry_guid, "Seek", "Jumps to a time in seconds.", CEV_ARRAY(kSeekParams), EType::Void, nullptr, false, 0, &FnSeek },
				{ EKind::Control, "ACDCF5FB-070D-439A-B430-A92609F06348"_cry_guid, "SetLoop", "Sets looping.", CEV_ARRAY(kSetLoopParams), EType::Void, nullptr, false, 0, &FnSetLoop },
				{ EKind::Control, "510FE2B6-D098-4C1A-9C7E-D74B77A1D22F"_cry_guid, "SetRate", "Sets the playback speed.", CEV_ARRAY(kSetRateParams), EType::Void, nullptr, false, 0, &FnSetRate },
				{ EKind::Control, "EB59C123-4D68-43F3-A979-65C544B6BA1C"_cry_guid, "SetLanguage", "Selects the audio language of a .cevvideo descriptor (reloads the media in place while playing).", CEV_ARRAY(kSetLanguageParams), EType::Void, nullptr, false, 0, &FnSetLanguage },
				{ EKind::Control, "737D69DA-7715-4384-8B6A-3F5C95E6F48D"_cry_guid, "SetVolume", "Sets the player volume.", CEV_ARRAY(kSetVolumeParams), EType::Void, nullptr, false, 0, &FnSetVolume },
				{ EKind::Control, "B68F6467-1C13-491C-B0F3-BFF0D3CBF1C6"_cry_guid, "SetOutput", "Chooses where the picture goes: only the surface texture, fullscreen on the screen, or a texture slot of a material.", CEV_ARRAY(kSetOutputParams), EType::Void, nullptr, false, 0, &FnSetOutput },
				{ EKind::Control, "079109C0-A530-4D82-BF32-5B8AF724C720"_cry_guid, "SetVariant", "Chooses the variant (resolution) of a .cevvideo descriptor.", CEV_ARRAY(kSetVariantParams), EType::Void, nullptr, false, 0, &FnSetVariant },
				{ EKind::Control, "76A06A36-55E3-46A3-A2F0-651F74507C41"_cry_guid, "SetSkippable", "Makes the video skippable: a key / mouse / gamepad button press stops it while it plays and sends the Skipped event.", CEV_ARRAY(kSetSkippableParams), EType::Void, nullptr, false, 0, &FnSetSkippable },
				{ EKind::Control, "0EABF916-13B4-4588-B90A-29C6951B3B99"_cry_guid, "SetDecode", "Chooses the decoder: Auto, GPU (Media Foundation hardware decode, falls back to the CPU with a warning) or CPU (libvpx). Applies at the next Load or Play from stopped.", CEV_ARRAY(kSetDecodeParams), EType::Void, nullptr, false, 0, &FnSetDecode },
				{ EKind::Control, "C3E284F2-F49E-4554-B15F-0A8A3DBC4FCB"_cry_guid, "UnbindMaterial", "Removes every material binding of the player (the Material output).", CEV_NOPARAMS, EType::Void, nullptr, false, 0, &FnUnbindMaterial },
				{ EKind::Control, "19B4033E-18B4-4488-B0B2-C1B42FD5C2E4"_cry_guid, "SetPauseDecodeWhenHidden", "Surface players: while no UI shows the surface, decode only (nothing is copied, converted or uploaded) and stop decoding after cev_hiddenStopSeconds; the clock, the sound and the events go on. On by default. Screen / Material outputs are always visible.", CEV_ARRAY(kSetPauseDecodeWhenHiddenParams), EType::Void, nullptr, false, 0, &FnSetPauseDecodeWhenHidden },
			};

			const SEntry kQueries[] =
			{
				{ EKind::Query, "00E397EB-B45E-4FE5-A4AC-D355A7B054F0"_cry_guid, "State", "State: 0 Stopped, 1 Playing, 2 Paused, 3 Ended, 4 Loading, 5 Error.", CEV_NOPARAMS, EType::Int, "State", false, 0, &FnState },
				{ EKind::Query, "6692C6D3-1BBB-447A-B680-49BF25DFAAAE"_cry_guid, "Time", "Current playback time in seconds.", CEV_NOPARAMS, EType::Float, "Time", false, 0, &FnTime },
				{ EKind::Query, "80EE65EA-C32C-404E-853B-51D1AED7B2EE"_cry_guid, "Duration", "Duration in seconds, 0 until loaded.", CEV_NOPARAMS, EType::Float, "Duration", false, 0, &FnDuration },
				{ EKind::Query, "1C5B665E-E1C1-49C8-8CC7-3C5D428F198B"_cry_guid, "Width", "Picture width in pixels, 0 until loaded.", CEV_NOPARAMS, EType::Int, "Width", false, 0, &FnWidth },
				{ EKind::Query, "E57C2BAD-C06D-479F-8FA6-1F26C709D041"_cry_guid, "Height", "Picture height in pixels, 0 until loaded.", CEV_NOPARAMS, EType::Int, "Height", false, 0, &FnHeight },
				{ EKind::Query, "B6CA4AE6-2F74-4BB2-B153-BEF9AC675213"_cry_guid, "TextureId", "Renderer texture id of the output texture, 0 while there is none.", CEV_NOPARAMS, EType::Int, "TextureId", false, 0, &FnTextureId },
				{ EKind::Query, "0CC4837F-F982-4246-9695-03991397069D"_cry_guid, "HasAlpha", "The video has an alpha channel.", CEV_NOPARAMS, EType::Bool, "HasAlpha", false, 0, &FnHasAlpha },
				{ EKind::Query, "EAF2740F-37EB-47E9-BFAE-A538D53EC1AF"_cry_guid, "IsPremultiplied", "The output texture holds premultiplied alpha (true for the RGBA8 layout).", CEV_NOPARAMS, EType::Bool, "IsPremultiplied", false, 0, &FnIsPremultiplied },
				{ EKind::Query, "B7A77A6C-E01F-4051-9B05-CA9596E6E49B"_cry_guid, "Language", "Audio language in use (descriptor name), empty without languages.", CEV_NOPARAMS, EType::String, "Language", false, 0, &FnLanguage },
				{ EKind::Query, "6AB75D22-6C00-4168-B33D-F06FDFACA93E"_cry_guid, "Variant", "Variant in use: 0 = main video, n = n-th variant.", CEV_NOPARAMS, EType::Int, "Variant", false, 0, &FnVariant },
				{ EKind::Query, "97C49A10-C7A1-40AC-BF8B-D7688F2CF190"_cry_guid, "SurfaceId", "Surface id of the player, empty for a player made by Create.", CEV_NOPARAMS, EType::String, "SurfaceId", false, 0, &FnSurfaceId },
				{ EKind::Query, "1DE894B9-3DBD-45B8-8614-441706F53725"_cry_guid, "Generation", "Counts changes of the texture (id, size or layout): fetch TextureId / Width / Height again when it changes.", CEV_NOPARAMS, EType::Int, "Generation", false, 0, &FnGeneration },
				{ EKind::Query, "40D5FCCC-EC86-45D5-9D1E-38DEFC23D6B9"_cry_guid, "PixelLayout", "Pixel layout of the texture: 0 = RGBA8 premultiplied, 1 = packed YUV 4:2:0 (R8), 2 = packed YUVA 4:2:0 (R8); planar only while every consumer of the texture source contract accepts it.", CEV_NOPARAMS, EType::Int, "PixelLayout", false, 0, &FnPixelLayout },
				{ EKind::Query, "803D34BF-61FB-4E84-BE3E-5A09CF51FE57"_cry_guid, "DecodeBackend", "Decoder in use: 0 = CPU (libvpx), 1 = GPU (Media Foundation).", CEV_NOPARAMS, EType::Int, "DecodeBackend", false, 0, &FnDecodeBackend },
				{ EKind::Query, "347FB1FF-D80B-4466-9F14-1A8D1DCDF6DB"_cry_guid, "FrameSerial", "Number of uploaded pictures as decimal text (never wraps); usable as a changed test.", CEV_NOPARAMS, EType::String, "FrameSerial", false, 0, &FnFrameSerial },
				{ EKind::Query, "099454C4-CF34-4256-942F-FF8370DB3641"_cry_guid, "IsLoaded", "The video is loaded: size, duration and texture are valid.", CEV_NOPARAMS, EType::Bool, "IsLoaded", false, 0, &FnIsLoaded },
				{ EKind::Query, "20DE7AD5-1A14-436C-921B-315869454C7B"_cry_guid, "IsLooping", "Looping is on.", CEV_NOPARAMS, EType::Bool, "IsLooping", false, 0, &FnIsLooping },
				{ EKind::Query, "1809C0FC-D74C-444B-BB5F-B5D852FCE18A"_cry_guid, "HasAudio", "The loaded video has audio.", CEV_NOPARAMS, EType::Bool, "HasAudio", false, 0, &FnHasAudio },
				{ EKind::Query, "AD99F4C8-E215-466F-A561-92370F82314E"_cry_guid, "Rate", "Playback speed last set (1 = normal).", CEV_NOPARAMS, EType::Float, "Rate", false, 0, &FnRate },
				{ EKind::Query, "B683DEC0-4EA3-4B01-9DEE-2A85987BABEE"_cry_guid, "Volume", "Player volume last set (0..1).", CEV_NOPARAMS, EType::Float, "Volume", false, 0, &FnVolume },
				{ EKind::Query, "6A378344-3AD7-4639-BC35-2254E21D51D9"_cry_guid, "Path", "Path given to the last Load, empty before.", CEV_NOPARAMS, EType::String, "Path", false, 0, &FnPath },
				{ EKind::Query, "99BFBA2F-54FE-4E5E-AC78-97D4583CA48F"_cry_guid, "PauseDecodeWhenHidden", "SetPauseDecodeWhenHidden is on (the default).", CEV_NOPARAMS, EType::Bool, "PauseDecodeWhenHidden", false, 0, &FnPauseDecodeWhenHidden },
			};

			const SEntry kEvents[] =
			{
				{ EKind::Event, "A89E3978-C26A-4D9F-B97D-1966C643A747"_cry_guid, "Ready", "The video is loaded: size, duration and texture are valid.", CEV_NOPARAMS, EType::Void, nullptr, false, 0, nullptr },
				{ EKind::Event, "9F772B30-27CD-4C59-9E93-AD587DB31EB0"_cry_guid, "Started", "Playback started.", CEV_NOPARAMS, EType::Void, nullptr, false, 0, nullptr },
				{ EKind::Event, "9438D812-2A0B-4B47-85FB-DAEBEEF755B5"_cry_guid, "NewFrame", "A new picture was uploaded to the texture (at most once per frame).", CEV_ARRAY(kNewFrameParams), EType::Void, nullptr, false, 0, nullptr },
				{ EKind::Event, "470E6DBF-A6B0-458E-809B-10977B9C56F6"_cry_guid, "Marker", "The playback position reached a marker of the .cevvideo descriptor.", CEV_ARRAY(kMarkerParams), EType::Void, nullptr, false, 0, nullptr },
				{ EKind::Event, "E9896D58-FD6A-43D0-9F0C-ECD8063562CB"_cry_guid, "Looped", "A loop iteration completed and playback restarted at 0.", CEV_NOPARAMS, EType::Void, nullptr, false, 0, nullptr },
				{ EKind::Event, "F9A1BE52-54C9-433B-94EF-E4FEAC8D2A27"_cry_guid, "Ended", "The end of a non-looping video was reached.", CEV_NOPARAMS, EType::Void, nullptr, false, 0, nullptr },
				{ EKind::Event, "45556174-A0F8-42D2-B5F2-2539AD540CBC"_cry_guid, "Skipped", "The player pressed a key and the skippable video was stopped.", CEV_NOPARAMS, EType::Void, nullptr, false, 0, nullptr },
				{ EKind::Event, "2631FA13-A3EE-4AE5-AF33-21ECE17D578A"_cry_guid, "Error", "Loading or playback failed (the player stays valid, state Error).", CEV_ARRAY(kErrorParams), EType::Void, nullptr, false, 0, nullptr },
			};
#undef CEV_ARRAY
#undef CEV_NOPARAMS

			static_assert(sizeof(kEvents) / sizeof(kEvents[0]) == eEvent_Count, "event list and EEvent are out of step");

			template<size_t N> SSpan Span(const SEntry (&a)[N]) { return SSpan{ a, a + N }; }

			SValue DefaultResult(EType type)
			{
				switch (type)
				{
				case EType::Bool: return SValue::FromBool(false);
				case EType::Int: return SValue::FromInt(0);
				case EType::Float: return SValue::FromFloat(0.f);
				case EType::String: return SValue::FromString("");
				default: return SValue();
				}
			}

			SValue DefaultValue(const SParam& p)
			{
				switch (p.type)
				{
				case EType::Bool: return SValue::FromBool(p.defNum != 0.0);
				case EType::Int: return SValue::FromInt(static_cast<int>(p.defNum));
				case EType::Float: return SValue::FromFloat(static_cast<float>(p.defNum));
				case EType::String: return SValue::FromString(p.defStr);
				default: return SValue();
				}
			}
		}

		SSpan Controls() { return Span(kControls); }
		SSpan Queries() { return Span(kQueries); }
		SSpan Events() { return Span(kEvents); }

		// ============================================================================================ Call
		bool Call(const SEntry& entry, int handle, const SValue* in, uint32_t inCount, SValue* pResult, IMaterial* pDefaultMaterial)
		{
			if (entry.fn == nullptr || entry.paramCount > kMaxParams)
			{
				LogWarning("api: <%s> is not callable", entry.name);
				return false;
			}
			if (!IsMainThread())
			{
				LogWarning("api: %s must be called on the main thread, call ignored", entry.name);
				return false;
			}
			if (mEnv == nullptr || mEnv->pApi == nullptr)
			{
				LogWarning("api: %s called while the plug-in is not available (shut down), ignored", entry.name);
				return false;
			}

			SValue args[kMaxParams];
			for (uint32_t i = 0; i < entry.paramCount; ++i)
			{
				if (in != nullptr && i < inCount && in[i].type != EType::Void)
					args[i] = in[i].As(entry.params[i].type);
				else
					args[i] = DefaultValue(entry.params[i]);
			}

			SValue local;
			SValue& result = pResult != nullptr ? *pResult : local;
			result = DefaultResult(entry.returnType);

			SCall call;
			call.handle = handle;
			call.args = args;
			call.argCount = entry.paramCount;
			call.pResult = &result;
			call.pDefaultMaterial = pDefaultMaterial;
			if (entry.NeedsPlayer())
			{
				call.pPlayer = mEnv->pApi->Find(handle, true); // warns for an invalid handle
				if (call.pPlayer == nullptr)
					return false;
			}
			return entry.fn(call);
		}

		const SEntry* FindByName(const char* name)
		{
			if (name == nullptr)
				return nullptr;
			for (const SSpan span : { Controls(), Queries(), Events() })
			{
				for (const SEntry& e : span)
				{
					if (stricmp(e.name, name) == 0)
						return &e;
				}
			}
			return nullptr;
		}

		const char* StateName(int state)
		{
			switch (static_cast<EVideoPlayerState>(state))
			{
			case EVideoPlayerState::Stopped: return "Stopped";
			case EVideoPlayerState::Playing: return "Playing";
			case EVideoPlayerState::Paused: return "Paused";
			case EVideoPlayerState::Ended: return "Ended";
			case EVideoPlayerState::Loading: return "Loading";
			case EVideoPlayerState::Error: return "Error";
			default: return "?";
			}
		}

		// ============================================================================================ self check
		int SelfCheck()
		{
			struct SSeen { CryGUID guid; const char* what; };
			std::vector<SSeen> seen;
			std::vector<const SEntry*> names;
			int problems = 0;
			size_t total = 0;

			auto note = [&](const CryGUID& guid, const char* what)
			{
				if (guid.IsNull())
				{
					LogWarning("api table: <%s> has a null GUID", what);
					++problems;
				}
				for (const SSeen& s : seen)
				{
					if (s.guid == guid)
					{
						LogWarning("api table: duplicate GUID %s: <%s> and <%s>", guid.ToDebugString(), s.what, what);
						++problems;
					}
				}
				seen.push_back(SSeen{ guid, what });
			};

			note(kSchematycComponentGuid, "Schematyc component");
			note(kCryVSComponentGuid, "CryVS component");
			note(kOutputModeTypeGuid, "output mode type");
			note(kVideoFileNameTypeGuid, "video file name type");
			note(kFitTypeGuid, "video fit type");
			note(kDecodeModeTypeGuid, "decode mode type");
			for (uint32_t i = 0; i < eCryVS_FunctionCount; ++i)
				note(kCryVSFunctionGuids[i], "CryVS global function");
			note(kCryVSResizedSignalGuid, "CryVS Resized signal");
			note(kCryVSClosedSignalGuid, "CryVS Closed signal");

			// retired GUIDs must never come back
			static const CryGUID retired[] =
			{
				"6215D8DE-93E8-4236-9DB5-9DDCFD4F1C0D"_cry_guid, // TextureName
				"2819FAC7-8B52-4BA3-9FA7-25BE56E317AD"_cry_guid, // InterfaceVersion
				"23441E42-DE20-4EA3-A312-F6D624D5D83C"_cry_guid, // Caps
			};

			const SSpan spans[3] = { Controls(), Queries(), Events() };
			for (uint32_t k = 0; k < 3; ++k)
			{
				for (const SEntry& e : spans[k])
				{
					++total;
					note(e.guid, e.name);
					for (const CryGUID& r : retired)
					{
						if (r == e.guid)
						{
							LogWarning("api table: <%s> uses the retired GUID %s", e.name, r.ToDebugString());
							++problems;
						}
					}
					if (e.name == nullptr || e.name[0] == '\0')
					{
						LogWarning("api table: an entry has no name");
						++problems;
						continue;
					}
					if (static_cast<uint32_t>(e.kind) != k)
					{
						LogWarning("api table: <%s> is in the wrong list", e.name);
						++problems;
					}
					for (const SEntry* const pEarlier : names)
					{
						if (stricmp(pEarlier->name, e.name) == 0)
						{
							LogWarning("api table: duplicate name <%s>", e.name);
							++problems;
						}
					}
					names.push_back(&e);
					if (e.paramCount > kMaxParams || (e.paramCount > 0 && e.params == nullptr))
					{
						LogWarning("api table: <%s> has %u parameters (max %u)", e.name, e.paramCount, kMaxParams);
						++problems;
					}
					for (uint32_t i = 0; i < e.paramCount && e.params != nullptr; ++i)
					{
						if (e.params[i].name == nullptr || e.params[i].name[0] == '\0' || e.params[i].type == EType::Void)
						{
							LogWarning("api table: <%s> parameter %u is malformed", e.name, i);
							++problems;
						}
						if (e.params[i].id == 0)
						{
							LogWarning("api table: <%s> parameter %u has no fixed id", e.name, i);
							++problems;
						}
						for (uint32_t j = 0; j < i; ++j)
						{
							if (e.params[j].id == e.params[i].id)
							{
								LogWarning("api table: <%s> parameters %u and %u share the id 0x%08x", e.name, j, i, e.params[i].id);
								++problems;
							}
						}
					}
					if ((e.kind == EKind::Event) != (e.fn == nullptr))
					{
						LogWarning("api table: <%s> %s", e.name, e.kind == EKind::Event ? "is an event but has a function" : "has no function");
						++problems;
					}
					if (e.kind == EKind::Query && e.returnType == EType::Void)
					{
						LogWarning("api table: query <%s> returns nothing", e.name);
						++problems;
					}
				}
			}
			static const char* const expectedEvents[eEvent_Count] = { "Ready", "Started", "NewFrame", "Marker", "Looped", "Ended", "Skipped", "Error" };
			for (uint32_t i = 0; i < eEvent_Count; ++i)
			{
				if (strcmp(Events()[i].name, expectedEvents[i]) != 0)
				{
					LogWarning("api table: event %u is <%s>, expected <%s>", i, Events()[i].name, expectedEvents[i]);
					++problems;
				}
			}

			if (problems != 0)
				CRY_ASSERT(false, "CEVPlayer API table has %d problem(s), see the log", problems);
			LogAlways("api table: %u controls, %u queries, %u events (%u entries), %d problem(s)",
				static_cast<unsigned>(Controls().size()), static_cast<unsigned>(Queries().size()), static_cast<unsigned>(Events().size()), static_cast<unsigned>(total), problems);
			return problems;
		}
	}
}
