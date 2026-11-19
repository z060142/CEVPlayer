#include "StdAfx.h"
#include "Editor/PreviewMedia.h"

#include <cstring>
#include <new>

#include <CrySystem/File/ICryPak.h>

#include "Asset/VideoDescriptorIO.h"

namespace CEVEditor
{
	namespace
	{
		bool Exists(const std::string& path)
		{
			return PreviewFileExists(path.c_str(), nullptr);
		}

		// Module/Player/VideoDescriptor.cpp ResolveSiblingPath
		std::string ResolveSibling(const std::string& basePath, const std::string& file)
		{
			if (file.empty())
				return std::string();
			const size_t slash = basePath.find_last_of("/\\");
			const std::string dir = slash == std::string::npos ? std::string() : basePath.substr(0, slash + 1);
			const std::string relative = dir + file;
			if (Exists(relative))
				return relative;
			if (Exists(file))
				return file;
			return relative;
		}

		cev::VideoRoute ParseRoute(const std::string& route)
		{
			if (stricmp(route.c_str(), "mixed") == 0)
				return cev::VideoRoute::Mixed;
			if (stricmp(route.c_str(), "cue") == 0)
				return cev::VideoRoute::Cue;
			return cev::VideoRoute::Embedded; // "", "embedded" and unknown values (the module warns and uses embedded)
		}
	}

	void CPreviewPakSource::EnsureLoaded() const
	{
		std::call_once(m_loadOnce, [this]
		{
			ICryPak* const pPak = gEnv != nullptr ? gEnv->pCryPak : nullptr;
			FILE* const file = pPak != nullptr ? pPak->FOpen(m_path.c_str(), "rb") : nullptr;
			if (file == nullptr)
			{
				m_status = cev::Status::IoError;
				return;
			}
			const size_t size = pPak->FGetSize(file);
			if (size > 0)
			{
				try
				{
					m_data.resize(size);
				}
				catch (const std::bad_alloc&)
				{
					pPak->FClose(file);
					m_data.clear();
					m_status = cev::Status::OutOfMemory;
					return;
				}
				if (pPak->FReadRaw(m_data.data(), 1, size, file) != size)
				{
					m_data.clear();
					m_status = cev::Status::IoError;
				}
			}
			pPak->FClose(file);
		});
	}

	uint64_t CPreviewPakSource::Length() const
	{
		EnsureLoaded();
		return m_data.size();
	}

	cev::Status CPreviewPakSource::Read(uint64_t offset, void* dst, size_t size)
	{
		EnsureLoaded();
		if (m_status != cev::Status::Ok)
			return m_status;
		if (offset > m_data.size() || size > m_data.size() - static_cast<size_t>(offset))
			return cev::Status::OutOfRange;
		if (size > 0)
			std::memcpy(dst, m_data.data() + static_cast<size_t>(offset), size);
		return cev::Status::Ok;
	}

	bool PreviewFileExists(const char* path, void*)
	{
		return path != nullptr && path[0] != '\0' && gEnv != nullptr && gEnv->pCryPak != nullptr && gEnv->pCryPak->IsFileExist(path);
	}

	cev::VideoDescriptor ToCoreDescriptor(const SDescriptor& desc, const std::string& descriptorRel)
	{
		cev::VideoDescriptor out;
		out.path = descriptorRel;
		out.route = ParseRoute(desc.route);
		out.video = ResolveSibling(descriptorRel, desc.video);
		out.avOffsetMs = desc.avOffsetMs;
		out.trigger = desc.trigger;
		out.pauseTrigger = desc.pauseTrigger;
		out.resumeTrigger = desc.resumeTrigger;
		out.defaultLanguage = desc.defaultLanguage;
		for (const SDescriptor::SLanguage& l : desc.languages)
		{
			if (l.name.empty() || cev::FindDescriptorLanguage(out, l.name.c_str()) >= 0)
				continue;
			cev::VideoDescriptor::SLanguage lang;
			lang.name = l.name;
			lang.embedded = l.embedded;
			lang.file = ResolveSibling(descriptorRel, l.file);
			lang.trigger = l.trigger;
			lang.pauseTrigger = l.pauseTrigger;
			lang.resumeTrigger = l.resumeTrigger;
			if (!lang.embedded && lang.file.empty() && lang.trigger.empty() && !(out.route == cev::VideoRoute::Cue && !out.trigger.empty()))
				continue;
			out.languages.push_back(lang);
		}
		for (const SDescriptor::SVariant& v : desc.variants)
		{
			if (v.file.empty() || v.width <= 0 || v.height <= 0 || v.width > 16384 || v.height > 16384)
				continue;
			cev::VideoDescriptor::SVariant variant;
			variant.file = ResolveSibling(descriptorRel, v.file);
			variant.width = v.width;
			variant.height = v.height;
			out.variants.push_back(variant);
		}
		for (const SDescriptor::SMarker& m : desc.markers)
		{
			if (m.name.empty() || !(m.time >= 0.0))
				continue;
			cev::VideoDescriptor::SMarker marker;
			marker.name = m.name;
			marker.time = m.time;
			out.markers.push_back(marker);
		}
		cev::SortDescriptorMarkers(out);
		if (!out.defaultLanguage.empty() && cev::FindDescriptorLanguage(out, out.defaultLanguage.c_str()) < 0)
			out.defaultLanguage.clear();
		return out;
	}
}
