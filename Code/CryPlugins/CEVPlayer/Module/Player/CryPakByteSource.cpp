#include "StdAfx.h"
#include "PluginEnv.h"
#include "CryPakByteSource.h"

#include <cstring>

#include <CrySystem/File/ICryPak.h>

namespace CEVPlayer
{
	CCryPakByteSource::CCryPakByteSource(const char* path)
		: m_path(path != nullptr ? path : "")
	{
	}

	void CCryPakByteSource::EnsureLoaded() const
	{
		std::call_once(m_loadOnce, [this] { Load(); });
	}

	void CCryPakByteSource::Load() const
	{
		ICryPak* const pPak = gEnv != nullptr ? gEnv->pCryPak : nullptr;
		if (pPak == nullptr)
		{
			m_status = cev::Status::IoError;
			return;
		}

		FILE* const file = pPak->FOpen(m_path.c_str(), "rb");
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
			const size_t read = pPak->FReadRaw(m_data.data(), 1, size, file);
			if (read != size)
			{
				m_data.clear();
				m_status = cev::Status::IoError;
			}
		}
		pPak->FClose(file);
	}

	uint64_t CCryPakByteSource::Length() const
	{
		EnsureLoaded();
		return m_data.size();
	}

	cev::Status CCryPakByteSource::Read(uint64_t offset, void* dst, size_t size)
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
}
