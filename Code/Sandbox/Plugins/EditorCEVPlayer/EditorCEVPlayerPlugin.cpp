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

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "AutoScript.h"
#include "Asset/ImportQueue.h"
#include "Asset/VideoImport.h"
#include "Editor/VideoEditor.h"
#include "FFmpeg/FfmpegService.h"
#include "Editor/PreviewPanel.h"
#include "Editor/PreviewSession.h"
#include "Editor/VideoViewport.h"
#include "Jobs/JobRunner.h"
#include "Offline/IOfflineOps.h"
#include "Offline/OfflineOpsReal.h"
#include "Settings/EditorSettings.h"

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
				if (CEVEditor::CFfmpegService* const pService = CEVEditor::CFfmpegService::Get())
					pService->EnsureDetected(); // lazy detection of ffmpeg on a job thread, cached
			}
			else if (event == eNotify_OnQuit)
			{
				CEVEditor::SetEditorReady(false);
				if (CEVEditor::CImportQueue* const pQueue = CEVEditor::CImportQueue::Get())
					pQueue->CancelAll(true); // nothing queued starts any more, the running ffmpeg is cancelled
				if (CEVEditor::CJobRunner* const pRunner = CEVEditor::CJobRunner::Get())
				{
					pRunner->CancelAll();
					// the process can end right after this notification (plug-ins are not always unloaded): let the cancelled jobs
					// stop their ffmpeg and remove their partial files first (about half a second)
					if (!pRunner->WaitIdle(8000))
						CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] a job was still running 8 s after the quit request");
				}
			}
		}
	};
	CQuitListener* g_pQuitListener = nullptr;

	// Options of the import commands. Scripts and ExecuteString treat `a=b` as a cvar assignment, so every option also exists as
	// `key:value`:  name:<asset> lang:<language> crf:<0..63> height:<px> variants:<h,h,...> fps:<n>   and the words
	// wav ogg split strip cue mixed overwrite recompress. crf / height imply recompress (re-encode of a WebM).
	bool ParseImportOptions(IConsoleCmdArgs* pArgs, int first, CEVEditor::SImportOptions& options, const char* szCommand, bool* pSequences = nullptr)
	{
		using namespace CEVEditor;
		bool ok = true;
		for (int i = first; i < pArgs->GetArgCount(); ++i)
		{
			const std::string arg = pArgs->GetArg(i);
			const size_t sep = arg.find_first_of("=:");
			const std::string key = sep == std::string::npos ? arg : arg.substr(0, sep);
			const std::string value = sep == std::string::npos ? std::string() : arg.substr(sep + 1);
			if (sep != std::string::npos && key == "name") options.assetName = value;
			else if (sep != std::string::npos && key == "lang") options.language = value;
			else if (sep != std::string::npos && key == "crf")
			{
				options.crf = std::atoi(value.c_str());
				options.recompress = true;
			}
			else if (sep != std::string::npos && key == "height")
			{
				options.height = std::atoi(value.c_str());
				options.recompress = true;
			}
			else if (sep != std::string::npos && key == "fps") options.fps = std::atof(value.c_str());
			else if (sep != std::string::npos && key == "variants")
			{
				std::string error;
				if (!ParseHeightList(value, options.variants, error))
				{
					CryLogAlways("[CEVEditor] %s: %s", szCommand, error.c_str());
					ok = false;
				}
			}
			else if (arg == "wav") options.wav = true;
			else if (arg == "ogg") options.ogg = true;
			else if (arg == "split") options.split = ESplitAudio::KeepEmbedded;
			else if (arg == "strip") options.split = ESplitAudio::StripEmbedded;
			else if (arg == "cue") options.route = "cue";
			else if (arg == "mixed") options.route = "mixed";
			else if (arg == "overwrite") options.overwrite = true;
			else if (arg == "recompress") options.recompress = true;
			else if (arg == "sequences" && pSequences != nullptr) *pSequences = true;
			else
			{
				CryLogAlways("[CEVEditor] %s: unknown option '%s'", szCommand, arg.c_str());
				ok = false;
			}
		}
		return ok;
	}

	// cev_editor_import <absSrcPath|absPngFolder> <assetFolderRelativeToAssets> [options]   (see ParseImportOptions)
	// Runs the importer without UI (the options of the dialog, defaults unless given). A plain WebM copy runs right here; everything
	// that needs FFmpeg (mp4 / mov / mkv / avi, a folder of PNG files, re-encode, variants) goes through the import queue.
	void CmdImport(IConsoleCmdArgs* pArgs)
	{
		using namespace CEVEditor;
		if (pArgs->GetArgCount() < 3)
		{
			CryLogAlways("[CEVEditor] usage: cev_editor_import <absSrcPath|absPngFolder> <assetFolderRelativeToAssets> [name:<asset>] [lang:<language>] [wav] [ogg] [split|strip] [cue|mixed] [overwrite] [crf:N] [height:PX] [variants:540,360] [fps:N] [recompress]");
			return;
		}
		const std::string source = pArgs->GetArg(1);
		const std::string folder = pArgs->GetArg(2);
		SImportOptions options;
		if (!ParseImportOptions(pArgs, 3, options, "cev_editor_import"))
			return;
		if (source.empty())
		{
			CryLogAlways("[CEVEditor] cev_editor_import: empty source path");
			return;
		}
		ESourceKind kind = ESourceKind::Webm;
		if (!ClassifySource(source, kind))
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] cev_editor_import failed: '%s' is not an importable source (%s, or a folder of PNG files)", source.c_str(), ImportExtensionsText().c_str());
			return;
		}
		if (options.NeedsFfmpeg(kind))
		{
			CFfmpegService* const pService = CFfmpegService::Get();
			CImportQueue* const pQueue = CImportQueue::Get();
			if (pService == nullptr || pQueue == nullptr)
				return;
			const SFfmpegStatus status = pService->Status();
			if (!status.Pending() && !status.Ready())
			{
				// console = non-interactive: refuse with the reason (the dialog flow would ask to download / choose ffmpeg.exe)
				CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] cev_editor_import refused: %s", pService->UnavailableMessage().c_str());
				return;
			}
			pService->EnsureDetected();
			const int batch = pQueue->BeginBatch(QString::fromUtf8(source.c_str()));
			pQueue->Enqueue(batch, source, folder, options);
			pQueue->EndBatch(batch);
			CryLogAlways("[CEVEditor] cev_editor_import: '%s' queued (FFmpeg), the result is logged as 'import ok' / 'import failed'", source.c_str());
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

	// cev_editor_import_batch <absFolderOrListFile> <assetFolderRelativeToAssets> [options] [sequences]
	// A folder: every webm / mp4 / mov / mkv / avi in it (not recursive; `sequences`: its sub folders are PNG sequences). A list
	// file: one path (file or PNG folder) per line, `#` starts a comment. One queue, one file at a time, a summary at the end.
	void CmdImportBatch(IConsoleCmdArgs* pArgs)
	{
		using namespace CEVEditor;
		namespace fs = std::filesystem;
		if (pArgs->GetArgCount() < 3)
		{
			CryLogAlways("[CEVEditor] usage: cev_editor_import_batch <absFolderOrListFile> <assetFolderRelativeToAssets> [options as cev_editor_import, no name:] [sequences]");
			return;
		}
		const std::string input = pArgs->GetArg(1);
		const std::string folder = pArgs->GetArg(2);
		SImportOptions options;
		bool sequences = false;
		if (!ParseImportOptions(pArgs, 3, options, "cev_editor_import_batch", &sequences))
			return;
		options.assetName.clear(); // each file names its asset
		CImportQueue* const pQueue = CImportQueue::Get();
		if (pQueue == nullptr)
			return;
		std::vector<std::string> sources;
		std::error_code ec;
		const fs::path path = fs::u8path(input);
		if (fs::is_directory(path, ec))
		{
			std::vector<std::string> found;
			for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec))
			{
				const std::string full = it->path().u8string();
				ESourceKind kind = ESourceKind::Webm;
				std::error_code ec2;
				if (it->is_directory(ec2))
				{
					if (sequences)
						found.push_back(full);
				}
				else if (ClassifySource(full, kind) && kind != ESourceKind::PngSequence)
				{
					found.push_back(full);
				}
			}
			std::sort(found.begin(), found.end());
			sources = found;
		}
		else if (fs::is_regular_file(path, ec))
		{
			std::ifstream in(path);
			std::string line;
			while (std::getline(in, line))
			{
				while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
					line.pop_back();
				size_t start = 0;
				while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
					++start;
				line = line.substr(start);
				if (!line.empty() && line[0] != '#')
					sources.push_back(line);
			}
		}
		else
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] cev_editor_import_batch: '%s' is neither a folder nor a list file", input.c_str());
			return;
		}
		if (sources.empty())
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] cev_editor_import_batch: nothing to import in '%s'", input.c_str());
			return;
		}
		if (CFfmpegService* const pService = CFfmpegService::Get())
			pService->EnsureDetected();
		const int batch = pQueue->BeginBatch(QString::fromUtf8(input.c_str()));
		for (const std::string& source : sources)
			pQueue->Enqueue(batch, source, folder, options);
		pQueue->EndBatch(batch);
		CryLogAlways("[CEVEditor] cev_editor_import_batch: %u file(s) queued from '%s' (one at a time); the summary is logged at the end", static_cast<unsigned>(sources.size()), input.c_str());
	}

	// cev_editor_import_cancel: cancels the running import and drops the queued ones
	void CmdImportCancel(IConsoleCmdArgs*)
	{
		if (CEVEditor::CImportQueue* const pQueue = CEVEditor::CImportQueue::Get())
		{
			CryLogAlways("[CEVEditor] cev_editor_import_cancel: %d queued, running job %d", pQueue->Pending(), pQueue->RunningJob());
			pQueue->CancelAll(false);
		}
	}

	// cev_editor_ffmpeg_status [redetect]: one log line (+ rejected candidates) about the ffmpeg in use
	void CmdFfmpegStatus(IConsoleCmdArgs* pArgs)
	{
		using namespace CEVEditor;
		CFfmpegService* const pService = CFfmpegService::Get();
		if (pService == nullptr)
			return;
		if (pArgs->GetArgCount() > 1 && std::strcmp(pArgs->GetArg(1), "redetect") == 0)
			pService->StartDetect(true);
		else
			pService->EnsureDetected();
		const std::string reason = CFfmpegService::DownloadDisabledReason();
		CryLogAlways("[CEVEditor] %s\n  download of the minimal build: %s", DescribeStatus(pService->Status()).c_str(), reason.empty() ? "enabled" : ("disabled: " + reason).c_str());
	}

	// cev_editor_ffmpeg_path <path|->: sets the persistent ffmpeg path (file or folder; "-" clears it) and detects again
	void CmdFfmpegPath(IConsoleCmdArgs* pArgs)
	{
		using namespace CEVEditor;
		CFfmpegService* const pService = CFfmpegService::Get();
		if (pService == nullptr)
			return;
		if (pArgs->GetArgCount() < 2)
		{
			CryLogAlways("[CEVEditor] usage: cev_editor_ffmpeg_path <path to ffmpeg.exe or its folder | -> (stored in %s)", EditorSettingsPath().c_str());
			return;
		}
		std::string path = pArgs->GetArg(1);
		if (path == "-")
			path.clear();
		for (int i = 2; i < pArgs->GetArgCount(); ++i) // a path with spaces arrives as several arguments
			path += std::string(" ") + pArgs->GetArg(i);
		pService->SetPath(path);
		CryLogAlways("[CEVEditor] cev_editor_ffmpeg_path: %s, detecting (see the next 'ffmpeg status' line)", path.empty() ? "cleared" : path.c_str());
	}

	// cev_editor_ffmpeg_install_test <zip> <sha256> [version]: the download flow with a local zip instead of the network (hash check,
	// staging, install into %LOCALAPPDATA%\CEVPlayer\ffmpeg\<version>); a wrong hash must refuse with a clear message
	void CmdFfmpegInstallTest(IConsoleCmdArgs* pArgs)
	{
		using namespace CEVEditor;
		CFfmpegService* const pService = CFfmpegService::Get();
		if (pService == nullptr)
			return;
		if (pArgs->GetArgCount() < 3)
		{
			CryLogAlways("[CEVEditor] usage: cev_editor_ffmpeg_install_test <zipPath> <sha256> [version]");
			return;
		}
		std::string error;
		const std::string version = pArgs->GetArgCount() > 3 ? pArgs->GetArg(3) : "localtest";
		if (pService->StartInstallFromZip(pArgs->GetArg(1), pArgs->GetArg(2), version, error) == 0)
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] cev_editor_ffmpeg_install_test: %s", error.c_str());
		else
			CryLogAlways("[CEVEditor] cev_editor_ffmpeg_install_test: started");
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

	// cev_editor_variants <asset.cevvideo> <heights, e.g. 540,360> [crf:N]: generates the variants (FFmpeg, on a job)
	void CmdVariants(IConsoleCmdArgs* pArgs)
	{
		using namespace CEVEditor;
		if (pArgs->GetArgCount() < 3)
		{
			CryLogAlways("[CEVEditor] usage: cev_editor_variants <asset.cevvideo> <heights, e.g. 540,360> [crf:N]");
			return;
		}
		CAsset* const pAsset = FindVideoAsset(pArgs->GetArg(1), "cev_editor_variants");
		if (pAsset == nullptr)
			return;
		STranscodeOps ops;
		ops.changeVariants = true;
		std::string error;
		if (!ParseHeightList(pArgs->GetArg(2), ops.variants, error))
		{
			CryLogAlways("[CEVEditor] cev_editor_variants: %s", error.c_str());
			return;
		}
		for (int i = 3; i < pArgs->GetArgCount(); ++i)
		{
			const std::string arg = pArgs->GetArg(i);
			if (arg.compare(0, 4, "crf:") == 0 || arg.compare(0, 4, "crf=") == 0)
				ops.crf = std::atoi(arg.c_str() + 4);
		}
		if (SubmitTranscodeOps(*pAsset, ops, error) == 0)
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] cev_editor_variants refused: %s", error.c_str());
		else
			CryLogAlways("[CEVEditor] cev_editor_variants: started for '%s'", pAsset->GetName().c_str());
	}

	// cev_editor_recompress <asset.cevvideo> [crf:N] [height:PX] [variants:a,b] [refresh]: re-encodes the video from the source copy
	// (variants of the asset are made again; `variants:` replaces the set, `refresh` copies the original file over the source copy first)
	void CmdRecompress(IConsoleCmdArgs* pArgs)
	{
		using namespace CEVEditor;
		if (pArgs->GetArgCount() < 2)
		{
			CryLogAlways("[CEVEditor] usage: cev_editor_recompress <asset.cevvideo> [crf:N] [height:PX] [variants:a,b] [refresh]");
			return;
		}
		CAsset* const pAsset = FindVideoAsset(pArgs->GetArg(1), "cev_editor_recompress");
		if (pAsset == nullptr)
			return;
		STranscodeOps ops;
		ops.recompress = true;
		ops.changeVariants = true;
		SDescriptor desc;
		std::string error;
		if (LoadDescriptor(AssetsAbsolute(DescriptorRel(*pAsset)), desc, error))
			ops.variants = VariantHeights(desc);
		for (int i = 2; i < pArgs->GetArgCount(); ++i)
		{
			const std::string arg = pArgs->GetArg(i);
			const size_t sep = arg.find_first_of("=:");
			const std::string key = sep == std::string::npos ? arg : arg.substr(0, sep);
			const std::string value = sep == std::string::npos ? std::string() : arg.substr(sep + 1);
			if (key == "crf") ops.crf = std::atoi(value.c_str());
			else if (key == "height") ops.height = std::atoi(value.c_str());
			else if (key == "refresh") ops.refreshSource = true;
			else if (key == "variants")
			{
				ops.removeOtherVariants = true;
				if (value != "none" && !ParseHeightList(value, ops.variants, error))
				{
					CryLogAlways("[CEVEditor] cev_editor_recompress: %s", error.c_str());
					return;
				}
				if (value == "none")
					ops.variants.clear();
			}
			else
			{
				CryLogAlways("[CEVEditor] cev_editor_recompress: unknown option '%s'", arg.c_str());
				return;
			}
		}
		if (SubmitTranscodeOps(*pAsset, ops, error) == 0)
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] cev_editor_recompress refused: %s", error.c_str());
		else
			CryLogAlways("[CEVEditor] cev_editor_recompress: started for '%s'", pAsset->GetName().c_str());
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
		CryLogAlways("[CEVEditor] status: viewports live=%d created=%d destroyed=%d, jobs running=%d, import queue pending=%d, offline ops: %s",
			CVideoViewport::LiveCount(), CVideoViewport::CreatedCount(), CVideoViewport::DestroyedCount(),
			CJobRunner::Get() != nullptr ? CJobRunner::Get()->RunningCount() : -1, CImportQueue::Get() != nullptr ? CImportQueue::Get()->Pending() : -1, GetOfflineOps().Name());
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
	m_pFfmpeg = std::make_unique<CEVEditor::CFfmpegService>();
	m_pQueue = std::make_unique<CEVEditor::CImportQueue>();
	m_pScript = std::make_unique<CEVEditor::CAutoScript>();
	g_pScript = m_pScript.get();
	g_pQuitListener = new CQuitListener();
	CEVEditor::RemoveStalePreviewFiles();

	// The offline library (CEVPlayerOffline, Offline/OfflineOpsReal.cpp) implements IOfflineOps; the stub of OfflineOps.cpp stays as
	// the fallback (SetOfflineOps(nullptr)).
	CEVEditor::SetOfflineOps(CEVEditor::GetRealOfflineOps());
	if (gEnv != nullptr && gEnv->pConsole != nullptr)
	{
		REGISTER_COMMAND("cev_editor_import", CmdImport, VF_NULL, "cev_editor_import <absSrcPath|absPngFolder> <assetFolder> [name:..] [lang:..] [wav] [ogg] [split|strip] [cue|mixed] [overwrite] [crf:N] [height:PX] [variants:540,360] [fps:N] [recompress]: import a video (webm mp4 mov mkv avi) or a folder of PNG files as a video asset without UI");
		REGISTER_COMMAND("cev_editor_import_batch", CmdImportBatch, VF_NULL, "cev_editor_import_batch <absFolderOrListFile> <assetFolder> [options] [sequences]: import many files on one queue (one ffmpeg at a time) with a summary");
		REGISTER_COMMAND("cev_editor_import_cancel", CmdImportCancel, VF_NULL, "cev_editor_import_cancel: cancel the running import and drop the queued ones");
		REGISTER_COMMAND("cev_editor_ffmpeg_status", CmdFfmpegStatus, VF_NULL, "cev_editor_ffmpeg_status [redetect]: log the ffmpeg in use (path, version, missing features)");
		REGISTER_COMMAND("cev_editor_ffmpeg_path", CmdFfmpegPath, VF_NULL, "cev_editor_ffmpeg_path <path|->: set (or clear with -) the persistent ffmpeg path and detect again");
		REGISTER_COMMAND("cev_editor_ffmpeg_install_test", CmdFfmpegInstallTest, VF_NULL, "cev_editor_ffmpeg_install_test <zip> <sha256> [version]: run the download install path with a local zip (hash check, staging)");
		REGISTER_COMMAND("cev_editor_variants", CmdVariants, VF_NULL, "cev_editor_variants <asset.cevvideo> <heights> [crf:N]: generate variants with FFmpeg");
		REGISTER_COMMAND("cev_editor_recompress", CmdRecompress, VF_NULL, "cev_editor_recompress <asset.cevvideo> [crf:N] [height:PX] [variants:a,b|none] [refresh]: re-encode the video from its source copy");
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
	if (m_pQueue)
		m_pQueue->CancelAll(true);
	if (m_pJobs)
		m_pJobs->CancelAll();
	m_pJobs.reset();
	m_pQueue.reset();   // after the jobs: their completions (which use the queue / the service) never run once the runner is gone
	m_pFfmpeg.reset();
	delete g_pQuitListener;
	g_pQuitListener = nullptr;

	if (gEnv != nullptr && gEnv->pConsole != nullptr)
	{
		IConsole* const pConsole = gEnv->pConsole;
		pConsole->RemoveCommand("cev_editor_import");
		pConsole->RemoveCommand("cev_editor_import_batch");
		pConsole->RemoveCommand("cev_editor_import_cancel");
		pConsole->RemoveCommand("cev_editor_ffmpeg_status");
		pConsole->RemoveCommand("cev_editor_ffmpeg_path");
		pConsole->RemoveCommand("cev_editor_ffmpeg_install_test");
		pConsole->RemoveCommand("cev_editor_variants");
		pConsole->RemoveCommand("cev_editor_recompress");
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
