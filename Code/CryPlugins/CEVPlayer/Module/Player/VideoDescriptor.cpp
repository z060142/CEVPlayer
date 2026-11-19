#include "StdAfx.h"
#include "PluginEnv.h"
#include "VideoDescriptor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <CrySystem/File/ICryPak.h>

namespace CEVPlayer
{
	namespace
	{
		constexpr size_t kMaxAudioTracks = 64;
		constexpr size_t kMaxVariants = 32;
		constexpr size_t kMaxMarkers = 4096;

		const char* Attr(const XmlNodeRef& node, const char* key)
		{
			const char* const value = node->haveAttr(key) ? node->getAttr(key) : nullptr;
			return value != nullptr ? value : "";
		}

		// The whole string must be a finite number.
		bool ParseDouble(const char* text, double& value)
		{
			if (text == nullptr || text[0] == '\0')
				return false;
			char* end = nullptr;
			const double v = std::strtod(text, &end);
			if (end == text || *end != '\0' || !std::isfinite(v))
				return false;
			value = v;
			return true;
		}

		bool ParseBool(const char* text)
		{
			return text != nullptr && (std::strcmp(text, "1") == 0 || stricmp(text, "true") == 0 || stricmp(text, "yes") == 0);
		}

		bool FileExists(const std::string& path)
		{
			return gEnv != nullptr && gEnv->pCryPak != nullptr && gEnv->pCryPak->IsFileExist(path.c_str());
		}
	}

	bool IsDescriptorPath(const char* path)
	{
		if (path == nullptr)
			return false;
		const size_t len = std::strlen(path);
		constexpr size_t extLen = 9; // ".cevvideo"
		return len > extLen && stricmp(path + len - extLen, ".cevvideo") == 0;
	}

	const char* RouteName(EVideoRoute route)
	{
		return cev::VideoRouteName(route);
	}

	std::string ResolveSiblingPath(const char* basePath, const char* file)
	{
		if (file == nullptr || file[0] == '\0')
			return std::string();
		std::string dir = basePath != nullptr ? basePath : "";
		const size_t slash = dir.find_last_of("/\\");
		dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
		const std::string relative = dir + file;
		if (FileExists(relative))
			return relative;
		if (FileExists(file))
			return file;
		return relative; // does not exist: the caller reports it under this name
	}

	int FindAudioTrack(const SVideoDescriptor& desc, const char* name)
	{
		return cev::FindDescriptorAudioTrack(desc, name);
	}

	bool LoadVideoDescriptor(const char* path, SVideoDescriptor& out)
	{
		out = SVideoDescriptor();
		out.path = path != nullptr ? path : "";
		if (path == nullptr || path[0] == '\0' || gEnv == nullptr || gEnv->pSystem == nullptr)
			return false;

		XmlNodeRef root = gEnv->pSystem->LoadXmlFromFile(path);
		if (!root)
		{
			LogError("descriptor <%s>: cannot be read or is not well formed XML", path);
			return false;
		}
		if (stricmp(root->getTag(), "CEVVideo") != 0)
		{
			LogError("descriptor <%s>: the root element is <%s>, expected <CEVVideo>", path, root->getTag());
			return false;
		}

		// ---- root attributes
		if (root->haveAttr("version"))
		{
			double version = 1.0;
			if (!ParseDouble(root->getAttr("version"), version))
				LogWarning("descriptor <%s>: invalid version=\"%s\", assuming 1", path, root->getAttr("version"));
			else if (version > 1.0)
				LogWarning("descriptor <%s>: version %.0f is newer than this plug-in understands (1), reading what is known", path, version);
		}
		const char* const route = Attr(root, "route");
		if (route[0] == '\0' || stricmp(route, "embedded") == 0)
			out.route = EVideoRoute::Embedded;
		else if (stricmp(route, "mixed") == 0)
			out.route = EVideoRoute::Mixed;
		else if (stricmp(route, "cue") == 0)
			out.route = EVideoRoute::Cue;
		else
		{
			LogWarning("descriptor <%s>: unknown route=\"%s\" (embedded | mixed | cue), using embedded", path, route);
			out.route = EVideoRoute::Embedded;
		}

		const char* const video = Attr(root, "video");
		if (video[0] == '\0')
		{
			LogError("descriptor <%s>: the video=\"...\" attribute is missing, nothing to play", path);
			return false;
		}
		out.video = ResolveSiblingPath(path, video);

		if (root->haveAttr("avOffsetMs") && !ParseDouble(root->getAttr("avOffsetMs"), out.avOffsetMs))
		{
			LogWarning("descriptor <%s>: invalid avOffsetMs=\"%s\", using 0", path, root->getAttr("avOffsetMs"));
			out.avOffsetMs = 0.0;
		}

		out.trigger = Attr(root, "trigger");
		out.pauseTrigger = Attr(root, "pauseTrigger");
		out.resumeTrigger = Attr(root, "resumeTrigger");
		if (out.route == EVideoRoute::Cue && out.trigger.empty())
		{
			// not fatal: every <Track> may bring its own trigger
			LogDebug("descriptor <%s>: route=\"cue\" without a default trigger, audio tracks need their own trigger", path);
		}

		// ---- sections
		const int childCount = std::max(0, root->getChildCount());
		for (int c = 0; c < childCount; ++c)
		{
			XmlNodeRef section = root->getChild(c);
			if (!section)
				continue;
			const char* const tag = section->getTag();
			const int count = std::max(0, section->getChildCount());

			if (stricmp(tag, "AudioTracks") == 0)
			{
				out.defaultTrack = Attr(section, "default");
				out.followGameLanguage = section->haveAttr("followGameLanguage") && ParseBool(section->getAttr("followGameLanguage"));
				for (int i = 0; i < count; ++i)
				{
					XmlNodeRef node = section->getChild(i);
					if (!node || stricmp(node->getTag(), "Track") != 0)
						continue;
					if (out.audioTracks.size() >= kMaxAudioTracks)
					{
						LogWarning("descriptor <%s>: more than %u audio tracks, the rest is ignored", path, static_cast<unsigned>(kMaxAudioTracks));
						break;
					}
					SVideoDescriptor::SAudioTrack track;
					track.name = Attr(node, "name");
					track.language = Attr(node, "language");
					track.embedded = node->haveAttr("embedded") && ParseBool(node->getAttr("embedded"));
					const char* const file = Attr(node, "file");
					if (file[0] != '\0')
						track.file = ResolveSiblingPath(path, file);
					track.trigger = Attr(node, "trigger");
					track.pauseTrigger = Attr(node, "pauseTrigger");
					track.resumeTrigger = Attr(node, "resumeTrigger");
					if (!track.embedded && track.file.empty() && track.trigger.empty() && !(out.route == EVideoRoute::Cue && !out.trigger.empty()))
					{
						LogWarning("descriptor <%s>: audio track <%s> has no embedded / file / trigger, skipped", path, track.name.c_str());
						continue;
					}
					out.audioTracks.push_back(track);
				}
			}
			else if (stricmp(tag, "Languages") == 0)
			{
				// pre-A1 form: not read (never released); the video plays with its embedded audio
				LogError("descriptor <%s>: <Languages> is not supported any more: use <AudioTracks default=\"...\"> with "
					"<Track name=\"...\" embedded=\"1\" | file=\"...\" | trigger=\"...\" [language=\"...\"]/> "
					"(followGameLanguage=\"1\" to choose by the game language); playing the embedded audio", path);
			}
			else if (stricmp(tag, "Variants") == 0)
			{
				const char* const rule = Attr(section, "rule");
				if (rule[0] != '\0' && stricmp(rule, "smallestNotBelowOutput") != 0)
					LogWarning("descriptor <%s>: unknown variant rule <%s>, using smallestNotBelowOutput", path, rule);
				for (int i = 0; i < count; ++i)
				{
					XmlNodeRef node = section->getChild(i);
					if (!node || stricmp(node->getTag(), "Variant") != 0)
						continue;
					if (out.variants.size() >= kMaxVariants)
					{
						LogWarning("descriptor <%s>: more than %u variants, the rest is ignored", path, static_cast<unsigned>(kMaxVariants));
						break;
					}
					const char* const file = Attr(node, "file");
					int width = 0;
					int height = 0;
					const bool haveSize = node->getAttr("width", width) && node->getAttr("height", height)
						&& width > 0 && height > 0 && width <= 16384 && height <= 16384;
					if (file[0] == '\0' || !haveSize)
					{
						LogWarning("descriptor <%s>: <Variant file=\"%s\"> needs a file and a positive width / height, skipped", path, file);
						continue;
					}
					SVideoDescriptor::SVariant variant;
					variant.file = ResolveSiblingPath(path, file);
					variant.width = width;
					variant.height = height;
					out.variants.push_back(variant);
				}
			}
			else if (stricmp(tag, "Markers") == 0)
			{
				for (int i = 0; i < count; ++i)
				{
					XmlNodeRef node = section->getChild(i);
					if (!node || stricmp(node->getTag(), "Marker") != 0)
						continue;
					if (out.markers.size() >= kMaxMarkers)
					{
						LogWarning("descriptor <%s>: more than %u markers, the rest is ignored", path, static_cast<unsigned>(kMaxMarkers));
						break;
					}
					SVideoDescriptor::SMarker marker;
					marker.name = Attr(node, "name");
					double time = 0.0;
					if (marker.name.empty() || !ParseDouble(Attr(node, "time"), time) || time < 0.0)
					{
						LogWarning("descriptor <%s>: <Marker name=\"%s\" time=\"%s\"> needs a name and a time >= 0, skipped", path, marker.name.c_str(), Attr(node, "time"));
						continue;
					}
					marker.time = time;
					out.markers.push_back(marker);
				}
			}
			else
			{
				LogDebug("descriptor <%s>: unknown element <%s> ignored", path, tag);
			}
		}
		cev::SortDescriptorMarkers(out);

		std::vector<cev::DescriptorTrackProblem> problems;
		cev::SanitizeDescriptorAudioTracks(out, problems);
		for (const cev::DescriptorTrackProblem& problem : problems)
		{
			switch (problem.kind)
			{
			case cev::DescriptorTrackProblem::InvalidName:
				LogWarning("descriptor <%s>: audio track name <%s> is invalid (1 to %u of A-Z a-z 0-9 _ . -), skipped", path, problem.name.c_str(), static_cast<unsigned>(cev::kMaxAudioTrackNameLength));
				break;
			case cev::DescriptorTrackProblem::Duplicate:
				LogWarning("descriptor <%s>: audio track <%s> is listed twice, the first one is used", path, problem.name.c_str());
				break;
			case cev::DescriptorTrackProblem::UnknownDefault:
				LogWarning("descriptor <%s>: default audio track <%s> is not listed, ignored", path, problem.name.c_str());
				break;
			}
		}
		if (out.followGameLanguage && !out.audioTracks.empty())
		{
			bool tagged = false;
			for (const SVideoDescriptor::SAudioTrack& t : out.audioTracks)
				tagged = tagged || !t.language.empty();
			if (!tagged)
				LogWarning("descriptor <%s>: followGameLanguage=\"1\" but no audio track has a language=\"...\" tag", path);
		}
		return true;
	}
}
