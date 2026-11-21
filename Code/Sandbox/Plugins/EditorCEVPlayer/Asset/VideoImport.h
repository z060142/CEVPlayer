// Import of a video file into a video asset and the follow-up operations on an existing asset. Shared by the importer
// (drag and drop, background thread), the console command cev_editor_import (main thread) and the editor window.
//
// Layout (docs/design/M4-asset-and-editor.md, M5-ffmpeg.md): a WebM source is COPIED into the asset's folder as <name>.webm
// (that is also the video the descriptor starts with). Any other source (mp4 / mov / mkv / avi) is copied as
// <name>_source.<ext> and transcoded by FFmpeg to <name>.webm; a PNG sequence is transcoded from its own folder (not copied).
// A WebM that is re-encoded at import is copied as <name>_source.webm. <name>_<height>p.webm are the variants listed in the
// descriptor's <Variants>. <name>.cevvideo and <name>.cevvideo.cryasset are written next to them.
// Details of the .cryasset keep the original path, the relative path of the source copy, its SHA-256 and the import settings.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include <CryExtension/CryGUID.h>

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

	//! What a source path is (decides copy / transcode).
	enum class ESourceKind
	{
		Webm,         //!< .webm: copied (no FFmpeg) unless the options re-encode it
		Video,        //!< .mp4 / .mov / .mkv / .avi: always transcoded by FFmpeg
		PngSequence,  //!< a folder of numbered PNG files: transcoded by FFmpeg
	};
	//! Classifies by extension / directory. False when the path is not an importable source (kind unchanged).
	bool ClassifySource(const std::string& absPath, ESourceKind& kind);
	//! "webm, mp4, mov, mkv, avi"
	std::string ImportExtensionsText();
	//! The heights the variant lists offer: 2160, 1440, 1080, 720, 540, 360.
	std::vector<int> StandardVariantHeights();
	//! "540,360" / "540 360" -> {540, 360} (ascending order is not required). False + error on a bad item.
	bool ParseHeightList(const std::string& text, std::vector<int>& out, std::string& error);

	struct SImportOptions
	{
		std::string assetName;               //!< "" = sanitised stem of the file name
		std::string audioTrack = "main";     //!< name of the audio track of the embedded audio (A1; not a language)
		std::string route = "embedded";      //!< embedded | mixed | cue (cue is applied once a WAV / Ogg exists)
		std::string trigger;                 //!< base of the ACE trigger names, "" = the asset name
		ESplitAudio split = ESplitAudio::None;
		bool wav = false;                    //!< export a WAV (needs the offline library)
		bool ogg = false;                    //!< export an Ogg Vorbis (needs the offline library)
		float oggQuality = 0.4f;
		bool overwrite = false;              //!< the caller / the user agreed to replace an existing asset of that name (not stored)
		// FFmpeg stage (M5)
		bool recompress = false;             //!< WebM sources: re-encode with crf / height (other sources are always encoded)
		int crf = 32;                        //!< libvpx-vp9 constant quality 0 (best) .. 63
		int height = 0;                      //!< 0 = keep the source height, else the output height (width follows)
		std::vector<int> variants;           //!< heights of the variants (<name>_<h>p.webm); heights not below the output are skipped
		double fps = 30.0;                   //!< PNG sequence frame rate

		bool NeedsAudioOps() const { return split != ESplitAudio::None || wav || ogg; }
		bool NeedsTranscode(ESourceKind kind) const { return kind != ESourceKind::Webm || recompress; }
		bool NeedsFfmpeg(ESourceKind kind) const { return NeedsTranscode(kind) || !variants.empty(); }
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

	//! What an import is going to do, worked out on the MAIN thread (the asset manager is not thread safe) before the
	//! copy / hash part runs on any thread. `exists` = an asset or files with that name are already there.
	struct SImportPlan
	{
		bool ok = false;
		std::string error;
		std::string folder;                  //!< normalised, trailing slash
		std::string assetName;
		std::string metadataRel;
		bool exists = false;                 //!< the asset (in the asset manager) or one of its main files exists
		bool inAssetManager = false;
		CryGUID guid;                        //!< GUID of the existing asset (kept when it is overwritten), else null
		std::vector<std::string> keptFiles;  //!< Files of the existing asset (all stay in the asset)
		std::string keptSdlFiles;            //!< Details sdlFiles / aceLibrary of the existing asset (kept)
		std::string keptAceLibrary;
		std::string detail;                  //!< human readable what exists, for the confirmation
		// M5
		ESourceKind kind = ESourceKind::Webm;
		std::string sourceCopyRel;           //!< where the source is copied to (relative to Assets), "" for a PNG sequence
		std::vector<std::string> dropFiles;  //!< files of the replaced asset that the new import makes obsolete (old source copy, old variants)
		std::string pngPattern;              //!< PNG sequence: ffmpeg input pattern (absolute, "<dir>/<prefix>%04d.png")
		int pngStart = 0;                    //!< first frame number
		int pngCount = 0;                    //!< frames found (the contiguous run from pngStart)
		std::string note;                    //!< a warning to show / log (PNG sequence gap, ...)
	};

	//! Main thread: validates the source / folder / name and inspects the target. Nothing is written, nothing released.
	SImportPlan PlanImport(const std::string& absSrc, const std::string& folderRel, const SImportOptions& options);
	//! Main thread: for an import that replaces an existing asset: closes the preview players that hold its files and
	//! waits for its jobs. False + `error` when files are still in use.
	bool PrepareOverwrite(const SImportPlan& plan, std::string& error);

	//! The quick part of an import: probe, copy, hash, write .cevvideo and .cryasset. Any thread; does not touch the
	//! asset manager or Qt (use PlanImport before and FinishImport after, on the main thread). The plan must be ok and,
	//! when plan.exists, the caller must have the consent (options.overwrite) and have run PrepareOverwrite.
	//! With the FFmpeg stage (non-WebM source, re-encode, variants) this runs for minutes: only on a job thread (CImportQueue),
	//! `pCtx` carries cancel / progress / log. A failed or cancelled run removes the files it created.
	SImportResult ImportVideoFile(const std::string& absSrc, const SImportPlan& plan, const SImportOptions& options, const std::atomic<bool>* cancel = nullptr, const SOpContext* pCtx = nullptr);
	//! Main thread, no UI: PlanImport + refuse when something exists and options.overwrite is false (the error says so,
	//! nothing is touched) + PrepareOverwrite + ImportVideoFile. For the console command.
	SImportResult ImportVideoFileNow(const std::string& absSrc, const std::string& folderRel, const SImportOptions& options);

	//! Main thread: closes the preview players that have files of the asset open and waits (cancelling) for the jobs
	//! that work on it. Call before files of the asset are replaced, renamed, moved or deleted. False when something
	//! still holds files after `timeoutMs`.
	bool ReleaseAssetFiles(const std::string& metadataRel, const CryGUID& guid, int timeoutMs, std::string& error);
	//! Implemented by the editor window (VideoEditor.cpp): releases the preview player of every window that has the
	//! asset open.
	void ReleaseEditorsOfAsset(const std::string& metadataRel);
	//! Key the job runner knows the jobs of an asset by.
	std::string AssetOwnerKey(const CryGUID& guid);

	//! Main thread: merges the asset into the asset manager and starts the audio operations the options ask for.
	void FinishImport(const SImportResult& result, const SImportOptions& options);
	//! Main thread: only the second half of FinishImport, for callers whose asset is merged by somebody else (the
	//! engine's drop handler merges what an importer returns). The asset must be in the asset manager by now.
	void StartPostImportOps(const std::string& metadataRel, const std::string& assetName, const SImportOptions& options);

	//! Main thread: reimport of an asset whose video is the source file itself (a plain WebM copy): when the original file
	//! outside the project changed since the import it is copied over the source file first; then the source file is probed and
	//! hashed and the Details are renewed (the import settings stay). The preview player must not have the file open. Assets
	//! whose video was made by FFmpeg (IsTranscodedAsset) are reimported by SubmitTranscodeOps instead: false for them.
	bool ReimportVideo(CAsset& asset, std::string& error);
	//! Main thread: the reimport of CVideoAssetImporter::ReimportAsset (the engine's Reimport, Asset Browser) with the import
	//! settings stored in the Details. Returns 0 (failed, `error`), 1 (done) or 2 (an FFmpeg job runs; `onFinished` follows).
	//! The editor windows that show the asset are refreshed afterwards.
	int ReimportWithStoredSettings(CAsset& asset, std::string& error, std::function<void(bool ok)> onFinished = std::function<void(bool)>());
	//! Implemented by the editor window (VideoEditor.cpp): re-reads the asset in every window that shows it and checks its
	//! source again (after a reimport).
	void RefreshEditorsAfterReimport(const std::string& metadataRel);

	//! Details sourcePath when it can be the engine's source file of the asset (same folder as the .cryasset), else "".
	std::string RegistrableSourceFile(const std::string& metadataRel, const CDetails& details);
	//! Main thread: an asset imported before the source file was registered (empty CAsset::GetSourceFile) whose Details name a
	//! source copy next to it gets it registered (the .cryasset is written). True when it was changed.
	bool EnsureSourceRegistered(CAsset& asset);
	//! "<size>:<mtime>" of a file, "" when it does not exist.
	std::string FileStamp(const std::string& absPath);
	//! The main video of the asset was transcoded from a separate source copy / PNG sequence (Details transcoded = 1).
	bool IsTranscodedAsset(const CAsset& asset);

	//! What the source check compares (from the Details): the files and the size / time stamps recorded with the hash.
	struct SSourceCheckInput
	{
		std::string copyAbs;                 //!< the source file next to the asset ("" = none, PNG sequence)
		std::string originalAbs;             //!< the original file outside the project ("" = none / PNG sequence folder)
		std::string importedSha256;
		std::string copyStamp;               //!< Details sourceStamp ("" = not recorded yet: hash)
		std::string originalStamp;           //!< Details originalStamp
	};
	SSourceCheckInput SourceCheckInputOf(const CDetails& details);
	//! Cheap (stat only, main thread is fine): the recorded stamps equal the files now, so no hash is needed.
	bool SourceStampsMatch(const SSourceCheckInput& in);

	//! Source status of an asset: size + modification time against the recorded stamps; only a file whose stamp differs (or that
	//! has none recorded) is hashed (call on a worker thread then).
	struct SSourceStatus
	{
		bool copyExists = false;
		bool copyMatchesImport = false;      //!< stamp as recorded, or hash of the copy == Details sourceSha256
		bool originalExists = false;
		bool originalMatchesImport = false;  //!< stamp as recorded, or hash of the original file == Details sourceSha256
		bool checked = false;
		bool confirming = false;             //!< a stamp differs: the hash check is running ("needs confirmation")
		bool hashed = false;                 //!< a file was hashed because its stamp differed / was missing
		bool reimportNeeded = false;
		std::string copyStamp;               //!< stamps of the files now (recorded again when the hash confirmed the content)
		std::string originalStamp;
		std::string originalPath;
		std::string text;
	};
	SSourceStatus CheckSourceStatus(const SSourceCheckInput& in, const std::atomic<bool>* cancel);
	//! Main thread: after a hash confirmed unchanged content, the current stamps go into the Details (no hash next time).
	bool RecordSourceStamps(CAsset& asset, const SSourceStatus& status);

	struct SAudioOps
	{
		bool split = false;
		bool strip = false;
		bool wav = false;
		bool ogg = false;
		float oggQuality = 0.4f;
		std::string audioTrack = "main";     //!< track the produced files are named after (<name>_<track>.ext) and listed as
		int trackIndex = 0;
		bool upgradeToCue = false;           //!< after the exports: UpgradeToCueRoute (needs a produced or existing WAV / Ogg)
	};

	//! Main thread: runs the requested operations on a job (IOfflineOps) and applies the results to the asset when the
	//! job is done. Returns the job id (0 = not started, `error` says why).
	int SubmitAudioOps(CAsset& asset, const SAudioOps& ops, std::string& error, std::function<void(bool ok)> onFinished = std::function<void(bool)>());

	//! FFmpeg operations on an existing asset (main thread to start, a job does the work, the result is applied on the main thread).
	struct STranscodeOps
	{
		bool recompress = false;          //!< re-encode the main video (crf / height) from the source copy (fallback: the current webm becomes the source copy)
		bool refreshSource = false;       //!< recompress: copy the original file over the source copy first
		bool refreshIfOriginalChanged = false; //!< recompress: copy it only when the original changed since the import (Reimport)
		int crf = 32;
		int height = 0;                   //!< 0 = keep the height of the source
		bool changeVariants = false;      //!< generate the variants in `variants`
		std::vector<int> variants;        //!< heights to (re)generate; heights not below the video are skipped
		bool removeOtherVariants = false; //!< variants of the asset that are not in `variants` are removed (files too)
	};
	//! Job id, 0 + error when it cannot start (no FFmpeg, no asset files, ...).
	int SubmitTranscodeOps(CAsset& asset, const STranscodeOps& ops, std::string& error, std::function<void(bool ok)> onFinished = std::function<void(bool)>());
	//! The heights of the variants in the descriptor of the asset (ascending).
	std::vector<int> VariantHeights(const SDescriptor& desc);

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
