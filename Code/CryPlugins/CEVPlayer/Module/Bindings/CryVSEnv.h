// CryVS bindings (community tree only). This header deliberately includes nothing from CryVS: Plugin.cpp includes
// it under #if CEV_HAVE_CRYVS, and CryVSEnv.cpp is only compiled when CMake found the CryVS headers
// (Code/CryEngine/CryCommon/CryVS) in the engine tree. The vanilla engine never sees any of it.
#pragma once

namespace CEVPlayer
{
	namespace CryVSBind
	{
		//! ESYSTEM_EVENT_GAME_POST_INIT: when gEnv->pCryVS exists, registers the types, the global functions of the three
		//! drawers (Video, VideoUI, VideoAdvanced), the component with its member functions and its signals (each element
		//! only when the registry does not have it yet), connects to the framework's envRefresh signal (which clears the
		//! whole registry) to register them again, and adds the console command cev_cryvstest. gEnv->pCryVS == nullptr:
		//! nothing happens.
		void Initialize();
		//! Plug-in UpdateBeforeRender: components in WatchSurfaceID mode follow their surface; cev_cryvstest steps.
		void Tick();
		//! Plug-in shutdown: disconnects from envRefresh, stops the self test and makes the components stop using the
		//! player API. (The registry has no removal API; see CryVSEnv.cpp.)
		void Shutdown();
	}
}
