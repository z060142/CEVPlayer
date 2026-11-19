// The API description table: the single source of truth of everything the script bindings expose.
//
// Every control, query and event is described ONCE (fixed GUID, name, description, parameters, return type, async
// flag and - for controls and queries - a plain C++ function that implements it on top of IVideoPlayer). The
// Schematyc component functions / signals, the CryVS elements and the console self test are generated from this
// table (one function per entry). The FlowGraph nodes (Bindings/FlowApiNodes.cpp) are ten hand described composite
// nodes that only call table functions (docs/design/M6-ui-source.md section 4.2); none of them contains any playback
// logic of its own.
//
// Players are addressed by an integer handle (see CApiPlayers). Handle 0 is never valid. A call with a handle that
// is not valid (never created, destroyed, plug-in shut down) is a safe no-op that logs a warning and returns false.
//
// The GUIDs in ApiTable.cpp are fixed literals: they are stored in saved flow graphs / Schematyc scripts / CryVS
// scripts and MUST NEVER CHANGE after the first release. Adding an entry means generating a new GUID for it.
//
// RETIRED GUIDs (entries removed in M6b; never reuse them for anything):
//   6215D8DE-93E8-4236-9DB5-9DDCFD4F1C0D  query TextureName
//   2819FAC7-8B52-4BA3-9FA7-25BE56E317AD  query InterfaceVersion   (console: cev_info)
//   23441E42-DE20-4EA3-A312-F6D624D5D83C  query Caps               (console: cev_info)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <CryExtension/CryGUID.h>

struct IMaterial;

namespace CEVPlayer
{
	struct IVideoPlayer;

	namespace Api
	{
		//! Parameter / return types. Everything the scripting systems can carry without custom types.
		enum class EType : uint8_t { Void, Bool, Int, Float, String };

		constexpr uint32_t kMaxParams = 6;

		//! One value of any EType (arguments and results of the table functions).
		struct SValue
		{
			EType type = EType::Void;
			bool b = false;
			int i = 0;
			float f = 0.f;
			std::string s;

			static SValue FromBool(bool v) { SValue r; r.type = EType::Bool; r.b = v; r.i = v ? 1 : 0; r.f = v ? 1.f : 0.f; return r; }
			static SValue FromInt(int v) { SValue r; r.type = EType::Int; r.b = v != 0; r.i = v; r.f = static_cast<float>(v); return r; }
			static SValue FromFloat(float v) { SValue r; r.type = EType::Float; r.b = v != 0.f; r.i = static_cast<int>(v); r.f = v; return r; }
			static SValue FromString(const char* v) { SValue r; r.type = EType::String; r.s = v != nullptr ? v : ""; return r; }

			//! Converts to `to` (numbers convert among themselves, a string only stays a string).
			SValue As(EType to) const;
			//! "name=value" style text for logs.
			std::string ToText() const;
		};

		struct SParam
		{
			EType type;
			const char* name;
			const char* description;
			double defNum;       //!< default of Bool / Int / Float parameters
			const char* defStr;  //!< default of String parameters
			//! Fixed id of the parameter (a four character code, unique within its entry, never 0). Schematyc stores the
			//! ports of a function by this id in saved scripts: it MUST NEVER CHANGE once released and is NOT derived from
			//! the position, so a parameter can be inserted or reordered later without breaking saved scripts. A new
			//! parameter takes an unused code. (The values of the first parameters are the former positional ids
			//! 'in00', 'in01', ... so scripts saved before the ids existed keep working.)
			uint32_t id;
		};

		enum class EKind : uint8_t { Control, Query, Event };

		//! Entry flags.
		enum EEntryFlags : uint32_t
		{
			eEntry_NoPlayer  = 1u << 0, //!< does not use a handle (Create, CreateForSurface)
			eEntry_Standalone = 1u << 1, //!< only for handle based users (FlowGraph, console): the Schematyc / CryVS component owns its player itself
		};

		//! Everything a table function gets. `args` always holds exactly paramCount values of the declared types
		//! (converted, defaults filled in); `pPlayer` is non-null unless the entry is eEntry_NoPlayer.
		struct SCall
		{
			int handle = 0;
			IVideoPlayer* pPlayer = nullptr;
			const SValue* args = nullptr;
			uint32_t argCount = 0;
			SValue* pResult = nullptr;        //!< set for queries (and Create), already of the declared return type
			IMaterial* pDefaultMaterial = nullptr; //!< SetOutput(Material) with an empty name uses this (the entity's material)
		};

		//! Plain function implementing a control / query. Returns false when the request was rejected.
		typedef bool (*Function)(const SCall& call);

		struct SEntry
		{
			EKind kind;
			CryGUID guid;                //!< fixed, never changes
			const char* name;            //!< unique within the table
			const char* description;
			const SParam* params;
			uint32_t paramCount;
			EType returnType;            //!< queries / Create: the result; events: Void
			const char* returnName;      //!< name of the result port ("Value", "Handle", ...)
			bool async;                  //!< completion is reported later through an event (Load: Ready / Error)
			uint32_t flags;
			Function fn;                 //!< null for events (their parameters are the payload)

			bool NeedsPlayer() const { return (flags & eEntry_NoPlayer) == 0; }
		};

		//! A contiguous range of the table (range-for compatible).
		struct SSpan
		{
			const SEntry* pBegin;
			const SEntry* pEnd;
			const SEntry* begin() const { return pBegin; }
			const SEntry* end() const { return pEnd; }
			size_t size() const { return static_cast<size_t>(pEnd - pBegin); }
			const SEntry& operator[](size_t i) const { return pBegin[i]; }
		};

		SSpan Controls();
		SSpan Queries();
		SSpan Events();

		//! Indices into Events() (checked by the self check).
		enum EEvent : uint32_t
		{
			eEvent_Ready = 0,
			eEvent_Started,
			eEvent_NewFrame,
			eEvent_Marker,
			eEvent_Looped,
			eEvent_Ended,
			eEvent_Skipped,
			eEvent_Error,
			eEvent_Count
		};

		//! SetOutput modes (parameter "Mode").
		enum class EOutputMode : int
		{
			Surface = 0,  //!< only the surface texture, nothing is drawn by the plug-in; only for players made by CreateForSurface
			Screen,       //!< fullscreen on the top layer, `Fit` decides how it is placed
			Material,     //!< bind the texture to a texture slot of a material
		};

		//! SetDecode modes (parameter "Mode"). Only recorded so far: the backend is always the CPU decoder.
		enum class EDecodeMode : int
		{
			Auto = 0,
			Gpu,
			Cpu,
		};

		//! Fixed GUIDs of the Schematyc / CryVS elements that are not table entries.
		extern const CryGUID kSchematycComponentGuid;
		extern const CryGUID kCryVSComponentGuid;
		extern const CryGUID kOutputModeTypeGuid;
		extern const CryGUID kVideoFileNameTypeGuid;
		extern const CryGUID kFitTypeGuid;
		extern const CryGUID kDecodeModeTypeGuid;

		//! CryVS global functions (three namespaces = three drawers, Bindings/CryVSEnv.cpp). Fixed GUIDs, like the table's.
		enum ECryVSFunction : uint32_t
		{
			eCryVS_Video_PlayOnScreen = 0,
			eCryVS_Video_PlayOnMaterial,
			eCryVS_Video_Stop,
			eCryVS_VideoUI_PlayToSurface,
			eCryVS_VideoUI_StopSurface,
			eCryVS_VideoUI_SurfaceInfo,
			eCryVS_VideoAdvanced_Open,
			eCryVS_VideoAdvanced_Close,
			eCryVS_VideoAdvanced_Play,
			eCryVS_VideoAdvanced_Pause,
			eCryVS_VideoAdvanced_Stop,
			eCryVS_VideoAdvanced_Seek,
			eCryVS_VideoAdvanced_SetLoop,
			eCryVS_VideoAdvanced_SetRate,
			eCryVS_VideoAdvanced_SetVolume,
			eCryVS_VideoAdvanced_SetLanguage,
			eCryVS_VideoAdvanced_SetVariant,
			eCryVS_VideoAdvanced_SetSkippable,
			eCryVS_VideoAdvanced_Info,
			eCryVS_VideoAdvanced_SetPauseDecodeWhenHidden, // M7
			eCryVS_FunctionCount
		};
		extern const CryGUID kCryVSFunctionGuids[eCryVS_FunctionCount];
		//! CryVS component signals of the WatchSurfaceID mode that are not table events (Ready / NewFrame reuse the events).
		extern const CryGUID kCryVSResizedSignalGuid;
		extern const CryGUID kCryVSClosedSignalGuid;

		//! Number of FlowGraph node classes (the composite nodes of FlowApiNodes.cpp).
		uint32_t FlowNodeCount();
		//! Name of the n-th FlowGraph node class ("Video:PlayOnScreen", ...), nullptr past the end.
		const char* FlowNodeClassName(uint32_t index);

		//! Decimal text of a frame serial (FrameSerial query, NewFrame event): no wrap.
		std::string SerialText(uint64_t serial);

		//! The one function every binding calls. Validates the thread and the handle (warning + false on failure),
		//! converts / defaults the arguments, then runs entry.fn. `in` may be shorter than paramCount.
		bool Call(const SEntry& entry, int handle, const SValue* in, uint32_t inCount, SValue* pResult, IMaterial* pDefaultMaterial = nullptr);

		//! Looks an entry up by name (any kind); nullptr when there is none.
		const SEntry* FindByName(const char* name);

		//! Startup self check: duplicate GUIDs / names, malformed entries -> CryWarning (and an assert in debug
		//! builds). Returns the number of problems found.
		int SelfCheck();

		//! Name of an EVideoPlayerState value ("Playing", ...).
		const char* StateName(int state);
	}
}
