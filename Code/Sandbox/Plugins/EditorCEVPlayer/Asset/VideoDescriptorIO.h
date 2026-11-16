// Editor side model of the runtime descriptor "<name>.cevvideo" (format: Module/Player/VideoDescriptor.h).
// The editor does not link the runtime module, so it has its own reader / writer. Names of video / audio files are
// relative to the folder of the descriptor (all files of one video asset live flat in one folder).
#pragma once

#include <string>
#include <vector>

namespace CEVEditor
{
	struct SDescriptor
	{
		struct SLanguage
		{
			std::string name;
			bool embedded = false;
			std::string file;
			std::string trigger;
			std::string pauseTrigger;
			std::string resumeTrigger;
		};
		struct SVariant
		{
			std::string file;
			int width = 0;
			int height = 0;
		};
		struct SMarker
		{
			std::string name;
			double time = 0.0;
		};

		std::string route = "embedded";     // embedded | mixed | cue
		std::string video;                  // main video, relative to the descriptor
		double avOffsetMs = 0.0;
		std::string trigger;
		std::string pauseTrigger;
		std::string resumeTrigger;
		std::string defaultLanguage;
		std::vector<SLanguage> languages;
		std::string variantRule;            // "" = the runtime default
		std::vector<SVariant> variants;
		std::vector<SMarker> markers;

		int FindLanguage(const std::string& name) const;
	};

	//! Reads the descriptor file (absolute UTF-8 path). Needs gEnv->pSystem (XML parser of the engine).
	bool LoadDescriptor(const std::string& absPath, SDescriptor& out, std::string& error);
	//! Writes it (UTF-8 XML, `version="1"`). Returns false and an error text when the file cannot be written.
	bool SaveDescriptor(const std::string& absPath, const SDescriptor& desc, std::string& error);
	//! The XML text SaveDescriptor writes.
	std::string DescriptorToXml(const SDescriptor& desc);
	//! Rewrites every file name (video, languages, variants) that starts with `oldPrefix` (case insensitive, like
	//! CAssetType::RenameAsset does for the files) so that it starts with `newPrefix`. Returns the number of changes.
	int RenameDescriptorFiles(SDescriptor& desc, const std::string& oldPrefix, const std::string& newPrefix);
}
