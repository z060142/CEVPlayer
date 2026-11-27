// SDL_mixer registration of the sounds of a video asset: copies the WAV / Ogg files to audio/sdlmixer/assets/cev/<asset>/
// (the SDL_mixer implementation only scans audio/sdlmixer/assets/) and writes the ACE library
// audio/sdlmixer/ace/cev_<asset>.xml with a start / pause / resume trigger per audio track (the format of
// tools/test_audio/cev_test.xml). The Audio Controls Editor has no API for other plug-ins, so this
// is plain XML + ESYSTEM_EVENT_AUDIO_REFRESH. Main thread (it sends a system event). Wwise / FMOD: not handled here, the
// info panel shows the manual steps.
#pragma once

#include <string>
#include <vector>

namespace CEVEditor
{
	struct SAceSound
	{
		std::string track;      //!< descriptor audio track name, "" for the only / unnamed sound
		std::string fileAbs;    //!< the .wav / .ogg in the asset folder (absolute, UTF-8)
	};

	struct SAceTriggers
	{
		std::string track;
		std::string play;
		std::string pause;
		std::string resume;
	};

	struct SAceResult
	{
		bool ok = false;
		std::string error;
		std::string libraryRel;                 //!< "audio/sdlmixer/ace/cev_<asset>.xml"
		std::vector<std::string> copiedRel;     //!< paths relative to Assets of the SDL_mixer copies
		std::vector<SAceTriggers> triggers;     //!< one per sound, same order
	};

	//! Name of the trigger "<kind>_<asset>[_<track>]" (kind: Play / Pause / Resume).
	std::string AceTriggerName(const char* kind, const std::string& assetName, const std::string& track);
	//! The library text for the given sounds (copied file names are the file names of `sounds`).
	std::string BuildAceXml(const std::string& assetName, const std::vector<SAceSound>& sounds);

	//! Copies the sounds, writes the library and sends the refresh. Re-registering an asset replaces its earlier
	//! registration. Fails (nothing written) when a trigger name is already used by another library.
	SAceResult RegisterAssetSounds(const std::string& assetName, const std::vector<SAceSound>& sounds);
	//! Deletes the copies and the library of `assetName` (silently ignores what does not exist) and sends the refresh.
	void RemoveAssetSounds(const std::string& assetName);
	//! ESYSTEM_EVENT_AUDIO_REFRESH (effective in Profile / Debug builds of the engine).
	void SendAudioRefresh();
}
