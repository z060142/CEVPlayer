// cev_contract_selftest: self test of the texture source contract (UI-SOURCE.md) and the
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
		//! cev_hidden_bench (measurement, M7): plays `path` looping into surface "CevBench" and toggles a consumer hint
		//! between hidden (`hiddenSeconds`) and visible (`visibleSeconds`) for `cycles` cycles; logs the catch-ups.
		void StartHiddenBench(const char* path, int cycles, float hiddenSeconds, float visibleSeconds);
		//! Plug-in UpdateBeforeRender, after the manager's tick (also drives cev_hidden_bench).
		void TickSelfTest();
		//! Plug-in shutdown: drops a running test.
		void ShutdownSelfTest();
	}
}
