// CryVS bindings (M6c). Compiled only when CMake found the CryVS headers (CEV_HAVE_CRYVS=1, community tree).
//
// What is registered (docs/design/M6-ui-source.md section 4.2, docs/tasks/M6c.md):
// - Three drawers = three namespaces of global functions (catalogue path Function::<Namespace>::<Name>): Video,
//   CEVPlayer::UI and CEVPlayer::Advanced. They are handle based and only call table functions (Api::Call); players they create
//   are script owned (DestroyScriptOwned removes them when a level unloads / the editor game mode ends). CryVS
//   function nodes are synchronous with one input and one output pin, so a FlowGraph node with several triggers
//   becomes several functions (CEVPlayer::Advanced::Play / Pause / Stop / Seek, one function per setting).
// - The component CEVPlayer::VideoPlayer keeps one member function per table entry (GUID = entry GUID) and one signal
//   per table event. With the property WatchSurfaceID set it owns no player and only follows that surface: signals
//   Ready, NewFrame, Resized, Closed (the FlowGraph node CEVPlayer:UI:SurfaceEvents).
// - Enumeration types Output, Screen Fit and Decode.
//
// Registration timing: plug-ins are loaded before CSystem::InitSchematyc (gEnv->pCryVS is null in Initialize), the
// first plug-in event after it is ESYSTEM_EVENT_GAME_POST_INIT. Nothing loads CryVS script files before that in this
// fork (no runtime CScriptRegistry::Load call; the editor plug-in's RefreshFiles(EditorFixUp) runs when Sandbox loads
// its plug-ins, after GAME_POST_INIT - see M6c report), so GAME_POST_INIT stays the registration point. Every
// CFramework::RefreshEnv() clears the registry and sends envRefresh: we register again from there. Each element is
// registered only when the registry does not have its GUID yet.
//
// Unregistration: IEnvRegistry has no removal API (only BlacklistElement, which hides a GUID for good). The elements
// stay in the registry until CryVS is unloaded; CSystem::ShutDown unloads CryVS (UnloadSchematycModule) before it
// deletes the plug-in manager, so the registry releases our objects while CEVPlayer.dll is still loaded.
#include "StdAfx.h"
#include "PluginEnv.h"
#include "CryVSEnv.h"

#include <algorithm>
#include <vector>

#include <CryVS/IFramework.h>
#include <CryVS/Env/IEnvRegistry.h>
#include <CryVS/EnvTypeDesc.h>
#include <CryVS/ComponentFactory.h>
#include <CryVS/IObject.h>
#include <CryVS/ISignal.h>
#include <CryVS/System/ComponentMemberFunction.h>
#include <CryVS/System/GlobalFunction.h>
#include <CryVS/System/Stack.h>
#include <CrySerialization/Enum.h>
#include <CrySerialization/STL.h>
#include <CrySerialization/Decorators/ResourceFilePath.h>
#include <Cry3DEngine/IMaterial.h>
#include <CryEntitySystem/IEntitySystem.h>
#include <CrySystem/ConsoleRegistration.h>
#include <CrySystem/ITimer.h>

#include "Bindings/ApiPlayers.h"
#include "Bindings/ApiTable.h"
#include "CEVCore/Status.h"

SERIALIZATION_ENUM_BEGIN_NESTED2(CEVPlayer, Api, EOutputMode, "Output")
	SERIALIZATION_ENUM(CEVPlayer::Api::EOutputMode::Surface, "Surface", "Surface (for UIs)")
	SERIALIZATION_ENUM(CEVPlayer::Api::EOutputMode::Screen, "Screen", "Screen")
	SERIALIZATION_ENUM(CEVPlayer::Api::EOutputMode::Material, "Material", "Material slot")
SERIALIZATION_ENUM_END()

SERIALIZATION_ENUM_BEGIN_NESTED2(CEVPlayer, Api, EDecodeMode, "Decode")
	SERIALIZATION_ENUM(CEVPlayer::Api::EDecodeMode::Auto, "Auto", "Auto")
	SERIALIZATION_ENUM(CEVPlayer::Api::EDecodeMode::Gpu, "GPU", "GPU")
	SERIALIZATION_ENUM(CEVPlayer::Api::EDecodeMode::Cpu, "CPU", "CPU")
SERIALIZATION_ENUM_END()

SERIALIZATION_ENUM_BEGIN_NESTED(CEVPlayer, EVideoFit, "Screen Fit")
	SERIALIZATION_ENUM(CEVPlayer::EVideoFit::Fill, "Fill", "Fill (stretch)")
	SERIALIZATION_ENUM(CEVPlayer::EVideoFit::Fit, "Fit", "Fit (keep aspect)")
	SERIALIZATION_ENUM(CEVPlayer::EVideoFit::Letterbox, "Letterbox", "Letterbox (black bars)")
SERIALIZATION_ENUM_END()

namespace CEVPlayer
{
	namespace CryVSBind
	{
		namespace
		{
			using Api::SValue;

			// ---------------------------------------------------------------------------------------- table access
			bool ApiAvailable() { return mEnv != nullptr && mEnv->pApi != nullptr; }
			bool HandleAlive(int handle) { return handle != 0 && ApiAvailable() && mEnv->pApi->Find(handle, false) != nullptr; }

			bool Run(const char* name, int handle, std::initializer_list<SValue> args = {}, SValue* pResult = nullptr, IMaterial* pMaterial = nullptr)
			{
				const Api::SEntry* const pEntry = Api::FindByName(name);
				if (pEntry == nullptr)
				{
					LogWarning("CryVS: API table entry <%s> is missing", name);
					return false;
				}
				SValue in[Api::kMaxParams];
				uint32_t n = 0;
				for (const SValue& v : args)
				{
					if (n < Api::kMaxParams)
						in[n++] = v;
				}
				return Api::Call(*pEntry, handle, in, n, pResult, pMaterial);
			}

			SValue Q(const char* name, int handle)
			{
				SValue v;
				Run(name, handle, {}, &v);
				return v;
			}

			SValue I(int v) { return SValue::FromInt(v); }
			SValue B(bool v) { return SValue::FromBool(v); }
			SValue F(float v) { return SValue::FromFloat(v); }
			SValue S(const char* v) { return SValue::FromString(v); }

			IMaterial* MaterialOfEntity(EntityId entityId)
			{
				IEntity* const pEntity = entityId != 0 && gEnv->pEntitySystem != nullptr ? gEnv->pEntitySystem->GetEntity(entityId) : nullptr;
				if (pEntity == nullptr)
					return nullptr;
				IMaterial* pMaterial = pEntity->GetMaterial();
				if (pMaterial == nullptr)
					pMaterial = pEntity->GetRenderMaterial(-1);
				return pMaterial;
			}

			//! Creates a script owned player (surfaceId non-null: a surface player). 0 (with a warning) on failure.
			int CreatePlayer(const char* what, const char* surfaceId)
			{
				SValue handle;
				const bool ok = surfaceId != nullptr ? Run("CreateForSurface", 0, { S(surfaceId) }, &handle) : Run("Create", 0, {}, &handle);
				if (!ok || handle.i == 0)
				{
					LogWarning("CryVS: %s: no player created (%s)", what, ApiAvailable() ? mEnv->pApi->LastErrorMessage() : "plug-in not available");
					return 0;
				}
				return handle.i;
			}

			void DestroyIfAlive(int handle)
			{
				if (HandleAlive(handle))
					Run("Destroy", handle);
			}

			// ---------------------------------------------------------------------------------------- global functions
			//! Port types of the global functions. Fit / Output / Decode are the registered enumerations; Entity is the
			//! CryVS entity id.
			enum class EPort : uint8 { Bool, Int, Float, String, Fit, Output, Decode, Entity };

			struct SPort
			{
				EPort type;
				const char* name;
				const char* description;
				double defNum;     //!< default of Bool / Int / Float / enumeration inputs
				const char* defStr; //!< default of String inputs
			};

			//! in[] holds the inputs (enumerations and the entity id as Int), out[] gets the outputs (declared types).
			typedef void (*TImpl)(const SValue* in, SValue* out);

			struct SGlobalFunction
			{
				Api::ECryVSFunction id;
				const char* ns;
				const char* name;
				const char* description;
				const SPort* inputs;
				uint32 inputCount;
				const SPort* outputs;
				uint32 outputCount;
				TImpl fn;
			};

			const char* const kVideoDesc = "Video file (.webm) or runtime descriptor (.cevvideo), for example Videos/intro.webm";
			const char* const kHandleDesc = "Player handle (0 = none)";
			const char* const kDecodeDesc = "Decoder: Auto, GPU or CPU (this build always decodes on the CPU)";
			const char* const kSurfaceDesc = "Name UIs use to show the video: 1 to 63 of A-Z a-z 0-9 _ . : -, case-insensitive, unique";

			// ----- Video
			const SPort kPlayOnScreenIn[] =
			{
				{ EPort::String, "Video", kVideoDesc, 0, "" },
				{ EPort::Fit, "Fit", "How the picture is placed: Fill stretches it, Fit keeps the aspect and shows the scene in the bars, Letterbox adds black bars", 2, nullptr },
				{ EPort::Bool, "Loop", "Restart at the beginning when the end is reached", 0, nullptr },
				{ EPort::Bool, "Skippable", "A key, mouse or gamepad button press stops the video", 0, nullptr },
				{ EPort::Decode, "Decode", kDecodeDesc, 0, nullptr },
			};
			const SPort kHandleOut[] = { { EPort::Int, "Handle", "Handle of the new player, 0 when it could not be created or loaded (see the log)", 0, nullptr } };
			const SPort kHandleIn[] = { { EPort::Int, "Handle", kHandleDesc, 0, nullptr } };

			void FnPlayOnScreen(const SValue* in, SValue* out)
			{
				out[0] = I(0);
				if (in[0].s.empty())
				{
					LogWarning("CryVS: CEVPlayer::PlayOnScreen: Video is empty");
					return;
				}
				const int h = CreatePlayer("CEVPlayer::PlayOnScreen", nullptr);
				if (h == 0)
					return;
				Run("SetDecode", h, { I(in[4].i) });
				Run("SetOutput", h, { I(static_cast<int>(Api::EOutputMode::Screen)), S(""), I(0), I(-1), I(in[1].i) });
				Run("SetLoop", h, { B(in[2].b) });
				Run("SetSkippable", h, { B(in[3].b) });
				if (!Run("Load", h, { S(in[0].s.c_str()) }) || !Run("Play", h))
				{
					DestroyIfAlive(h);
					return;
				}
				out[0] = I(h);
			}

			const SPort kPlayOnMaterialIn[] =
			{
				{ EPort::Entity, "Entity", "Entity whose material is used when Material is empty", 0, nullptr },
				{ EPort::String, "Video", kVideoDesc, 0, "" },
				{ EPort::String, "Material", "Material that shows the video; empty uses the material of Entity", 0, "" },
				{ EPort::Int, "Slot", "Texture slot of the material that gets the video (0 = diffuse)", 0, nullptr },
				{ EPort::Int, "SubMaterial", "Sub-material index; -1 uses the material itself", -1, nullptr },
				{ EPort::Bool, "Loop", "Restart at the beginning when the end is reached", 0, nullptr },
				{ EPort::Bool, "Autoplay", "Play at once; otherwise the video is only loaded (CEVPlayer::Advanced::Play starts it)", 1, nullptr },
				{ EPort::Decode, "Decode", kDecodeDesc, 0, nullptr },
			};

			void FnPlayOnMaterial(const SValue* in, SValue* out)
			{
				out[0] = I(0);
				if (in[1].s.empty())
				{
					LogWarning("CryVS: CEVPlayer::PlayOnMaterial: Video is empty");
					return;
				}
				const int h = CreatePlayer("CEVPlayer::PlayOnMaterial", nullptr);
				if (h == 0)
					return;
				Run("SetDecode", h, { I(in[7].i) });
				if (!Run("SetOutput", h, { I(static_cast<int>(Api::EOutputMode::Material)), S(in[2].s.c_str()), I(in[3].i), I(in[4].i), I(2) }, nullptr, MaterialOfEntity(static_cast<EntityId>(in[0].i))))
				{
					LogWarning("CryVS: CEVPlayer::PlayOnMaterial: material binding failed (no material, material not found or bad slot)");
					DestroyIfAlive(h);
					return;
				}
				Run("SetLoop", h, { B(in[5].b) });
				if (!Run("Load", h, { S(in[1].s.c_str()) }) || (in[6].b && !Run("Play", h)))
				{
					DestroyIfAlive(h);
					return;
				}
				out[0] = I(h);
			}

			void FnVideoStop(const SValue* in, SValue*)
			{
				if (in[0].i != 0 && ApiAvailable() && mEnv->pApi->IsScriptOwned(in[0].i))
					Run("Destroy", in[0].i);
				else if (in[0].i != 0)
					LogWarning("CryVS: CEVPlayer::Stop: handle %d is not a player made by a script function, ignored", in[0].i);
			}

			// ----- CEVPlayer::UI
			const SPort kPlayToSurfaceIn[] =
			{
				{ EPort::String, "Video", kVideoDesc, 0, "" },
				{ EPort::String, "SurfaceID", kSurfaceDesc, 0, "" },
				{ EPort::Bool, "Loop", "Restart at the beginning when the end is reached", 0, nullptr },
				{ EPort::Bool, "Autoplay", "Play at once; otherwise the video is only loaded (CEVPlayer::Advanced::Play starts it)", 1, nullptr },
				{ EPort::Decode, "Decode", kDecodeDesc, 0, nullptr },
				{ EPort::Bool, "PauseDecodeWhenHidden", "While no UI shows the surface only decode, and stop decoding after cev_hiddenStopSeconds", 1, nullptr },
			};

			void FnPlayToSurface(const SValue* in, SValue* out)
			{
				out[0] = I(0);
				if (in[0].s.empty())
				{
					LogWarning("CryVS: CEVPlayer::UI::PlayToSurface: Video is empty");
					return;
				}
				const int h = CreatePlayer("CEVPlayer::UI::PlayToSurface", in[1].s.c_str());
				if (h == 0)
					return;
				Run("SetDecode", h, { I(in[4].i) });
				Run("SetLoop", h, { B(in[2].b) });
				Run("SetPauseDecodeWhenHidden", h, { B(in[5].b) });
				if (!Run("Load", h, { S(in[0].s.c_str()) }) || (in[3].b && !Run("Play", h)))
				{
					DestroyIfAlive(h);
					return;
				}
				out[0] = I(h);
			}

			const SPort kSurfaceIn[] = { { EPort::String, "SurfaceID", "Surface id (made by PlayToSurface, a Video Player component or another plug-in)", 0, "" } };

			void FnStopSurface(const SValue* in, SValue*)
			{
				const int h = ApiAvailable() ? mEnv->pApi->AttachSurface(in[0].s.c_str()) : 0;
				if (h == 0)
					return; // no such surface
				if (mEnv->pApi->IsScriptOwned(h))
					Run("Destroy", h);
				else
					LogWarning("CryVS: CEVPlayer::UI::StopSurface: surface <%s> was not made by a script function, left alone", in[0].s.c_str());
			}

			const SPort kSurfaceInfoOut[] =
			{
				{ EPort::Bool, "Exists", "The surface exists; when false every other output is 0 or empty", 0, nullptr },
				{ EPort::Int, "TextureId", "Renderer texture id of the surface, 0 while there is none", 0, nullptr },
				{ EPort::Int, "Width", "Picture width in pixels, 0 until loaded", 0, nullptr },
				{ EPort::Int, "Height", "Picture height in pixels, 0 until loaded", 0, nullptr },
				{ EPort::Int, "PixelLayout", "Pixel layout of the texture: 0 = RGBA8, 1 = packed YUV420 (R8), 2 = packed YUVA420 (R8)", 0, nullptr },
				{ EPort::Bool, "Premultiplied", "The texture holds premultiplied alpha (RGBA8 layout)", 0, nullptr },
				{ EPort::Bool, "HasAlpha", "The video has an alpha channel", 0, nullptr },
				{ EPort::String, "FrameSerial", "Number of uploaded pictures as decimal text", 0, "" },
				{ EPort::Int, "State", "0 Stopped, 1 Playing, 2 Paused, 3 Ended, 4 Loading, 5 Error", 0, nullptr },
			};

			void FnSurfaceInfo(const SValue* in, SValue* out)
			{
				out[0] = B(false); out[1] = I(0); out[2] = I(0); out[3] = I(0); out[4] = I(0);
				out[5] = B(false); out[6] = B(false); out[7] = S(""); out[8] = I(0);
				const int h = ApiAvailable() ? mEnv->pApi->AttachSurface(in[0].s.c_str()) : 0;
				if (h == 0)
					return;
				out[0] = B(true);
				out[1] = Q("TextureId", h);
				out[2] = Q("Width", h);
				out[3] = Q("Height", h);
				out[4] = Q("PixelLayout", h);
				out[5] = Q("IsPremultiplied", h);
				out[6] = Q("HasAlpha", h);
				out[7] = Q("FrameSerial", h);
				out[8] = Q("State", h);
			}

			// ----- CEVPlayer::Advanced
			const SPort kOpenIn[] =
			{
				{ EPort::String, "Video", kVideoDesc, 0, "" },
				{ EPort::Output, "Output", "Where the picture goes: Surface (a named surface for UIs), Screen (fullscreen) or Material (a texture slot)", 1, nullptr },
				{ EPort::String, "SurfaceID", "Output Surface: the surface name UIs use", 0, "" },
				{ EPort::Fit, "Fit", "Output Screen: Fill stretches, Fit keeps the aspect with the scene in the bars, Letterbox adds black bars", 2, nullptr },
				{ EPort::String, "Material", "Output Material: material name", 0, "" },
				{ EPort::Int, "Slot", "Output Material: texture slot (0 = diffuse)", 0, nullptr },
				{ EPort::Int, "SubMaterial", "Output Material: sub-material index, -1 = the material itself", -1, nullptr },
				{ EPort::Decode, "Decode", kDecodeDesc, 0, nullptr },
			};

			void FnOpen(const SValue* in, SValue* out)
			{
				out[0] = I(0);
				if (in[0].s.empty())
				{
					LogWarning("CryVS: CEVPlayer::Advanced::Open: Video is empty");
					return;
				}
				const int output = in[1].i;
				const int h = CreatePlayer("CEVPlayer::Advanced::Open", output == static_cast<int>(Api::EOutputMode::Surface) ? in[2].s.c_str() : nullptr);
				if (h == 0)
					return;
				Run("SetDecode", h, { I(in[7].i) });
				if (!Run("SetOutput", h, { I(output), S(in[4].s.c_str()), I(in[5].i), I(in[6].i), I(in[3].i) }))
				{
					LogWarning("CryVS: CEVPlayer::Advanced::Open: SetOutput failed (unknown Output, or the material is missing / not found / bad slot)");
					DestroyIfAlive(h);
					return;
				}
				if (!Run("Load", h, { S(in[0].s.c_str()) }))
				{
					DestroyIfAlive(h);
					return;
				}
				out[0] = I(h);
			}

			void FnClose(const SValue* in, SValue*)
			{
				if (in[0].i != 0 && ApiAvailable() && mEnv->pApi->IsScriptOwned(in[0].i))
					Run("Destroy", in[0].i);
				else if (in[0].i != 0)
					LogWarning("CryVS: CEVPlayer::Advanced::Close: handle %d is not a player made by a script function, ignored", in[0].i);
			}

			void FnPlay(const SValue* in, SValue*) { Run("Play", in[0].i); }
			void FnPause(const SValue* in, SValue*) { Run("Pause", in[0].i); }
			void FnStop(const SValue* in, SValue*) { Run("Stop", in[0].i); }

			const SPort kSeekIn[] = { { EPort::Int, "Handle", kHandleDesc, 0, nullptr }, { EPort::Float, "Seconds", "Target time in seconds", 0, nullptr } };
			void FnSeek(const SValue* in, SValue*) { Run("Seek", in[0].i, { in[1] }); }

			const SPort kSetLoopIn[] = { { EPort::Int, "Handle", kHandleDesc, 0, nullptr }, { EPort::Bool, "Loop", "Restart at the beginning when the end is reached", 0, nullptr } };
			void FnSetLoop(const SValue* in, SValue*) { Run("SetLoop", in[0].i, { in[1] }); }

			const SPort kSetRateIn[] = { { EPort::Int, "Handle", kHandleDesc, 0, nullptr }, { EPort::Float, "Rate", "Playback speed 0.25 to 4 (1 = normal)", 1, nullptr } };
			void FnSetRate(const SValue* in, SValue*) { Run("SetRate", in[0].i, { in[1] }); }

			const SPort kSetVolumeIn[] = { { EPort::Int, "Handle", kHandleDesc, 0, nullptr }, { EPort::Float, "Volume", "Player volume 0 to 1", 1, nullptr } };
			void FnSetVolume(const SValue* in, SValue*) { Run("SetVolume", in[0].i, { in[1] }); }

			const SPort kSetLanguageIn[] = { { EPort::Int, "Handle", kHandleDesc, 0, nullptr }, { EPort::String, "Language", "Audio language of the .cevvideo descriptor (empty = default choice)", 0, "" } };
			void FnSetLanguage(const SValue* in, SValue*) { Run("SetLanguage", in[0].i, { in[1] }); }

			const SPort kSetVariantIn[] = { { EPort::Int, "Handle", kHandleDesc, 0, nullptr }, { EPort::Int, "Variant", "-2 follows cev_variant, -1 automatic, 0 main video, n the n-th variant", -2, nullptr } };
			void FnSetVariant(const SValue* in, SValue*) { Run("SetVariant", in[0].i, { in[1] }); }

			const SPort kSetSkippableIn[] = { { EPort::Int, "Handle", kHandleDesc, 0, nullptr }, { EPort::Bool, "Skippable", "A button press stops the playing video and sends Skipped", 0, nullptr } };
			void FnSetSkippable(const SValue* in, SValue*) { Run("SetSkippable", in[0].i, { in[1] }); }

			const SPort kSetPauseDecodeWhenHiddenIn[] = { { EPort::Int, "Handle", kHandleDesc, 0, nullptr }, { EPort::Bool, "Pause", "A surface no UI shows only decodes, and stops decoding after cev_hiddenStopSeconds", 1, nullptr } };
			void FnSetPauseDecodeWhenHidden(const SValue* in, SValue*) { Run("SetPauseDecodeWhenHidden", in[0].i, { in[1] }); }

			const SPort kInfoOut[] =
			{
				{ EPort::Int, "State", "0 Stopped, 1 Playing, 2 Paused, 3 Ended, 4 Loading, 5 Error", 0, nullptr },
				{ EPort::Float, "Time", "Current playback time in seconds", 0, nullptr },
				{ EPort::Float, "Duration", "Duration in seconds, 0 until loaded", 0, nullptr },
				{ EPort::Int, "Width", "Picture width in pixels, 0 until loaded", 0, nullptr },
				{ EPort::Int, "Height", "Picture height in pixels, 0 until loaded", 0, nullptr },
				{ EPort::Bool, "HasAlpha", "The video has an alpha channel", 0, nullptr },
				{ EPort::String, "Language", "Audio language in use, empty without languages", 0, "" },
				{ EPort::Int, "Variant", "Variant in use: 0 = main video, n = n-th variant", 0, nullptr },
				{ EPort::Int, "DecodeBackend", "Decoder in use: 0 = CPU, 1 = GPU", 0, nullptr },
				{ EPort::String, "FrameSerial", "Number of uploaded pictures as decimal text", 0, "" },
				{ EPort::String, "SurfaceId", "Surface id of the player, empty for Screen / Material players", 0, "" },
			};

			void FnInfo(const SValue* in, SValue* out)
			{
				static const char* const queries[] = { "State", "Time", "Duration", "Width", "Height", "HasAlpha", "Language", "Variant", "DecodeBackend", "FrameSerial", "SurfaceId" };
				const bool alive = HandleAlive(in[0].i);
				if (!alive && in[0].i != 0)
					LogWarning("CryVS: CEVPlayer::Advanced::Info: invalid player handle %d", in[0].i);
				for (uint32 i = 0; i < CRY_ARRAY_COUNT(queries); ++i)
					out[i] = alive ? Q(queries[i], in[0].i) : SValue();
			}

#define CEV_PORTS(a) a, CRY_ARRAY_COUNT(a)
#define CEV_NOPORTS nullptr, 0
			const SGlobalFunction kGlobalFunctions[] =
			{
				{ Api::eCryVS_Video_PlayOnScreen, "CEVPlayer", "PlayOnScreen", "Plays a video fullscreen on top of the game and returns its handle. The player belongs to the script: CEVPlayer::Stop, or the end of the level / game mode, destroys it.", CEV_PORTS(kPlayOnScreenIn), CEV_PORTS(kHandleOut), &FnPlayOnScreen },
				{ Api::eCryVS_Video_PlayOnMaterial, "CEVPlayer", "PlayOnMaterial", "Plays a video on a texture slot of a material (a screen in the level) and returns its handle. CEVPlayer::Stop, or the end of the level / game mode, destroys it.", CEV_PORTS(kPlayOnMaterialIn), CEV_PORTS(kHandleOut), &FnPlayOnMaterial },
				{ Api::eCryVS_Video_Stop, "CEVPlayer", "Stop", "Stops the video and destroys its player (handle from PlayOnScreen / PlayOnMaterial).", CEV_PORTS(kHandleIn), CEV_NOPORTS, &FnVideoStop },
				{ Api::eCryVS_VideoUI_PlayToSurface, "CEVPlayer::UI", "PlayToSurface", "Plays a video into a named surface that any UI can show by its SurfaceID and returns its handle. CEVPlayer::UI::StopSurface, or the end of the level / game mode, destroys it.", CEV_PORTS(kPlayToSurfaceIn), CEV_PORTS(kHandleOut), &FnPlayToSurface },
				{ Api::eCryVS_VideoUI_StopSurface, "CEVPlayer::UI", "StopSurface", "Stops the video of a surface made by PlayToSurface (or CEVPlayer::Advanced::Open) and destroys the surface.", CEV_PORTS(kSurfaceIn), CEV_NOPORTS, &FnStopSurface },
				{ Api::eCryVS_VideoUI_SurfaceInfo, "CEVPlayer::UI", "SurfaceInfo", "Reads a surface by its SurfaceID: texture id, size and layout for UI systems that use the texture directly.", CEV_PORTS(kSurfaceIn), CEV_PORTS(kSurfaceInfoOut), &FnSurfaceInfo },
				{ Api::eCryVS_VideoAdvanced_Open, "CEVPlayer::Advanced", "Open", "Creates a player for the chosen Output, starts loading Video and returns the handle for the other VideoAdvanced functions. CEVPlayer::Advanced::Close, or the end of the level / game mode, destroys it.", CEV_PORTS(kOpenIn), CEV_PORTS(kHandleOut), &FnOpen },
				{ Api::eCryVS_VideoAdvanced_Close, "CEVPlayer::Advanced", "Close", "Destroys a player made by Open.", CEV_PORTS(kHandleIn), CEV_NOPORTS, &FnClose },
				{ Api::eCryVS_VideoAdvanced_Play, "CEVPlayer::Advanced", "Play", "Plays from the start, or continues a paused video.", CEV_PORTS(kHandleIn), CEV_NOPORTS, &FnPlay },
				{ Api::eCryVS_VideoAdvanced_Pause, "CEVPlayer::Advanced", "Pause", "Pauses playback.", CEV_PORTS(kHandleIn), CEV_NOPORTS, &FnPause },
				{ Api::eCryVS_VideoAdvanced_Stop, "CEVPlayer::Advanced", "Stop", "Stops and goes back to the beginning (the player stays).", CEV_PORTS(kHandleIn), CEV_NOPORTS, &FnStop },
				{ Api::eCryVS_VideoAdvanced_Seek, "CEVPlayer::Advanced", "Seek", "Jumps to a time in seconds.", CEV_PORTS(kSeekIn), CEV_NOPORTS, &FnSeek },
				{ Api::eCryVS_VideoAdvanced_SetLoop, "CEVPlayer::Advanced", "SetLoop", "Sets looping.", CEV_PORTS(kSetLoopIn), CEV_NOPORTS, &FnSetLoop },
				{ Api::eCryVS_VideoAdvanced_SetRate, "CEVPlayer::Advanced", "SetRate", "Sets the playback speed.", CEV_PORTS(kSetRateIn), CEV_NOPORTS, &FnSetRate },
				{ Api::eCryVS_VideoAdvanced_SetVolume, "CEVPlayer::Advanced", "SetVolume", "Sets the player volume.", CEV_PORTS(kSetVolumeIn), CEV_NOPORTS, &FnSetVolume },
				{ Api::eCryVS_VideoAdvanced_SetLanguage, "CEVPlayer::Advanced", "SetLanguage", "Selects the audio language of a .cevvideo descriptor.", CEV_PORTS(kSetLanguageIn), CEV_NOPORTS, &FnSetLanguage },
				{ Api::eCryVS_VideoAdvanced_SetVariant, "CEVPlayer::Advanced", "SetVariant", "Chooses the variant (resolution) of a .cevvideo descriptor.", CEV_PORTS(kSetVariantIn), CEV_NOPORTS, &FnSetVariant },
				{ Api::eCryVS_VideoAdvanced_SetSkippable, "CEVPlayer::Advanced", "SetSkippable", "Makes the video skippable: a button press stops it while it plays.", CEV_PORTS(kSetSkippableIn), CEV_NOPORTS, &FnSetSkippable },
				{ Api::eCryVS_VideoAdvanced_Info, "CEVPlayer::Advanced", "Info", "Reads the state, time, size, language, variant, decoder, frame serial and surface id of a player handle.", CEV_PORTS(kHandleIn), CEV_PORTS(kInfoOut), &FnInfo },
				{ Api::eCryVS_VideoAdvanced_SetPauseDecodeWhenHidden, "CEVPlayer::Advanced", "SetPauseDecodeWhenHidden", "Surface players: while no UI shows the surface only decode, and stop decoding after cev_hiddenStopSeconds (on by default).", CEV_PORTS(kSetPauseDecodeWhenHiddenIn), CEV_NOPORTS, &FnSetPauseDecodeWhenHidden },
			};
			static_assert(CRY_ARRAY_COUNT(kGlobalFunctions) == Api::eCryVS_FunctionCount, "one entry per ECryVSFunction");
#undef CEV_PORTS
#undef CEV_NOPORTS

			//! A global function described by an SGlobalFunction: parameters 0..inputCount-1 are the inputs, the outputs
			//! follow (bound as reference parameters, the way CryVS marks out parameters).
			class CApiGlobalFunction final : public CryVS::GlobalFunction::CBase
			{
			public:
				explicit CApiGlobalFunction(const SGlobalFunction& desc)
					: CBase(Api::kCryVSFunctionGuids[desc.id], desc.name, __FILE__, "Code")
					, m_desc(desc)
				{
					for (uint32 i = 0; i < desc.inputCount; ++i)
						AddParamOf(desc.inputs[i].type, CryVS::ParamFlags::NONE);
					// outputs: non-const reference parameters (strings stored as CPoolString: a const char* parameter
					// counts as a pointer to const and cannot be bound as an output)
					for (uint32 i = 0; i < desc.outputCount; ++i)
					{
						if (desc.outputs[i].type == EPort::String)
							AddParam<CryVS::CPoolString>(CryVS::ParamFlags::IS_REFERENCE);
						else
							AddParamOf(desc.outputs[i].type, CryVS::ParamFlags::IS_REFERENCE);
					}

					CryVS::IGlobalFunction& self = *this; // the typed BindInput overloads live on the interface
					self.SetName(desc.name);
					self.SetNamespace(desc.ns);
					self.SetAuthor("CEVPlayer");
					self.SetDescription(desc.description);
					for (uint32 i = 0; i < desc.inputCount; ++i)
					{
						const SPort& p = desc.inputs[i];
						switch (p.type)
						{
						case EPort::Bool: self.BindInput(i, p.name, p.description, p.defNum != 0.0); break;
						case EPort::Int: self.BindInput(i, p.name, p.description, static_cast<int32>(p.defNum)); break;
						case EPort::Float: self.BindInput(i, p.name, p.description, static_cast<float>(p.defNum)); break;
						case EPort::Fit: self.BindInput(i, p.name, p.description, static_cast<EVideoFit>(static_cast<int>(p.defNum))); break;
						case EPort::Output: self.BindInput(i, p.name, p.description, static_cast<Api::EOutputMode>(static_cast<int>(p.defNum))); break;
						case EPort::Decode: self.BindInput(i, p.name, p.description, static_cast<Api::EDecodeMode>(static_cast<int>(p.defNum))); break;
						case EPort::Entity: self.BindInput(i, p.name, p.description); break;
						default: self.BindInput(i, p.name, p.description, p.defStr != nullptr ? p.defStr : ""); break;
						}
					}
					for (uint32 i = 0; i < desc.outputCount; ++i)
						self.BindOutput(desc.inputCount + i, desc.outputs[i].name, desc.outputs[i].description);
				}

				virtual void Call(const CryVS::TVariantConstArray& inputs, const CryVS::TVariantArray& outputs) const override
				{
					CryVS::CVariantArrayInputArchive inputsArchive(inputs);
					CryVS::CVariantArrayOutputArchive outputsArchive(outputs);

					SValue in[kMaxPorts];
					SValue out[kMaxPorts];
					for (uint32 i = 0; i < m_desc.inputCount && i < kMaxPorts; ++i)
					{
						switch (m_desc.inputs[i].type)
						{
						case EPort::Bool: { bool v = false; LoadParam(inputsArchive, i, v); in[i] = B(v); break; }
						case EPort::Int: { int32 v = 0; LoadParam(inputsArchive, i, v); in[i] = I(v); break; }
						case EPort::Float: { float v = 0.f; LoadParam(inputsArchive, i, v); in[i] = F(v); break; }
						case EPort::Fit: { EVideoFit v = EVideoFit::Letterbox; LoadParam(inputsArchive, i, v); in[i] = I(static_cast<int>(v)); break; }
						case EPort::Output: { Api::EOutputMode v = Api::EOutputMode::Screen; LoadParam(inputsArchive, i, v); in[i] = I(static_cast<int>(v)); break; }
						case EPort::Decode: { Api::EDecodeMode v = Api::EDecodeMode::Auto; LoadParam(inputsArchive, i, v); in[i] = I(static_cast<int>(v)); break; }
						case EPort::Entity: { CryVS::ExplicitEntityId v; LoadParam(inputsArchive, i, v); in[i] = I(static_cast<int>(v.GetValue())); break; }
						default: { const char* v = ""; LoadParam(inputsArchive, i, v); in[i] = S(v); break; }
						}
					}

					m_desc.fn(in, out);

					for (uint32 k = 0; k < m_desc.outputCount && k < kMaxPorts; ++k)
					{
						const uint32 idx = m_desc.inputCount + k;
						switch (m_desc.outputs[k].type)
						{
						case EPort::Bool: { bool v = out[k].As(Api::EType::Bool).b; StoreParam(outputsArchive, idx, v); break; }
						case EPort::Float: { float v = out[k].As(Api::EType::Float).f; StoreParam(outputsArchive, idx, v); break; }
						case EPort::String: { const char* v = out[k].s.c_str(); StoreParam(outputsArchive, idx, v); break; }
						default: { int32 v = out[k].As(Api::EType::Int).i; StoreParam(outputsArchive, idx, v); break; }
						}
					}
				}

			private:
				static constexpr uint32 kMaxPorts = 12;

				void AddParamOf(EPort type, CryVS::ParamFlags::EValue flags)
				{
					switch (type)
					{
					case EPort::Bool: AddParam<bool>(flags); break;
					case EPort::Int: AddParam<int32>(flags); break;
					case EPort::Float: AddParam<float>(flags); break;
					case EPort::Fit: AddParam<EVideoFit>(flags); break;
					case EPort::Output: AddParam<Api::EOutputMode>(flags); break;
					case EPort::Decode: AddParam<Api::EDecodeMode>(flags); break;
					case EPort::Entity: AddParam<CryVS::ExplicitEntityId>(flags); break;
					default: AddParam<const char*>(flags); break;
					}
				}

				const SGlobalFunction& m_desc;
			};

			// ---------------------------------------------------------------------------------------- properties
			struct SProperties
			{
				string video;
				Api::EOutputMode outputMode = Api::EOutputMode::Screen;
				string surfaceId;                  // Surface: the surface id UIs use
				string material;                   // Material: material name (empty = entity material)
				int32 slot = 0;
				EVideoFit fit = EVideoFit::Letterbox;
				string language;
				bool loop = false;
				bool autoplay = false;
				bool skippable = false;
				Api::EDecodeMode decode = Api::EDecodeMode::Auto;
				string watchSurfaceId;             // non-empty: own no player, only follow this surface

				void Serialize(Serialization::IArchive& archive)
				{
					archive(Serialization::ResourceFilePath(video, "Video files (*.webm;*.cevvideo)|*.webm;*.cevvideo", "Videos"), "video", "Video");
					archive(outputMode, "outputMode", "Output");
					archive(surfaceId, "surfaceId", "SurfaceID");
					archive(material, "material", "Material");
					archive(slot, "slot", "Slot");
					archive(fit, "fit", "Screen Fit");
					archive(language, "language", "Language");
					archive(loop, "loop", "Loop");
					archive(autoplay, "autoplay", "Autoplay");
					archive(skippable, "skippable", "Skippable");
					archive(decode, "decode", "Decode");
					archive(watchSurfaceId, "watchSurfaceId", "WatchSurfaceID");
				}
			};

			class CVideoComponent;
			std::vector<CVideoComponent*> s_watchers; // components in WatchSurfaceID mode (polled from Tick)

			// ---------------------------------------------------------------------------------------- component
			class CVideoComponent final : public CryVS::IComponent, public Api::IEventSink
			{
			public:
				// CryVS::IComponent
				virtual bool Init(const CryVS::SComponentParams& params) override
				{
					m_pObject = &params.object;
					m_entityId = params.object.GetEntityId().GetValue();
					if (const SProperties* pProps = params.properties.ToPtr<SProperties>())
						m_props = *pProps;
					return true;
				}

				virtual void Run(CryVS::ESimulationMode simulationMode) override
				{
					if (simulationMode == CryVS::ESimulationMode::Game)
						Start();
				}

				virtual void Shutdown() override { Destroy(); m_pObject = nullptr; }
				virtual CryVS::INetworkSpawnParamsPtr GetNetworkSpawnParams() const override { return CryVS::INetworkSpawnParamsPtr(); }
				// ~CryVS::IComponent

				~CVideoComponent() { Destroy(); }

				bool CallApi(const Api::SEntry& entry, const SValue* in, uint32_t inCount, SValue* pResult)
				{
					if (IsWatching())
					{
						// the watched surface belongs to someone else: queries and controls go to it, nothing is created
						if (entry.NeedsPlayer() && !HandleAlive(m_handle))
							return false;
						return Api::Call(entry, m_handle, in, inCount, pResult, EntityMaterial());
					}
					if (entry.NeedsPlayer() && !EnsurePlayer())
						return false;
					return Api::Call(entry, m_handle, in, inCount, pResult, EntityMaterial());
				}

				// Api::IEventSink
				virtual void OnApiEvent(int handle, uint32_t event, const SValue* args, uint32_t argCount) override
				{
					if (handle != m_handle || m_pObject == nullptr || event >= Api::eEvent_Count)
						return;
					if (IsWatching())
					{
						OnWatchedEvent(event, args, argCount);
						return;
					}
					const Api::SEntry& e = Api::Events()[event];
					CryVS::CInPlaceStack stack;
					for (uint32_t i = 0; i < e.paramCount && i < argCount; ++i)
						PushValue(stack, e.params[i].type, args[i]);
					m_pObject->ProcessSignal(e.guid, stack);
				}

				//! WatchSurfaceID mode, once per frame: attach when the surface appears, Closed when it went away, Resized
				//! when its texture changed.
				void Poll()
				{
					if (!IsWatching() || m_pObject == nullptr || !ApiAvailable())
						return;
					if (m_handle != 0 && !HandleAlive(m_handle))
					{
						// destroyed by its owner (the record is gone with it)
						m_handle = 0;
						m_ready = false;
						SendSignal(Api::kCryVSClosedSignalGuid);
					}
					if (m_handle == 0)
					{
						m_handle = mEnv->pApi->AttachSurface(m_props.watchSurfaceId.c_str());
						if (m_handle == 0)
							return; // not there yet
						mEnv->pApi->Subscribe(m_handle, this);
						if (Q("IsLoaded", m_handle).b)
						{
							// loaded before we followed it: its Ready event is gone
							m_ready = true;
							m_generation = Q("Generation", m_handle).i;
							SendSignal(Api::Events()[Api::eEvent_Ready].guid);
						}
						return;
					}
					if (m_ready && Q("Generation", m_handle).i != m_generation)
						SendResized();
				}

				void Detach() { Destroy(); m_pObject = nullptr; } // plug-in shutdown

			private:
				bool IsWatching() const { return !m_props.watchSurfaceId.empty(); }

				static void PushValue(CryVS::CInPlaceStack& stack, Api::EType type, const SValue& v)
				{
					switch (type)
					{
					case Api::EType::Bool: stack.Push(CryVS::CVariant(v.b)); break;
					case Api::EType::Int: stack.Push(CryVS::CVariant(static_cast<int32>(v.i))); break;
					case Api::EType::Float: stack.Push(CryVS::CVariant(v.f)); break;
					default: stack.Push(CryVS::CVariant(v.s.c_str())); break;
					}
				}

				void SendSignal(const ::CryGUID& guid)
				{
					CryVS::CInPlaceStack stack;
					m_pObject->ProcessSignal(guid, stack);
				}

				void SendResized()
				{
					m_generation = Q("Generation", m_handle).i;
					CryVS::CInPlaceStack stack;
					stack.Push(CryVS::CVariant(static_cast<int32>(Q("TextureId", m_handle).i)));
					stack.Push(CryVS::CVariant(static_cast<int32>(Q("Width", m_handle).i)));
					stack.Push(CryVS::CVariant(static_cast<int32>(Q("Height", m_handle).i)));
					m_pObject->ProcessSignal(Api::kCryVSResizedSignalGuid, stack);
				}

				void OnWatchedEvent(uint32_t event, const SValue* args, uint32_t argCount)
				{
					if (event == Api::eEvent_Ready)
					{
						// a reload of the surface (another video) sends Ready again: a changed texture is a Resized too
						const bool changed = m_ready && Q("Generation", m_handle).i != m_generation;
						m_ready = true;
						if (changed)
							SendResized();
						else
							m_generation = Q("Generation", m_handle).i;
						SendSignal(Api::Events()[Api::eEvent_Ready].guid);
					}
					else if (event == Api::eEvent_NewFrame)
					{
						CryVS::CInPlaceStack stack;
						PushValue(stack, Api::EType::String, argCount > 0 ? args[0] : S(""));
						m_pObject->ProcessSignal(Api::Events()[Api::eEvent_NewFrame].guid, stack);
					}
				}

				IMaterial* EntityMaterial() const
				{
					IEntity* const pEntity = gEnv->pEntitySystem != nullptr ? gEnv->pEntitySystem->GetEntity(m_entityId) : nullptr;
					return pEntity != nullptr ? pEntity->GetMaterial() : nullptr;
				}

				bool EnsurePlayer()
				{
					if (!ApiAvailable())
						return false;
					if (m_handle != 0 && mEnv->pApi->Find(m_handle, false) != nullptr)
						return true;
					m_handle = m_props.outputMode == Api::EOutputMode::Surface ? mEnv->pApi->CreateForSurface(m_props.surfaceId.c_str(), true) : mEnv->pApi->Create(true);
					if (m_handle == 0)
						return false;
					mEnv->pApi->Subscribe(m_handle, this);
					using namespace Api;
					const SValue loop[1] = { SValue::FromBool(m_props.loop) };
					const SValue language[1] = { SValue::FromString(m_props.language.c_str()) };
					const SValue skippable[1] = { SValue::FromBool(m_props.skippable) };
					const SValue decode[1] = { SValue::FromInt(static_cast<int>(m_props.decode)) };
					const SValue output[5] = { SValue::FromInt(static_cast<int>(m_props.outputMode)), SValue::FromString(m_props.material.c_str()),
						SValue::FromInt(m_props.slot), SValue::FromInt(-1), SValue::FromInt(static_cast<int>(m_props.fit)) };
					Call(*FindByName("SetDecode"), m_handle, decode, 1, nullptr);
					Call(*FindByName("SetLoop"), m_handle, loop, 1, nullptr);
					Call(*FindByName("SetLanguage"), m_handle, language, 1, nullptr);
					Call(*FindByName("SetSkippable"), m_handle, skippable, 1, nullptr);
					Call(*FindByName("SetOutput"), m_handle, output, 5, nullptr, EntityMaterial());
					return true;
				}

				void Start()
				{
					Destroy();
					if (IsWatching())
					{
						s_watchers.push_back(this);
						Poll();
						return;
					}
					if (!EnsurePlayer() || m_props.video.empty())
						return;
					const SValue path[1] = { SValue::FromString(m_props.video.c_str()) };
					if (Api::Call(*Api::FindByName("Load"), m_handle, path, 1, nullptr) && m_props.autoplay)
						Api::Call(*Api::FindByName("Play"), m_handle, nullptr, 0, nullptr);
				}

				void Destroy()
				{
					s_watchers.erase(std::remove(s_watchers.begin(), s_watchers.end(), this), s_watchers.end());
					if (m_handle != 0 && ApiAvailable())
					{
						mEnv->pApi->Unsubscribe(m_handle, this);
						// a watched surface belongs to someone else (AttachSurface may even return its owner's handle)
						if (!IsWatching() && mEnv->pApi->Find(m_handle, false) != nullptr)
							mEnv->pApi->Destroy(m_handle);
					}
					m_handle = 0;
					m_ready = false;
				}

				CryVS::IObject* m_pObject = nullptr;
				EntityId m_entityId = 0;
				SProperties m_props;
				int m_handle = 0;
				int m_generation = 0;
				bool m_ready = false;
			};

			// ---------------------------------------------------------------------------------------- member functions
			//! A component member function whose parameters come from a table entry; Call forwards to the component.
			class CTableFunction final : public CryVS::ComponentMemberFunctionUtils::CBase
			{
			public:
				CTableFunction(const Api::SEntry& entry, const ::CryGUID& componentGuid)
					: CBase(entry.guid, componentGuid, entry.name, __FILE__, "Code")
					, m_entry(entry)
					, m_hasReturn(entry.returnType != Api::EType::Void)
				{
					if (m_hasReturn)
						AddParamOf(entry.returnType, CryVS::ComponentMemberFunctionUtils::EParamFlags::IsReturn);
					for (uint32_t i = 0; i < entry.paramCount; ++i)
						AddParamOf(entry.params[i].type, CryVS::ComponentMemberFunctionUtils::EParamFlags::None);

					CryVS::IComponentMemberFunction& self = *this; // the typed BindInput overloads live on the interface
					self.SetAuthor("CEVPlayer");
					self.SetDescription(entry.description);
					if (m_hasReturn)
						self.BindOutput(0, entry.returnName != nullptr ? entry.returnName : "Result", entry.description);
					for (uint32_t i = 0; i < entry.paramCount; ++i)
					{
						const Api::SParam& p = entry.params[i];
						const uint32 idx = i + (m_hasReturn ? 1u : 0u);
						switch (p.type)
						{
						case Api::EType::Bool: self.BindInput(idx, p.name, p.description, p.defNum != 0.0); break;
						case Api::EType::Int: self.BindInput(idx, p.name, p.description, static_cast<int32>(p.defNum)); break;
						case Api::EType::Float: self.BindInput(idx, p.name, p.description, static_cast<float>(p.defNum)); break;
						default: self.BindInput(idx, p.name, p.description, p.defStr != nullptr ? p.defStr : ""); break;
						}
					}
				}

				virtual void Call(CryVS::IComponent& component, const CryVS::TVariantConstArray& inputs, const CryVS::TVariantArray& outputs) const override
				{
					CryVS::CVariantArrayInputArchive inputsArchive(inputs);
					CryVS::CVariantArrayOutputArchive outputsArchive(outputs);

					const uint32 base = m_hasReturn ? 1u : 0u;
					SValue in[Api::kMaxParams];
					for (uint32_t i = 0; i < m_entry.paramCount && i < Api::kMaxParams; ++i)
					{
						switch (m_entry.params[i].type)
						{
						case Api::EType::Bool: { bool v = false; LoadParam(inputsArchive, base + i, v); in[i] = SValue::FromBool(v); break; }
						case Api::EType::Int: { int32 v = 0; LoadParam(inputsArchive, base + i, v); in[i] = SValue::FromInt(v); break; }
						case Api::EType::Float: { float v = 0.f; LoadParam(inputsArchive, base + i, v); in[i] = SValue::FromFloat(v); break; }
						default: { const char* v = ""; LoadParam(inputsArchive, base + i, v); in[i] = SValue::FromString(v); break; }
						}
					}

					SValue result;
					static_cast<CVideoComponent&>(component).CallApi(m_entry, in, m_entry.paramCount, &result);

					if (m_hasReturn)
					{
						switch (m_entry.returnType)
						{
						case Api::EType::Bool: { bool v = result.b; StoreParam(outputsArchive, 0, v); break; }
						case Api::EType::Int: { int32 v = result.i; StoreParam(outputsArchive, 0, v); break; }
						case Api::EType::Float: { float v = result.f; StoreParam(outputsArchive, 0, v); break; }
						default: { const char* v = result.s.c_str(); StoreParam(outputsArchive, 0, v); break; }
						}
					}
				}

			private:
				void AddParamOf(Api::EType type, CryVS::ComponentMemberFunctionUtils::EParamFlags flags)
				{
					switch (type)
					{
					case Api::EType::Bool: AddParam<bool>(flags); break;
					case Api::EType::Int: AddParam<int32>(flags); break;
					case Api::EType::Float: AddParam<float>(flags); break;
					default: AddParam<const char*>(flags); break;
					}
				}

				const Api::SEntry& m_entry;
				bool m_hasReturn;
			};

			// ---------------------------------------------------------------------------------------- registration
			TemplateUtils::CConnectionScope s_connectionScope;
			bool s_connected = false;
			bool s_shutdown = false;
			bool s_commandRegistered = false;
			uint32 s_lastAdded = 0;   // totals of the last RegisterAll (self test)
			uint32 s_lastPresent = 0;
			constexpr uint32 kElementCount = 3 + Api::eCryVS_FunctionCount + 1 + 36 + Api::eEvent_Count + 2; // types, globals, component, members, signals

			struct SCounts
			{
				uint32 added = 0;
				uint32 present = 0;
			};

			void AddSignalInput(CryVS::ISignal& signal, Api::EType type, const char* name, const char* description)
			{
				switch (type)
				{
				case Api::EType::Bool: signal.AddInput(name, description, false); break;
				case Api::EType::Int: signal.AddInput(name, description, int32(0)); break;
				case Api::EType::Float: signal.AddInput(name, description, 0.f); break;
				default: signal.AddInput(name, description, ""); break;
				}
			}

			CryVS::ISignalPtr NewSignal(CryVS::IEnvRegistry& registry, const ::CryGUID& guid, const char* name, const char* description, SCounts& counts)
			{
				if (registry.GetSignal(guid))
				{
					++counts.present;
					return CryVS::ISignalPtr();
				}
				CryVS::ISignalPtr pSignal = registry.CreateSignal(guid);
				if (!pSignal)
				{
					LogWarning("CryVS: signal <%s> could not be created", name);
					return pSignal;
				}
				pSignal->SetSenderGUID(Api::kCryVSComponentGuid);
				pSignal->SetName(name);
				pSignal->SetFileName(__FILE__, "Code");
				pSignal->SetAuthor("CEVPlayer");
				pSignal->SetDescription(description);
				++counts.added;
				return pSignal;
			}

			void RegisterAll()
			{
				if (s_shutdown || gEnv == nullptr || gEnv->pCryVS == nullptr)
					return;
				CryVS::IEnvRegistry& registry = gEnv->pCryVS->GetEnvRegistry();
				SCounts types, globals, component, members, signals;

				// enumeration types
				auto addType = [&](const ::CryGUID& guid, CryVS::IEnvTypeDescPtr pDesc)
				{
					if (registry.GetTypeDesc(guid) != nullptr)
						++types.present;
					else if (registry.RegisterTypeDesc(pDesc))
						++types.added;
				};
				addType(Api::kOutputModeTypeGuid, CryVS::MakeEnvTypeDescShared(Api::kOutputModeTypeGuid, "VideoOutput", "Where a video goes: Surface, Screen or Material", Api::EOutputMode::Screen, CryVS::EEnvTypeFlags::None));
				addType(Api::kFitTypeGuid, CryVS::MakeEnvTypeDescShared(Api::kFitTypeGuid, "VideoFit", "How a fullscreen video is placed: Fill, Fit or Letterbox", EVideoFit::Letterbox, CryVS::EEnvTypeFlags::None));
				addType(Api::kDecodeModeTypeGuid, CryVS::MakeEnvTypeDescShared(Api::kDecodeModeTypeGuid, "VideoDecode", "Decoder choice: Auto, GPU or CPU", Api::EDecodeMode::Auto, CryVS::EEnvTypeFlags::None));

				// global functions (three drawers)
				for (const SGlobalFunction& desc : kGlobalFunctions)
				{
					if (registry.GetGlobalFunction(Api::kCryVSFunctionGuids[desc.id]))
					{
						++globals.present;
						continue;
					}
					if (registry.RegisterGlobalFunction(CryVS::IGlobalFunctionPtr(std::make_shared<CApiGlobalFunction>(desc))))
						++globals.added;
					else
						LogWarning("CryVS: global function %s::%s was refused by the registry", desc.ns, desc.name);
				}

				// component
				const ::CryGUID componentGuid = Api::kCryVSComponentGuid;
				if (registry.GetComponentFactory(componentGuid))
					++component.present;
				else
				{
					CryVS::IComponentFactoryPtr pFactory = CRYVS_MAKE_COMPONENT_FACTORY_SHARED(CVideoComponent, SProperties, componentGuid);
					pFactory->SetName("VideoPlayer");
					pFactory->SetNamespace("CEVPlayer");
					pFactory->SetAuthor("CEVPlayer");
					pFactory->SetDescription("Plays a .webm video or .cevvideo descriptor into a surface for UIs, to the screen or to a material slot; with WatchSurfaceID it only follows a surface (Ready, NewFrame, Resized, Closed)");
					if (registry.RegisterComponentFactory(pFactory))
						++component.added;
				}

				// member functions (one per table entry; Create / Destroy: the component owns its player)
				for (const Api::SSpan span : { Api::Controls(), Api::Queries() })
				{
					for (const Api::SEntry& e : span)
					{
						if ((e.flags & Api::eEntry_Standalone) != 0)
							continue;
						if (registry.GetComponentMemberFunction(e.guid))
						{
							++members.present;
							continue;
						}
						if (registry.RegisterComponentMemberFunction(CryVS::IComponentMemberFunctionPtr(std::make_shared<CTableFunction>(e, componentGuid))))
							++members.added;
					}
				}

				// signals: one per table event, plus Resized / Closed of the WatchSurfaceID mode
				for (const Api::SEntry& e : Api::Events())
				{
					if (CryVS::ISignalPtr pSignal = NewSignal(registry, e.guid, e.name, e.description, signals))
					{
						for (uint32_t i = 0; i < e.paramCount; ++i)
							AddSignalInput(*pSignal, e.params[i].type, e.params[i].name, e.params[i].description);
					}
				}
				if (CryVS::ISignalPtr pSignal = NewSignal(registry, Api::kCryVSResizedSignalGuid, "Resized", "WatchSurfaceID: the texture of the surface changed (size, texture id or layout)", signals))
				{
					AddSignalInput(*pSignal, Api::EType::Int, "TextureId", "Renderer texture id of the surface");
					AddSignalInput(*pSignal, Api::EType::Int, "Width", "Picture width in pixels");
					AddSignalInput(*pSignal, Api::EType::Int, "Height", "Picture height in pixels");
				}
				NewSignal(registry, Api::kCryVSClosedSignalGuid, "Closed", "WatchSurfaceID: the surface was destroyed; the component waits for it to appear again", signals);

				LogAlways("CryVS: registration: types %u new / %u present, global functions %u / %u, component %u / %u, member functions %u / %u, signals %u / %u",
					types.added, types.present, globals.added, globals.present, component.added, component.present, members.added, members.present, signals.added, signals.present);
				s_lastAdded = types.added + globals.added + component.added + members.added + signals.added;
				s_lastPresent = types.present + globals.present + component.present + members.present + signals.present;
			}

			void OnEnvRefresh()
			{
				LogAlways("CryVS: envRefresh received, registering again");
				RegisterAll();
			}

			// ---------------------------------------------------------------------------------------- cev_cryvstest
			namespace SelfTest
			{
				enum class EStep { Idle, WaitSurface, WaitOpen };

				EStep s_step = EStep::Idle;
				float s_waitUntil = 0.f;
				int s_checks = 0;
				int s_failed = 0;
				int s_surfaceHandle = 0;
				int s_openHandle = 0;
				const char* const kSurface = "cryvstest_surface";
				const char* const kOpenSurface = "cryvstest_open";
				string s_video;

				float Now() { return gEnv->pTimer != nullptr ? gEnv->pTimer->GetAsyncCurTime() : 0.f; }

				void Check(bool ok, const char* fmt, ...)
				{
					char text[512];
					va_list args;
					va_start(args, fmt);
					cry_vsprintf(text, fmt, args);
					va_end(args);
					++s_checks;
					if (!ok)
						++s_failed;
					LogAlways("cev_cryvstest: %s %s", ok ? "ok  " : "FAIL", text);
				}

				CryVS::CAggregateTypeId TypeIdOf(EPort type)
				{
					switch (type)
					{
					case EPort::Bool: return CryVS::GetTypeInfo<bool>().GetTypeId();
					case EPort::Int: return CryVS::GetTypeInfo<int32>().GetTypeId();
					case EPort::Float: return CryVS::GetTypeInfo<float>().GetTypeId();
					case EPort::Fit: return CryVS::GetTypeInfo<EVideoFit>().GetTypeId();
					case EPort::Output: return CryVS::GetTypeInfo<Api::EOutputMode>().GetTypeId();
					case EPort::Decode: return CryVS::GetTypeInfo<Api::EDecodeMode>().GetTypeId();
					case EPort::Entity: return CryVS::GetTypeInfo<CryVS::ExplicitEntityId>().GetTypeId();
					default: return CryVS::GetTypeInfo<CryVS::CPoolString>().GetTypeId();
					}
				}

				CryVS::CAggregateTypeId TypeIdOf(Api::EType type)
				{
					switch (type)
					{
					case Api::EType::Bool: return CryVS::GetTypeInfo<bool>().GetTypeId();
					case Api::EType::Int: return CryVS::GetTypeInfo<int32>().GetTypeId();
					case Api::EType::Float: return CryVS::GetTypeInfo<float>().GetTypeId();
					default: return CryVS::GetTypeInfo<CryVS::CPoolString>().GetTypeId();
					}
				}

				bool SameType(const CryVS::IAnyConstPtr& pValue, const CryVS::CAggregateTypeId& expected)
				{
					return pValue && pValue->GetTypeInfo().GetTypeId() == expected;
				}

				//! Calls a registered global function through IGlobalFunction::Call with its default inputs, `set` changing
				//! some of them (index = input index; every input of these functions is one variant), and returns the outputs.
				CryVS::TVariantVector CallGlobal(Api::ECryVSFunction id, std::initializer_list<std::pair<uint32, CryVS::CVariant>> set)
				{
					CryVS::TVariantVector outputs;
					CryVS::IGlobalFunctionConstPtr pFunction = gEnv->pCryVS->GetEnvRegistry().GetGlobalFunction(Api::kCryVSFunctionGuids[id]);
					if (!pFunction)
						return outputs;
					const CryVS::TVariantConstArray defaults = pFunction->GetVariantInputs();
					CryVS::TVariantVector inputs(defaults.begin(), defaults.begin() + defaults.size());
					for (const auto& s : set)
					{
						if (s.first < inputs.size())
							inputs[s.first] = s.second;
					}
					const CryVS::TVariantConstArray defaultOutputs = pFunction->GetVariantOutputs();
					outputs.assign(defaultOutputs.begin(), defaultOutputs.begin() + defaultOutputs.size());
					pFunction->Call(CryVS::TVariantConstArray(inputs), CryVS::TVariantArray(outputs));
					return outputs;
				}

				void Finish()
				{
					if (s_surfaceHandle != 0)
						DestroyIfAlive(s_surfaceHandle);
					if (s_openHandle != 0)
						DestroyIfAlive(s_openHandle);
					s_surfaceHandle = s_openHandle = 0;
					s_step = EStep::Idle;
					LogAlways("cev_cryvstest: %s (%d checks, %d failed)", s_failed == 0 ? "PASS" : "FAIL", s_checks, s_failed);
				}

				void CheckRegistry()
				{
					CryVS::IEnvRegistry& registry = gEnv->pCryVS->GetEnvRegistry();

					// global functions: name, namespace, inputs and outputs (count, name, type)
					for (const SGlobalFunction& desc : kGlobalFunctions)
					{
						CryVS::IGlobalFunctionConstPtr pFunction = registry.GetGlobalFunction(Api::kCryVSFunctionGuids[desc.id]);
						Check(pFunction != nullptr, "global function %s::%s registered (GUID %s)", desc.ns, desc.name, Api::kCryVSFunctionGuids[desc.id].ToDebugString());
						if (!pFunction)
							continue;
						bool ok = strcmp(pFunction->GetName(), desc.name) == 0 && strcmp(pFunction->GetNamespace(), desc.ns) == 0
							&& pFunction->GetInputCount() == desc.inputCount && pFunction->GetOutputCount() == desc.outputCount
							&& pFunction->GetVariantInputs().size() == desc.inputCount && pFunction->GetVariantOutputs().size() == desc.outputCount;
						string ports;
						for (uint32 i = 0; ok && i < desc.inputCount; ++i)
						{
							ok = strcmp(pFunction->GetInputName(i), desc.inputs[i].name) == 0 && SameType(pFunction->GetInputValue(i), TypeIdOf(desc.inputs[i].type));
							ports += string(i == 0 ? "" : ", ") + pFunction->GetInputName(i);
						}
						ports += " -> ";
						for (uint32 i = 0; ok && i < desc.outputCount; ++i)
						{
							ok = strcmp(pFunction->GetOutputName(i), desc.outputs[i].name) == 0 && SameType(pFunction->GetOutputValue(i), TypeIdOf(desc.outputs[i].type));
							ports += string(i == 0 ? "" : ", ") + pFunction->GetOutputName(i);
						}
						Check(ok, "Function::%s::%s (%s) - %u in / %u out, names and types match", pFunction->GetNamespace(), pFunction->GetName(), ports.c_str(),
							static_cast<uint32>(pFunction->GetInputCount()), static_cast<uint32>(pFunction->GetOutputCount()));
					}

					// component and its member functions
					CryVS::IComponentFactoryConstPtr pFactory = registry.GetComponentFactory(Api::kCryVSComponentGuid);
					Check(pFactory != nullptr, "component %s::%s registered", pFactory ? pFactory->GetNamespace() : "?", pFactory ? pFactory->GetName() : "?");
					uint32 memberCount = 0;
					for (const Api::SSpan span : { Api::Controls(), Api::Queries() })
					{
						for (const Api::SEntry& e : span)
						{
							if ((e.flags & Api::eEntry_Standalone) != 0)
								continue;
							++memberCount;
							CryVS::IComponentMemberFunctionConstPtr pFunction = registry.GetComponentMemberFunction(e.guid);
							const uint32 outs = e.returnType != Api::EType::Void ? 1u : 0u;
							bool ok = pFunction && strcmp(pFunction->GetName(), e.name) == 0 && pFunction->GetComponentGUID() == Api::kCryVSComponentGuid
								&& pFunction->GetInputCount() == e.paramCount && pFunction->GetOutputCount() == outs;
							for (uint32 i = 0; ok && i < e.paramCount; ++i)
								ok = SameType(pFunction->GetInputValue(i), TypeIdOf(e.params[i].type));
							if (ok && outs != 0)
								ok = SameType(pFunction->GetOutputValue(0), TypeIdOf(e.returnType));
							Check(ok, "member function CEVPlayer::VideoPlayer::%s - %u in / %u out, types match", e.name, e.paramCount, outs);
						}
					}
					Check(memberCount == 36, "%u member functions (36 expected)", memberCount);

					// signals of the component
					uint32 signalCount = 0;
					auto visitSignal = [&signalCount](const CryVS::ISignalConstPtr& pSignal)
					{
						if (pSignal->GetSenderGUID() == Api::kCryVSComponentGuid)
						{
							string inputs;
							for (uint32 i = 0; i < pSignal->GetInputCount(); ++i)
								inputs += string(i == 0 ? "" : ", ") + pSignal->GetInputName(i);
							LogAlways("cev_cryvstest: signal CEVPlayer::VideoPlayer::%s (%s) GUID %s", pSignal->GetName(), inputs.c_str(), pSignal->GetGUID().ToDebugString());
							++signalCount;
						}
						return CryVS::EVisitStatus::Continue;
					};
					registry.VisitSignals(CryVS::EnvSignalVisitor::FromLambdaFunction(visitSignal));
					Check(signalCount == Api::eEvent_Count + 2, "%u component signals (%u expected: 8 events + Resized + Closed)", signalCount, static_cast<uint32>(Api::eEvent_Count + 2));

					// enumeration types
					Check(registry.GetTypeDesc(Api::kOutputModeTypeGuid) != nullptr && registry.GetTypeDesc(Api::kFitTypeGuid) != nullptr
						&& registry.GetTypeDesc(Api::kDecodeModeTypeGuid) != nullptr, "types VideoOutput, VideoFit, VideoDecode registered");
				}

				void Start(const char* video)
				{
					if (gEnv->pCryVS == nullptr || !ApiAvailable())
					{
						LogWarning("cev_cryvstest: CryVS or the player API is not available");
						return;
					}
					if (s_step != EStep::Idle)
						Finish();
					s_checks = s_failed = 0;
					s_video = video != nullptr && video[0] != 0 ? video : "Videos/vp9_alpha.webm";
					LogAlways("cev_cryvstest: start (%s)", s_video.c_str());

					// per element duplicate guard. (IFramework::RefreshEnv cannot be exercised here: in this fork
					// CEnvRegistry::Clear keeps the function descriptors and BaseEnv's re-registration then fails
					// CRYVS_SYSTEM_ASSERT_FATAL(!GetFunction(guid)) - an engine problem, not ours.)
					RegisterAll();
					Check(s_lastAdded == 0 && s_lastPresent == kElementCount, "a second registration adds nothing (%u new, %u of %u present)", s_lastAdded, s_lastPresent, kElementCount);

					CheckRegistry();

					// CEVPlayer::UI::PlayToSurface (Loop, Autoplay)
					CryVS::TVariantVector out = CallGlobal(Api::eCryVS_VideoUI_PlayToSurface, { { 0, CryVS::CVariant(s_video.c_str()) }, { 1, CryVS::CVariant(kSurface) }, { 2, CryVS::CVariant(true) }, { 3, CryVS::CVariant(true) } });
					s_surfaceHandle = out.size() == 1 ? out[0].AsInt32() : 0;
					Check(s_surfaceHandle != 0 && ApiAvailable() && mEnv->pApi->IsScriptOwned(s_surfaceHandle), "CEVPlayer::UI::PlayToSurface returned a script owned handle (%d)", s_surfaceHandle);
					out = CallGlobal(Api::eCryVS_VideoUI_PlayToSurface, { { 0, CryVS::CVariant(s_video.c_str()) }, { 1, CryVS::CVariant(kSurface) } });
					Check(out.size() == 1 && out[0].AsInt32() == 0, "CEVPlayer::UI::PlayToSurface with a SurfaceID in use returns 0");
					s_step = EStep::WaitSurface;
					s_waitUntil = Now() + 2.f;
				}

				void CheckSurfaceInfo()
				{
					const CryVS::TVariantVector out = CallGlobal(Api::eCryVS_VideoUI_SurfaceInfo, { { 0, CryVS::CVariant(kSurface) } });
					const int h = s_surfaceHandle;
					const bool sizes = out.size() == 9;
					Check(sizes, "CEVPlayer::UI::SurfaceInfo has 9 outputs (%u)", static_cast<uint32>(out.size()));
					if (!sizes)
						return;
					LogAlways("cev_cryvstest: SurfaceInfo Exists=%d TextureId=%d Width=%d Height=%d PixelLayout=%d Premultiplied=%d HasAlpha=%d FrameSerial=%s State=%d",
						out[0].AsBool(), out[1].AsInt32(), out[2].AsInt32(), out[3].AsInt32(), out[4].AsInt32(), out[5].AsBool(), out[6].AsBool(), out[7].c_str(), out[8].AsInt32());
					Check(out[0].AsBool(), "SurfaceInfo Exists");
					Check(out[1].AsInt32() == Q("TextureId", h).i && out[1].AsInt32() != 0, "SurfaceInfo TextureId = table TextureId (%d)", Q("TextureId", h).i);
					Check(out[2].AsInt32() == Q("Width", h).i && out[3].AsInt32() == Q("Height", h).i && out[2].AsInt32() > 0, "SurfaceInfo Width x Height = table (%dx%d)", Q("Width", h).i, Q("Height", h).i);
					Check(out[4].AsInt32() == Q("PixelLayout", h).i && out[5].AsBool() == Q("IsPremultiplied", h).b && out[6].AsBool() == Q("HasAlpha", h).b, "SurfaceInfo PixelLayout / Premultiplied / HasAlpha = table");
					Check(atoi(out[7].c_str()) > 0 && atoi(out[7].c_str()) <= atoi(Q("FrameSerial", h).s.c_str()), "SurfaceInfo FrameSerial is decimal text <= table FrameSerial (%s <= %s)", out[7].c_str(), Q("FrameSerial", h).s.c_str());
					Check(out[8].AsInt32() == Q("State", h).i && out[8].AsInt32() == static_cast<int>(EVideoPlayerState::Playing), "SurfaceInfo State = table State = Playing (%d)", out[8].AsInt32());

					CallGlobal(Api::eCryVS_VideoUI_StopSurface, { { 0, CryVS::CVariant(kSurface) } });
					Check(!HandleAlive(h), "CEVPlayer::UI::StopSurface destroyed the surface player");
					const CryVS::TVariantVector after = CallGlobal(Api::eCryVS_VideoUI_SurfaceInfo, { { 0, CryVS::CVariant(kSurface) } });
					Check(after.size() == 9 && !after[0].AsBool() && after[1].AsInt32() == 0, "SurfaceInfo after StopSurface: Exists false, TextureId 0");
					s_surfaceHandle = 0;

					// CEVPlayer::Advanced::Open (Output Surface) / Play
					const CryVS::TVariantVector open = CallGlobal(Api::eCryVS_VideoAdvanced_Open, { { 0, CryVS::CVariant(s_video.c_str()) }, { 1, CryVS::CVariant(static_cast<int32>(Api::EOutputMode::Surface)) }, { 2, CryVS::CVariant(kOpenSurface) } });
					s_openHandle = open.size() == 1 ? open[0].AsInt32() : 0;
					Check(s_openHandle != 0 && mEnv->pApi->IsScriptOwned(s_openHandle), "CEVPlayer::Advanced::Open (Output Surface) returned a script owned handle (%d)", s_openHandle);
					CallGlobal(Api::eCryVS_VideoAdvanced_SetLoop, { { 0, CryVS::CVariant(static_cast<int32>(s_openHandle)) }, { 1, CryVS::CVariant(true) } });
					CallGlobal(Api::eCryVS_VideoAdvanced_SetVolume, { { 0, CryVS::CVariant(static_cast<int32>(s_openHandle)) }, { 1, CryVS::CVariant(0.f) } });
					CallGlobal(Api::eCryVS_VideoAdvanced_Play, { { 0, CryVS::CVariant(static_cast<int32>(s_openHandle)) } });
					Check(Q("IsLooping", s_openHandle).b && Q("Volume", s_openHandle).f == 0.f, "CEVPlayer::Advanced::SetLoop / SetVolume applied");
					s_step = EStep::WaitOpen;
					s_waitUntil = Now() + 1.5f;
				}

				void CheckInfo()
				{
					const int h = s_openHandle;
					const CryVS::TVariantVector out = CallGlobal(Api::eCryVS_VideoAdvanced_Info, { { 0, CryVS::CVariant(static_cast<int32>(h)) } });
					const bool sizes = out.size() == 11;
					Check(sizes, "CEVPlayer::Advanced::Info has 11 outputs (%u)", static_cast<uint32>(out.size()));
					if (sizes)
					{
						LogAlways("cev_cryvstest: Info State=%d Time=%.3f Duration=%.3f Width=%d Height=%d HasAlpha=%d Language='%s' Variant=%d DecodeBackend=%d FrameSerial=%s SurfaceId=%s",
							out[0].AsInt32(), out[1].AsFloat(), out[2].AsFloat(), out[3].AsInt32(), out[4].AsInt32(), out[5].AsBool(), out[6].c_str(), out[7].AsInt32(), out[8].AsInt32(), out[9].c_str(), out[10].c_str());
						Check(out[0].AsInt32() == static_cast<int>(EVideoPlayerState::Playing) && out[0].AsInt32() == Q("State", h).i, "Info State = Playing = table");
						Check(out[1].AsFloat() > 0.5f && out[1].AsFloat() <= Q("Time", h).f, "Info Time advanced (%.3f s)", out[1].AsFloat());
						Check(out[2].AsFloat() == Q("Duration", h).f && out[3].AsInt32() == Q("Width", h).i && out[4].AsInt32() == Q("Height", h).i && out[3].AsInt32() > 0, "Info Duration / Width / Height = table");
						Check(out[5].AsBool() == Q("HasAlpha", h).b && strcmp(out[6].c_str(), Q("Language", h).s.c_str()) == 0 && out[7].AsInt32() == Q("Variant", h).i && out[8].AsInt32() == Q("DecodeBackend", h).i,
							"Info HasAlpha / Language / Variant / DecodeBackend = table");
						Check(atoi(out[9].c_str()) > 0 && stricmp(out[10].c_str(), kOpenSurface) == 0, "Info FrameSerial > 0 and SurfaceId = %s (%s)", kOpenSurface, out[10].c_str());
					}
					CallGlobal(Api::eCryVS_VideoAdvanced_Pause, { { 0, CryVS::CVariant(static_cast<int32>(h)) } });
					Check(Q("State", h).i == static_cast<int>(EVideoPlayerState::Paused), "CEVPlayer::Advanced::Pause");
					CallGlobal(Api::eCryVS_VideoAdvanced_Close, { { 0, CryVS::CVariant(static_cast<int32>(h)) } });
					Check(!HandleAlive(h), "CEVPlayer::Advanced::Close destroyed the player");
					const CryVS::TVariantVector closed = CallGlobal(Api::eCryVS_VideoAdvanced_Info, { { 0, CryVS::CVariant(static_cast<int32>(h)) } });
					Check(closed.size() == 11 && closed[0].AsInt32() == 0 && closed[3].AsInt32() == 0, "Info on the closed handle gives zeros");
					s_openHandle = 0;
					Finish();
				}

				void Tick()
				{
					if (s_step == EStep::Idle || Now() < s_waitUntil)
						return;
					if (gEnv->pCryVS == nullptr || !ApiAvailable())
					{
						Check(false, "CryVS or the player API went away");
						Finish();
						return;
					}
					if (s_step == EStep::WaitSurface)
						CheckSurfaceInfo();
					else if (s_step == EStep::WaitOpen)
						CheckInfo();
				}

				void Stop()
				{
					s_step = EStep::Idle;
					s_surfaceHandle = s_openHandle = 0; // the API is shutting down: its players go with it
				}

				void Command(IConsoleCmdArgs* args)
				{
					Start(args != nullptr && args->GetArgCount() > 1 ? args->GetArg(1) : nullptr);
				}
			}
		}

		void Initialize()
		{
			if (gEnv == nullptr || gEnv->pCryVS == nullptr)
			{
				LogDebug("CryVS: gEnv->pCryVS is null, CryVS bindings skipped");
				return;
			}
			s_shutdown = false;
			RegisterAll();
			if (!s_connected)
			{
				gEnv->pCryVS->Signals().envRefresh.Connect(CryVS::EnvRefreshSignal::Delegate::FromGlobalFunction<&OnEnvRefresh>(), s_connectionScope);
				s_connected = true;
			}
			if (!s_commandRegistered && gEnv->pConsole != nullptr)
			{
				REGISTER_COMMAND("cev_cryvstest", SelfTest::Command, VF_NULL, "Self test of the CryVS bindings: checks every registered global function / member function / signal by GUID, "
					"calls CEVPlayer::UI::PlayToSurface, SurfaceInfo, StopSurface and CEVPlayer::Advanced::Open, Info, Close. cev_cryvstest [path], logs PASS / FAIL");
				s_commandRegistered = true;
			}
		}

		void Tick()
		{
			SelfTest::Tick();
			if (s_watchers.empty())
				return;
			const std::vector<CVideoComponent*> watchers = s_watchers; // a signal handler may stop a component
			for (CVideoComponent* pComponent : watchers)
			{
				if (std::find(s_watchers.begin(), s_watchers.end(), pComponent) != s_watchers.end())
					pComponent->Poll();
			}
		}

		void Shutdown()
		{
			s_shutdown = true;
			SelfTest::Stop();
			const std::vector<CVideoComponent*> watchers = s_watchers;
			for (CVideoComponent* pComponent : watchers)
				pComponent->Detach();
			s_watchers.clear();
			if (s_commandRegistered && gEnv != nullptr && gEnv->pConsole != nullptr)
			{
				gEnv->pConsole->RemoveCommand("cev_cryvstest");
				s_commandRegistered = false;
			}
			if (s_connected)
			{
				s_connectionScope.Release();
				s_connected = false;
			}
		}
	}
}
