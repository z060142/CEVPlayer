// cev_flowtest: a self test of the FlowGraph composite nodes (Bindings/FlowApiNodes.cpp). Builds a flow graph in
// memory (IFlowSystem::CreateFlowGraph) with every node class, drives their input ports through a test-only probe node
// type that is registered for the duration of the test, checks the outputs over a few seconds and logs a PASS / FAIL
// line ("[CEVPlayer] cev_flowtest: PASS ..."). Development only.
#pragma once

namespace CEVPlayer
{
	namespace Api
	{
		//! Starts the test (ignored while one runs). `pathA` = main video (default Videos/vp9_alpha.webm), `pathB` = a
		//! video of another size for the Resized check (default Videos/vp9_odd_321x181.webm).
		void StartFlowTest(const char* pathA, const char* pathB);
		//! Plug-in UpdateBeforeRender.
		void TickFlowTest();
		//! Plug-in shutdown: drops a running test (its graph and probe type).
		void ShutdownFlowTest();
	}
}
