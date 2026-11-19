// Console variables and commands of the plug-in (cev_*).
#pragma once

namespace CEVPlayer
{
	void RegisterConsole();
	void UnregisterConsole();
	// Development-only measurement probes (S0): per-frame steps called by the plug-in.
	void DevProbesBeforeRender();
	void DevProbesAfterRender();
	void DevProbesShutdown();
	// Development-only test driver (cev_run_script): runs the next lines of the script, called by the plug-in each frame.
	void TickRunScript();
	void ShutdownRunScript();
}
