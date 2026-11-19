// What the preview needs from the engine besides Qt: reading files (CryPak, like the runtime module, so files in a
// pak and loose files of the project both work) and turning the editor's descriptor model into the engine independent
// cev::VideoDescriptor that CEVCore/DescriptorPlan.h decides on.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "CEVCore/DescriptorPlan.h"
#include "CEVCore/IByteSource.h"

namespace CEVEditor
{
	struct SDescriptor;

	//! IByteSource over CryPak; the whole file is read on first use, which happens on a decode worker (PlayerCore::Load
	//! opens the file there), so the main thread never waits for the disk and no file handle stays open afterwards.
	//! Same behaviour as the runtime module's CCryPakByteSource (Module/Player/CryPakByteSource.*), which the editor
	//! cannot compile in (it belongs to the module's precompiled header).
	class CPreviewPakSource final : public cev::IByteSource
	{
	public:
		explicit CPreviewPakSource(const std::string& path) : m_path(path) {}
		cev::Status Read(uint64_t offset, void* dst, size_t size) override;
		uint64_t Length() const override;

	private:
		void EnsureLoaded() const;

		std::string m_path;
		mutable std::vector<uint8_t> m_data;
		mutable std::once_flag m_loadOnce;
		mutable cev::Status m_status = cev::Status::Ok;
	};

	//! cev::DescriptorFileExistsFn over CryPak (game relative paths).
	bool PreviewFileExists(const char* path, void* user);

	//! The editor descriptor (file names relative to the descriptor's folder) as the runtime module would see it after
	//! reading `descriptorRel` (game relative): file names resolved like ResolveSiblingPath of the module, the same
	//! languages skipped (unnamed, listed twice, without embedded / file / trigger), an unlisted default language
	//! cleared, markers sorted. Limits of the module's reader (counts, sizes) are not repeated here.
	cev::VideoDescriptor ToCoreDescriptor(const SDescriptor& desc, const std::string& descriptorRel);
}
