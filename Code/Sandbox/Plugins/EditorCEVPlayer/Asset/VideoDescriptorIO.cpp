#include "StdAfx.h"
#include "Asset/VideoDescriptorIO.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <CrySystem/ISystem.h>
#include <CrySystem/XML/IXml.h>

#include <CrySerialization/IArchive.h>
#include <CrySerialization/STL.h>
#include <CrySerialization/StringList.h>
#include <CrySerialization/Decorators/Range.h>
#include <CrySerialization/Decorators/Resources.h>
#include <CrySerialization/Decorators/ResourcesAudio.h>

namespace CEVEditor
{
	namespace
	{
		std::string Attr(const XmlNodeRef& node, const char* key)
		{
			const char* const value = node->haveAttr(key) ? node->getAttr(key) : nullptr;
			return value != nullptr ? value : "";
		}

		bool ParseDouble(const std::string& text, double& value)
		{
			if (text.empty())
				return false;
			char* end = nullptr;
			const double v = std::strtod(text.c_str(), &end);
			if (end == text.c_str() || *end != '\0')
				return false;
			value = v;
			return true;
		}

		std::string Escape(const std::string& s)
		{
			std::string out;
			out.reserve(s.size() + 8);
			for (char c : s)
			{
				switch (c)
				{
				case '&': out += "&amp;"; break;
				case '<': out += "&lt;"; break;
				case '>': out += "&gt;"; break;
				case '"': out += "&quot;"; break;
				default: out += c; break;
				}
			}
			return out;
		}

		void AppendAttr(std::string& out, const char* key, const std::string& value)
		{
			if (value.empty())
				return;
			out += ' ';
			out += key;
			out += "=\"";
			out += Escape(value);
			out += '"';
		}

		std::string FormatDouble(double v)
		{
			char buf[64];
			std::snprintf(buf, sizeof(buf), "%.6g", v);
			return buf;
		}

		bool ReplacePrefix(std::string& value, const std::string& oldPrefix, const std::string& newPrefix)
		{
			if (oldPrefix.empty() || value.size() < oldPrefix.size())
				return false;
			if (_strnicmp(value.c_str(), oldPrefix.c_str(), oldPrefix.size()) != 0)
				return false;
			value = newPrefix + value.substr(oldPrefix.size());
			return true;
		}
	}

	int SDescriptor::FindLanguage(const std::string& name) const
	{
		for (size_t i = 0; i < languages.size(); ++i)
		{
			if (_stricmp(languages[i].name.c_str(), name.c_str()) == 0)
				return static_cast<int>(i);
		}
		return -1;
	}

	bool LoadDescriptor(const std::string& absPath, SDescriptor& out, std::string& error)
	{
		out = SDescriptor();
		if (gEnv == nullptr || gEnv->pSystem == nullptr)
		{
			error = "the engine is not available";
			return false;
		}
		std::ifstream file(std::filesystem::u8path(absPath), std::ios::binary);
		if (!file)
		{
			error = "cannot open " + absPath;
			return false;
		}
		const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		XmlNodeRef root = gEnv->pSystem->LoadXmlFromBuffer(text.data(), text.size());
		if (!root)
		{
			error = "malformed XML in " + absPath;
			return false;
		}
		if (_stricmp(root->getTag(), "CEVVideo") != 0)
		{
			error = std::string("the root element is <") + root->getTag() + ">, expected <CEVVideo>";
			return false;
		}
		const std::string route = Attr(root, "route");
		out.route = route.empty() ? "embedded" : route;
		out.video = Attr(root, "video");
		ParseDouble(Attr(root, "avOffsetMs"), out.avOffsetMs);
		out.trigger = Attr(root, "trigger");
		out.pauseTrigger = Attr(root, "pauseTrigger");
		out.resumeTrigger = Attr(root, "resumeTrigger");

		const int childCount = root->getChildCount() > 0 ? root->getChildCount() : 0;
		for (int c = 0; c < childCount; ++c)
		{
			XmlNodeRef section = root->getChild(c);
			if (!section)
				continue;
			const char* const tag = section->getTag();
			const int count = section->getChildCount() > 0 ? section->getChildCount() : 0;
			if (_stricmp(tag, "Languages") == 0)
			{
				out.defaultLanguage = Attr(section, "default");
				for (int i = 0; i < count; ++i)
				{
					XmlNodeRef node = section->getChild(i);
					if (!node || _stricmp(node->getTag(), "Language") != 0)
						continue;
					SDescriptor::SLanguage lang;
					lang.name = Attr(node, "name");
					const std::string embedded = Attr(node, "embedded");
					lang.embedded = embedded == "1" || _stricmp(embedded.c_str(), "true") == 0 || _stricmp(embedded.c_str(), "yes") == 0;
					lang.file = Attr(node, "file");
					lang.trigger = Attr(node, "trigger");
					lang.pauseTrigger = Attr(node, "pauseTrigger");
					lang.resumeTrigger = Attr(node, "resumeTrigger");
					out.languages.push_back(lang);
				}
			}
			else if (_stricmp(tag, "Variants") == 0)
			{
				out.variantRule = Attr(section, "rule");
				for (int i = 0; i < count; ++i)
				{
					XmlNodeRef node = section->getChild(i);
					if (!node || _stricmp(node->getTag(), "Variant") != 0)
						continue;
					SDescriptor::SVariant variant;
					variant.file = Attr(node, "file");
					node->getAttr("width", variant.width);
					node->getAttr("height", variant.height);
					out.variants.push_back(variant);
				}
			}
			else if (_stricmp(tag, "Markers") == 0)
			{
				for (int i = 0; i < count; ++i)
				{
					XmlNodeRef node = section->getChild(i);
					if (!node || _stricmp(node->getTag(), "Marker") != 0)
						continue;
					SDescriptor::SMarker marker;
					marker.name = Attr(node, "name");
					ParseDouble(Attr(node, "time"), marker.time);
					out.markers.push_back(marker);
				}
			}
		}
		if (out.video.empty())
		{
			error = "the video attribute is missing";
			return false;
		}
		return true;
	}

	std::string DescriptorToXml(const SDescriptor& d)
	{
		std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CEVVideo version=\"1\"";
		AppendAttr(out, "route", d.route);
		AppendAttr(out, "video", d.video);
		out += " avOffsetMs=\"" + FormatDouble(d.avOffsetMs) + "\"";
		AppendAttr(out, "trigger", d.trigger);
		AppendAttr(out, "pauseTrigger", d.pauseTrigger);
		AppendAttr(out, "resumeTrigger", d.resumeTrigger);
		const bool hasChildren = !d.languages.empty() || !d.variants.empty() || !d.markers.empty();
		if (!hasChildren)
		{
			out += "/>\n";
			return out;
		}
		out += ">\n";
		if (!d.languages.empty())
		{
			out += "\t<Languages";
			AppendAttr(out, "default", d.defaultLanguage);
			out += ">\n";
			for (const SDescriptor::SLanguage& l : d.languages)
			{
				out += "\t\t<Language";
				AppendAttr(out, "name", l.name);
				if (l.embedded)
					out += " embedded=\"1\"";
				AppendAttr(out, "file", l.file);
				AppendAttr(out, "trigger", l.trigger);
				AppendAttr(out, "pauseTrigger", l.pauseTrigger);
				AppendAttr(out, "resumeTrigger", l.resumeTrigger);
				out += "/>\n";
			}
			out += "\t</Languages>\n";
		}
		if (!d.variants.empty())
		{
			out += "\t<Variants";
			AppendAttr(out, "rule", d.variantRule);
			out += ">\n";
			for (const SDescriptor::SVariant& v : d.variants)
			{
				out += "\t\t<Variant";
				AppendAttr(out, "file", v.file);
				out += " width=\"" + std::to_string(v.width) + "\" height=\"" + std::to_string(v.height) + "\"/>\n";
			}
			out += "\t</Variants>\n";
		}
		if (!d.markers.empty())
		{
			out += "\t<Markers>\n";
			for (const SDescriptor::SMarker& m : d.markers)
			{
				out += "\t\t<Marker";
				AppendAttr(out, "name", m.name);
				out += " time=\"" + FormatDouble(m.time) + "\"/>\n";
			}
			out += "\t</Markers>\n";
		}
		out += "</CEVVideo>\n";
		return out;
	}

	bool DescriptorsEqual(const SDescriptor& a, const SDescriptor& b)
	{
		return DescriptorToXml(a) == DescriptorToXml(b);
	}

	namespace
	{
		bool SameLanguage(const SDescriptor::SLanguage& a, const SDescriptor::SLanguage& b)
		{
			return a.name == b.name && a.embedded == b.embedded && a.file == b.file && a.trigger == b.trigger
				&& a.pauseTrigger == b.pauseTrigger && a.resumeTrigger == b.resumeTrigger;
		}

		template<typename T>
		const T& Pick(const T& base, const T& mine, const T& theirs)
		{
			return mine == base ? theirs : mine;
		}

		const SDescriptor::SLanguage* FindByName(const std::vector<SDescriptor::SLanguage>& list, const std::string& name)
		{
			for (const SDescriptor::SLanguage& l : list)
			{
				if (_stricmp(l.name.c_str(), name.c_str()) == 0)
					return &l;
			}
			return nullptr;
		}
	}

	SDescriptor MergeDescriptors(const SDescriptor& base, const SDescriptor& mine, const SDescriptor& theirs)
	{
		SDescriptor out;
		out.route = Pick(base.route, mine.route, theirs.route);
		out.video = Pick(base.video, mine.video, theirs.video);
		out.avOffsetMs = mine.avOffsetMs == base.avOffsetMs ? theirs.avOffsetMs : mine.avOffsetMs;
		out.trigger = Pick(base.trigger, mine.trigger, theirs.trigger);
		out.pauseTrigger = Pick(base.pauseTrigger, mine.pauseTrigger, theirs.pauseTrigger);
		out.resumeTrigger = Pick(base.resumeTrigger, mine.resumeTrigger, theirs.resumeTrigger);
		out.defaultLanguage = Pick(base.defaultLanguage, mine.defaultLanguage, theirs.defaultLanguage);
		out.variantRule = Pick(base.variantRule, mine.variantRule, theirs.variantRule);

		// variants / markers: whole list from whoever changed it (the user wins when both did)
		{
			SDescriptor probeBase, probeMine;
			probeBase.variants = base.variants;
			probeMine.variants = mine.variants;
			out.variants = DescriptorsEqual(probeBase, probeMine) ? theirs.variants : mine.variants;
			SDescriptor markerBase, markerMine;
			markerBase.markers = base.markers;
			markerMine.markers = mine.markers;
			out.markers = DescriptorsEqual(markerBase, markerMine) ? theirs.markers : mine.markers;
		}

		// languages by name
		for (const SDescriptor::SLanguage& m : mine.languages)
		{
			const SDescriptor::SLanguage* const b = FindByName(base.languages, m.name);
			const SDescriptor::SLanguage* const t = FindByName(theirs.languages, m.name);
			if (b != nullptr && SameLanguage(*b, m))
			{
				if (t != nullptr)
					out.languages.push_back(*t);   // untouched by the user: the file's version (a job may have filled the file / triggers)
				// else: the file dropped it and the user did not edit it
			}
			else
			{
				out.languages.push_back(m);        // edited or added by the user
			}
		}
		for (const SDescriptor::SLanguage& b : base.languages)
		{
			if (FindByName(mine.languages, b.name) != nullptr)
				continue;
			// the user removed it: stays removed unless the file changed that entry meanwhile
			const SDescriptor::SLanguage* const t = FindByName(theirs.languages, b.name);
			if (t != nullptr && !SameLanguage(*t, b))
				out.languages.push_back(*t);
		}
		for (const SDescriptor::SLanguage& t : theirs.languages)
		{
			if (FindByName(base.languages, t.name) == nullptr && FindByName(mine.languages, t.name) == nullptr)
				out.languages.push_back(t);        // added by a job
		}
		if (!out.defaultLanguage.empty() && out.FindLanguage(out.defaultLanguage) < 0)
			out.defaultLanguage.clear();
		return out;
	}

	bool SaveDescriptor(const std::string& absPath, const SDescriptor& desc, std::string& error)
	{
		const std::filesystem::path path = std::filesystem::u8path(absPath);
		const std::filesystem::path tmp = std::filesystem::u8path(absPath + ".tmp");
		{
			std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
			if (!file)
			{
				error = "cannot write " + absPath;
				return false;
			}
			const std::string xml = DescriptorToXml(desc);
			file.write(xml.data(), static_cast<std::streamsize>(xml.size()));
			if (!file)
			{
				error = "write error in " + absPath;
				return false;
			}
		}
		std::error_code ec;
		std::filesystem::rename(tmp, path, ec); // replaces an existing file on Windows
		if (ec)
		{
			std::filesystem::remove(tmp, ec);
			error = "cannot replace " + absPath;
			return false;
		}
		return true;
	}

	int RenameDescriptorFiles(SDescriptor& desc, const std::string& oldPrefix, const std::string& newPrefix)
	{
		int changes = 0;
		changes += ReplacePrefix(desc.video, oldPrefix, newPrefix) ? 1 : 0;
		for (SDescriptor::SLanguage& l : desc.languages)
			changes += ReplacePrefix(l.file, oldPrefix, newPrefix) ? 1 : 0;
		for (SDescriptor::SVariant& v : desc.variants)
			changes += ReplacePrefix(v.file, oldPrefix, newPrefix) ? 1 : 0;
		return changes;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Property tree (yasli) serialization of the edited fields. Strings go through CryString temporaries (what the decorators
	// take); a value is written back only on input.
	namespace
	{
		void SerializeString(yasli::Archive& ar, std::string& value, const char* name, const char* label)
		{
			string v = value.c_str();
			ar(v, name, label);
			if (ar.isInput())
				value = v.c_str();
		}

		void SerializeTrigger(yasli::Archive& ar, std::string& value, const char* name, const char* label)
		{
			string v = value.c_str();
			ar(Serialization::AudioTrigger(v), name, label); // the selector is registered by the Audio Controls Editor
			if (ar.isInput())
				value = v.c_str();
		}

		bool StartsWithNoCase(const std::string& text, const std::string& prefix)
		{
			return !prefix.empty() && text.size() >= prefix.size() && _strnicmp(text.c_str(), prefix.c_str(), prefix.size()) == 0;
		}
	}

	void SDescriptor::SLanguage::Serialize(yasli::Archive& ar)
	{
		SerializeString(ar, name, "name", "^Name");
		ar(embedded, "embedded", "Embedded audio track");
		// the file is stored relative to the descriptor; the picker shows / takes a game path
		const SDescriptorSerializeContext* const pContext = ar.context<SDescriptorSerializeContext>();
		const std::string folder = pContext != nullptr ? pContext->folderRel : std::string();
		string path = (file.empty() || file.find('/') != std::string::npos) ? string(file.c_str()) : string((folder + file).c_str());
		if (ar.isEdit())
			ar(Serialization::ResourceFilePath(path, "Audio (webm, wav, ogg)|*.webm;*.wav;*.ogg"), "file", "External audio file");
		else
			ar(path, "file", "External audio file");
		if (ar.isInput())
		{
			std::string p = path.c_str();
			for (char& ch : p)
			{
				if (ch == '\\')
					ch = '/';
			}
			if (StartsWithNoCase(p, folder) && p.find('/', folder.size()) == std::string::npos)
				p = p.substr(folder.size());
			file = p;
		}
		SerializeTrigger(ar, trigger, "trigger", "Start trigger");
		SerializeTrigger(ar, pauseTrigger, "pauseTrigger", "Pause trigger");
		SerializeTrigger(ar, resumeTrigger, "resumeTrigger", "Resume trigger");
	}

	void SDescriptor::SMarker::Serialize(yasli::Archive& ar)
	{
		SerializeString(ar, name, "name", "^Name");
		ar(Serialization::Range(time, 0.0, 1.0e6), "time", "^Time (s)");
	}

	void SDescriptor::Serialize(yasli::Archive& ar)
	{
		Serialization::StringList routes;
		routes.push_back("embedded");
		routes.push_back("mixed");
		routes.push_back("cue");
		Serialization::StringListValue routeValue(routes, route.c_str());
		ar(routeValue, "route", "Audio route");
		if (ar.isInput())
			route = routeValue.c_str();

		ar(Serialization::Range(avOffsetMs, -5000.0, 5000.0), "avOffsetMs", "A/V offset (ms)");
		ar.doc("Positive: the picture is shown later than the sound by this much.");

		Serialization::StringList names;
		names.push_back("");
		for (const SLanguage& l : languages)
			names.push_back(l.name.c_str());
		Serialization::StringListValue defaultValue(names, defaultLanguage.c_str());
		ar(defaultValue, "defaultLanguage", "Default language");
		if (ar.isInput())
			defaultLanguage = defaultValue.c_str();

		ar(languages, "languages", "Languages");
		ar(markers, "markers", "Markers");
	}
}
