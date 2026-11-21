// IByteSource over CryPak. The whole file is preloaded into memory on first use, which happens on a decode
// worker thread (PlayerCore::Load opens and indexes the file there), so the main thread never waits for disk.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "CEVCore/IByteSource.h"

namespace CEVPlayer
{
	class CCryPakByteSource final : public cev::IByteSource
	{
	public:
		explicit CCryPakByteSource(const char* path);

		cev::Status Read(uint64_t offset, void* dst, size_t size) override;
		uint64_t Length() const override;

	private:
		void EnsureLoaded() const;
		void Load() const;

		std::string m_path;
		mutable std::vector<uint8_t> m_data;
		mutable std::once_flag m_loadOnce; // Length() / Read() may be the first call on different threads
		mutable cev::Status m_status = cev::Status::Ok;
	};
}
