#include "CEVCore/IByteSource.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif
	#ifndef NOMINMAX
		#define NOMINMAX
	#endif
	#include <windows.h>
#endif

namespace cev
{
	Status MemoryByteSource::Read(uint64_t offset, void* dst, size_t size)
	{
		if (size == 0)
			return Status::Ok;
		if (dst == nullptr)
			return Status::InvalidArgument;
		if (offset > m_size || size > m_size - static_cast<size_t>(offset))
			return Status::OutOfRange;
		std::memcpy(dst, m_view + offset, size);
		return Status::Ok;
	}

	namespace
	{
		std::FILE* OpenFile(const char* utf8Path)
		{
#if defined(_WIN32)
			wchar_t wide[1024];
			const int n = MultiByteToWideChar(CP_UTF8, 0, utf8Path, -1, wide, static_cast<int>(sizeof(wide) / sizeof(wide[0])));
			if (n <= 0)
				return nullptr;
			std::FILE* f = nullptr;
			if (_wfopen_s(&f, wide, L"rb") != 0)
				return nullptr;
			return f;
#else
			return std::fopen(utf8Path, "rb");
#endif
		}

		int Seek64(std::FILE* f, uint64_t pos)
		{
#if defined(_WIN32)
			return _fseeki64(f, static_cast<__int64>(pos), SEEK_SET);
#else
			return fseeko(f, static_cast<off_t>(pos), SEEK_SET);
#endif
		}
	}

	Status FileByteSource::Open(const char* utf8Path, bool preload, std::unique_ptr<IByteSource>& out)
	{
		out.reset();
		if (utf8Path == nullptr)
			return Status::InvalidArgument;

		std::FILE* f = OpenFile(utf8Path);
		if (f == nullptr)
			return Status::IoError;

		if (std::fseek(f, 0, SEEK_END) != 0)
		{
			std::fclose(f);
			return Status::IoError;
		}
#if defined(_WIN32)
		const long long len = _ftelli64(f);
#else
		const long long len = ftello(f);
#endif
		if (len < 0 || Seek64(f, 0) != 0)
		{
			std::fclose(f);
			return Status::IoError;
		}

		if (preload)
		{
			std::vector<uint8_t> data;
			try
			{
				data.resize(static_cast<size_t>(len));
			}
			catch (...)
			{
				std::fclose(f);
				return Status::OutOfMemory;
			}
			const size_t got = len > 0 ? std::fread(data.data(), 1, data.size(), f) : 0;
			std::fclose(f);
			if (got != data.size())
				return Status::IoError;
			try
			{
				out = std::make_unique<MemoryByteSource>(std::move(data));
			}
			catch (...)
			{
				return Status::OutOfMemory;
			}
			return Status::Ok;
		}

		try
		{
			std::unique_ptr<FileByteSource> src = std::make_unique<FileByteSource>(Key{});
			src->m_file = f;
			src->m_length = static_cast<uint64_t>(len);
			src->m_pos = 0;
			out = std::move(src);
		}
		catch (...)
		{
			std::fclose(f);
			return Status::OutOfMemory;
		}
		return Status::Ok;
	}

	FileByteSource::~FileByteSource()
	{
		if (m_file != nullptr)
			std::fclose(static_cast<std::FILE*>(m_file));
	}

	Status FileByteSource::Read(uint64_t offset, void* dst, size_t size)
	{
		if (size == 0)
			return Status::Ok;
		if (dst == nullptr || m_file == nullptr)
			return Status::InvalidArgument;
		if (offset > m_length || size > m_length - offset)
			return Status::OutOfRange;

		std::FILE* f = static_cast<std::FILE*>(m_file);
		if (offset != m_pos && Seek64(f, offset) != 0)
			return Status::IoError;
		const size_t got = std::fread(dst, 1, size, f);
		if (got != size)
		{
			m_pos = ~0ull; // unknown position, force a seek next time
			return Status::IoError;
		}
		m_pos = offset + size;
		return Status::Ok;
	}
}
