// DescriptorPlan: engine independent part of a video descriptor ("<name>.cevvideo"): the data model and the
// decisions taken from it when media is loaded - which video file (main video or a variant), which audio source
// (embedded track, external language file, or a cue trigger), the A/V offset and the markers.
//
// Shared by the runtime module (Module/Player: it reads the XML through the engine and resolves file names with
// CryPak) and the Sandbox preview (it reads the descriptor with its own reader). Reading the file, resolving names
// and logging stay with the caller: every function here only takes the parsed data, the caller's language / variant
// requests and a file existence callback, and reports what it decided (including what it had to skip) so that the
// caller can log it in its own words. No engine header, no I/O, no allocation besides the std::string results.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cev
{
	enum class VideoRoute : unsigned char { Embedded, Mixed, Cue };

	struct VideoDescriptor
	{
		struct SLanguage
		{
			std::string name;
			std::string file;          // resolved path of the external audio file ("" = none)
			std::string trigger;       // cue route trigger name, "" = the root trigger
			std::string pauseTrigger;  // cue route: event action "pause" trigger, "" = the root one
			std::string resumeTrigger; // cue route: event action "resume" trigger, "" = the root one
			bool embedded = false;
		};
		struct SVariant
		{
			std::string file;          // resolved path
			int width = 0;
			int height = 0;
		};
		struct SMarker
		{
			std::string name;
			double time = 0.0;
		};

		std::string path;          // the descriptor itself
		VideoRoute route = VideoRoute::Embedded;
		std::string video;         // resolved path of the main video
		double avOffsetMs = 0.0;   // applied with PlayerCore::SetAvOffset (picture later by this much, see PlayerCore.h)
		std::string trigger;       // cue route default trigger (languages without their own use it)
		std::string pauseTrigger;  // cue route default pause trigger
		std::string resumeTrigger; // cue route default resume trigger
		std::string defaultLanguage;
		std::vector<SLanguage> languages;
		std::vector<SVariant> variants;
		std::vector<SMarker> markers; // sorted by time (SortDescriptorMarkers)
	};

	//! Existence test for a resolved file name (CryPak, file system, ...). `user` is passed through.
	using DescriptorFileExistsFn = bool (*)(const char* path, void* user);

	const char* VideoRouteName(VideoRoute route);

	//! Index of the language called `name` (ASCII case insensitive), -1 when there is none or `name` is empty.
	int FindDescriptorLanguage(const VideoDescriptor& desc, const char* name);

	//! Stable sort of the markers by time.
	void SortDescriptorMarkers(VideoDescriptor& desc);

	//! A/V offset of the descriptor in seconds (PlayerConfig::avOffsetSeconds; PlayerCore clamps it).
	inline double DescriptorAvOffsetSeconds(const VideoDescriptor& desc) { return desc.avOffsetMs / 1000.0; }

	struct DescriptorAudioPlan
	{
		std::string language;       // language in use ("" = none matched)
		std::string externalPath;   // external audio file (mixed route), "" = none
		bool cue = false;           // the audio is a cue trigger (route cue, or a trigger-only language of a mixed video)
		std::string trigger;        // cue: start trigger ("" = nothing to play)
		std::string pauseTrigger;
		std::string resumeTrigger;
		std::string missingExternal; // the language names this external file but it does not exist: embedded track used
	};

	//! Audio source for the descriptor. `requests` are tried in order (the first one that names a listed language
	//! wins; empty / unknown names are skipped), e.g. { SetLanguage(), g_languageAudio, g_language, default }.
	//! Route cue: always cue, the language (if any) may override the triggers. Route embedded: the embedded track
	//! whatever the language says. Route mixed: the language's external file when it exists, a trigger-only
	//! language makes it cue, otherwise the embedded track.
	void ResolveDescriptorAudio(const VideoDescriptor& desc, const char* const* requests, size_t requestCount,
		DescriptorFileExistsFn exists, void* user, DescriptorAudioPlan& out);

	struct DescriptorVariantChoice
	{
		std::string videoPath;      // file to open
		int variant = -1;           // 1 based variant index, -1 = the main video
		int width = 0;              // of the chosen variant (0 for the main video)
		int height = 0;
		bool forcedOutOfRange = false; // a forced variant > 0 that does not exist: main video
		std::string missingFile;    // the chosen variant's file does not exist: main video
	};

	//! Video file for the descriptor. `forced` >= 0 forces a variant (0 = main video, 1..n = variant), `forced` < 0
	//! picks automatically when `autoPick` is set: the smallest variant whose height is not below `outputHeight`
	//! (none big enough: the main video); without `autoPick` the main video.
	DescriptorVariantChoice PickDescriptorVariant(const VideoDescriptor& desc, int forced, bool autoPick, int outputHeight,
		DescriptorFileExistsFn exists, void* user);
}
