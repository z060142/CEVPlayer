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

	// A1 test commit: stubs, implemented in the next commit
	bool IsValidAudioTrackName(const char*) { return false; }
	int FindDescriptorAudioTrack(const VideoDescriptor&, const char*) { return -1; }
	int FindDescriptorAudioTrackByLanguage(const VideoDescriptor&, const char*) { return -1; }
	void SanitizeDescriptorAudioTracks(VideoDescriptor&, std::vector<DescriptorTrackProblem>& problems) { problems.clear(); }

	void SortDescriptorMarkers(VideoDescriptor& desc)
	{
		std::stable_sort(desc.markers.begin(), desc.markers.end(),
			[](const VideoDescriptor::SMarker& a, const VideoDescriptor::SMarker& b) { return a.time < b.time; });
	}

	void ResolveDescriptorAudio(const VideoDescriptor&, const char*, const char* const*, size_t, DescriptorFileExistsFn, void*, DescriptorAudioPlan& out)
	{
		out = DescriptorAudioPlan();
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
