// cev_apitest: a self test of the API table. Creates a player through the table functions only (handles, no
// IVideoPlayer pointers), loads a video, plays it, queries state / time after 2 seconds, stops, destroys, and logs a
// PASS / FAIL line ("[CEVPlayer] cev_apitest: PASS ...").
#pragma once

namespace CEVPlayer
{
	namespace Api
	{
		//! Starts the test (ignored while one runs). `path` = video, nullptr = Videos/vp9_alpha.webm.
		void StartApiTest(const char* path);
		//! Plug-in UpdateBeforeRender.
		void TickApiTest();
		//! Plug-in shutdown: drops a running test.
		void ShutdownApiTest();
	}
}
