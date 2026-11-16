// Random access byte source. CRYENGINE side implements it on top of CryPak, the selftest uses stdio.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "CEVCore/Status.h"

namespace cev
{
	class IByteSource
	{
	public:
		virtual ~IByteSource() = default;

		// Reads exactly `size` bytes starting at `offset`. Returns OutOfRange when the range is not fully
		// inside the source and IoError on a device error. Calls are never concurrent for one source
		// (but may come from different threads one after another).
		virtual Status Read(uint64_t offset, void* dst, size_t size) = 0;
		virtual uint64_t Length() const = 0;
	};

	// Source over memory. Either a non-owning view or an owning vector (whole-file preload).
	class MemoryByteSource final : public IByteSource
	{
	public:
		MemoryByteSource(const uint8_t* data, size_t size) : m_view(data), m_size(size) {}
		explicit MemoryByteSource(std::vector<uint8_t>&& data)
			: m_owned(std::move(data)), m_view(m_owned.data()), m_size(m_owned.size()) {}

		Status Read(uint64_t offset, void* dst, size_t size) override;
		uint64_t Length() const override { return m_size; }

	private:
		std::vector<uint8_t> m_owned;
		const uint8_t* m_view;
		size_t m_size;
	};

	// stdio based file source (UTF-8 path). preload = true reads the whole file into memory and returns a
	// MemoryByteSource instead (no file handle stays open).
	class FileByteSource final : public IByteSource
	{
	public:
		static Status Open(const char* utf8Path, bool preload, std::unique_ptr<IByteSource>& out);

		struct Key { explicit Key() = default; }; // construct through Open() only
		explicit FileByteSource(Key) {}
		~FileByteSource() override;
		Status Read(uint64_t offset, void* dst, size_t size) override;
		uint64_t Length() const override { return m_length; }

	private:
		void* m_file = nullptr; // FILE*
		uint64_t m_length = 0;
		uint64_t m_pos = 0;
	};
}
