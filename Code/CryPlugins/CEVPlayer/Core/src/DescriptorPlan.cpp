#include "CEVCore/DescriptorPlan.h"

#include <algorithm>

namespace cev
{
	namespace
	{
		char Lower(char c)
		{
			return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
		}

		bool EqualsNoCase(const char* a, const char* b)
		{
			for (; *a != '\0' && *b != '\0'; ++a, ++b)
			{
				if (Lower(*a) != Lower(*b))
					return false;
			}
			return *a == *b;
		}

		bool Exists(DescriptorFileExistsFn exists, void* user, const std::string& path)
		{
			return !path.empty() && exists != nullptr && exists(path.c_str(), user);
		}
	}

	const char* VideoRouteName(VideoRoute route)
	{
		switch (route)
		{
		case VideoRoute::Embedded: return "embedded";
		case VideoRoute::Mixed:    return "mixed";
		case VideoRoute::Cue:      return "cue";
		}
		return "?";
	}

	bool IsValidAudioTrackName(const char* name)
	{
		if (name == nullptr || name[0] == '\0')
			return false;
		size_t len = 0;
		for (const char* p = name; *p != '\0'; ++p, ++len)
		{
			const char c = *p;
			const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
			if (!ok || len >= kMaxAudioTrackNameLength)
				return false;
		}
		return true;
	}

	int FindDescriptorAudioTrack(const VideoDescriptor& desc, const char* name)
	{
		if (name == nullptr || name[0] == '\0')
			return -1;
		for (size_t i = 0; i < desc.audioTracks.size(); ++i)
		{
			if (EqualsNoCase(desc.audioTracks[i].name.c_str(), name))
				return static_cast<int>(i);
		}
		return -1;
	}

	int FindDescriptorAudioTrackByLanguage(const VideoDescriptor& desc, const char* language)
	{
		if (language == nullptr || language[0] == '\0')
			return -1;
		for (size_t i = 0; i < desc.audioTracks.size(); ++i)
		{
			const std::string& tag = desc.audioTracks[i].language;
			if (!tag.empty() && EqualsNoCase(tag.c_str(), language))
				return static_cast<int>(i);
		}
		return -1;
	}

	void SanitizeDescriptorAudioTracks(VideoDescriptor& desc, std::vector<DescriptorTrackProblem>& problems)
	{
		problems.clear();
		std::vector<VideoDescriptor::SAudioTrack> kept;
		kept.reserve(desc.audioTracks.size());
		for (VideoDescriptor::SAudioTrack& t : desc.audioTracks)
		{
			DescriptorTrackProblem problem;
			problem.name = t.name;
			if (!IsValidAudioTrackName(t.name.c_str()))
			{
				problem.kind = DescriptorTrackProblem::InvalidName;
				problems.push_back(problem);
				continue;
			}
			bool duplicate = false;
			for (const VideoDescriptor::SAudioTrack& k : kept)
				duplicate = duplicate || EqualsNoCase(k.name.c_str(), t.name.c_str());
			if (duplicate)
			{
				problem.kind = DescriptorTrackProblem::Duplicate;
				problems.push_back(problem);
				continue;
			}
			kept.push_back(std::move(t));
		}
		desc.audioTracks = std::move(kept);
		if (!desc.defaultTrack.empty() && FindDescriptorAudioTrack(desc, desc.defaultTrack.c_str()) < 0)
		{
			DescriptorTrackProblem problem;
			problem.kind = DescriptorTrackProblem::UnknownDefault;
			problem.name = desc.defaultTrack;
			problems.push_back(problem);
			desc.defaultTrack.clear();
		}
	}

	void SortDescriptorMarkers(VideoDescriptor& desc)
	{
		std::stable_sort(desc.markers.begin(), desc.markers.end(),
			[](const VideoDescriptor::SMarker& a, const VideoDescriptor::SMarker& b) { return a.time < b.time; });
	}

	void ResolveDescriptorAudio(const VideoDescriptor& desc, const char* requestedTrack, const char* const* gameLanguages,
		size_t gameLanguageCount, DescriptorFileExistsFn exists, void* user, DescriptorAudioPlan& out)
	{
		out = DescriptorAudioPlan();
		int index = -1;
		if (requestedTrack != nullptr && requestedTrack[0] != '\0')
		{
			index = FindDescriptorAudioTrack(desc, requestedTrack);
			if (index >= 0)
				out.reason = DescriptorTrackReason::Requested;
			else
				out.unknownRequest = true;
		}
		if (index < 0 && desc.followGameLanguage && gameLanguages != nullptr)
		{
			for (size_t i = 0; i < gameLanguageCount && index < 0; ++i)
				index = FindDescriptorAudioTrackByLanguage(desc, gameLanguages[i]);
			if (index >= 0)
				out.reason = DescriptorTrackReason::GameLanguage;
		}
		if (index < 0)
		{
			index = FindDescriptorAudioTrack(desc, desc.defaultTrack.c_str());
			if (index >= 0)
				out.reason = DescriptorTrackReason::Default;
		}
		if (index < 0 && !desc.audioTracks.empty())
		{
			index = 0;
			out.reason = DescriptorTrackReason::First;
		}

		if (desc.route == VideoRoute::Cue)
		{
			// every track may bring its own trigger, the root attributes are the defaults
			out.cue = true;
			out.trigger = desc.trigger;
			out.pauseTrigger = desc.pauseTrigger;
			out.resumeTrigger = desc.resumeTrigger;
			if (index >= 0)
			{
				const VideoDescriptor::SAudioTrack& cueTrack = desc.audioTracks[static_cast<size_t>(index)];
				out.track = cueTrack.name;
				if (!cueTrack.trigger.empty())
				{
					out.trigger = cueTrack.trigger;
					out.pauseTrigger = cueTrack.pauseTrigger;
					out.resumeTrigger = cueTrack.resumeTrigger;
				}
				else
				{
					if (!cueTrack.pauseTrigger.empty())
						out.pauseTrigger = cueTrack.pauseTrigger;
					if (!cueTrack.resumeTrigger.empty())
						out.resumeTrigger = cueTrack.resumeTrigger;
				}
			}
			return;
		}

		if (index < 0)
			return; // no tracks: the embedded audio
		const VideoDescriptor::SAudioTrack& track = desc.audioTracks[static_cast<size_t>(index)];
		out.track = track.name;
		if (desc.route != VideoRoute::Mixed)
			return; // route "embedded": the embedded audio whatever the track entry says

		if (!track.file.empty())
		{
			if (Exists(exists, user, track.file))
				out.externalPath = track.file;
			else
				out.missingExternal = track.file;
		}
		else if (!track.embedded && !track.trigger.empty())
		{
			// cue-only track of a mixed video: the audio comes from the trigger, the embedded audio is not played
			out.cue = true;
			out.trigger = track.trigger;
			out.pauseTrigger = track.pauseTrigger.empty() ? desc.pauseTrigger : track.pauseTrigger;
			out.resumeTrigger = track.resumeTrigger.empty() ? desc.resumeTrigger : track.resumeTrigger;
		}
	}

	DescriptorVariantChoice PickDescriptorVariant(const VideoDescriptor& desc, int forced, bool autoPick, int outputHeight,
		DescriptorFileExistsFn exists, void* user)
	{
		DescriptorVariantChoice out;
		out.videoPath = desc.video;
		const std::vector<VideoDescriptor::SVariant>& variants = desc.variants;
		if (variants.empty())
			return out;

		int chosen = -1; // index into variants
		if (forced >= 0)
		{
			if (forced >= 1 && static_cast<size_t>(forced) <= variants.size())
				chosen = forced - 1;
			else if (forced > 0)
				out.forcedOutOfRange = true;
		}
		else if (autoPick && outputHeight > 0) // unknown height (0): the main video
		{
			// smallest variant not below the output; none big enough: the main video (the biggest picture)
			for (size_t i = 0; i < variants.size(); ++i)
			{
				if (variants[i].height >= outputHeight && (chosen < 0 || variants[i].height < variants[static_cast<size_t>(chosen)].height))
					chosen = static_cast<int>(i);
			}
		}
		if (chosen >= 0 && !Exists(exists, user, variants[static_cast<size_t>(chosen)].file))
		{
			out.missingFile = variants[static_cast<size_t>(chosen)].file;
			chosen = -1;
		}
		if (chosen < 0)
			return out;
		const VideoDescriptor::SVariant& v = variants[static_cast<size_t>(chosen)];
		out.variant = chosen + 1;
		out.videoPath = v.file;
		out.width = v.width;
		out.height = v.height;
		return out;
	}
}
