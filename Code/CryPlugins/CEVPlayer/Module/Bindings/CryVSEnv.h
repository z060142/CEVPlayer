// CryVS bindings (community tree only). This header deliberately includes nothing from CryVS: Plugin.cpp includes
// it under #if CEV_HAVE_CRYVS, and CryVSEnv.cpp is only compiled when CMake found the CryVS headers
// (Code/CryEngine/CryCommon/CryVS) in the engine tree. The vanilla engine never sees any of it.
#pragma once

namespace CEVPlayer
{
	namespace CryVSBind
	{
		//! ESYSTEM_EVENT_GAME_POST_INIT: when gEnv->pCryVS exists, registers the component, its functions and signals
		//! (generated from the API table) once and connects to the framework's envRefresh signal, which clears the
		//! whole registry on every refresh, to register them again. gEnv->pCryVS == nullptr: nothing happens.
		void Initialize();
		//! Plug-in shutdown: disconnects from envRefresh and makes the component stop using the player API.
		void Shutdown();
	}
}
