// The IOfflineOps implementation over CEVPlayerOffline (Offline/OfflineOpsReal.cpp). The plug-in installs it at start up.
#pragma once

namespace CEVEditor
{
	struct IOfflineOps;
	IOfflineOps* GetRealOfflineOps();
}
