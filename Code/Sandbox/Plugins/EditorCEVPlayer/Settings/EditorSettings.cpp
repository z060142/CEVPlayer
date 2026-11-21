#include "StdAfx.h"
#include "Settings/EditorSettings.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <CrySerialization/yasli/decorators/FileOpen.h>
#include <IEditor.h>

namespace CEVEditor
{
	//! Implemented by the FFmpeg service (FfmpegService.cpp): the preference changed (Preferences window, load, reset).
	void OnFfmpegPreferenceChanged();

	namespace
	{
		QJsonObject ReadObject(const QString& path)
		{
			QFile file(path);
			if (!file.open(QIODevice::ReadOnly))
				return QJsonObject();
			const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
			return doc.isObject() ? doc.object() : QJsonObject();
		}

		std::string Trimmed(std::string s)
		{
			auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '"'; };
			while (!s.empty() && isSpace(static_cast<unsigned char>(s.front())))
				s.erase(s.begin());
			while (!s.empty() && isSpace(static_cast<unsigned char>(s.back())))
				s.pop_back();
			return s;
		}
	}

	SCevEditorPreferences gCevEditorPreferences;
	REGISTER_PREFERENCES_PAGE_PTR(SCevEditorPreferences, &gCevEditorPreferences)

	SCevEditorPreferences::SCevEditorPreferences()
		: SPreferencePage("General", kPreferencesPagePath)
	{
	}

	bool SCevEditorPreferences::Serialize(yasli::Archive& ar)
	{
		const string before = ffmpegPath;
		ar.openBlock("ffmpeg", "FFmpeg");
		if (ar.isEdit())
		{
			// a browse button in the Preferences window; the value is a plain absolute path (file or folder)
			yasli::FileOpen file(ffmpegPath, "ffmpeg (ffmpeg.exe);;All files (*)");
			ar(file, "ffmpegPath", "FFmpeg executable");
			ar.doc("ffmpeg.exe (or its folder) used by the video import, re-encode and variants. It needs libvpx-vp9, libopus and the webm muxer. Empty: the downloaded minimal build, then the system PATH.");
		}
		else
		{
			ar(ffmpegPath, "ffmpegPath", "FFmpeg executable");
		}
		ar.closeBlock();
		ar(legacySettingsImported, "legacySettingsImported", nullptr); // no label: not shown in the Preferences window
		if (ar.isInput() && before != ffmpegPath)
			OnFfmpegPreferenceChanged();
		return true;
	}

	std::string FfmpegPathSetting()
	{
		const std::string value = Trimmed(gCevEditorPreferences.ffmpegPath.c_str());
		if (!value.empty())
			return value;
		if (!qEnvironmentVariableIsEmpty("CEV_EDITOR_SETTINGS"))
			return Trimmed(LoadEditorSetting(SettingKey::kFfmpegPath)); // tests: the legacy file at another location
		return std::string();
	}

	void SetFfmpegPathSetting(const std::string& path)
	{
		const string value = Trimmed(path).c_str();
		if (value == gCevEditorPreferences.ffmpegPath)
			return;
		gCevEditorPreferences.ffmpegPath = value;
		if (GetIEditor() != nullptr && GetIEditor()->GetPreferences() != nullptr)
			GetIEditor()->GetPreferences()->Save();
	}

	bool MigrateLegacyEditorSettings()
	{
		if (gCevEditorPreferences.legacySettingsImported)
			return false;
		gCevEditorPreferences.legacySettingsImported = true;
		const std::string legacy = Trimmed(LoadEditorSetting(SettingKey::kFfmpegPath));
		const bool import = !legacy.empty() && gCevEditorPreferences.ffmpegPath.empty();
		if (import)
		{
			gCevEditorPreferences.ffmpegPath = legacy.c_str();
			CryLogAlways("[CEVEditor] FFmpeg path imported from the old settings file %s into the editor preferences (%s): %s", EditorSettingsPath().c_str(), kPreferencesPagePath, legacy.c_str());
		}
		if (GetIEditor() != nullptr && GetIEditor()->GetPreferences() != nullptr)
			GetIEditor()->GetPreferences()->Save(); // remembers legacySettingsImported
		return import;
	}

	void OpenEditorPreferences()
	{
		if (GetIEditor() != nullptr)
			GetIEditor()->ExecuteCommand("general.open_pane 'Preferences'");
	}

	std::string EditorSettingsPath()
	{
		const QByteArray overridePath = qgetenv("CEV_EDITOR_SETTINGS");
		if (!overridePath.isEmpty())
			return std::string(overridePath.constData());
		QString base = QString::fromLocal8Bit(qgetenv("LOCALAPPDATA"));
		if (base.isEmpty())
			base = QDir::homePath() + QStringLiteral("/AppData/Local");
		return std::string((QDir::fromNativeSeparators(base) + QStringLiteral("/CEVPlayer/editor.json")).toUtf8().constData());
	}

	std::string LoadEditorSetting(const char* key)
	{
		const QJsonObject obj = ReadObject(QString::fromUtf8(EditorSettingsPath().c_str()));
		return std::string(obj.value(QString::fromUtf8(key)).toString().toUtf8().constData());
	}
}
