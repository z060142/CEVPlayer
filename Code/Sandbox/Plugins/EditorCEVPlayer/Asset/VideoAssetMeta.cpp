#include "StdAfx.h"
#include "Asset/VideoAssetMeta.h"

#include <cstdio>
#include <cstdlib>

#include <AssetSystem/Asset.h>
#include <PathUtils.h>

namespace CEVEditor
{
	CDetails::CDetails(const CAsset& asset)
	{
		for (const auto& kv : asset.GetDetails())
			m_pairs.emplace_back(kv.first.c_str(), kv.second.c_str());
	}

	void CDetails::Set(const char* key, const std::string& value)
	{
		for (auto& kv : m_pairs)
		{
			if (kv.first == key)
			{
				kv.second = value;
				return;
			}
		}
		m_pairs.emplace_back(key, value);
	}

	void CDetails::SetInt(const char* key, int value)
	{
		Set(key, std::to_string(value));
	}

	void CDetails::SetDouble(const char* key, double value)
	{
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.6g", value);
		Set(key, buf);
	}

	std::string CDetails::Get(const char* key) const
	{
		for (const auto& kv : m_pairs)
		{
			if (kv.first == key)
				return kv.second;
		}
		return std::string();
	}

	bool CDetails::Has(const char* key) const
	{
		for (const auto& kv : m_pairs)
		{
			if (kv.first == key)
				return true;
		}
		return false;
	}

	int CDetails::GetInt(const char* key, int def) const
	{
		const std::string v = Get(key);
		return v.empty() ? def : std::atoi(v.c_str());
	}

	double CDetails::GetDouble(const char* key, double def) const
	{
		const std::string v = Get(key);
		return v.empty() ? def : std::atof(v.c_str());
	}

	std::vector<std::pair<string, string>> CDetails::ToPairs() const
	{
		std::vector<std::pair<string, string>> out;
		out.reserve(m_pairs.size());
		for (const auto& kv : m_pairs)
			out.emplace_back(kv.first.c_str(), kv.second.c_str());
		return out;
	}

	std::string JoinSettings(const std::vector<std::pair<std::string, std::string>>& kv)
	{
		std::string out;
		for (const auto& p : kv)
		{
			if (!out.empty())
				out += ';';
			out += p.first + "=" + p.second;
		}
		return out;
	}

	std::vector<std::pair<std::string, std::string>> SplitSettings(const std::string& text)
	{
		std::vector<std::pair<std::string, std::string>> out;
		for (const std::string& item : SplitList(text, ';'))
		{
			const size_t eq = item.find('=');
			if (eq == std::string::npos)
				out.emplace_back(item, std::string());
			else
				out.emplace_back(item.substr(0, eq), item.substr(eq + 1));
		}
		return out;
	}

	std::vector<std::string> SplitList(const std::string& text, char sep)
	{
		std::vector<std::string> out;
		size_t start = 0;
		while (start <= text.size())
		{
			size_t end = text.find(sep, start);
			if (end == std::string::npos)
				end = text.size();
			if (end > start)
				out.push_back(text.substr(start, end - start));
			start = end + 1;
		}
		return out;
	}

	std::string JoinList(const std::vector<std::string>& items, char sep)
	{
		std::string out;
		for (const std::string& s : items)
		{
			if (!out.empty())
				out += sep;
			out += s;
		}
		return out;
	}

	std::string AssetsAbsolute(const std::string& relPath)
	{
		std::string root = PathUtil::GetGameProjectAssetsPath().c_str();
		for (char& c : root)
		{
			if (c == '\\')
				c = '/';
		}
		if (!root.empty() && root.back() != '/')
			root += '/';
		std::string rel = relPath;
		for (char& c : rel)
		{
			if (c == '\\')
				c = '/';
		}
		while (!rel.empty() && rel.front() == '/')
			rel.erase(rel.begin());
		return root + rel;
	}

	std::string FolderOf(const std::string& path)
	{
		const size_t slash = path.find_last_of("/\\");
		return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
	}

	std::string FileNameOf(const std::string& path)
	{
		const size_t slash = path.find_last_of("/\\");
		return slash == std::string::npos ? path : path.substr(slash + 1);
	}

	std::string StemOf(const std::string& path)
	{
		const std::string file = FileNameOf(path);
		const size_t dot = file.find('.');
		return dot == std::string::npos ? file : file.substr(0, dot);
	}
}
