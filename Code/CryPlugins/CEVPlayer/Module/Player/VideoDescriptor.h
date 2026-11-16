// Runtime descriptor of a video asset: "<name>.cevvideo" (XML, read with ISystem::LoadXmlFromFile, read only).
//
//   <CEVVideo version="1" route="embedded|mixed|cue" video="intro.webm" avOffsetMs="0"
//             trigger="Play_Intro" pauseTrigger="Pause_Intro" resumeTrigger="Resume_Intro">
//     (trigger / pauseTrigger / resumeTrigger on the root = defaults of the cue route for languages without their own)
//     <Languages default="english">
//       <Language name="english" embedded="1"/>                 use the embedded audio track
//       <Language name="japanese" file="intro.japanese.webm"/>   external WebM (audio only)
//       <Language name="german" trigger="Play_Intro_DE"/>        cue route (M3): CryAudio trigger of this language
//                 (optional pauseTrigger="..." resumeTrigger="..." per language)
//     </Languages>
//     <Variants rule="smallestNotBelowOutput">
//       <Variant file="intro_720.webm" width="1280" height="720"/>
//     </Variants>
//     <Markers><Marker name="explosion" time="3.25"/></Markers>
//   </CEVVideo>
//
// Parsing is defensive: a missing / invalid attribute gets a default and a warning, a broken child element is
// skipped with a warning, malformed XML or a missing "video" attribute makes the descriptor unusable (error, the
// load is rejected). File names are relative to the folder of the descriptor (a path that exists as given
// relative to the game root is accepted too). Main thread only.
#pragma once

#include <string>
#include <vector>

namespace CEVPlayer
{
	enum class EVideoRoute : unsigned char { Embedded, Mixed, Cue };

	struct SVideoDescriptor
	{
		struct SLanguage
		{
			std::string name;
			std::string file;      // resolved CryPak path of the external audio file ("" = none)
			std::string trigger;   // cue route trigger name (M3), "" = the root trigger
			std::string pauseTrigger;  // cue route: event action "pause" trigger, "" = the root one
			std::string resumeTrigger; // cue route: event action "resume" trigger, "" = the root one
			bool embedded = false;
		};
		struct SVariant
		{
			std::string file;      // resolved CryPak path
			int width = 0;
			int height = 0;
		};
		struct SMarker
		{
			std::string name;
			double time = 0.0;
		};

		std::string path;          // the descriptor itself
		EVideoRoute route = EVideoRoute::Embedded;
		std::string video;         // resolved CryPak path of the main video
		double avOffsetMs = 0.0;   // applied with PlayerCore::SetAvOffset (picture later by this much, see PlayerCore.h)
		std::string trigger;       // cue route default trigger (languages without their own use it)
		std::string pauseTrigger;  // cue route default pause trigger
		std::string resumeTrigger; // cue route default resume trigger
		std::string defaultLanguage;
		std::vector<SLanguage> languages;
		std::vector<SVariant> variants;
		std::vector<SMarker> markers; // sorted by time
	};

	//! True when `path` ends in ".cevvideo" (case insensitive).
	bool IsDescriptorPath(const char* path);

	//! Loads and validates the descriptor at `path`. Returns false (error logged) when it is unusable.
	bool LoadVideoDescriptor(const char* path, SVideoDescriptor& out);

	//! `file` relative to the folder of `basePath` (or as given when that exists instead).
	std::string ResolveSiblingPath(const char* basePath, const char* file);

	//! Index of the language called `name` (case insensitive), -1 when there is none.
	int FindLanguage(const SVideoDescriptor& desc, const char* name);

	const char* RouteName(EVideoRoute route);
}
