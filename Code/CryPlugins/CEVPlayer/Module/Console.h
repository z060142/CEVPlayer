// Console variables and commands of the plug-in (cev_* and the legacy vp_* ones).
#pragma once

namespace CEVPlayer
{
	void RegisterConsole();
	void UnregisterConsole();
	// Development-only measurement probes (S0): per-frame steps called by the plug-in.
	void DevProbesBeforeRender();
	void DevProbesAfterRender();
	void DevProbesShutdown();
}
