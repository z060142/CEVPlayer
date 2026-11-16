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
}
