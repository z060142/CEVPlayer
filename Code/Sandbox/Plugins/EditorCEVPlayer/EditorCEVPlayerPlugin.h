// EditorCEVPlayer: Sandbox plug-in of the CEVPlayer video plug-in (video asset type, importer, tool window).
#pragma once

#include "IPlugin.h"

#include <memory>

#if defined(EditorCEVPlayer_EXPORTS)
	#define EDITOR_CEV_API DLL_EXPORT
#else
	#define EDITOR_CEV_API DLL_IMPORT
#endif

namespace CEVEditor
{
	class CJobRunner;
	class CAutoScript;
	class CFfmpegService;
	class CImportQueue;
}

class EDITOR_CEV_API CEditorCEVPlayerPlugin : public IPlugin
{
public:
	CEditorCEVPlayerPlugin();
	~CEditorCEVPlayerPlugin();

	int32       GetPluginVersion() { return 1; }
	const char* GetPluginName() { return "CEVPlayer"; }
	const char* GetPluginDescription() { return "CEVPlayer video asset type, importer and editor"; }

private:
	std::unique_ptr<CEVEditor::CJobRunner> m_pJobs;
	std::unique_ptr<CEVEditor::CFfmpegService> m_pFfmpeg;
	std::unique_ptr<CEVEditor::CImportQueue> m_pQueue;
	std::unique_ptr<CEVEditor::CAutoScript> m_pScript;
};
