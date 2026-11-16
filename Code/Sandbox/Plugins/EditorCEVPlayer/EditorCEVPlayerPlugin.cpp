#include "StdAfx.h"
#include "EditorCEVPlayerPlugin.h"

#include <CryCore/Platform/platform_impl.inl>
#include <CrySystem/IConsole.h>
#include <CrySystem/ConsoleRegistration.h>

#include <AssetSystem/Asset.h>
#include <AssetSystem/AssetManager.h>
#include <IEditor.h>
#include <PathUtils.h>

#include <QFile>
#include <QIcon>

#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "AutoScript.h"
#include "Asset/VideoImport.h"
#include "Editor/VideoEditor.h"
#include "Editor/PreviewPanel.h"
#include "Editor/PreviewSession.h"
#include "Editor/VideoViewport.h"
#include "Jobs/JobRunner.h"
#include "Offline/IOfflineOps.h"
#include "Offline/OfflineOpsReal.h"

REGISTER_PLUGIN(CEditorCEVPlayerPlugin);

namespace
{
	CEVEditor::CAutoScript* g_pScript = nullptr;

	// eNotify_OnQuit: running jobs are asked to stop at once, so that joining their threads when the plug-in is released
	// does not hold the shutdown up (the threads are never detached).
	class CQuitListener final : public IAutoEditorNotifyListener
	{
	public:
		void OnEditorNotifyEvent(EEditorNotifyEvent event) override
		{
			if (event == eNotify_OnMainFrameInitialized)
			{
				CEVEditor::SetEditorReady(true); // panes created from now on may create their viewport at once
			}
			else if (event == eNotify_OnQuit)
			{
				CEVEditor::SetEditorReady(false);
				if (CEVEditor::CJobRunner* const pRunner = CEVEditor::CJobRunner::Get())
					pRunner->CancelAll();
			}
		}
	};
	CQuitListener* g_pQuitListener = nullptr;

	// cev_editor_import <absSrcPath> <assetFolderRelativeToAssets> [name=<asset>] [lang=<language>] [wav] [ogg] [split|strip] [cue|mixed]
	// Runs the importer without UI (the options of the dialog, defaults unless given) on the main thread.
	void CmdImport(IConsoleCmdArgs* pArgs)
	{
		using namespace CEVEditor;
		if (pArgs->GetArgCount() < 3)
		{
			CryLogAlways("[CEVEditor] usage: cev_editor_import <absSrcPath> <assetFolderRelativeToAssets> [name=<asset>|name:<asset>] [lang=<language>|lang:<language>] [wav] [ogg] [split|strip] [cue|mixed] [overwrite]");
			return;
		}
		const std::string source = pArgs->GetArg(1);
		const std::string folder = pArgs->GetArg(2);
		SImportOptions options;
		for (int i = 3; i < pArgs->GetArgCount(); ++i)
		{
			const std::string arg = pArgs->GetArg(i);
			// `name=x` works typed in the console; scripts and ExecuteString treat `a=b` as a cvar assignment, so `name:x` is accepted too
			if (arg.compare(0, 5, "name=") == 0 || arg.compare(0, 5, "name:") == 0) options.assetName = arg.substr(5);
			else if (arg.compare(0, 5, "lang=") == 0 || arg.compare(0, 5, "lang:") == 0) options.language = arg.substr(5);
			else if (arg == "wav") options.wav = true;
			else if (arg == "ogg") options.ogg = true;
			else if (arg == "split") options.split = ESplitAudio::KeepEmbedded;
			else if (arg == "strip") options.split = ESplitAudio::StripEmbedded;
			else if (arg == "cue") options.route = "cue";
			else if (arg == "mixed") options.route = "mixed";
			else if (arg == "overwrite") options.overwrite = true;
			else CryLogAlways("[CEVEditor] cev_editor_import: unknown option '%s'", arg.c_str());
		}
		if (source.empty())
		{
			CryLogAlways("[CEVEditor] cev_editor_import: empty source path");
			return;
		}
		// Console = non-interactive: an existing asset of that name is refused (no prompt) unless `overwrite` is given.
		const SImportResult result = ImportVideoFileNow(source, folder, options);
		if (!result.ok)
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] cev_editor_import failed: %s", result.error.c_str());
			return;
		}
		FinishImport(result, options);
		CryLogAlways("[CEVEditor] cev_editor_import ok: %s", result.metadataRel.c_str());
	}

	// cev_editor_open [<asset.cevvideo>]   (path relative to Assets, or absolute); without an argument only the window opens
	void CmdOpen(IConsoleCmdArgs* pArgs)
	{
		using namespace CEVEditor;
		if (pArgs->GetArgCount() < 2)
		{
			IPane* const pPane = GetIEditor()->CreateDockable(CVideoEditor::kEditorName);
			CryLogAlways("[CEVEditor] cev_editor_open: window %s", pPane != nullptr ? "opened" : "FAILED to open");
			return;
		}
		std::string path = pArgs->GetArg(1);
		for (char& c : path)
		{
			if (c == '\\')
				c = '/';
		}
		if (path.size() > 2 && path[1] == ':')
			path = PathUtil::AbsolutePathToGamePath(path.c_str()).c_str();
		CAssetManager* const pManager = CAssetManager::GetInstance();
		if (pManager == nullptr || path.empty())
			return;
		CAsset* pAsset = pManager->FindAssetForMetadata((path + ".cryasset").c_str());
		if (pAsset == nullptr)
			pAsset = pManager->FindAssetForMetadata(path.c_str());
		if (pAsset == nullptr)
			pAsset = pManager->FindAssetForFile(path.c_str());
		if (pAsset == nullptr)
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] cev_editor_open: no asset for '%s'", path.c_str());
			return;
		}
		if (pAsset->GetType() == nullptr || strcmp(pAsset->GetType()->GetTypeName(), "CEVVideo") != 0)
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] cev_editor_open: '%s' is not a video asset", path.c_str());
			return;
		}
		pAsset->Edit();
		CVideoEditor* const pEditor = CVideoEditor::Current();
		CryLogAlways("[CEVEditor] cev_editor_open: '%s' %s", pAsset->GetName().c_str(), pEditor != nullptr && pEditor->HasAsset() ? "opened in the tool window" : "did not open");
	}

	// The video asset an argument names: path of the .cevvideo / .cevvideo.cryasset relative to Assets (or absolute)
	CAsset* FindVideoAsset(const char* szArg, const char* szCommand)
	{
		std::string path = szArg != nullptr ? szArg : "";
		for (char& c : path)
		{
			if (c == '\\')
				c = '/';
		}
		if (path.size() > 2 && path[1] == ':')
			path = PathUtil::AbsolutePathToGamePath(path.c_str()).c_str();
		CAssetManager* const pManager = CAssetManager::GetInstance();
		CAsset* pAsset = nullptr;
		if (pManager != nullptr && !path.empty())
		{
			pAsset = pManager->FindAssetForMetadata((path + ".cryasset").c_str());
			if (pAsset == nullptr)
				pAsset = pManager->FindAssetForMetadata(path.c_str());
		}
		if (pAsset == nullptr || pAsset->GetType() == nullptr || std::strcmp(pAsset->GetType()->GetTypeName(), "CEVVideo") != 0)
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] %s: no video asset for '%s'", szCommand, path.c_str());
			return nullptr;
		}
		return pAsset;
	}

	// cev_editor_rename <asset.cevvideo> <newName>   (asset manager rename: files, descriptor, ACE registration)
	void CmdRename(IConsoleCmdArgs* pArgs)
	{
		if (pArgs->GetArgCount() < 3)
		{
			CryLogAlways("[CEVEditor] usage: cev_editor_rename <asset.cevvideo> <newName>");
			return;
		}
		CAsset* const pAsset = FindVideoAsset(pArgs->GetArg(1), "cev_editor_rename");
		if (pAsset == nullptr)
			return;
		const std::string oldName = pAsset->GetName().c_str();
		const bool ok = CAssetManager::GetInstance()->RenameAsset(pAsset, pArgs->GetArg(2));
		CryLogAlways("[CEVEditor] cev_editor_rename: '%s' -> '%s': %s", oldName.c_str(), pArgs->GetArg(2), ok ? "ok" : "FAILED");
	}

	// cev_editor_move <asset.cevvideo> <destinationFolderRelativeToAssets>
	void CmdMove(IConsoleCmdArgs* pArgs)
	{
		if (pArgs->GetArgCount() < 3)
		{
			CryLogAlways("[CEVEditor] usage: cev_editor_move <asset.cevvideo> <destinationFolder>");
			return;
		}
		CAsset* const pAsset = FindVideoAsset(pArgs->GetArg(1), "cev_editor_move");
		if (pAsset == nullptr)
			return;
		CAssetManager::GetInstance()->MoveAssets({ pAsset }, pArgs->GetArg(2));
		CryLogAlways("[CEVEditor] cev_editor_move: '%s' is now %s", pAsset->GetName().c_str(), pAsset->GetMetadataFile().c_str());
	}

	// cev_editor_delete <asset.cevvideo>   (asset manager delete: asset files, SDL copies, ACE library, sound assets)
	void CmdDelete(IConsoleCmdArgs* pArgs)
	{
		if (pArgs->GetArgCount() < 2)
		{
			CryLogAlways("[CEVEditor] usage: cev_editor_delete <asset.cevvideo>");
			return;
		}
		CAsset* const pAsset = FindVideoAsset(pArgs->GetArg(1), "cev_editor_delete");
		if (pAsset == nullptr)
			return;
		const std::string name = pAsset->GetName().c_str();
		CAssetManager::GetInstance()->DeleteAssetsWithFiles({ pAsset }); // the file operations run on the engine's queue
		CryLogAlways("[CEVEditor] cev_editor_delete: '%s' deletion started", name.c_str());
	}

	// cev_editor_close: closes the tool windows
	void CmdClose(IConsoleCmdArgs*)
	{
		using namespace CEVEditor;
		int closed = 0;
		for (int i = 0; i < 8; ++i)
		{
			IPane* const pPane = GetIEditor()->FindDockable(CVideoEditor::kEditorName);
			if (pPane == nullptr || pPane->GetWidget() == nullptr)
				break;
			QWidget* const pWidget = pPane->GetWidget();
			if (!pWidget->isVisible() && i > 0)
				break;
			// Close the tab pane that hosts the editor (QTabPane::closeEvent closes the editor and releases the pane in the
			// docking manager). Closing only the editor widget leaves an empty tool window behind and later crashes
			// in the tool window manager when that window is closed (seen in the first smoke run).
			QWidget* const pHost = pWidget->parentWidget();
			if (pHost != nullptr && std::strcmp(pHost->metaObject()->className(), "QTabPane") == 0)
				pHost->close();
			else
				pWidget->close();
			++closed;
			if (GetIEditor()->FindDockable(CVideoEditor::kEditorName) == pPane)
				break; // still there (hidden, deleted later): do not loop on it
		}
		CryLogAlways("[CEVEditor] cev_editor_close: closed %d window(s)", closed);
	}

	// cev_editor_status: one log line with the counters the smoke test checks
	void CmdStatus(IConsoleCmdArgs*)
	{
		using namespace CEVEditor;
		CryLogAlways("[CEVEditor] status: viewports live=%d created=%d destroyed=%d, jobs running=%d, offline ops: %s",
			CVideoViewport::LiveCount(), CVideoViewport::CreatedCount(), CVideoViewport::DestroyedCount(),
			CJobRunner::Get() != nullptr ? CJobRunner::Get()->RunningCount() : -1, GetOfflineOps().Name());
	}

	// cev_editor_script <file>: runs a list of console commands from the idle notification (see AutoScript.h)
	void CmdScript(IConsoleCmdArgs* pArgs)
	{
		if (pArgs->GetArgCount() < 2 || g_pScript == nullptr)
		{
			CryLogAlways("[CEVEditor] usage: cev_editor_script <file>");
			return;
		}
		g_pScript->Start(pArgs->GetArg(1));
	}
}

CEditorCEVPlayerPlugin::CEditorCEVPlayerPlugin()
{
	m_pJobs = std::make_unique<CEVEditor::CJobRunner>();
	m_pScript = std::make_unique<CEVEditor::CAutoScript>();
	g_pScript = m_pScript.get();
	g_pQuitListener = new CQuitListener();
	CEVEditor::RemoveStalePreviewFiles();

	// The offline library (CEVPlayerOffline, Offline/OfflineOpsReal.cpp) implements IOfflineOps; the stub of OfflineOps.cpp stays as
	// the fallback (SetOfflineOps(nullptr)).
	CEVEditor::SetOfflineOps(CEVEditor::GetRealOfflineOps());
	if (gEnv != nullptr && gEnv->pConsole != nullptr)
	{
		REGISTER_COMMAND("cev_editor_import", CmdImport, VF_NULL, "cev_editor_import <absSrcPath> <assetFolderRelativeToAssets> [name=..] [lang=..] [wav] [ogg] [split|strip] [cue|mixed]: import a WebM as a video asset without UI");
		REGISTER_COMMAND("cev_editor_open", CmdOpen, VF_NULL, "cev_editor_open [<asset.cevvideo>]: open the CEV Video Editor (on that asset)");
		REGISTER_COMMAND("cev_editor_close", CmdClose, VF_NULL, "cev_editor_close: close the CEV Video Editor windows");
		REGISTER_COMMAND("cev_editor_status", CmdStatus, VF_NULL, "cev_editor_status: log viewport / job counters of the CEV editor");
		REGISTER_COMMAND("cev_editor_rename", CmdRename, VF_NULL, "cev_editor_rename <asset.cevvideo> <newName>: rename a video asset through the asset manager");
		REGISTER_COMMAND("cev_editor_move", CmdMove, VF_NULL, "cev_editor_move <asset.cevvideo> <folder>: move a video asset through the asset manager");
		REGISTER_COMMAND("cev_editor_delete", CmdDelete, VF_NULL, "cev_editor_delete <asset.cevvideo>: delete a video asset with all its files");
		REGISTER_COMMAND("cev_editor_script", CmdScript, VF_NULL, "cev_editor_script <file>: run a list of console commands (one per line, wait_seconds N) from the editor idle loop");
	}
	// the asset type icon is a Qt resource of this DLL (EditorCEVPlayer.qrc); CryIcon would silently show the "missing" icon
	const bool iconOk = QFile::exists(QStringLiteral(":/EditorCEVPlayer/icons/assets_video.ico")) && !QIcon(QStringLiteral(":/EditorCEVPlayer/icons/assets_video.ico")).isNull();
	CryLogAlways("[CEVEditor] plugin loaded (EditorCEVPlayer 1, offline ops: %s, icon resource: %s)", CEVEditor::GetOfflineOps().Name(), iconOk ? "ok" : "MISSING");
	if (!iconOk)
		CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] icon resource :/EditorCEVPlayer/icons/assets_video.ico is missing");
}

CEditorCEVPlayerPlugin::~CEditorCEVPlayerPlugin()
{
	// Order: stop the script, then cancel + join the job threads (they use the offline ops and file I/O only), then the
	// commands and the offline ops go. No job thread may run once any of those is gone.
	if (m_pScript)
		m_pScript->Stop();
	g_pScript = nullptr;
	if (m_pJobs)
		m_pJobs->CancelAll();
	m_pJobs.reset();
	delete g_pQuitListener;
	g_pQuitListener = nullptr;

	if (gEnv != nullptr && gEnv->pConsole != nullptr)
	{
		IConsole* const pConsole = gEnv->pConsole;
		pConsole->RemoveCommand("cev_editor_import");
		pConsole->RemoveCommand("cev_editor_open");
		pConsole->RemoveCommand("cev_editor_close");
		pConsole->RemoveCommand("cev_editor_status");
		pConsole->RemoveCommand("cev_editor_rename");
		pConsole->RemoveCommand("cev_editor_move");
		pConsole->RemoveCommand("cev_editor_delete");
		pConsole->RemoveCommand("cev_editor_script");
	}
	g_pScript = nullptr;
	CEVEditor::SetOfflineOps(nullptr);
	CryLogAlways("[CEVEditor] plugin unloaded");
}
