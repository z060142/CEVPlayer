#include "StdAfx.h"
#include "EditorCEVPlayerPlugin.h"

#include <CryCore/Platform/platform_impl.inl>
#include <CrySystem/IConsole.h>
#include <CrySystem/ConsoleRegistration.h>

#include <AssetSystem/Asset.h>
#include <AssetSystem/AssetManager.h>
#include <IEditor.h>
#include <IResourceSelectorHost.h>
#include <PathUtils.h>
#include <CrySerialization/IArchiveHost.h>

#include <QFile>
#include <QIcon>
#include <QPixmap>
#include <QPointer>
#include <QScrollArea>
#include <QTimer>

#include <QToolWindowManager/QToolWindowManager.h>
#include <QToolWindowManager/IToolWindowArea.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iterator>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

#include "AutoScript.h"
#include "Asset/ImportQueue.h"
#include "Asset/Sha256.h"
#include "Asset/VideoImport.h"
#include "Editor/VideoEditor.h"
#include "FFmpeg/FfmpegService.h"
#include "Editor/PreviewPanel.h"
#include "Editor/InfoPanel.h"
#include "Editor/PreviewAudio.h"
#include "Editor/PreviewSession.h"
#include "Editor/VideoPreviewWidget.h"
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
				if (CEVEditor::CFfmpegService* const pService = CEVEditor::CFfmpegService::Get())
					pService->EnsureDetected(); // lazy detection of ffmpeg on a job thread, cached
			}
			else if (event == eNotify_OnIdleUpdate && !m_preferencesReady)
			{
				// the first idle update comes after the editor loaded the preferences (CEditorImpl::InitFinished): the old
				// settings file can be imported into the preference page now, and the FFmpeg service takes the value over
				m_preferencesReady = true;
				CEVEditor::MigrateLegacyEditorSettings();
				if (CEVEditor::CFfmpegService* const pService = CEVEditor::CFfmpegService::Get())
				{
					pService->ReloadConfiguredPath();
					pService->EnsureDetected();
				}
			}
			else if (event == eNotify_OnQuit)
			{
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

	private:
		bool m_preferencesReady = false;
	};
	CQuitListener* g_pQuitListener = nullptr;

	// Options of the import commands. Scripts and ExecuteString treat `a=b` as a cvar assignment, so every option also exists as
	// `key:value`:  name:<asset> track:<audio track name> crf:<0..63> height:<px> variants:<h,h,...> fps:<n>   and the words
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
			else if (sep != std::string::npos && key == "track") options.audioTrack = value;
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
			CryLogAlways("[CEVEditor] usage: cev_editor_import <absSrcPath|absPngFolder> <assetFolderRelativeToAssets> [name:<asset>] [track:<audio track>] [wav] [ogg] [split|strip] [cue|mixed] [overwrite] [crf:N] [height:PX] [variants:540,360] [fps:N] [recompress]");
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
			CryLogAlways("[CEVEditor] usage: cev_editor_ffmpeg_path <path to ffmpeg.exe or its folder | -> (stored in the editor preferences, %s)", CEVEditor::kPreferencesPagePath);
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

	// cev_editor_ffmpeg_download: test hook for the Info panel's "Download minimal build" button; calls the same
	// CFfmpegService::StartDownload (WinHTTP fetch of the pinned GitHub release, SHA-256 check, install into
	// %LOCALAPPDATA%\CEVPlayer\ffmpeg\<version>, then a new detection)
	void CmdFfmpegDownload(IConsoleCmdArgs*)
	{
		using namespace CEVEditor;
		CFfmpegService* const pService = CFfmpegService::Get();
		if (pService == nullptr)
			return;
		std::string error;
		if (pService->StartDownload(error) == 0)
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] cev_editor_ffmpeg_download: %s", error.c_str());
		else
			CryLogAlways("[CEVEditor] cev_editor_ffmpeg_download: started");
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
		CVideoEditor* const pEditor = CVideoEditor::Current();
		CVideoPreviewPanel* const pPanel = pEditor != nullptr ? pEditor->PreviewPanel() : nullptr;
		CryLogAlways("[CEVEditor] status: preview timers=%d decode pools=%d audio callbacks=%llu, panel %s (visible %d, timer %d, ready %d, state %d, serial %llu, paints %llu), jobs running=%d, import queue pending=%d, offline ops: %s",
			CVideoPreviewPanel::ActiveTimers(), CPreviewSession::LivePools(), static_cast<unsigned long long>(CPreviewAudioSource::TotalCallbacks()),
			pPanel != nullptr ? "yes" : "no", pPanel != nullptr && pPanel->isVisible() ? 1 : 0, pPanel != nullptr && pPanel->TimerActive() ? 1 : 0,
			pPanel != nullptr && pPanel->Session().IsReady() ? 1 : 0, pPanel != nullptr ? static_cast<int>(pPanel->Session().State()) : -1,
			static_cast<unsigned long long>(pPanel != nullptr ? pPanel->Session().FrameSerial() : 0),
			static_cast<unsigned long long>(pPanel != nullptr ? pPanel->PictureWidget()->PaintCount() : 0),
			CJobRunner::Get() != nullptr ? CJobRunner::Get()->RunningCount() : -1, CImportQueue::Get() != nullptr ? CImportQueue::Get()->Pending() : -1, GetOfflineOps().Name());
	}

	// ---- cev_editor_preview_test [png:<path>] [pause:<seconds>] [track:<name>] [close] ----
	// The preview of the open video editor: loads (waits for ready), plays 1 s (the frame serial must advance, the audio device
	// must pull PCM unless there is no device / no audio), pauses (no paintEvent during the pause), optionally saves the picture
	// widget as PNG, optionally closes the editor windows and checks that no preview timer and no decode thread is left.
	// Runs from Qt timers (the console command returns at once); the result is one PASS / FAIL line.
	struct SPreviewTest
	{
		QPointer<CEVEditor::CVideoPreviewPanel> panel;
		std::string png;
		double pauseSeconds = 5.0;
		bool close = false;
		std::string track;       // A1: switch to this audio track while playing (clock continuous, sound still pulled)
		bool trackDone = false;
		double trackTime0 = 0.0;
		double trackWall0 = 0.0;
		uint64_t trackCallbacks0 = 0;
		int checks = 0;
		int failed = 0;
		int waitedMs = 0;
		uint64_t serial0 = 0;
		uint64_t callbacks0 = 0;
		uint64_t paints0 = 0;

		void Check(bool ok, const char* what, const std::string& detail = std::string())
		{
			++checks;
			if (!ok)
				++failed;
			CryLogAlways("[CEVEditor] preview_test: %s %s%s%s", ok ? "ok  " : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
		}
	};
	bool g_previewTestRunning = false;

	void PreviewTestEnd(const std::shared_ptr<SPreviewTest>& t)
	{
		CryLogAlways("[CEVEditor] preview_test: %s (%d checks, %d failed)", t->failed == 0 ? "PASS" : "FAIL", t->checks, t->failed);
		g_previewTestRunning = false;
	}

	void PreviewTestAfterClose(std::shared_ptr<SPreviewTest> t)
	{
		using namespace CEVEditor;
		t->Check(CVideoPreviewPanel::ActiveTimers() == 0, "no preview timer after closing the window", "timers " + std::to_string(CVideoPreviewPanel::ActiveTimers()));
		t->Check(CPreviewSession::LivePools() == 0, "no preview decode threads after closing the window", "decode pools " + std::to_string(CPreviewSession::LivePools()));
		PreviewTestEnd(t);
	}

	void PreviewTestAfterPause(std::shared_ptr<SPreviewTest> t)
	{
		using namespace CEVEditor;
		if (!t->panel)
		{
			t->Check(false, "panel still exists after the pause");
			PreviewTestEnd(t);
			return;
		}
		const uint64_t paints = t->panel->PictureWidget()->PaintCount();
		const std::string seconds = QString::number(t->pauseSeconds, 'f', 1).toStdString();
		t->Check(paints == t->paints0, "no paintEvent while paused", std::to_string(paints - t->paints0) + " paints in " + seconds + " s");
		if (!t->png.empty())
		{
			const QPixmap shot = t->panel->PictureWidget()->grab();
			const bool saved = !shot.isNull() && shot.save(QString::fromUtf8(t->png.c_str()), "PNG");
			t->Check(saved, "picture saved", t->png + " (" + std::to_string(shot.width()) + "x" + std::to_string(shot.height()) + ")");
		}
		if (!t->close)
		{
			PreviewTestEnd(t);
			return;
		}
		gEnv->pConsole->ExecuteString("cev_editor_close");
		QTimer::singleShot(1500, [t]() { PreviewTestAfterClose(t); });
	}

	void PreviewTestAfterPlay(std::shared_ptr<SPreviewTest> t);

	double PreviewTestWall()
	{
		return gEnv != nullptr && gEnv->pTimer != nullptr ? gEnv->pTimer->GetAsyncCurTime() : 0.0;
	}

	//! A1: 1 s after the switch: the new track is in use, the clock went on from where it was, the device still pulls PCM.
	void PreviewTestAfterTrackSwitch(std::shared_ptr<SPreviewTest> t)
	{
		using namespace CEVEditor;
		if (!t->panel)
		{
			t->Check(false, "panel still exists after the audio track switch");
			PreviewTestEnd(t);
			return;
		}
		CPreviewSession& session = t->panel->Session();
		if (!session.IsReady() && session.IsLoading() && t->waitedMs < 15000)
		{
			t->waitedMs += 100;
			QTimer::singleShot(100, [t]() { PreviewTestAfterTrackSwitch(t); });
			return;
		}
		t->waitedMs = 0;
		const double elapsed = PreviewTestWall() - t->trackWall0;
		const double time = session.Time();
		t->Check(_stricmp(session.AudioTrack().c_str(), t->track.c_str()) == 0, "audio track switched", session.AudioTrack() + session.Notes());
		t->Check(session.State() == cev::PlayerState::Playing, "still playing after the switch", "state " + std::to_string(static_cast<int>(session.State())));
		// continuous: from the time at the switch onwards, not back at 0 (loop off; the clip is longer than the test)
		t->Check(time > t->trackTime0 + 0.2 && time < t->trackTime0 + elapsed + 0.5, "clock continuous across the switch",
			std::to_string(t->trackTime0) + " s -> " + std::to_string(time) + " s in " + std::to_string(elapsed) + " s");
		const uint64_t callbacks = CPreviewAudioSource::TotalCallbacks() - t->trackCallbacks0;
		if (!session.HasAudio())
			t->Check(false, "audio after the switch", "the media has no audio");
		else if (!session.AudioDeviceRunning())
			t->Check(true, "audio after the switch", "no audio device (" + session.AudioDeviceError() + "): playing silently");
		else
			t->Check(callbacks > 0, "audio device still pulls PCM after the switch", std::to_string(callbacks) + " callbacks");
		t->trackDone = true;
		PreviewTestAfterPlay(t);
	}

	void PreviewTestAfterPlay(std::shared_ptr<SPreviewTest> t)
	{
		using namespace CEVEditor;
		if (!t->panel)
		{
			t->Check(false, "panel still exists after playing");
			PreviewTestEnd(t);
			return;
		}
		CPreviewSession& session = t->panel->Session();
		const uint64_t serial = session.FrameSerial();
		t->Check(serial > t->serial0, "frame serial advances while playing", std::to_string(t->serial0) + " -> " + std::to_string(serial));
		const uint64_t callbacks = CPreviewAudioSource::TotalCallbacks() - t->callbacks0;
		if (!session.HasAudio())
			t->Check(true, "audio", "the media has no audio");
		else if (!session.AudioDeviceRunning())
			t->Check(true, "audio", "no audio device (" + session.AudioDeviceError() + "): playing silently");
		else if (!t->trackDone)
			t->Check(callbacks > 0, "audio device pulls PCM", std::to_string(callbacks) + " callbacks in 1 s");
		if (!t->track.empty() && !t->trackDone)
		{
			const std::vector<std::string>& names = session.AudioTrackNames();
			const bool listed = std::any_of(names.begin(), names.end(), [t](const std::string& n) { return _stricmp(n.c_str(), t->track.c_str()) == 0; });
			t->Check(listed, "the Audio track list offers the requested track", t->track);
			t->trackTime0 = session.Time();
			t->trackWall0 = PreviewTestWall();
			t->trackCallbacks0 = CPreviewAudioSource::TotalCallbacks();
			const bool selected = t->panel->SelectAudioTrack(t->track);
			t->Check(selected, "audio track selected in the preview's Audio track list", t->track);
			QTimer::singleShot(1000, [t]() { PreviewTestAfterTrackSwitch(t); });
			return;
		}
		t->Check(t->panel->TimerActive(), "preview timer runs while visible");
		t->panel->PlayPause(); // pause
		t->Check(session.State() == cev::PlayerState::Paused, "paused", "state " + std::to_string(static_cast<int>(session.State())));
		// let the last frame / the smooth repaint of the pause land, then count
		QTimer::singleShot(500, [t]()
		{
			if (t->panel)
				t->paints0 = t->panel->PictureWidget()->PaintCount();
			QTimer::singleShot(static_cast<int>(t->pauseSeconds * 1000.0), [t]() { PreviewTestAfterPause(t); });
		});
	}

	void PreviewTestWaitReady(std::shared_ptr<SPreviewTest> t)
	{
		using namespace CEVEditor;
		if (!t->panel)
		{
			t->Check(false, "panel exists while loading");
			PreviewTestEnd(t);
			return;
		}
		CPreviewSession& session = t->panel->Session();
		if (!session.IsReady() && session.IsLoading() && t->waitedMs < 15000)
		{
			t->waitedMs += 100;
			QTimer::singleShot(100, [t]() { PreviewTestWaitReady(t); });
			return;
		}
		std::string detail = std::to_string(t->waitedMs) + " ms";
		if (session.IsReady())
		{
			detail += ", " + std::to_string(session.Info().width) + "x" + std::to_string(session.Info().height) + (session.Info().hasAlpha ? " alpha" : " opaque")
				+ (session.HasAudio() ? " audio" : " no audio") + session.Notes();
		}
		t->Check(session.IsReady(), "preview loaded", detail);
		if (!session.IsReady())
		{
			PreviewTestEnd(t);
			return;
		}
		if (session.State() == cev::PlayerState::Playing)
			t->panel->PlayPause();
		t->serial0 = session.FrameSerial();
		t->callbacks0 = CPreviewAudioSource::TotalCallbacks();
		t->panel->PlayPause(); // play
		t->Check(session.State() == cev::PlayerState::Playing, "playing", "state " + std::to_string(static_cast<int>(session.State())));
		QTimer::singleShot(1000, [t]() { PreviewTestAfterPlay(t); });
	}

	void CmdPreviewTest(IConsoleCmdArgs* pArgs)
	{
		using namespace CEVEditor;
		if (g_previewTestRunning)
		{
			CryLogAlways("[CEVEditor] preview_test: already running");
			return;
		}
		auto t = std::make_shared<SPreviewTest>();
		for (int i = 1; i < pArgs->GetArgCount(); ++i)
		{
			const std::string arg = pArgs->GetArg(i);
			if (arg.compare(0, 4, "png:") == 0 || arg.compare(0, 4, "png=") == 0)
				t->png = arg.substr(4);
			else if (arg.compare(0, 6, "pause:") == 0 || arg.compare(0, 6, "pause=") == 0)
				t->pauseSeconds = std::max(0.5, std::atof(arg.c_str() + 6));
			else if (arg == "close")
				t->close = true;
			else if (arg.compare(0, 6, "track:") == 0 || arg.compare(0, 6, "track=") == 0)
				t->track = arg.substr(6);
			else if (!t->png.empty())
				t->png += " " + arg; // a path with spaces arrives as several arguments
		}
		CVideoEditor* const pEditor = CVideoEditor::Current();
		t->panel = pEditor != nullptr ? pEditor->PreviewPanel() : nullptr;
		g_previewTestRunning = true;
		if (!t->panel)
		{
			t->Check(false, "an open video editor with a preview panel");
			PreviewTestEnd(t);
			return;
		}
		PreviewTestWaitReady(t);
	}

	// ---- cev_editor_info_test [legacy:<asset.cevvideo>] [alt:<abs .webm>] [trigger:<name>] [ffmpeg:<path>] ----
	// Self test of the info panel's engine facilities on the asset open in the video editor (E1): descriptor edits through the
	// property tree's serialization path -> modified -> Save -> values on disk; the ACE trigger selector; the engine's Reimport with
	// the stored settings; source check by size / time (touch -> confirmed by hash; new content -> reimport needed -> Reimport);
	// the FFmpeg path preference (and the one-time import of the old settings file). `legacy:` also checks that an asset imported
	// before E1 got its source file registered when it was opened. The asset is modified (use a scratch asset). One PASS / FAIL line.
	struct SInfoTest
	{
		std::string legacy;
		std::string alt;
		std::string trigger = "mute_all";
		std::string ffmpeg = "T:/cev/e1/pref_test/ffmpeg.exe";
		QPointer<CEVEditor::CVideoEditor> editor;
		std::string metadataRel;
		std::string copyAbs;
		std::string settingsBefore;
		std::string prefBefore;
		int checks = 0;
		int failed = 0;
		int waitedMs = 0;

		void Check(bool ok, const char* what, const std::string& detail = std::string())
		{
			++checks;
			if (!ok)
				++failed;
			CryLogAlways("[CEVEditor] info_test: %s %s%s%s", ok ? "ok  " : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
		}
		CAsset* Asset() const { return CEVEditor::CVideoEditor::Current() != nullptr && editor ? editor->GetAssetBeingEdited() : nullptr; }
	};
	bool g_infoTestRunning = false;

	void InfoTestEnd(const std::shared_ptr<SInfoTest>& t)
	{
		CryLogAlways("[CEVEditor] info_test: %s (%d checks, %d failed)", t->failed == 0 ? "PASS" : "FAIL", t->checks, t->failed);
		g_infoTestRunning = false;
	}

	std::string ReadTextFile(const std::string& abs)
	{
		std::ifstream in(std::filesystem::u8path(abs), std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

	//! Waits (event loop) until the editor's source check has a result, then calls `next`.
	void InfoTestWaitSource(std::shared_ptr<SInfoTest> t, std::function<void(std::shared_ptr<SInfoTest>)> next)
	{
		if (t->editor && !t->editor->State().source.checked && t->waitedMs < 30000)
		{
			t->waitedMs += 100;
			QTimer::singleShot(100, [t, next]() { InfoTestWaitSource(t, next); });
			return;
		}
		t->waitedMs = 0;
		next(t);
	}

	void InfoTestPreferences(std::shared_ptr<SInfoTest> t)
	{
		using namespace CEVEditor;
		CFfmpegService* const pService = CFfmpegService::Get();
		// the old settings file (CEV_EDITOR_SETTINGS of the runner) was imported once at the first idle update
		const std::string legacyValue = LoadEditorSetting(SettingKey::kFfmpegPath);
		t->Check(gCevEditorPreferences.legacySettingsImported, "old settings file was looked at once (preference remembers it)", EditorSettingsPath());
		if (!legacyValue.empty())
			t->Check(std::string(gCevEditorPreferences.ffmpegPath.c_str()) == legacyValue, "FFmpeg path imported from the old settings file", std::string(gCevEditorPreferences.ffmpegPath.c_str()));
		t->Check(!MigrateLegacyEditorSettings(), "the import runs only once");
		t->prefBefore = gCevEditorPreferences.ffmpegPath.c_str();
		// the Preferences window writes the page through its Serialize (edit archive); here the JSON archive does the same input
		auto setThroughPage = [](const std::string& value) -> bool
		{
			SCevEditorPreferences page;
			page.ffmpegPath = value.c_str();
			page.legacySettingsImported = gCevEditorPreferences.legacySettingsImported;
			DynArray<char> buffer;
			Serialization::IArchiveHost* const pHost = gEnv->pSystem->GetArchiveHost();
			if (!pHost->SaveJsonBuffer(buffer, Serialization::SStruct(page)) || !pHost->LoadJsonBuffer(Serialization::SStruct(gCevEditorPreferences), buffer.data(), buffer.size()))
				return false;
			GetIEditor()->GetPreferences()->Save();
			return true;
		};
		t->Check(setThroughPage(t->ffmpeg), "FFmpeg path set through the preference page", t->ffmpeg);
		QTimer::singleShot(1500, [t, pService, setThroughPage]()
		{
			const std::string configured = pService != nullptr ? pService->ConfiguredPath() : std::string();
			t->Check(configured == t->ffmpeg, "FFmpeg service took the preference over", configured);
			gEnv->pConsole->ExecuteString("cev_editor_ffmpeg_status");
			const std::string described = pService != nullptr ? DescribeStatus(pService->Status()) : std::string();
			t->Check(described.find("configured path: " + t->ffmpeg) != std::string::npos, "cev_editor_ffmpeg_status shows it", described.substr(0, 300));
			// back to what it was (the runner restores the whole settings folder anyway)
			t->Check(setThroughPage(t->prefBefore), "FFmpeg path preference restored", t->prefBefore.empty() ? "(empty)" : t->prefBefore);
			QTimer::singleShot(1500, [t]() { InfoTestEnd(t); });
		});
	}

	void InfoTestAfterSecondReimport(std::shared_ptr<SInfoTest> t)
	{
		using namespace CEVEditor;
		const SSourceStatus& s = t->editor->State().source;
		t->Check(s.checked && !s.reimportNeeded && s.copyMatchesImport, "after Reimport: source up to date", s.text);
		CAsset* const pAsset = t->Asset();
		if (pAsset != nullptr)
		{
			std::string sha;
			CEVEditor::Sha256File(t->copyAbs, sha);
			t->Check(CDetails(*pAsset).Get(DetailKey::kSourceSha256) == sha, "Reimport recorded the hash of the new source content");
			t->Check(CDetails(*pAsset).Get(DetailKey::kImportSettings) == t->settingsBefore, "import settings unchanged by the second Reimport", CDetails(*pAsset).Get(DetailKey::kImportSettings));
		}
		InfoTestPreferences(t);
	}

	void InfoTestAfterContentChange(std::shared_ptr<SInfoTest> t)
	{
		using namespace CEVEditor;
		const SSourceStatus& s = t->editor->State().source;
		t->Check(s.checked && s.reimportNeeded, "new source content: reimport needed", s.text);
		const bool started = t->editor->Reimport();
		t->Check(started, "Reimport (CAsset::Reimport) after the content change");
		QTimer::singleShot(300, [t]() { InfoTestWaitSource(t, InfoTestAfterSecondReimport); });
	}

	void InfoTestAfterTouch(std::shared_ptr<SInfoTest> t)
	{
		using namespace CEVEditor;
		const SSourceStatus& s = t->editor->State().source;
		t->Check(s.checked && s.hashed && s.copyMatchesImport && !s.reimportNeeded, "touched source: hash says unchanged", s.text);
		QTimer::singleShot(1500, [t]()
		{
			CAsset* const pAsset = t->Asset();
			const std::string stamp = pAsset != nullptr ? CDetails(*pAsset).Get(DetailKey::kSourceStamp) : std::string();
			t->Check(!stamp.empty() && stamp == FileStamp(t->copyAbs), "new size / time stamp recorded (next check without hash)", stamp);
			t->editor->RecheckSource();
			const SSourceStatus& again = t->editor->State().source;
			t->Check(again.checked && !again.hashed && !again.confirming, "check again: stat only, no hash", again.text);
			if (t->alt.empty())
			{
				t->Check(false, "alt:<webm> given for the content change");
				InfoTestPreferences(t);
				return;
			}
			std::string error;
			if (pAsset == nullptr || !ReleaseAssetFiles(t->metadataRel, pAsset->GetGUID(), 10000, error))
				t->Check(false, "release the files before replacing the source", error);
			std::error_code ec;
			std::filesystem::copy_file(std::filesystem::u8path(t->alt), std::filesystem::u8path(t->copyAbs), std::filesystem::copy_options::overwrite_existing, ec);
			t->Check(!ec, "source file replaced by other content", t->alt + (ec ? " -> " + ec.message() : std::string()));
			t->editor->RecheckSource();
			t->Check(t->editor->State().source.confirming, "after the replacement: needs confirmation");
			InfoTestWaitSource(t, InfoTestAfterContentChange);
		});
	}

	void InfoTestAfterReimport(std::shared_ptr<SInfoTest> t)
	{
		using namespace CEVEditor;
		CAsset* const pAsset = t->Asset();
		if (pAsset == nullptr)
		{
			t->Check(false, "asset still open after Reimport");
			InfoTestEnd(t);
			return;
		}
		const CDetails details(*pAsset);
		t->Check(details.Get(DetailKey::kImportSettings) == t->settingsBefore, "import settings unchanged by Reimport", details.Get(DetailKey::kImportSettings));
		t->Check(details.Get(DetailKey::kImportState) == "ready", "import state ready after Reimport");
		t->Check(!pAsset->GetSourceFile().empty(), "source file still registered after Reimport", pAsset->GetSourceFile().c_str());
		const SSourceStatus& s = t->editor->State().source;
		t->Check(s.checked && !s.reimportNeeded, "source up to date after Reimport", s.text);
		// touch: new modification time, same content
		std::string error;
		ReleaseAssetFiles(t->metadataRel, pAsset->GetGUID(), 10000, error);
		std::error_code ec;
		const auto now = std::filesystem::file_time_type::clock::now();
		std::filesystem::last_write_time(std::filesystem::u8path(t->copyAbs), now, ec);
		t->Check(!ec, "source file touched", ec ? ec.message() : FileStamp(t->copyAbs));
		t->editor->RecheckSource();
		const SSourceStatus& pending = t->editor->State().source;
		t->Check(pending.confirming && !pending.checked, "touched source: needs confirmation", pending.text);
		InfoTestWaitSource(t, InfoTestAfterTouch);
	}

	void InfoTestStart(std::shared_ptr<SInfoTest> t)
	{
		using namespace CEVEditor;
		CVideoEditor* const pEditor = t->editor.data();
		CVideoInfoPanel* const pPanel = pEditor != nullptr ? pEditor->InfoPanel() : nullptr;
		CAsset* const pAsset = t->Asset();
		if (pPanel == nullptr || pAsset == nullptr)
		{
			t->Check(false, "an open video editor with an asset and an info panel");
			InfoTestEnd(t);
			return;
		}
		t->metadataRel = pAsset->GetMetadataFile().c_str();
		const CDetails details(*pAsset);
		t->copyAbs = AssetsAbsolute(details.Get(DetailKey::kSourcePath));
		t->settingsBefore = details.Get(DetailKey::kImportSettings);

		// ---- legacy asset: the source file was registered when it was opened
		if (!t->legacy.empty())
		{
			CAsset* const pLegacy = CAssetManager::GetInstance()->FindAssetForMetadata((t->legacy + ".cryasset").c_str());
			if (pLegacy == nullptr)
			{
				t->Check(false, "legacy asset found", t->legacy);
			}
			else
			{
				const std::string expected = CDetails(*pLegacy).Get(DetailKey::kSourcePath);
				t->Check(!expected.empty() && _stricmp(pLegacy->GetSourceFile().c_str(), expected.c_str()) == 0, "legacy asset: source file registered", std::string(pLegacy->GetSourceFile().c_str()) + " (Details sourcePath " + expected + ")");
				const std::string xml = ReadTextFile(AssetsAbsolute(pLegacy->GetMetadataFile().c_str()));
				t->Check(xml.find("source=\"" + FileNameOf(expected) + "\"") != std::string::npos, "legacy asset: .cryasset on disk has the source attribute", "source=\"" + FileNameOf(expected) + "\"");
				t->Check(pLegacy->HasSourceFile() && pLegacy->GetType()->IsImported(), "legacy asset: Reimport available (IsImported && HasSourceFile, the Asset Browser's condition)");
			}
		}
		t->Check(pAsset->HasSourceFile() && pAsset->GetType()->IsImported() && !pAsset->IsImmutable(), "test asset: Reimport available (IsImported && HasSourceFile && !immutable)", pAsset->GetSourceFile().c_str());

		// ---- descriptor through the property tree's serialization path
		SDescriptor d = pPanel->EditedDescriptor();
		d.avOffsetMs = 123.5;
		SDescriptor::SMarker marker;
		marker.name = "e1_marker";
		marker.time = 1.25;
		d.markers.push_back(marker);
		// A1: audio tracks - add three (File / Trigger / Embedded), change the first one, then remove one in a second edit
		const size_t tracksBefore = d.audioTracks.size();
		const std::string firstName = tracksBefore > 0 ? d.audioTracks.front().name : std::string();
		if (tracksBefore > 0)
			d.audioTracks.front().language = "english";
		{
			SDescriptor::SAudioTrack music;
			music.name = "a1_music";
			music.source = SDescriptor::SAudioTrack::ESource::File;
			music.file = "a1_music.webm";
			SDescriptor::SAudioTrack commentary;
			commentary.name = "a1_commentary";
			commentary.source = SDescriptor::SAudioTrack::ESource::Trigger;
			commentary.trigger = t->trigger;
			commentary.language = "japanese";
			SDescriptor::SAudioTrack removed;
			removed.name = "a1_removed";
			removed.source = SDescriptor::SAudioTrack::ESource::Embedded;
			d.audioTracks.push_back(music);
			d.audioTracks.push_back(commentary);
			d.audioTracks.push_back(removed);
		}
		d.defaultTrack = "a1_music";
		d.followGameLanguage = true;
		pAsset->SetModified(false);
		std::string error;
		const bool applied = pPanel->ApplyThroughSerializationForTest(d, error);
		t->Check(applied, "edit applied through SDescriptor::Serialize (JSON archive -> tree object -> tree change handler)", error);
		{
			SDescriptor second = pPanel->EditedDescriptor();
			const int index = second.FindAudioTrack("a1_removed");
			t->Check(index >= 0, "track a1_removed added through the serialization path");
			if (index >= 0)
				second.audioTracks.erase(second.audioTracks.begin() + index);
			std::string error2;
			t->Check(pPanel->ApplyThroughSerializationForTest(second, error2), "second edit (remove a1_removed) applied through SDescriptor::Serialize", error2);
		}
		auto checkTracks = [t, tracksBefore, firstName](const SDescriptor& s, const char* where)
		{
			const int music = s.FindAudioTrack("a1_music");
			const int commentary = s.FindAudioTrack("a1_commentary");
			std::string names;
			for (const SDescriptor::SAudioTrack& a : s.audioTracks)
				names += (names.empty() ? "" : ",") + a.name + "[" + a.language + "]";
			t->Check(s.audioTracks.size() == tracksBefore + 2 && s.FindAudioTrack("a1_removed") < 0 && music >= 0 && commentary >= 0,
				(std::string("audio tracks added / removed ") + where).c_str(), names);
			t->Check(music >= 0 && s.audioTracks[music].source == SDescriptor::SAudioTrack::ESource::File && s.audioTracks[music].file == "a1_music.webm" && s.audioTracks[music].language.empty(),
				(std::string("File track a1_music (no language) ") + where).c_str());
			t->Check(commentary >= 0 && s.audioTracks[commentary].source == SDescriptor::SAudioTrack::ESource::Trigger && s.audioTracks[commentary].trigger == t->trigger
				&& s.audioTracks[commentary].language == "japanese", (std::string("Trigger track a1_commentary took an existing trigger name and the language tag ") + where).c_str(), t->trigger);
			if (tracksBefore > 0)
				t->Check(s.audioTracks.front().name == firstName && s.audioTracks.front().language == "english", (std::string("first track changed (language tag english) ") + where).c_str(), firstName);
			t->Check(s.defaultTrack == "a1_music" && s.followGameLanguage, (std::string("Default track a1_music and Follow game language ") + where).c_str(), s.defaultTrack);
		};
		const SDescriptor& state = pEditor->State().descriptor;
		t->Check(pAsset->IsModified(), "asset is modified after the edit");
		t->Check(state.avOffsetMs == 123.5, "A/V offset in the editor", std::to_string(state.avOffsetMs));
		const bool markerInState = std::any_of(state.markers.begin(), state.markers.end(), [](const SDescriptor::SMarker& m) { return m.name == "e1_marker" && std::fabs(m.time - 1.25) < 1e-9; });
		t->Check(markerInState, "marker in the editor");
		const SStaticResourceSelectorEntry* const pSelector = GetIEditor()->GetResourceSelectorHost() != nullptr ? GetIEditor()->GetResourceSelectorHost()->GetSelector("AudioTrigger") : nullptr;
		t->Check(pSelector != nullptr, "the AudioTrigger resource selector is registered (Audio Controls Editor)");
		checkTracks(state, "in the editor");

		// ---- save, then the file on disk
		const bool saved = pEditor->OnSave();
		t->Check(saved && !pAsset->IsModified(), "saved (not modified any more)");
		SDescriptor disk;
		const bool read = LoadDescriptor(AssetsAbsolute(DescriptorRel(*pAsset)), disk, error);
		t->Check(read && disk.avOffsetMs == 123.5, "A/V offset on disk after reloading", read ? std::to_string(disk.avOffsetMs) : error);
		t->Check(read && std::any_of(disk.markers.begin(), disk.markers.end(), [](const SDescriptor::SMarker& m) { return m.name == "e1_marker" && std::fabs(m.time - 1.25) < 1e-6; }), "marker on disk after reloading");
		if (read)
			checkTracks(disk, "on disk after reloading");
		else
			t->Check(false, "descriptor read back", error);
		const std::string xml = ReadTextFile(AssetsAbsolute(DescriptorRel(*pAsset)));
		t->Check(xml.find("<AudioTracks default=\"a1_music\" followGameLanguage=\"1\">") != std::string::npos && xml.find("<Languages") == std::string::npos,
			"the file has <AudioTracks default= followGameLanguage=\"1\"> and no <Languages>");
		pEditor->RefreshAssetState();
		t->Check(pPanel->EditedDescriptor().avOffsetMs == 123.5, "property tree shows the saved value after the reload");

		// ---- the engine's Reimport with the stored settings
		InfoTestWaitSource(t, [](std::shared_ptr<SInfoTest> t2)
		{
			const bool started = t2->editor && t2->editor->Reimport();
			t2->Check(started, "Reimport (CAsset::Reimport -> CVideoAssetImporter::ReimportAsset)");
			QTimer::singleShot(300, [t2]() { InfoTestWaitSource(t2, InfoTestAfterReimport); });
		});
	}

	void CmdInfoTest(IConsoleCmdArgs* pArgs)
	{
		using namespace CEVEditor;
		if (g_infoTestRunning)
		{
			CryLogAlways("[CEVEditor] info_test: already running");
			return;
		}
		auto t = std::make_shared<SInfoTest>();
		for (int i = 1; i < pArgs->GetArgCount(); ++i)
		{
			const std::string arg = pArgs->GetArg(i);
			auto value = [&arg](const char* key) -> const char*
			{
				const size_t n = std::strlen(key);
				return arg.compare(0, n, key) == 0 && arg.size() > n && (arg[n] == ':' || arg[n] == '=') ? arg.c_str() + n + 1 : nullptr;
			};
			if (const char* v = value("legacy")) t->legacy = v;
			else if (const char* v = value("alt")) t->alt = v;
			else if (const char* v = value("trigger")) t->trigger = v;
			else if (const char* v = value("ffmpeg")) t->ffmpeg = v;
			else CryLogAlways("[CEVEditor] info_test: unknown argument '%s'", arg.c_str());
		}
		t->editor = CVideoEditor::Current();
		g_infoTestRunning = true;
		InfoTestStart(t);
	}

	// cev_editor_info_grab png:<path>: saves the info panel of the open video editor as PNG (QWidget::grab): <path> is the
	// visible part, <path without .png>_full.png the whole scrolled content
	void CmdInfoGrab(IConsoleCmdArgs* pArgs)
	{
		using namespace CEVEditor;
		std::string png;
		for (int i = 1; i < pArgs->GetArgCount(); ++i)
		{
			const std::string arg = pArgs->GetArg(i);
			if (arg.compare(0, 4, "png:") == 0 || arg.compare(0, 4, "png=") == 0)
				png = arg.substr(4);
			else if (!png.empty())
				png += " " + arg; // a path with spaces arrives as several arguments
		}
		CVideoEditor* const pEditor = CVideoEditor::Current();
		CVideoInfoPanel* const pPanel = pEditor != nullptr ? pEditor->InfoPanel() : nullptr;
		if (pPanel == nullptr || png.empty())
		{
			CryLogAlways("[CEVEditor] info_grab: FAILED (%s)", png.empty() ? "usage: cev_editor_info_grab png:<path>" : "no info panel");
			return;
		}
		const QPixmap visible = pPanel->grab();
		const bool ok1 = !visible.isNull() && visible.save(QString::fromUtf8(png.c_str()), "PNG");
		std::string full = png;
		if (full.size() > 4 && _stricmp(full.c_str() + full.size() - 4, ".png") == 0)
			full.resize(full.size() - 4);
		full += "_full.png";
		bool ok2 = false;
		int fullW = 0, fullH = 0;
		if (QScrollArea* const pScroll = pPanel->findChild<QScrollArea*>())
		{
			if (QWidget* const pContent = pScroll->widget())
			{
				const QPixmap all = pContent->grab();
				fullW = all.width();
				fullH = all.height();
				ok2 = !all.isNull() && all.save(QString::fromUtf8(full.c_str()), "PNG");
			}
		}
		CryLogAlways("[CEVEditor] info_grab: %s %s (%dx%d), %s %s (%dx%d)", ok1 ? "saved" : "FAILED", png.c_str(), visible.width(), visible.height(),
			ok2 ? "saved" : "FAILED", full.c_str(), fullW, fullH);
	}

	// cev_editor_preview_dock <float|dock|tab_other|tab_preview>: moves the preview panel of the open video editor through the
	// docking manager the way a user drags it (floating window, back into the area of the info panel, switching tabs there)
	void CmdPreviewDock(IConsoleCmdArgs* pArgs)
	{
		using namespace CEVEditor;
		const std::string op = pArgs->GetArgCount() > 1 ? pArgs->GetArg(1) : "";
		CVideoEditor* const pEditor = CVideoEditor::Current();
		CVideoPreviewPanel* const pPanel = pEditor != nullptr ? pEditor->PreviewPanel() : nullptr;
		if (pPanel == nullptr)
		{
			CryLogAlways("[CEVEditor] preview_dock %s: FAILED, no preview panel", op.c_str());
			return;
		}
		QToolWindowManager* pManager = nullptr;
		QWidget* pTool = nullptr;
		for (QToolWindowManager* m : pEditor->findChildren<QToolWindowManager*>())
		{
			for (QWidget* w = pPanel; w != nullptr && pTool == nullptr; w = w->parentWidget())
			{
				if (m->ownsToolWindow(w))
				{
					pManager = m;
					pTool = w;
				}
			}
			if (pManager != nullptr)
				break;
		}
		if (pManager == nullptr)
		{
			CryLogAlways("[CEVEditor] preview_dock %s: FAILED, the docking manager does not own the preview panel", op.c_str());
			return;
		}
		QWidget* pOther = nullptr;
		for (QWidget* w : pManager->toolWindows())
		{
			if (w != pTool && (qobject_cast<CVideoInfoPanel*>(w) != nullptr || w->findChild<CVideoInfoPanel*>() != nullptr))
				pOther = w;
		}
		bool ok = true;
		if (op == "float")
			pManager->moveToolWindow(pTool, QToolWindowAreaTarget(QToolWindowAreaReference::Floating, -1, QRect(200, 200, 800, 520)));
		else if (op == "dock" && pOther != nullptr && pManager->areaOf(pOther) != nullptr)
			pManager->moveToolWindow(pTool, pManager->areaOf(pOther), QToolWindowAreaReference::Combine);
		else if ((op == "tab_other" || op == "tab_preview") && pOther != nullptr && pManager->areaOf(pTool) != nullptr && pManager->areaOf(pTool) == pManager->areaOf(pOther))
			pManager->areaOf(pTool)->setCurrentWidget(op == "tab_other" ? pOther : pTool);
		else
			ok = false;
		CryLogAlways("[CEVEditor] preview_dock %s: %s (tool %s, other panel %s, floating %d)", op.c_str(), ok ? "done" : "FAILED (usage: float | dock | tab_other | tab_preview)",
			pTool->metaObject()->className(), pOther != nullptr ? pOther->metaObject()->className() : "none", pManager->isFloatingWrapper(pTool->window()) ? 1 : 0);
	}

	// cev_editor_frametime <seconds> [label]: statistics of the editor's main loop over the next <seconds> (interval between two
	// eNotify_OnIdleUpdate notifications = one editor frame, the main viewport's render included): count, average, p50, p99, max.
	class CFrameTimer final : public IAutoEditorNotifyListener
	{
	public:
		void Start(double seconds, const std::string& label)
		{
			m_seconds = seconds;
			m_label = label;
			m_samples.clear();
			m_samples.reserve(65536);
			m_started = false;
			m_running = true;
		}

		void OnEditorNotifyEvent(EEditorNotifyEvent event) override
		{
			if (event == eNotify_OnQuit)
			{
				m_running = false;
				return;
			}
			if (event != eNotify_OnIdleUpdate || !m_running)
				return;
			const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
			if (!m_started)
			{
				m_started = true;
				m_begin = now;
				m_last = now;
				return;
			}
			m_samples.push_back(std::chrono::duration<double, std::milli>(now - m_last).count());
			m_last = now;
			if (std::chrono::duration<double>(now - m_begin).count() < m_seconds)
				return;
			m_running = false;
			std::vector<double> sorted = m_samples;
			std::sort(sorted.begin(), sorted.end());
			double sum = 0.0;
			for (double v : sorted)
				sum += v;
			const size_t n = sorted.size();
			const double avg = n > 0 ? sum / static_cast<double>(n) : 0.0;
			const auto pct = [&](double p) { return n > 0 ? sorted[std::min(n - 1, static_cast<size_t>(p * static_cast<double>(n - 1) + 0.5))] : 0.0; };
			CryLogAlways("[CEVEditor] frametime %s: %u frames in %.1f s, avg %.3f ms (%.1f fps), p50 %.3f ms, p99 %.3f ms, max %.3f ms",
				m_label.c_str(), static_cast<unsigned>(n), m_seconds, avg, avg > 0.0 ? 1000.0 / avg : 0.0, pct(0.5), pct(0.99), n > 0 ? sorted.back() : 0.0);
		}

	private:
		double m_seconds = 10.0;
		std::string m_label;
		std::vector<double> m_samples;
		bool m_started = false;
		bool m_running = false;
		std::chrono::steady_clock::time_point m_begin;
		std::chrono::steady_clock::time_point m_last;
	};
	CFrameTimer* g_pFrameTimer = nullptr;

	void CmdFrameTime(IConsoleCmdArgs* pArgs)
	{
		if (g_pFrameTimer == nullptr)
			g_pFrameTimer = new CFrameTimer(); // lives until the process ends (listener of the editor)
		const double seconds = pArgs->GetArgCount() > 1 ? std::max(1.0, std::atof(pArgs->GetArg(1))) : 10.0;
		const std::string label = pArgs->GetArgCount() > 2 ? pArgs->GetArg(2) : "";
		g_pFrameTimer->Start(seconds, label);
		CryLogAlways("[CEVEditor] frametime %s: measuring %.1f s", label.c_str(), seconds);
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
		REGISTER_COMMAND("cev_editor_import", CmdImport, VF_NULL, "cev_editor_import <absSrcPath|absPngFolder> <assetFolder> [name:..] [track:..] [wav] [ogg] [split|strip] [cue|mixed] [overwrite] [crf:N] [height:PX] [variants:540,360] [fps:N] [recompress]: import a video (webm mp4 mov mkv avi) or a folder of PNG files as a video asset without UI");
		REGISTER_COMMAND("cev_editor_import_batch", CmdImportBatch, VF_NULL, "cev_editor_import_batch <absFolderOrListFile> <assetFolder> [options] [sequences]: import many files on one queue (one ffmpeg at a time) with a summary");
		REGISTER_COMMAND("cev_editor_import_cancel", CmdImportCancel, VF_NULL, "cev_editor_import_cancel: cancel the running import and drop the queued ones");
		REGISTER_COMMAND("cev_editor_ffmpeg_status", CmdFfmpegStatus, VF_NULL, "cev_editor_ffmpeg_status [redetect]: log the ffmpeg in use (path, version, missing features)");
		REGISTER_COMMAND("cev_editor_ffmpeg_path", CmdFfmpegPath, VF_NULL, "cev_editor_ffmpeg_path <path|->: set (or clear with -) the persistent ffmpeg path and detect again");
		REGISTER_COMMAND("cev_editor_ffmpeg_install_test", CmdFfmpegInstallTest, VF_NULL, "cev_editor_ffmpeg_install_test <zip> <sha256> [version]: run the download install path with a local zip (hash check, staging)");
		REGISTER_COMMAND("cev_editor_ffmpeg_download", CmdFfmpegDownload, VF_NULL, "cev_editor_ffmpeg_download: what the info panel's Download minimal build button does (download the pinned release, verify the SHA-256, install, detect again)");
		REGISTER_COMMAND("cev_editor_variants", CmdVariants, VF_NULL, "cev_editor_variants <asset.cevvideo> <heights> [crf:N]: generate variants with FFmpeg");
		REGISTER_COMMAND("cev_editor_recompress", CmdRecompress, VF_NULL, "cev_editor_recompress <asset.cevvideo> [crf:N] [height:PX] [variants:a,b|none] [refresh]: re-encode the video from its source copy");
		REGISTER_COMMAND("cev_editor_open", CmdOpen, VF_NULL, "cev_editor_open [<asset.cevvideo>]: open the CEV Video Editor (on that asset)");
		REGISTER_COMMAND("cev_editor_close", CmdClose, VF_NULL, "cev_editor_close: close the CEV Video Editor windows");
		REGISTER_COMMAND("cev_editor_status", CmdStatus, VF_NULL, "cev_editor_status: log preview / job counters of the CEV editor");
		REGISTER_COMMAND("cev_editor_preview_test", CmdPreviewTest, VF_NULL, "cev_editor_preview_test [png:<path>] [pause:<s>] [track:<name>] [close]: self test of the preview of the open video editor (load, play 1 s, optionally switch the audio track while playing, pause, no repaint while paused, PNG, close)");
		REGISTER_COMMAND("cev_editor_info_test", CmdInfoTest, VF_NULL, "cev_editor_info_test [legacy:<asset.cevvideo>] [alt:<abs webm>] [trigger:<name>] [ffmpeg:<path>]: self test of the info panel on the open (scratch) asset: property tree edit, save, Reimport, source check, FFmpeg preference");
		REGISTER_COMMAND("cev_editor_info_grab", CmdInfoGrab, VF_NULL, "cev_editor_info_grab png:<path>: save the info panel of the open video editor as PNG (visible part and whole content)");
		REGISTER_COMMAND("cev_editor_preview_dock", CmdPreviewDock, VF_NULL, "cev_editor_preview_dock <float|dock|tab_other|tab_preview>: move the preview panel through the docking manager (test)");
		REGISTER_COMMAND("cev_editor_rename", CmdRename, VF_NULL, "cev_editor_rename <asset.cevvideo> <newName>: rename a video asset through the asset manager");
		REGISTER_COMMAND("cev_editor_move", CmdMove, VF_NULL, "cev_editor_move <asset.cevvideo> <folder>: move a video asset through the asset manager");
		REGISTER_COMMAND("cev_editor_delete", CmdDelete, VF_NULL, "cev_editor_delete <asset.cevvideo>: delete a video asset with all its files");
		REGISTER_COMMAND("cev_editor_frametime", CmdFrameTime, VF_NULL, "cev_editor_frametime <seconds> [label]: log editor frame time statistics (idle update intervals) over the next seconds");
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
		pConsole->RemoveCommand("cev_editor_ffmpeg_download");
		pConsole->RemoveCommand("cev_editor_variants");
		pConsole->RemoveCommand("cev_editor_recompress");
		pConsole->RemoveCommand("cev_editor_open");
		pConsole->RemoveCommand("cev_editor_close");
		pConsole->RemoveCommand("cev_editor_status");
		pConsole->RemoveCommand("cev_editor_preview_test");
		pConsole->RemoveCommand("cev_editor_preview_dock");
		pConsole->RemoveCommand("cev_editor_info_grab");
		pConsole->RemoveCommand("cev_editor_info_test");
		pConsole->RemoveCommand("cev_editor_rename");
		pConsole->RemoveCommand("cev_editor_move");
		pConsole->RemoveCommand("cev_editor_delete");
		pConsole->RemoveCommand("cev_editor_script");
		pConsole->RemoveCommand("cev_editor_frametime");
	}
	g_pScript = nullptr;
	delete g_pFrameTimer; // unregisters its editor listener before the DLL goes
	g_pFrameTimer = nullptr;
	CEVEditor::SetOfflineOps(nullptr);
	CryLogAlways("[CEVEditor] plugin unloaded");
}
