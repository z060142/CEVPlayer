// Stream information of a WebM file read with CEVPlayerCore's demuxer (container headers + packet index only, no
// decoding). It is the editor's own fallback for the info panel, the import Details and the frame rate of the
// transport bar; the full probe (every audio track, codec delay) is IOfflineOps::ProbeMedia.
#pragma once

#include "Offline/IOfflineOps.h"

#include <atomic>
#include <string>

namespace CEVEditor
{
	//! Safe on any thread (no engine access). `cancel` is polled while the packet index is built.
	bool ReadBasicMediaInfo(const std::string& absUtf8Path, SMediaInfo& out, std::string& error, const std::atomic<bool>* cancel = nullptr);
	//! The full probe of the offline library when it is wired (every audio track), else ReadBasicMediaInfo().
	//! Any thread (job thread).
	bool ProbeMediaFile(const std::string& absUtf8Path, SMediaInfo& out, std::string& error, const std::atomic<bool>* cancel = nullptr);
}
