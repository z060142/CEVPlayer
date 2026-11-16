#include "StdAfx.h"
#include "Audio/AceRegistrar.h"

#include <filesystem>
#include <fstream>
#include <iterator>

#include <CrySystem/ISystem.h>

#include "Asset/VideoAssetMeta.h"

namespace CEVEditor
{
	namespace fs = std::filesystem;

	namespace
	{
		const char* const kAssetsDir = "audio/sdlmixer/assets/cev/";
		const char* const kAceDir = "audio/sdlmixer/ace/";

		std::string LibraryRel(const std::string& assetName)
		{
			return std::string(kAceDir) + "cev_" + assetName + ".xml";
		}

		std::string XmlEscape(const std::string& s)
		{
			std::string out;
			for (char c : s)
			{
				switch (c)
				{
				case '&': out += "&amp;"; break;
				case '<': out += "&lt;"; break;
				case '>': out += "&gt;"; break;
				case '"': out += "&quot;"; break;
				default: out += c; break;
				}
			}
			return out;
		}

		// Does another ACE library already define a trigger called `name`?
		bool TriggerUsedElsewhere(const std::string& name, const fs::path& ownLibrary)
		{
			std::error_code ec;
			const fs::path dir = fs::u8path(AssetsAbsolute(kAceDir));
			if (!fs::is_directory(dir, ec))
				return false;
			const std::string needle = "<Trigger name=\"" + XmlEscape(name) + "\"";
			for (const fs::directory_entry& entry : fs::directory_iterator(dir, ec))
			{
				if (entry.path().extension() != ".xml" || entry.path() == ownLibrary)
					continue;
				std::ifstream file(entry.path(), std::ios::binary);
				const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
				if (text.find(needle) != std::string::npos)
					return true;
			}
			return false;
		}
	}

	std::string AceTriggerName(const char* kind, const std::string& assetName, const std::string& language)
	{
		std::string name = std::string(kind) + "_" + assetName;
		if (!language.empty())
			name += "_" + language;
		return name;
	}

	std::string BuildAceXml(const std::string& assetName, const std::vector<SAceSound>& sounds)
	{
		const size_t triggerCount = sounds.size() * 3;
		std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
		xml += "<AudioSystemData name=\"cev_" + XmlEscape(assetName) + "\" version=\"2\" triggers=\"" + std::to_string(triggerCount)
			+ "\" parameters=\"0\" switches=\"0\" states=\"0\" environments=\"0\" preloads=\"0\" settings=\"0\" files=\"0\">\n";
		xml += "\t<Triggers>\n";
		const std::string path = "cev/" + assetName;
		for (const SAceSound& sound : sounds)
		{
			const std::string file = XmlEscape(FileNameOf(sound.fileAbs));
			const char* const kinds[3] = { "Play", "Pause", "Resume" };
			const char* const types[3] = { "start", "pause", "resume" };
			for (int i = 0; i < 3; ++i)
			{
				xml += "\t\t<Trigger name=\"" + XmlEscape(AceTriggerName(kinds[i], assetName, sound.language)) + "\">\n";
				xml += "\t\t\t<Event name=\"" + file + "\" path=\"" + XmlEscape(path) + "\" type=\"" + types[i] + "\"";
				if (i == 0)
					xml += " volume=\"0\" loop_count=\"1\"";
				xml += "/>\n\t\t</Trigger>\n";
			}
		}
		xml += "\t</Triggers>\n";
		xml += "\t<ImplData events=\"" + std::to_string(triggerCount) + "\" parameters=\"0\" parametersadvanced=\"0\" switchstates=\"0\" files=\"0\"/>\n";
		xml += "</AudioSystemData>\n";
		return xml;
	}

	void SendAudioRefresh()
	{
		if (gEnv != nullptr && gEnv->pSystem != nullptr && gEnv->pSystem->GetISystemEventDispatcher() != nullptr)
			gEnv->pSystem->GetISystemEventDispatcher()->OnSystemEvent(ESYSTEM_EVENT_AUDIO_REFRESH, 0, 0);
	}

	void RemoveAssetSounds(const std::string& assetName)
	{
		std::error_code ec;
		fs::remove_all(fs::u8path(AssetsAbsolute(std::string(kAssetsDir) + assetName)), ec);
		fs::remove(fs::u8path(AssetsAbsolute(LibraryRel(assetName))), ec);
		SendAudioRefresh();
	}

	SAceResult RegisterAssetSounds(const std::string& assetName, const std::vector<SAceSound>& sounds)
	{
		SAceResult result;
		if (assetName.empty() || sounds.empty())
		{
			result.error = "no sounds to register";
			return result;
		}
		std::error_code ec;
		const fs::path libraryPath = fs::u8path(AssetsAbsolute(LibraryRel(assetName)));

		for (const SAceSound& sound : sounds)
		{
			if (!fs::is_regular_file(fs::u8path(sound.fileAbs), ec))
			{
				result.error = "sound file not found: " + sound.fileAbs;
				return result;
			}
			for (const char* kind : { "Play", "Pause", "Resume" })
			{
				const std::string trigger = AceTriggerName(kind, assetName, sound.language);
				if (TriggerUsedElsewhere(trigger, libraryPath))
				{
					result.error = "the trigger name " + trigger + " is already used by another ACE library";
					return result;
				}
			}
		}

		// replace an earlier registration of this asset (the copies, not the library: it is overwritten below)
		fs::remove_all(fs::u8path(AssetsAbsolute(std::string(kAssetsDir) + assetName)), ec);
		const fs::path copyDir = fs::u8path(AssetsAbsolute(std::string(kAssetsDir) + assetName));
		fs::create_directories(copyDir, ec);
		if (ec)
		{
			result.error = "cannot create " + copyDir.u8string();
			return result;
		}
		for (const SAceSound& sound : sounds)
		{
			const std::string fileName = FileNameOf(sound.fileAbs);
			fs::copy_file(fs::u8path(sound.fileAbs), copyDir / fs::u8path(fileName), fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				result.error = "cannot copy " + sound.fileAbs + " to " + copyDir.u8string();
				return result;
			}
			result.copiedRel.push_back(std::string(kAssetsDir) + assetName + "/" + fileName);

			SAceTriggers t;
			t.language = sound.language;
			t.play = AceTriggerName("Play", assetName, sound.language);
			t.pause = AceTriggerName("Pause", assetName, sound.language);
			t.resume = AceTriggerName("Resume", assetName, sound.language);
			result.triggers.push_back(t);
		}

		fs::create_directories(libraryPath.parent_path(), ec);
		const fs::path tmp = fs::u8path(libraryPath.u8string() + ".tmp");
		{
			std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
			const std::string xml = BuildAceXml(assetName, sounds);
			file.write(xml.data(), static_cast<std::streamsize>(xml.size()));
			if (!file)
			{
				result.error = "cannot write " + tmp.u8string();
				return result;
			}
		}
		fs::rename(tmp, libraryPath, ec);
		if (ec)
		{
			fs::remove(tmp, ec);
			result.error = "cannot write " + libraryPath.u8string();
			return result;
		}
		result.libraryRel = LibraryRel(assetName);
		result.ok = true;
		SendAudioRefresh();
		return result;
	}
}
