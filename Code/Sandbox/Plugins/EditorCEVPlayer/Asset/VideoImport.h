// Import of a video file into a video asset and the follow-up operations on an existing asset. Shared by the importer
// (drag and drop, background thread), the console command cev_editor_import (main thread) and the editor window.
//
// Layout (docs/design/M4-asset-and-editor.md): the source video is COPIED into the asset's folder as <name>.webm (that
// is also the video the descriptor starts with), <name>.cevvideo and <name>.cevvideo.cryasset are written next to it.
// Details of the .cryasset keep the original path, the relative path of the copy, its SHA-256 and the import settings.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Asset/VideoAssetMeta.h"
#include "Asset/VideoDescriptorIO.h"
#include "Offline/IOfflineOps.h"

class CAsset;

namespace CEVEditor
{
	enum class ESplitAudio
	{
		None,          //!< the audio stays inside the video only
		KeepEmbedded,  //!< also write the audio track as an external WebM (the video keeps its audio)
		StripEmbedded, //!< external audio only: the descriptor plays a picture-only copy of the video
	};

	struct SImportOptions
	{
		std::string assetName;               //!< "" = sanitised stem of the file name
		std::string language = "english";    //!< name of the (embedded) audio language
		std::string route = "embedded";      //!< embedded | mixed | cue (cue is applied once a WAV / Ogg exists)
		std::string trigger;                 //!< base of the ACE trigger names, "" = the asset name
		ESplitAudio split = ESplitAudio::None;
		bool wav = false;                    //!< export a WAV (needs the offline library)
		bool ogg = false;                    //!< export an Ogg Vorbis (needs the offline library)
		float oggQuality = 0.4f;

		bool NeedsAudioOps() const { return split != ESplitAudio::None || wav || ogg; }
		std::string ToSettingsString() const;
		static SImportOptions FromSettingsString(const std::string& text);
	};

	struct SImportResult
	{
		bool ok = false;
		std::string error;
		CAsset* pAsset = nullptr;            //!< new (not yet merged into the asset manager) or the existing asset
		std::string assetName;
		std::string metadataRel;             //!< "<folder>/<name>.cevvideo.cryasset", relative to Assets
	};

	//! Letters, digits, '_' and '-' only (asset names must not contain '.'); never empty.
	std::string SanitizeAssetName(const std::string& text);
	//! `folder` normalised: forward slashes, no leading slash, one trailing slash ("" stays "").
	std::string NormalizeFolder(const std::string& folderRel);

	//! The quick part of an import: validate, copy, hash, probe, write .cevvideo and .cryasset. Any thread, does not
	//! touch the asset manager (use FinishImport on the main thread). Existing files of the same asset are overwritten.
	SImportResult ImportVideoFile(const std::string& absSrc, const std::string& folderRel, const SImportOptions& options);

	//! Main thread: merges the asset into the asset manager and starts the audio operations the options ask for.
	void FinishImport(const SImportResult& result, const SImportOptions& options);
	//! Main thread: only the second half of FinishImport, for callers whose asset is merged by somebody else (the
	//! engine's drop handler merges what an importer returns). The asset must be in the asset manager by now.
	void StartPostImportOps(const std::string& metadataRel, const std::string& assetName, const SImportOptions& options);

	//! Main thread: copy the original file over the copy again (hash, probe and Details are renewed). The preview
	//! player must not have the file open.
	bool ReimportVideo(CAsset& asset, std::string& error);

	//! Source status of an asset, computed by hashing (call on a worker thread: reads whole files).
	struct SSourceStatus
	{
		bool copyExists = false;
		bool copyMatchesImport = false;      //!< hash of the copy == Details sourceSha256
		bool originalExists = false;
		bool originalMatchesImport = false;  //!< hash of the original file == Details sourceSha256
		bool checked = false;
		std::string originalPath;
		std::string text;
	};
	SSourceStatus CheckSourceStatus(const std::string& copyAbs, const std::string& originalAbs, const std::string& importedSha256, const std::atomic<bool>* cancel);

	struct SAudioOps
	{
		bool split = false;
		bool strip = false;
		bool wav = false;
		bool ogg = false;
		float oggQuality = 0.4f;
		std::string language = "english";
		int trackIndex = 0;
		bool upgradeToCue = false;           //!< after the exports: UpgradeToCueRoute (needs a produced or existing WAV / Ogg)
	};

	//! Main thread: runs the requested operations on a job (IOfflineOps) and applies the results to the asset when the
	//! job is done. Returns the job id (0 = not started, `error` says why).
	int SubmitAudioOps(CAsset& asset, const SAudioOps& ops, std::string& error, std::function<void(bool ok)> onFinished = std::function<void(bool)>());

	//! Main thread: registers the WAV / Ogg files of the asset with SDL_mixer / ACE (Audio/AceRegistrar) and switches the
	//! descriptor to route "cue" with the generated triggers. Fails when the asset has no WAV / Ogg.
	bool UpgradeToCueRoute(CAsset& asset, std::string& error);

	//! Main thread: writes new Files and / or Details into the .cryasset of an asset that is in the asset manager.
	bool WriteAssetFilesAndDetails(CAsset& asset, const std::vector<std::string>* files, const CDetails* details);
	//! Names (relative to Assets) of the files of an asset (.cevvideo first).
	std::vector<std::string> AssetFileList(const CAsset& asset);
	//! Relative-to-Assets path of the descriptor of the asset.
	std::string DescriptorRel(const CAsset& asset);
}
