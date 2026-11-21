// DescriptorPlan: engine independent part of a video descriptor ("<name>.cevvideo"): the data model and the
// decisions taken from it when media is loaded - which video file (main video or a variant), which audio source
// (embedded track, external audio file, or a cue trigger), the A/V offset and the markers.
//
// Shared by the runtime module (Module/Player: it reads the XML through the engine and resolves file names with
// CryPak) and the Sandbox preview (it reads the descriptor with its own reader). Reading the file, resolving names
// and logging stay with the caller: every function here only takes the parsed data, the caller's audio track /
// variant requests and a file existence callback, and reports what it decided (including what it had to skip) so that the
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
		//! One audio track (A1). `name` is the author's identifier the track is chosen by; `language` is an optional
		//! tag that only matters when the descriptor sets followGameLanguage.
		struct SAudioTrack
		{
			std::string name;
			std::string language;      // optional tag (free text, an engine language name such as "english" is advised)
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
		std::string trigger;       // cue route default trigger (tracks without their own use it)
		std::string pauseTrigger;  // cue route default pause trigger
		std::string resumeTrigger; // cue route default resume trigger
		std::string defaultTrack;          // name of the default audio track, "" = the first one
		bool followGameLanguage = false;   // match g_languageAudio / g_language against the tracks' language tags
		std::vector<SAudioTrack> audioTracks;
		std::vector<SVariant> variants;
		std::vector<SMarker> markers; // sorted by time (SortDescriptorMarkers)
	};

	//! Existence test for a resolved file name (CryPak, file system, ...). `user` is passed through.
	using DescriptorFileExistsFn = bool (*)(const char* path, void* user);

	const char* VideoRouteName(VideoRoute route);

	//! Longest audio track name.
	constexpr size_t kMaxAudioTrackNameLength = 63;

	//! A valid audio track name: 1 to kMaxAudioTrackNameLength of A-Z a-z 0-9 _ . -
	bool IsValidAudioTrackName(const char* name);

	//! Index of the audio track called `name` (ASCII case insensitive), -1 when there is none or `name` is empty.
	int FindDescriptorAudioTrack(const VideoDescriptor& desc, const char* name);

	//! Index of the first audio track whose language tag is `language` (ASCII case insensitive), -1 when there is
	//! none or `language` is empty.
	int FindDescriptorAudioTrackByLanguage(const VideoDescriptor& desc, const char* language);

	struct DescriptorTrackProblem
	{
		enum Kind : unsigned char { InvalidName, Duplicate, UnknownDefault };
		Kind kind = InvalidName;
		std::string name; // the offending track name (UnknownDefault: the default's name)
	};

	//! Drops tracks with an invalid name and later duplicates of a name (the first one stays) and clears a default
	//! that names no remaining track. `problems` is cleared and receives one entry per change, in list order.
	void SanitizeDescriptorAudioTracks(VideoDescriptor& desc, std::vector<DescriptorTrackProblem>& problems);

	//! Stable sort of the markers by time.
	void SortDescriptorMarkers(VideoDescriptor& desc);

	//! A/V offset of the descriptor in seconds (PlayerConfig::avOffsetSeconds; PlayerCore clamps it).
	inline double DescriptorAvOffsetSeconds(const VideoDescriptor& desc) { return desc.avOffsetMs / 1000.0; }

	//! Why a track was chosen (ResolveDescriptorAudio).
	enum class DescriptorTrackReason : unsigned char { None, Requested, GameLanguage, Default, First };

	struct DescriptorAudioPlan
	{
		std::string track;          // audio track in use ("" = the descriptor has no tracks: embedded audio)
		DescriptorTrackReason reason = DescriptorTrackReason::None;
		bool unknownRequest = false; // a track was requested by a name the descriptor does not list (ignored)
		std::string externalPath;   // external audio file (mixed route), "" = none
		bool cue = false;           // the audio is a cue trigger (route cue, or a trigger-only track of a mixed video)
		std::string trigger;        // cue: start trigger ("" = nothing to play)
		std::string pauseTrigger;
		std::string resumeTrigger;
		std::string missingExternal; // the track names this external file but it does not exist: embedded track used
	};

	//! Audio source for the descriptor (A1). The track is chosen in this order: (1) `requestedTrack` (SetAudioTrack;
	//! an unknown name sets unknownRequest and counts as not given); (2) only when desc.followGameLanguage is set:
	//! `gameLanguages` in order (e.g. { g_languageAudio, g_language }) matched against the tracks' language tags;
	//! (3) the default track; (4) the first track. Without tracks: the embedded audio. `gameLanguages` is not read
	//! at all when followGameLanguage is off.
	//! Route cue: always cue, the track (if any) may override the triggers. Route embedded: the embedded audio
	//! whatever the track says. Route mixed: the track's external file when it exists, a trigger-only track makes it
	//! cue, otherwise the embedded audio.
	void ResolveDescriptorAudio(const VideoDescriptor& desc, const char* requestedTrack, const char* const* gameLanguages,
		size_t gameLanguageCount, DescriptorFileExistsFn exists, void* user, DescriptorAudioPlan& out);

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
	//! (none big enough: the main video); without `autoPick`, or with `outputHeight` <= 0 (unknown), the main video.
	//! `outputHeight` is the window height (fullscreen) or the aggregated on-screen pixel height of a surface (M7).
	DescriptorVariantChoice PickDescriptorVariant(const VideoDescriptor& desc, int forced, bool autoPick, int outputHeight,
		DescriptorFileExistsFn exists, void* user);
}
