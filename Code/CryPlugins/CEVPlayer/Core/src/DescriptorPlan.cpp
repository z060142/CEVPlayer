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

	int FindDescriptorLanguage(const VideoDescriptor& desc, const char* name)
	{
		if (name == nullptr || name[0] == '\0')
			return -1;
		for (size_t i = 0; i < desc.languages.size(); ++i)
		{
			if (EqualsNoCase(desc.languages[i].name.c_str(), name))
				return static_cast<int>(i);
		}
		return -1;
	}

	void SortDescriptorMarkers(VideoDescriptor& desc)
	{
		std::stable_sort(desc.markers.begin(), desc.markers.end(),
			[](const VideoDescriptor::SMarker& a, const VideoDescriptor::SMarker& b) { return a.time < b.time; });
	}

	void ResolveDescriptorAudio(const VideoDescriptor& desc, const char* const* requests, size_t requestCount,
		DescriptorFileExistsFn exists, void* user, DescriptorAudioPlan& out)
	{
		out = DescriptorAudioPlan();
		int index = -1;
		for (size_t i = 0; i < requestCount && requests != nullptr; ++i)
		{
			index = FindDescriptorLanguage(desc, requests[i]);
			if (index >= 0)
				break;
		}

		if (desc.route == VideoRoute::Cue)
		{
			// every language may bring its own trigger, the root attributes are the defaults
			out.cue = true;
			out.trigger = desc.trigger;
			out.pauseTrigger = desc.pauseTrigger;
			out.resumeTrigger = desc.resumeTrigger;
			if (index >= 0)
			{
				const VideoDescriptor::SLanguage& cueLang = desc.languages[static_cast<size_t>(index)];
				out.language = cueLang.name;
				if (!cueLang.trigger.empty())
				{
					out.trigger = cueLang.trigger;
					out.pauseTrigger = cueLang.pauseTrigger;
					out.resumeTrigger = cueLang.resumeTrigger;
				}
				else
				{
					if (!cueLang.pauseTrigger.empty())
						out.pauseTrigger = cueLang.pauseTrigger;
					if (!cueLang.resumeTrigger.empty())
						out.resumeTrigger = cueLang.resumeTrigger;
				}
			}
			return;
		}

		if (index < 0)
			return; // no language matches: the embedded track
		const VideoDescriptor::SLanguage& lang = desc.languages[static_cast<size_t>(index)];
		out.language = lang.name;
		if (desc.route != VideoRoute::Mixed)
			return; // route "embedded": the embedded track whatever the language entry says

		if (!lang.file.empty())
		{
			if (Exists(exists, user, lang.file))
				out.externalPath = lang.file;
			else
				out.missingExternal = lang.file;
		}
		else if (!lang.embedded && !lang.trigger.empty())
		{
			// cue-only language of a mixed video: the audio comes from the trigger, the embedded track is not played
			out.cue = true;
			out.trigger = lang.trigger;
			out.pauseTrigger = lang.pauseTrigger.empty() ? desc.pauseTrigger : lang.pauseTrigger;
			out.resumeTrigger = lang.resumeTrigger.empty() ? desc.resumeTrigger : lang.resumeTrigger;
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
		else if (autoPick)
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
