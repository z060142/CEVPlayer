// The API description table: the single source of truth of everything the script bindings expose.
//
// Every control, query and event is described ONCE (fixed GUID, name, description, parameters, return type, async
// flag and - for controls and queries - a plain C++ function that implements it on top of IVideoPlayer). The
// FlowGraph nodes, the Schematyc component functions / signals, the CryVS elements and the console self test are all
// generated from this table; none of them contains any playback logic of its own.
//
// Players are addressed by an integer handle (see CApiPlayers). Handle 0 is never valid. A call with a handle that
// is not valid (never created, destroyed, plug-in shut down) is a safe no-op that logs a warning and returns false.
//
// The GUIDs in ApiTable.cpp are fixed literals: they are stored in saved flow graphs / Schematyc scripts / CryVS
// scripts and MUST NEVER CHANGE after the first release. Adding an entry means generating a new GUID for it.
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
			eEntry_NoPlayer  = 1u << 0, //!< does not use a handle (Create, InterfaceVersion, Caps)
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
			Texture = 0,  //!< only the texture (named at Create / generated), nothing is drawn by the plug-in
			Screen,       //!< fullscreen on the top layer, `Fit` decides how it is placed
			Material,     //!< bind the texture to a texture slot of a material
		};

		//! Fixed GUIDs of the Schematyc / CryVS elements that are not table entries.
		extern const CryGUID kSchematycComponentGuid;
		extern const CryGUID kCryVSComponentGuid;
		extern const CryGUID kOutputModeTypeGuid;
		extern const CryGUID kVideoFileNameTypeGuid;
		extern const CryGUID kFitTypeGuid;

		//! Number of FlowGraph node classes generated from the table.
		uint32_t FlowNodeCount();

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
