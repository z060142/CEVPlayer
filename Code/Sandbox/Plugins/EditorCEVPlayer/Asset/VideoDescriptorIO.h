// Editor side model of the runtime descriptor "<name>.cevvideo" (format: Module/Player/VideoDescriptor.h).
// The editor does not link the runtime module, so it has its own reader / writer. Names of video / audio files are
// relative to the folder of the descriptor (all files of one video asset live flat in one folder).
#pragma once

#include <string>
#include <vector>

namespace yasli { class Archive; }

namespace CEVEditor
{
	//! Context of SDescriptor::Serialize (yasli::Context): the folder of the descriptor (relative to Assets, trailing slash), so
	//! that the language files are shown / chosen as game paths and stored relative to the descriptor.
	struct SDescriptorSerializeContext
	{
		std::string folderRel;
	};

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

			void Serialize(yasli::Archive& ar);
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

			void Serialize(yasli::Archive& ar);
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
		//! The fields the user edits in the info panel's property tree: audio route, A/V offset, default language, languages
		//! (name, embedded, external file, ACE triggers), markers. The video, the root triggers and the variants are not part
		//! of it (they keep their values on input).
		void Serialize(yasli::Archive& ar);
	};

	//! Reads the descriptor file (absolute UTF-8 path). Needs gEnv->pSystem (XML parser of the engine).
	bool LoadDescriptor(const std::string& absPath, SDescriptor& out, std::string& error);
	//! Writes it (UTF-8 XML, `version="1"`). Returns false and an error text when the file cannot be written.
	bool SaveDescriptor(const std::string& absPath, const SDescriptor& desc, std::string& error);
	//! The XML text SaveDescriptor writes.
	std::string DescriptorToXml(const SDescriptor& desc);
	//! Same content (compared through the XML text).
	bool DescriptorsEqual(const SDescriptor& a, const SDescriptor& b);
	//! Three way merge for "the file changed on disk (a job rewrote it) while the user has unsaved edits in memory":
	//! `base` = what was last read from / written to the file, `mine` = the edited copy, `theirs` = the file now.
	//! Every field the user changed (mine != base) keeps the user's value, every other field takes the file's.
	//! Languages are matched by name; lists (markers, variants) are taken as a whole from whoever changed them.
	SDescriptor MergeDescriptors(const SDescriptor& base, const SDescriptor& mine, const SDescriptor& theirs);
	//! Rewrites every file name (video, languages, variants) that starts with `oldPrefix` (case insensitive, like
	//! CAssetType::RenameAsset does for the files) so that it starts with `newPrefix`. Returns the number of changes.
	int RenameDescriptorFiles(SDescriptor& desc, const std::string& oldPrefix, const std::string& newPrefix);
}
