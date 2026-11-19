// Persistent settings of the CEV editor plug-in: a page of the editor's preferences (Edit > Preferences > CEVPlayer / FFmpeg,
// stored by the Sandbox in its Preferences.pref of the user folder).
//
// The plug-in used to keep them in its own JSON file, %LOCALAPPDATA%\CEVPlayer\editor.json {"ffmpegPath": "..."}. That file is
// only read now: once, as the initial value of the preference (MigrateLegacyEditorSettings), and as a read fallback while the
// preference is empty when CEV_EDITOR_SETTINGS=<file> names another location (tests / smoke runs must not touch the user's
// settings). Main thread only (the FFmpeg service keeps its own copy for the job threads).
#pragma once

#include <string>

#include <EditorFramework/Preferences.h>

namespace CEVEditor
{
	struct SCevEditorPreferences : public SPreferencePage
	{
		SCevEditorPreferences();
		bool Serialize(yasli::Archive& ar) override;

		string ffmpegPath;                    //!< ffmpeg.exe or its folder, see FfmpegLocator; "" = look in the usual places
		bool legacySettingsImported = false;  //!< editor.json was read into this page once (hidden)
	};
	extern SCevEditorPreferences gCevEditorPreferences;

	//! Path of the preferences page in the Preferences window.
	constexpr const char* kPreferencesPagePath = "CEVPlayer/FFmpeg";

	//! The FFmpeg path: the preference, or (while it is empty and CEV_EDITOR_SETTINGS is set) the value of that JSON file.
	std::string FfmpegPathSetting();
	//! Stores the FFmpeg path in the preference (and saves the preferences).
	void SetFfmpegPathSetting(const std::string& path);
	//! Once per user (the preference remembers it): copies ffmpegPath of the legacy editor.json into the preference when the
	//! preference is empty. Call after the editor loaded the preferences (first idle update). True when a value was imported.
	bool MigrateLegacyEditorSettings();
	//! Opens the editor's Preferences window.
	void OpenEditorPreferences();

	//! UTF-8 path of the legacy settings file (CEV_EDITOR_SETTINGS or %LOCALAPPDATA%\CEVPlayer\editor.json).
	std::string EditorSettingsPath();
	//! The value stored in the legacy settings file, "" when the file or the key does not exist.
	std::string LoadEditorSetting(const char* key);

	namespace SettingKey
	{
		constexpr const char* kFfmpegPath = "ffmpegPath";   // file or folder, see FfmpegLocator
	}
}
