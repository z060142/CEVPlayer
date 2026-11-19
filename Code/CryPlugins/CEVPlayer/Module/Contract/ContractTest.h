// cev_contract_selftest: self test of the texture source contract (docs/design/M6-ui-source.md section 3) and the
// surface id registry. It reaches the contract only through this module's own export (GetProcAddress of
// "CryTexSource_v1"), plays two videos of different sizes into one surface and logs a PASS / FAIL line
// ("[CEVPlayer] cev_contract_selftest: PASS ...").
#pragma once

namespace CEVPlayer
{
	namespace Contract
	{
		//! Starts the test (ignored while one runs). pathA / pathB: videos of different sizes, nullptr = defaults.
		void StartSelfTest(const char* pathA, const char* pathB);
		//! Plug-in UpdateBeforeRender, after the manager's tick.
		void TickSelfTest();
		//! Plug-in shutdown: drops a running test.
		void ShutdownSelfTest();
	}
}
