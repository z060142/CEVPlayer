// Console variables and commands of the plug-in (cev_*).
#pragma once

#include <cstddef>

namespace CEVPlayer
{
	namespace FullscreenOutput { struct SDebugLayer; }

	void RegisterConsole();
	void UnregisterConsole();
	// Development-only measurement probes (S0): per-frame steps called by the plug-in.
	void DevProbesBeforeRender();
	//! The debug layers of the probes for this frame (cev_show_surface, cev_probe_r8, cev_probe_sync,
	//! cev_probe_loadtime draw); valid until the next call. Taken by the fullscreen output step.
	void DevProbesDebugLayers(const FullscreenOutput::SDebugLayer*& pLayers, size_t& count);
	void DevProbesShutdown();
	// Development-only test driver (cev_run_script): runs the next lines of the script, called by the plug-in each frame.
	void TickRunScript();
	void ShutdownRunScript();
}
