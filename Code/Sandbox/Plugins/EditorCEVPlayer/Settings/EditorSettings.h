// Persistent settings of the CEV editor plug-in: a small JSON file in the user folder (not in the project, not in a pak).
//   %LOCALAPPDATA%\CEVPlayer\editor.json      {"ffmpegPath": "C:/tools/ffmpeg/bin/ffmpeg.exe"}
// CEV_EDITOR_SETTINGS=<file> overrides the location (tests / smoke runs must not touch the user's settings).
// Main thread only (the service keeps its own copy for the job threads).
#pragma once

#include <string>

namespace CEVEditor
{
	//! UTF-8 path of the settings file (never empty; the folder may not exist yet).
	std::string EditorSettingsPath();
	//! The stored value, "" when the file or the key does not exist.
	std::string LoadEditorSetting(const char* key);
	//! Writes the key (other keys of the file stay); an empty value removes it. False + error when the file cannot be written.
	bool SaveEditorSetting(const char* key, const std::string& value, std::string& error);

	namespace SettingKey
	{
		constexpr const char* kFfmpegPath = "ffmpegPath";   // file or folder, see FfmpegLocator
	}
}
