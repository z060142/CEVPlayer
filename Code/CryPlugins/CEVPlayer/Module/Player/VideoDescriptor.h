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

#include "CEVCore/DescriptorPlan.h"

namespace CEVPlayer
{
	// The data model and the decisions taken from it (variant, audio source) live in CEVPlayerCore
	// (CEVCore/DescriptorPlan.h), shared with the Sandbox preview; reading the XML and resolving file names through
	// CryPak stay here.
	using EVideoRoute = cev::VideoRoute;
	using SVideoDescriptor = cev::VideoDescriptor;

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
