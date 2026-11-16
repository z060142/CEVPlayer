#include "StdAfx.h"
#include "Asset/VideoImport.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

#include <AssetSystem/Asset.h>
#include <AssetSystem/AssetImportContext.h>
#include <AssetSystem/AssetManager.h>
#include <AssetSystem/AssetType.h>
#include <AssetSystem/EditableAsset.h>
#include <Notifications/NotificationCenter.h>
#include <IEditor.h>
#include <PathUtils.h>

#include "Asset/Sha256.h"
#include "Audio/AceRegistrar.h"
#include "FFmpeg/FfmpegService.h"
#include "Jobs/JobRunner.h"
#include "Offline/MediaInfo.h"

namespace CEVEditor
{
	namespace fs = std::filesystem;

	namespace
	{
		const char* const kTypeName = "CEVVideo";

		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		bool HasExt(const std::string& path, const char* ext)
		{
			const size_t dot = path.find_last_of('.');
			return dot != std::string::npos && Lower(path.substr(dot + 1)) == ext;
		}

		bool SafeFolder(const std::string& folder)
		{
			if (folder.find(':') != std::string::npos)
				return false;
			for (const std::string& part : SplitList(folder, '/'))
			{
				if (part == ".." || part == ".")
					return false;
			}
			return true;
		}

		CAsset* FindAssetByMetadata(const std::string& metadataRel)
		{
			CAssetManager* const pManager = CAssetManager::GetInstance();
			return pManager != nullptr ? pManager->FindAssetForMetadata(metadataRel.c_str()) : nullptr;
		}

		void FillMediaDetails(CDetails& d, const SMediaInfo& info)
		{
			d.SetInt(DetailKey::kWidth, info.width);
			d.SetInt(DetailKey::kHeight, info.height);
			d.SetDouble(DetailKey::kDuration, info.durationSec);
			d.SetDouble(DetailKey::kFps, info.fps);
			d.Set(DetailKey::kCodec, info.videoCodec);
			d.SetInt(DetailKey::kAlpha, info.hasAlpha ? 1 : 0);
			std::string tracks;
			for (const SAudioTrackInfo& t : info.audioTracks)
			{
				if (!tracks.empty())
					tracks += ", ";
				tracks += t.codec + " " + std::to_string(t.sampleRate) + " Hz " + std::to_string(t.channels) + " ch";
			}
			d.Set(DetailKey::kAudioTracks, tracks);
		}

		bool CopyFileReplace(const fs::path& src, const fs::path& dst, std::string& error)
		{
			std::error_code ec;
			if (fs::exists(dst, ec) && fs::equivalent(src, dst, ec))
				return true; // already there
			const fs::path tmp = fs::u8path(dst.u8string() + ".tmp");
			fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				error = "cannot copy " + src.u8string() + " to " + tmp.u8string() + ": " + ec.message();
				return false;
			}
			fs::rename(tmp, dst, ec);
			if (ec)
			{
				std::error_code ignore;
				fs::remove(tmp, ignore);
				error = "cannot move the copy to " + dst.u8string() + ": " + ec.message();
				return false;
			}
			return true;
		}

		void Notify(bool warning, const QString& title, const QString& text)
		{
			if (GetIEditor() == nullptr || GetIEditor()->GetNotificationCenter() == nullptr)
				return;
			if (warning)
				GetIEditor()->GetNotificationCenter()->ShowWarning(title, text);
			else
				GetIEditor()->GetNotificationCenter()->ShowInfo(title, text);
		}
	}

	// ---------------------------------------------------------------------------------------------------------------
	std::string SImportOptions::ToSettingsString() const
	{
		const char* splitName = split == ESplitAudio::None ? "none" : split == ESplitAudio::KeepEmbedded ? "keep" : "strip";
		char quality[32];
		std::snprintf(quality, sizeof(quality), "%.2f", oggQuality);
		char fpsText[32];
		std::snprintf(fpsText, sizeof(fpsText), "%.3f", fps);
		std::string variantText;
		for (int h : variants)
			variantText += (variantText.empty() ? "" : ",") + std::to_string(h);
		return JoinSettings({
			{ "language", language }, { "route", route }, { "trigger", trigger }, { "split", splitName },
			{ "wav", wav ? "1" : "0" }, { "ogg", ogg ? "1" : "0" }, { "oggQuality", quality },
			{ "recompress", recompress ? "1" : "0" }, { "crf", std::to_string(crf) }, { "height", std::to_string(height) },
			{ "variants", variantText }, { "fps", fpsText } });
	}

	SImportOptions SImportOptions::FromSettingsString(const std::string& text)
	{
		SImportOptions o;
		for (const auto& kv : SplitSettings(text))
		{
			if (kv.first == "language") o.language = kv.second;
			else if (kv.first == "route") o.route = kv.second;
			else if (kv.first == "trigger") o.trigger = kv.second;
			else if (kv.first == "split") o.split = kv.second == "keep" ? ESplitAudio::KeepEmbedded : kv.second == "strip" ? ESplitAudio::StripEmbedded : ESplitAudio::None;
			else if (kv.first == "wav") o.wav = kv.second == "1";
			else if (kv.first == "ogg") o.ogg = kv.second == "1";
			else if (kv.first == "oggQuality") o.oggQuality = static_cast<float>(std::atof(kv.second.c_str()));
			else if (kv.first == "recompress") o.recompress = kv.second == "1";
			else if (kv.first == "crf") o.crf = std::atoi(kv.second.c_str());
			else if (kv.first == "height") o.height = std::atoi(kv.second.c_str());
			else if (kv.first == "fps") o.fps = std::atof(kv.second.c_str());
			else if (kv.first == "variants") { std::string ignored; ParseHeightList(kv.second, o.variants, ignored); }
		}
		return o;
	}

	std::string SanitizeAssetName(const std::string& text)
	{
		std::string out;
		for (unsigned char c : text)
			out += (std::isalnum(c) != 0 && c < 128) || c == '_' || c == '-' ? static_cast<char>(c) : '_';
		return out.empty() ? std::string("video") : out;
	}

	std::string NormalizeFolder(const std::string& folderRel)
	{
		std::string f = folderRel;
		for (char& c : f)
		{
			if (c == '\\')
				c = '/';
		}
		while (!f.empty() && f.front() == '/')
			f.erase(f.begin());
		if (!f.empty() && f.back() != '/')
			f += '/';
		return f;
	}

	std::vector<std::string> AssetFileList(const CAsset& asset)
	{
		std::vector<std::string> out;
		for (const string& f : asset.GetFiles())
			out.emplace_back(f.c_str());
		return out;
	}

	std::string DescriptorRel(const CAsset& asset)
	{
		return asset.GetFilesCount() > 0 ? std::string(asset.GetFile(0).c_str()) : std::string();
	}

	bool WriteAssetFilesAndDetails(CAsset& asset, const std::vector<std::string>* files, const CDetails* details)
	{
		CAssetImportContext ctx;
		CEditableAsset edit = ctx.CreateEditableAsset(asset);
		if (files != nullptr)
		{
			std::vector<string> list;
			for (const std::string& f : *files)
				list.emplace_back(f.c_str());
			edit.SetFiles(list);
		}
		if (details != nullptr)
			edit.SetDetails(details->ToPairs());
		return edit.WriteToFile();
	}

	// ---------------------------------------------------------------------------------------------------------------
	std::string AssetOwnerKey(const CryGUID& guid)
	{
		return std::string(guid.ToString().c_str());
	}

	// ---------------------------------------------------------------------------------------------------------------
	bool ClassifySource(const std::string& absPath, ESourceKind& kind)
	{
		std::error_code ec;
		const fs::path path = fs::u8path(absPath);
		if (fs::is_directory(path, ec))
		{
			kind = ESourceKind::PngSequence;
			return true;
		}
		if (HasExt(absPath, "webm"))
		{
			kind = ESourceKind::Webm;
			return true;
		}
		for (const char* ext : { "mp4", "mov", "mkv", "avi" })
		{
			if (HasExt(absPath, ext))
			{
				kind = ESourceKind::Video;
				return true;
			}
		}
		return false;
	}

	std::string ImportExtensionsText()
	{
		return "webm, mp4, mov, mkv, avi";
	}

	std::vector<int> StandardVariantHeights()
	{
		return { 2160, 1440, 1080, 720, 540, 360 };
	}

	bool ParseHeightList(const std::string& text, std::vector<int>& out, std::string& error)
	{
		out.clear();
		std::string item;
		auto flush = [&]() -> bool
		{
			if (item.empty())
				return true;
			char* end = nullptr;
			const long v = std::strtol(item.c_str(), &end, 10);
			if (end == item.c_str() || *end != '\0' || v < 16 || v > 8640)
			{
				error = "bad height '" + item + "' (a number of pixels, 16 .. 8640)";
				return false;
			}
			if (std::find(out.begin(), out.end(), static_cast<int>(v)) == out.end())
				out.push_back(static_cast<int>(v));
			item.clear();
			return true;
		};
		for (char c : text)
		{
			if (c == ',' || c == ';' || c == ' ')
			{
				if (!flush())
					return false;
			}
			else
			{
				item += c;
			}
		}
		return flush();
	}

	namespace
	{
		// The numbered PNG files of a folder -> an ffmpeg image2 pattern. The biggest group of files with the same prefix and the
		// same number of digits wins; only the contiguous run from the lowest number is used (ffmpeg stops at the first gap).
		bool DetectPngSequence(const fs::path& dir, SImportPlan& plan, std::string& error)
		{
			struct SGroup { std::map<long, int> numbers; std::string prefix; int digits = 0; };
			std::map<std::pair<std::string, int>, SGroup> groups;
			std::error_code ec;
			for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
			{
				if (!it->is_regular_file(ec) || !HasExt(it->path().u8string(), "png"))
					continue;
				const std::string stem = it->path().stem().u8string();
				size_t digitsStart = stem.size();
				while (digitsStart > 0 && std::isdigit(static_cast<unsigned char>(stem[digitsStart - 1])) != 0)
					--digitsStart;
				const int digits = static_cast<int>(stem.size() - digitsStart);
				if (digits == 0 || digits > 9)
					continue;
				SGroup& g = groups[{ stem.substr(0, digitsStart), digits }];
				g.prefix = stem.substr(0, digitsStart);
				g.digits = digits;
				g.numbers[std::atol(stem.c_str() + digitsStart)] = 1;
			}
			const SGroup* best = nullptr;
			for (const auto& kv : groups)
			{
				if (best == nullptr || kv.second.numbers.size() > best->numbers.size())
					best = &kv.second;
			}
			if (best == nullptr || best->numbers.size() < 2)
			{
				error = "the folder has no numbered PNG sequence (at least two files like name0001.png, name0002.png): " + dir.u8string();
				return false;
			}
			if (best->prefix.find('%') != std::string::npos || dir.u8string().find('%') != std::string::npos)
			{
				error = "a '%' in the folder or file name cannot be used for an image sequence: rename it";
				return false;
			}
			const long first = best->numbers.begin()->first;
			long expected = first;
			int run = 0;
			for (const auto& kv : best->numbers)
			{
				if (kv.first != expected)
					break;
				++run;
				++expected;
			}
			char digitsText[16];
			std::snprintf(digitsText, sizeof(digitsText), "%%0%dd", best->digits);
			std::string base = dir.u8string();
			for (char& c : base)
			{
				if (c == '\\')
					c = '/';
			}
			if (!base.empty() && base.back() != '/')
				base += '/';
			plan.pngPattern = base + best->prefix + digitsText + ".png";
			plan.pngStart = static_cast<int>(first);
			plan.pngCount = run;
			if (run < static_cast<int>(best->numbers.size()))
				plan.note = "the PNG sequence has a gap after frame " + std::to_string(first + run - 1) + ": only " + std::to_string(run) + " of " + std::to_string(best->numbers.size()) + " files are used";
			return true;
		}

		bool IsVariantFileOf(const std::string& rel, const std::string& folder, const std::string& name)
		{
			// <folder><name>_<digits>p.webm
			if (FolderOf(rel) != folder)
				return false;
			const std::string file = FileNameOf(rel);
			if (_strnicmp(file.c_str(), name.c_str(), name.size()) != 0 || file.size() <= name.size() + 1 || file[name.size()] != '_')
				return false;
			const std::string rest = file.substr(name.size() + 1);
			size_t i = 0;
			while (i < rest.size() && std::isdigit(static_cast<unsigned char>(rest[i])) != 0)
				++i;
			return i > 0 && _stricmp(rest.c_str() + i, "p.webm") == 0;
		}
	}

	SImportPlan PlanImport(const std::string& absSrc, const std::string& folderRel, const SImportOptions& options)
	{
		SImportPlan p;
		const fs::path src = fs::u8path(absSrc);
		std::error_code ec;
		if (!fs::exists(src, ec))
		{
			p.error = "source not found: " + absSrc;
			return p;
		}
		if (!ClassifySource(absSrc, p.kind))
		{
			p.error = "this file type cannot be imported (" + ImportExtensionsText() + ", or a folder of PNG files): " + absSrc;
			return p;
		}
		if (p.kind == ESourceKind::PngSequence)
		{
			if (!DetectPngSequence(src, p, p.error))
				return p;
		}
		else if (!fs::is_regular_file(src, ec))
		{
			p.error = "source file not found: " + absSrc;
			return p;
		}
		p.folder = NormalizeFolder(folderRel);
		if (!SafeFolder(p.folder))
		{
			p.error = "invalid destination folder: " + folderRel;
			return p;
		}
		const std::string stem = p.kind == ESourceKind::PngSequence ? src.filename().u8string() : src.stem().u8string();
		p.assetName = options.assetName.empty() ? SanitizeAssetName(stem) : SanitizeAssetName(options.assetName);
		p.metadataRel = p.folder + p.assetName + ".cevvideo.cryasset";
		{
			string reason;
			if (!CAssetType::IsValidAssetPath(p.metadataRel.c_str(), reason))
			{
				p.error = std::string("invalid asset path ") + p.metadataRel + ": " + reason.c_str();
				return p;
			}
		}
		if (options.NeedsTranscode(p.kind) || p.kind != ESourceKind::Webm)
		{
			if (options.crf < 0 || options.crf > 63)
			{
				p.error = "crf must be between 0 and 63";
				return p;
			}
			if (options.height < 0 || options.height > 8640 || (options.height != 0 && options.height < 16))
			{
				p.error = "the output height must be 0 (keep) or 16 .. 8640";
				return p;
			}
		}
		if (p.kind == ESourceKind::Webm)
			p.sourceCopyRel = p.folder + p.assetName + (options.recompress ? "_source.webm" : ".webm");
		else if (p.kind == ESourceKind::Video)
		{
			const std::string ext = Lower(src.extension().u8string()); // ".mp4"
			p.sourceCopyRel = p.folder + p.assetName + "_source" + ext;
		}

		// what is already there? (asset manager: main thread only)
		if (CAsset* const pExisting = FindAssetByMetadata(p.metadataRel))
		{
			p.exists = true;
			p.inAssetManager = true;
			p.guid = pExisting->GetGUID();
			p.keptFiles = AssetFileList(*pExisting);
			const CDetails old(*pExisting);
			p.keptSdlFiles = old.Get(DetailKey::kSdlFiles);
			p.keptAceLibrary = old.Get(DetailKey::kAceLibrary);
			// the new import makes the old source copy (when it has another name) and the old variants obsolete
			const std::string oldSource = old.Get(DetailKey::kSourcePath);
			if (!oldSource.empty() && _stricmp(oldSource.c_str(), p.sourceCopyRel.c_str()) != 0 && _stricmp(oldSource.c_str(), (p.folder + p.assetName + ".webm").c_str()) != 0)
				p.dropFiles.push_back(oldSource);
			for (const std::string& f : p.keptFiles)
			{
				if (IsVariantFileOf(f, p.folder, p.assetName))
					p.dropFiles.push_back(f);
			}
			p.detail = "The video asset '" + p.assetName + "' already exists in " + (p.folder.empty() ? std::string("the Assets root") : p.folder) +
				" (" + std::to_string(p.keptFiles.size()) + " files). Importing replaces its descriptor, its video and its source copy" +
				(p.dropFiles.empty() ? std::string(".") : std::string(" and removes its variants."));
		}
		else
		{
			for (const char* ext : { ".cevvideo.cryasset", ".cevvideo", ".webm" })
			{
				const std::string rel = p.folder + p.assetName + ext;
				if (fs::exists(fs::u8path(AssetsAbsolute(rel)), ec))
				{
					p.exists = true;
					p.detail = "The file '" + rel + "' already exists (no video asset is registered for it). Importing overwrites it.";
					break;
				}
			}
		}
		p.ok = true;
		return p;
	}

	bool ReleaseAssetFiles(const std::string& metadataRel, const CryGUID& guid, int timeoutMs, std::string& error)
	{
		ReleaseEditorsOfAsset(metadataRel); // players hold the video file open
		if (CJobRunner* const pRunner = CJobRunner::Get())
		{
			if (!guid.IsNull() && !pRunner->CancelAndWait(AssetOwnerKey(guid), timeoutMs))
			{
				error = "a background job still works on the files of this asset";
				return false;
			}
		}
		return true;
	}

	bool PrepareOverwrite(const SImportPlan& plan, std::string& error)
	{
		if (!plan.exists)
			return true;
		return ReleaseAssetFiles(plan.metadataRel, plan.guid, 15000, error);
	}

	SImportResult ImportVideoFileNow(const std::string& absSrc, const std::string& folderRel, const SImportOptions& options)
	{
		SImportResult r;
		ESourceKind kind = ESourceKind::Webm;
		if (ClassifySource(absSrc, kind) && options.NeedsFfmpeg(kind))
		{
			r.error = "this import needs FFmpeg and runs for minutes: use the import queue (CImportQueue)";
			return r;
		}
		const SImportPlan plan = PlanImport(absSrc, folderRel, options);
		if (!plan.ok)
		{
			r.error = plan.error;
			return r;
		}
		r.assetName = plan.assetName;
		r.metadataRel = plan.metadataRel;
		if (plan.exists)
		{
			if (!options.overwrite)
			{
				r.error = "refused: " + plan.detail + " Choose another asset name (name=...) or allow it explicitly (overwrite).";
				return r;
			}
			if (!PrepareOverwrite(plan, r.error))
				return r;
		}
		return ImportVideoFile(absSrc, plan, options);
	}

	SImportResult ImportVideoFile(const std::string& absSrc, const SImportPlan& plan, const SImportOptions& options, const std::atomic<bool>* cancel, const SOpContext* pCtx)
	{
		SImportResult r;
		r.assetName = plan.assetName;
		r.metadataRel = plan.metadataRel;
		if (!plan.ok)
		{
			r.error = plan.error;
			return r;
		}
		if (plan.exists && !options.overwrite)
		{
			r.error = "refused: " + plan.detail;
			return r;
		}
		SOpContext ctx;
		if (pCtx != nullptr)
			ctx = *pCtx;
		else
			ctx.cancel = cancel;
		cancel = ctx.cancel;
		auto cancelled = [cancel]() { return cancel != nullptr && cancel->load(); };

		const fs::path src = fs::u8path(absSrc);
		std::error_code ec;
		const std::string& folder = plan.folder;
		const std::string& name = plan.assetName;
		const bool transcode = options.NeedsTranscode(plan.kind);

		// FFmpeg first: nothing is written when it is missing
		std::string exe;
		if (options.NeedsFfmpeg(plan.kind))
		{
			CFfmpegService* const pService = CFfmpegService::Get();
			std::string why;
			if (pService == nullptr)
				why = "the FFmpeg service is not available";
			else if (!pService->WaitForExe(cancel, exe, why))
			{
				r.error = why;
				return r;
			}
		}

		const fs::path dstDir = fs::u8path(AssetsAbsolute(folder));
		fs::create_directories(dstDir, ec);
		if (ec)
		{
			r.error = "cannot create the folder " + dstDir.u8string() + ": " + ec.message();
			return r;
		}
		const std::string videoName = name + ".webm";
		const std::string descriptorName = name + ".cevvideo";
		const fs::path videoPath = dstDir / fs::u8path(videoName);
		const fs::path stagePath = dstDir / fs::u8path(name + ".stage.webm");

		// files this run creates from nothing: gone again when the run fails or is cancelled (existing files are never deleted)
		std::vector<fs::path> created;
		auto track = [&created](const fs::path& p)
		{
			std::error_code e;
			if (!fs::exists(p, e))
				created.push_back(p);
		};
		auto fail = [&](const std::string& message) -> SImportResult&
		{
			std::error_code e;
			fs::remove(stagePath, e);
			for (const fs::path& p : created)
				fs::remove(p, e);
			r.error = cancelled() ? std::string("cancelled") : message;
			return r;
		};
		if (cancelled())
			return fail("cancelled");

		// ---- 1. the source copy (and, for a plain WebM, that is the video) ----
		std::string sourceSha;
		fs::path copyPath;
		if (plan.kind != ESourceKind::PngSequence)
		{
			copyPath = fs::u8path(AssetsAbsolute(plan.sourceCopyRel));
			track(copyPath);
			if (!CopyFileReplace(src, copyPath, r.error))
				return fail(r.error);
			if (!Sha256File(copyPath.u8string(), sourceSha, cancel))
				return fail("cannot hash the copied video");
		}
		ctx.Progress(0.05f);

		// ---- 2. the encode ----
		const int variantCount = static_cast<int>(options.variants.size());
		const float encodeEnd = transcode ? (variantCount > 0 ? 0.05f + 0.95f / static_cast<float>(1 + variantCount) : 1.f) : 0.05f;
		SEncodeOptions enc;
		enc.crf = options.crf;
		enc.height = options.height;
		if (transcode)
		{
			ctx.Log(std::string("encoding ") + (plan.kind == ESourceKind::PngSequence ? plan.pngPattern : copyPath.u8string()) + " -> " + videoName + " (VP9 / Opus, crf " + std::to_string(options.crf) +
				(options.height > 0 ? ", height " + std::to_string(options.height) : std::string(", source size")) + ")");
			if (plan.kind == ESourceKind::PngSequence)
			{
				enc.inputFps = options.fps > 0.0 ? options.fps : 30.0;
				enc.inputStartNumber = plan.pngStart;
			}
			const SOpResult encoded = FfmpegTranscode(exe, plan.kind == ESourceKind::PngSequence ? plan.pngPattern : copyPath.u8string(), stagePath.u8string(), enc, SubContext(ctx, 0.05f, encodeEnd));
			if (!encoded.Ok())
				return fail(encoded.status == EOpStatus::Cancelled ? std::string("cancelled") : "FFmpeg failed: " + encoded.message);
			track(videoPath);
			fs::rename(stagePath, videoPath, ec);
			if (ec)
				return fail("cannot move the encoded video to " + videoPath.u8string() + ": " + ec.message() + " (is it open in a player?)");
		}

		// ---- 3. what the video is ----
		SMediaInfo info;
		std::string probeError;
		if (!ProbeMediaFile(videoPath.u8string(), info, probeError, cancel))
			return fail("cannot read the video: " + probeError);

		// ---- 4. the variants ----
		std::vector<SDescriptor::SVariant> madeVariants;
		if (variantCount > 0)
		{
			std::vector<int> heights = options.variants;
			std::sort(heights.begin(), heights.end(), std::greater<int>());
			heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
			std::vector<int> usable;
			for (int h : heights)
			{
				if (h < info.height)
					usable.push_back(h);
				else
					ctx.Log("variant " + std::to_string(h) + "p skipped: not below the video height (" + std::to_string(info.height) + ")");
			}
			const float span = transcode ? 0.95f / static_cast<float>(1 + variantCount) : 1.f / static_cast<float>(std::max(1, variantCount));
			float at = transcode ? encodeEnd : 0.05f;
			SEncodeOptions venc = enc;
			venc.height = 0;
			venc.inputFps = 0.0;
			venc.inputStartNumber = -1;
			for (int h : usable)
			{
				if (cancelled())
					return fail("cancelled");
				const std::string file = name + "_" + std::to_string(h) + "p.webm";
				const fs::path varPath = dstDir / fs::u8path(file);
				ctx.Log("variant " + std::to_string(h) + "p -> " + file);
				track(varPath);
				const SOpResult made = FfmpegMakeVariant(exe, videoPath.u8string(), varPath.u8string(), h, venc, SubContext(ctx, at, at + span));
				at += span;
				if (!made.Ok())
					return fail(made.status == EOpStatus::Cancelled ? std::string("cancelled") : "variant " + std::to_string(h) + "p failed: " + made.message);
				SDescriptor::SVariant v;
				v.file = file;
				v.height = h;
				v.width = info.height > 0 ? (static_cast<int>(std::llround(static_cast<double>(info.width) * h / info.height)) + 1) / 2 * 2 : 0;
				SMediaInfo vinfo;
				std::string vError;
				if (ProbeMediaFile(varPath.u8string(), vinfo, vError, cancel) && vinfo.valid)
				{
					v.width = vinfo.width;
					v.height = vinfo.height;
				}
				madeVariants.push_back(v);
			}
		}
		if (cancelled())
			return fail("cancelled");

		// ---- 5. descriptor, details, .cryasset ----
		SDescriptor desc;
		desc.route = options.route == "mixed" ? "mixed" : "embedded"; // "cue" is applied once a WAV / Ogg exists
		desc.video = videoName;
		if (!info.audioTracks.empty())
		{
			SDescriptor::SLanguage lang;
			lang.name = options.language.empty() ? "english" : options.language;
			lang.embedded = true;
			desc.languages.push_back(lang);
			desc.defaultLanguage = lang.name;
		}
		std::sort(madeVariants.begin(), madeVariants.end(), [](const SDescriptor::SVariant& a, const SDescriptor::SVariant& b) { return a.height < b.height; });
		desc.variants = madeVariants;
		if (!desc.variants.empty())
			desc.variantRule = "smallestNotBelowOutput";
		track(dstDir / fs::u8path(descriptorName));
		std::string error;
		if (!SaveDescriptor((dstDir / fs::u8path(descriptorName)).u8string(), desc, error))
			return fail(error);

		CDetails details;
		details.Set(DetailKey::kSourcePath, plan.sourceCopyRel);
		details.Set(DetailKey::kSourceOriginal, absSrc);
		details.Set(DetailKey::kSourceSha256, sourceSha);
		const std::uintmax_t sourceSize = plan.kind == ESourceKind::PngSequence ? 0 : fs::file_size(src, ec);
		details.Set(DetailKey::kSourceSize, std::to_string(static_cast<unsigned long long>(ec ? 0 : sourceSize)));
		details.Set(DetailKey::kSourceKind, plan.kind == ESourceKind::Webm ? "webm" : plan.kind == ESourceKind::Video ? "video" : "png-sequence");
		details.Set(DetailKey::kTranscoded, transcode ? "1" : "0");
		details.Set(DetailKey::kImportSettings, options.ToSettingsString());
		details.Set(DetailKey::kImportState, "ready");
		FillMediaDetails(details, info);
		details.Set(DetailKey::kRoute, desc.route);
		std::vector<std::string> languageNames;
		for (const SDescriptor::SLanguage& l : desc.languages)
			languageNames.push_back(l.name);
		details.Set(DetailKey::kLanguages, JoinList(languageNames, ','));

		// Overwriting: the new CAsset carries the GUID of the existing one; MergeAssets (main thread, FinishImport) then
		// updates the existing asset in place. The asset manager's own objects are never touched from this thread.
		std::vector<std::string> files = { folder + descriptorName, folder + videoName };
		if (!plan.sourceCopyRel.empty() && std::find(files.begin(), files.end(), plan.sourceCopyRel) == files.end())
			files.push_back(plan.sourceCopyRel);
		for (const SDescriptor::SVariant& v : madeVariants)
			files.push_back(folder + v.file);
		for (const std::string& kept : plan.keptFiles)
		{
			const bool dropped = std::any_of(plan.dropFiles.begin(), plan.dropFiles.end(), [&kept](const std::string& d) { return _stricmp(d.c_str(), kept.c_str()) == 0; });
			const bool already = std::any_of(files.begin(), files.end(), [&kept](const std::string& f) { return _stricmp(f.c_str(), kept.c_str()) == 0; });
			if (!dropped && !already)
				files.push_back(kept);
		}
		if (!plan.keptSdlFiles.empty())
			details.Set(DetailKey::kSdlFiles, plan.keptSdlFiles);
		if (!plan.keptAceLibrary.empty())
			details.Set(DetailKey::kAceLibrary, plan.keptAceLibrary);

		std::unique_ptr<CAsset> pAsset(new CAsset(kTypeName, plan.guid.IsNull() ? CryGUID::Create() : plan.guid, name.c_str()));
		CAssetImportContext importCtx;
		CEditableAsset edit = importCtx.CreateEditableAsset(*pAsset);
		edit.SetMetadataFile(r.metadataRel.c_str());
		std::vector<string> fileList;
		for (const std::string& f : files)
			fileList.emplace_back(f.c_str());
		edit.SetFiles(fileList);
		edit.SetDetails(details.ToPairs());
		track(fs::u8path(AssetsAbsolute(r.metadataRel)));
		if (!edit.WriteToFile())
			return fail("cannot write " + r.metadataRel + " (read-only file or folder?)");

		// the replaced asset's files that this import made obsolete (old source copy, old variants that were not made again)
		for (const std::string& dropped : plan.dropFiles)
		{
			const bool reused = std::any_of(files.begin(), files.end(), [&dropped](const std::string& f) { return _stricmp(f.c_str(), dropped.c_str()) == 0; });
			if (!reused)
				fs::remove(fs::u8path(AssetsAbsolute(dropped)), ec);
		}
		r.pAsset = pAsset.release();
		r.ok = true;
		ctx.Progress(1.f);
		return r;
	}

	void FinishImport(const SImportResult& result, const SImportOptions& options)
	{
		if (!result.ok || result.pAsset == nullptr)
			return;
		CAssetManager::GetInstance()->MergeAssets({ result.pAsset });
		CryLogAlways("[CEVEditor] imported video asset '%s' (%s)", result.assetName.c_str(), result.metadataRel.c_str());
		StartPostImportOps(result.metadataRel, result.assetName, options);
	}

	void StartPostImportOps(const std::string& metadataRel, const std::string& assetName, const SImportOptions& options)
	{
		if (!options.NeedsAudioOps() && options.route != "cue")
			return;
		CAsset* const pAsset = FindAssetByMetadata(metadataRel);
		if (pAsset == nullptr)
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] audio operations for '%s' not started: the asset is not in the asset manager yet", assetName.c_str());
			return;
		}
		SAudioOps ops;
		ops.split = options.split != ESplitAudio::None;
		ops.strip = options.split == ESplitAudio::StripEmbedded;
		ops.wav = options.wav;
		ops.ogg = options.ogg;
		ops.oggQuality = options.oggQuality;
		ops.language = options.language.empty() ? "english" : options.language;
		ops.upgradeToCue = options.route == "cue";
		if (ops.upgradeToCue && !ops.wav && !ops.ogg)
			ops.wav = true; // the cue route needs a sound to point at
		std::string error;
		if (SubmitAudioOps(*pAsset, ops, error) == 0)
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] audio operations for '%s' not started: %s", assetName.c_str(), error.c_str());
	}

	// ---------------------------------------------------------------------------------------------------------------
	bool ReimportVideo(CAsset& asset, std::string& error)
	{
		CDetails details(asset);
		if (details.Get(DetailKey::kTranscoded) == "1")
		{
			error = "this asset's video was made by FFmpeg: reimport runs as an FFmpeg operation";
			return false;
		}
		const std::string original = details.Get(DetailKey::kSourceOriginal);
		const std::string copyRel = details.Get(DetailKey::kSourcePath);
		if (original.empty() || copyRel.empty())
		{
			error = "the asset has no recorded source file";
			return false;
		}
		std::error_code ec;
		const fs::path src = fs::u8path(original);
		if (!fs::is_regular_file(src, ec))
		{
			error = "the original file no longer exists: " + original;
			return false;
		}
		SMediaInfo info;
		if (!ProbeMediaFile(original, info, error))
			return false;
		if (!CopyFileReplace(src, fs::u8path(AssetsAbsolute(copyRel)), error))
			return false;
		std::string sha;
		if (!Sha256File(AssetsAbsolute(copyRel), sha))
		{
			error = "cannot hash the copied video";
			return false;
		}
		details.Set(DetailKey::kSourceSha256, sha);
		details.Set(DetailKey::kSourceSize, std::to_string(static_cast<unsigned long long>(fs::file_size(src, ec))));
		FillMediaDetails(details, info);
		details.Set(DetailKey::kImportState, "ready");
		if (!WriteAssetFilesAndDetails(asset, nullptr, &details))
		{
			error = "cannot write the .cryasset";
			return false;
		}
		return true;
	}

	SSourceStatus CheckSourceStatus(const std::string& copyAbs, const std::string& originalAbs, const std::string& importedSha256, const std::atomic<bool>* cancel)
	{
		SSourceStatus s;
		s.originalPath = originalAbs;
		std::error_code ec;
		std::string hash;
		s.copyExists = !copyAbs.empty() && fs::is_regular_file(fs::u8path(copyAbs), ec);
		if (s.copyExists && Sha256File(copyAbs, hash, cancel))
			s.copyMatchesImport = importedSha256.empty() || hash == importedSha256;
		s.originalExists = !originalAbs.empty() && fs::is_regular_file(fs::u8path(originalAbs), ec);
		if (s.originalExists && Sha256File(originalAbs, hash, cancel))
			s.originalMatchesImport = importedSha256.empty() || hash == importedSha256;
		s.checked = !(cancel != nullptr && cancel->load());
		if (!s.copyExists)
			s.text = "The copy of the source video is missing.";
		else if (!s.copyMatchesImport)
			s.text = "The copy of the source video was changed after the import (hash differs).";
		else if (!s.originalExists)
			s.text = originalAbs.empty() ? "Source up to date (no original path recorded)." : "Source up to date. The original file is not at its recorded location any more.";
		else if (!s.originalMatchesImport)
			s.text = "Source updated, reimport needed: the original file differs from the imported copy.";
		else
			s.text = "Source up to date.";
		return s;
	}

	// ---------------------------------------------------------------------------------------------------------------
	namespace
	{
		struct SProduced
		{
			std::string kind;  // "split" | "wav" | "ogg" | "strip"
			std::string rel;   // relative to Assets
		};

		// Applies the files a finished job produced to the asset (main thread).
		void ApplyProduced(CAsset& asset, const std::vector<SProduced>& produced, const SAudioOps& ops)
		{
			if (produced.empty())
				return;
			std::vector<std::string> files = AssetFileList(asset);
			for (const SProduced& p : produced)
			{
				if (std::find(files.begin(), files.end(), p.rel) == files.end())
					files.push_back(p.rel);
			}

			const std::string descAbs = AssetsAbsolute(DescriptorRel(asset));
			SDescriptor desc;
			std::string error;
			if (LoadDescriptor(descAbs, desc, error))
			{
				bool changed = false;
				for (const SProduced& p : produced)
				{
					const std::string file = FileNameOf(p.rel);
					if (p.kind == "split")
					{
						int index = desc.FindLanguage(ops.language);
						if (index < 0)
						{
							SDescriptor::SLanguage lang;
							lang.name = ops.language;
							desc.languages.push_back(lang);
							index = static_cast<int>(desc.languages.size()) - 1;
						}
						desc.languages[static_cast<size_t>(index)].file = file;
						desc.languages[static_cast<size_t>(index)].embedded = false;
						if (desc.defaultLanguage.empty())
							desc.defaultLanguage = ops.language;
						if (desc.route == "embedded")
							desc.route = "mixed";
						changed = true;
					}
					else if (p.kind == "strip")
					{
						desc.video = file;
						changed = true;
					}
				}
				if (changed && !SaveDescriptor(descAbs, desc, error))
					CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] cannot update the descriptor of '%s': %s", asset.GetName().c_str(), error.c_str());
			}
			else
			{
				CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] cannot read the descriptor of '%s': %s", asset.GetName().c_str(), error.c_str());
			}

			CDetails details(asset);
			details.Set(DetailKey::kRoute, desc.route);
			std::vector<std::string> languageNames;
			for (const SDescriptor::SLanguage& l : desc.languages)
				languageNames.push_back(l.name);
			details.Set(DetailKey::kLanguages, JoinList(languageNames, ','));
			WriteAssetFilesAndDetails(asset, &files, &details);
		}
	}

	int SubmitAudioOps(CAsset& asset, const SAudioOps& ops, std::string& error, std::function<void(bool ok)> onFinished)
	{
		CJobRunner* const pRunner = CJobRunner::Get();
		if (pRunner == nullptr)
		{
			error = "the job runner is not available";
			return 0;
		}
		if (!(ops.split || ops.strip || ops.wav || ops.ogg || ops.upgradeToCue))
		{
			error = "no operation requested";
			return 0;
		}
		const CDetails details(asset);
		const std::string folder = FolderOf(asset.GetMetadataFile().c_str());
		const std::string name = asset.GetName().c_str();
		const std::string metadataRel = asset.GetMetadataFile().c_str();
		// the audio comes from <name>.webm: the copy of a WebM source, or what FFmpeg made of a transcoded source (the source
		// copy may be an mp4 / mov). Older assets without that file fall back to the recorded source copy.
		std::string copyRel = folder + name + ".webm";
		{
			std::error_code existsEc;
			if (!fs::exists(fs::u8path(AssetsAbsolute(copyRel)), existsEc) && !details.Get(DetailKey::kSourcePath).empty())
				copyRel = details.Get(DetailKey::kSourcePath);
		}
		const std::string srcAbs = AssetsAbsolute(copyRel);
		const std::string lang = ops.language.empty() ? "english" : ops.language;

		const std::string splitRel = folder + name + "_" + lang + ".webm";
		const std::string wavRel = folder + name + "_" + lang + ".wav";
		const std::string oggRel = folder + name + "_" + lang + ".ogg";
		const std::string stripRel = folder + name + "_silent.webm";

		auto produced = std::make_shared<std::vector<SProduced>>();
		const SAudioOps opsCopy = ops;

		// The job thread only runs IOfflineOps and file I/O. the calls below reach CEVPlayerOffline
		// once SetOfflineOps() was given the real implementation; with the stub every call reports NotAvailable.
		auto work = [=](const SOpContext& ctx) -> SOpResult
		{
			IOfflineOps& offline = GetOfflineOps();
			SOpResult last;
			last.status = EOpStatus::Ok;
			struct SStep { bool wanted; const char* what; const char* kind; const std::string* rel; };
			const SStep steps[] = {
				{ opsCopy.split, "split audio", "split", &splitRel },
				{ opsCopy.wav, "export WAV", "wav", &wavRel },
				{ opsCopy.ogg, "export Ogg Vorbis", "ogg", &oggRel },
				{ opsCopy.strip, "strip audio", "strip", &stripRel },
			};
			int done = 0;
			int total = 0;
			for (const SStep& s : steps)
				total += s.wanted ? 1 : 0;
			// Cancelled / failed run (the user, quit, a delete / rename of the asset): the completion that would list the
			// files may not run, so the files this run created from nothing go away again (earlier files stay).
			std::vector<std::string> createdNew;
			auto discardCreated = [&createdNew, produced]()
			{
				for (const std::string& abs : createdNew)
				{
					std::error_code ec;
					fs::remove(fs::u8path(abs), ec);
				}
				createdNew.clear();
				produced->clear();
			};
			for (const SStep& s : steps)
			{
				if (!s.wanted)
					continue;
				if (ctx.Cancelled())
				{
					discardCreated();
					last.status = EOpStatus::Cancelled;
					last.message = "cancelled";
					return last;
				}
				ctx.Log(std::string(s.what) + " -> " + *s.rel);
				const std::string dstAbs = AssetsAbsolute(*s.rel);
				std::error_code existsEc;
				const bool existedBefore = fs::exists(fs::u8path(dstAbs), existsEc);
				SOpResult r;
				const std::string kind = s.kind;
				if (kind == "split")
					r = offline.ExtractAudioWebm(srcAbs, opsCopy.trackIndex, dstAbs, ctx);
				else if (kind == "wav")
					r = offline.ExportWav(srcAbs, opsCopy.trackIndex, dstAbs, ctx);
				else if (kind == "ogg")
					r = offline.ExportOggVorbis(srcAbs, opsCopy.trackIndex, dstAbs, opsCopy.oggQuality, ctx);
				else
					r = offline.StripAudio(srcAbs, dstAbs, ctx);
				if (!r.Ok())
				{
					discardCreated();
					r.message = std::string(s.what) + " failed: " + r.message;
					return r;
				}
				if (!existedBefore)
					createdNew.push_back(dstAbs);
				produced->push_back({ kind, *s.rel });
				ctx.Progress(static_cast<float>(++done) / static_cast<float>(total));
			}
			return last;
		};

		const CryGUID guid = asset.GetGUID();
		auto onDone = [metadataRel, guid, produced, opsCopy, name, onFinished](const SOpResult& result)
		{
			// by GUID first: the asset may have been renamed / moved while the job ran (those operations wait for the job)
			CAsset* pAsset = CAssetManager::GetInstance()->FindAssetById(guid);
			if (pAsset == nullptr)
				pAsset = FindAssetByMetadata(metadataRel);
			if (pAsset == nullptr)
			{
				// the asset was deleted meanwhile: do not leave the files the job produced behind
				for (const SProduced& p : *produced)
				{
					std::error_code ec;
					fs::remove(fs::u8path(AssetsAbsolute(p.rel)), ec);
				}
				CryLogAlways("[CEVEditor] audio operations for '%s': the asset is gone, %u produced file(s) removed", name.c_str(), static_cast<unsigned>(produced->size()));
				if (onFinished)
					onFinished(false);
				return;
			}
			ApplyProduced(*pAsset, *produced, opsCopy);
			std::string error;
			bool cueOk = true;
			if (opsCopy.upgradeToCue && result.Ok())
				cueOk = UpgradeToCueRoute(*pAsset, error);
			CDetails details(*pAsset);
			details.Set(DetailKey::kImportState, result.Ok() && cueOk ? "ready" : "failed");
			WriteAssetFilesAndDetails(*pAsset, nullptr, &details);
			if (result.Ok() && cueOk)
			{
				CryLogAlways("[CEVEditor] audio operations for '%s' finished", name.c_str());
				Notify(false, QObject::tr("Video asset"), QObject::tr("Audio operations for %1 finished.").arg(QString::fromUtf8(name.c_str())));
			}
			else
			{
				const std::string why = !result.Ok() ? result.message : error;
				CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] audio operations for '%s' did not complete: %s", name.c_str(), why.c_str());
				Notify(true, QObject::tr("Video asset"), QObject::tr("Audio operations for %1 did not complete: %2").arg(QString::fromUtf8(name.c_str()), QString::fromUtf8(why.c_str())));
			}
			if (onFinished)
				onFinished(result.Ok() && cueOk);
		};

		return pRunner->Submit(QObject::tr("Audio operations: %1").arg(QString::fromUtf8(name.c_str())), work, onDone, AssetOwnerKey(guid));
	}

	// ---------------------------------------------------------------------------------------------------------------
	// ---------------------------------------------------------------------------------------------------------------
	std::vector<int> VariantHeights(const SDescriptor& desc)
	{
		std::vector<int> out;
		for (const SDescriptor::SVariant& v : desc.variants)
			out.push_back(v.height);
		std::sort(out.begin(), out.end());
		return out;
	}

	namespace
	{
		// What a transcode job made; the completion (main thread) applies it to the asset.
		struct STranscodeJobResult
		{
			bool recompressed = false;
			std::string newSourceRel;                    // set when the source copy was created / refreshed
			std::string newSourceSha;
			std::string newSourceSize;
			SMediaInfo media;                            // of the new main video (recompress)
			bool mediaKnown = false;
			std::vector<SDescriptor::SVariant> variants; // made by this job
			fs::path stage;                              // <name>.stage.webm, renamed over the main video by the completion
			fs::path stripStage;                         // <name>_silent.stage.webm
			std::vector<fs::path> created;               // new files (removed again when the completion cannot apply them)
		};

		void RemoveQuiet(const fs::path& p)
		{
			if (p.empty())
				return;
			std::error_code ec;
			fs::remove(p, ec);
		}

		void ApplyVariants(SDescriptor& desc, const std::vector<SDescriptor::SVariant>& made, const std::vector<std::string>& removedFiles)
		{
			for (const std::string& f : removedFiles)
			{
				desc.variants.erase(std::remove_if(desc.variants.begin(), desc.variants.end(), [&f](const SDescriptor::SVariant& v) { return _stricmp(v.file.c_str(), f.c_str()) == 0; }), desc.variants.end());
			}
			for (const SDescriptor::SVariant& v : made)
			{
				auto it = std::find_if(desc.variants.begin(), desc.variants.end(), [&v](const SDescriptor::SVariant& o) { return _stricmp(o.file.c_str(), v.file.c_str()) == 0; });
				if (it != desc.variants.end())
					*it = v;
				else
					desc.variants.push_back(v);
			}
			std::stable_sort(desc.variants.begin(), desc.variants.end(), [](const SDescriptor::SVariant& a, const SDescriptor::SVariant& b) { return a.height < b.height; });
			desc.variantRule = desc.variants.empty() ? std::string() : std::string("smallestNotBelowOutput");
		}
	}

	bool IsTranscodedAsset(const CAsset& asset)
	{
		return CDetails(asset).Get(DetailKey::kTranscoded) == "1";
	}

	int SubmitTranscodeOps(CAsset& asset, const STranscodeOps& ops, std::string& error, std::function<void(bool ok)> onFinished)
	{
		CJobRunner* const pRunner = CJobRunner::Get();
		CFfmpegService* const pService = CFfmpegService::Get();
		if (pRunner == nullptr || pService == nullptr)
		{
			error = "the job runner is not available";
			return 0;
		}
		if (!ops.recompress && !ops.changeVariants)
		{
			error = "no operation requested";
			return 0;
		}
		{
			const SFfmpegStatus status = pService->Status();
			if (!status.Pending() && !status.Ready())
			{
				error = pService->UnavailableMessage();
				return 0;
			}
			if (status.state == SFfmpegStatus::EState::Unknown)
				pService->EnsureDetected();
		}
		const CDetails details(asset);
		const std::string folder = FolderOf(asset.GetMetadataFile().c_str());
		const std::string name = asset.GetName().c_str();
		const std::string metadataRel = asset.GetMetadataFile().c_str();
		const CryGUID guid = asset.GetGUID();
		SDescriptor desc;
		if (!LoadDescriptor(AssetsAbsolute(DescriptorRel(asset)), desc, error))
			return 0;
		const std::string masterRel = folder + name + ".webm";
		const std::string mainRel = folder + desc.video;
		const bool stripped = _stricmp(FileNameOf(desc.video).c_str(), (name + "_silent.webm").c_str()) == 0;
		std::error_code ec;
		if (!fs::is_regular_file(fs::u8path(AssetsAbsolute(mainRel)), ec))
		{
			error = "the video file of the asset is missing: " + mainRel;
			return 0;
		}
		std::string sourceRel = details.Get(DetailKey::kSourcePath);
		if (ops.recompress)
		{
			if (!fs::is_regular_file(fs::u8path(AssetsAbsolute(masterRel)), ec) && sourceRel.empty())
			{
				error = "the asset has neither a source copy nor " + masterRel;
				return 0;
			}
			if (ops.refreshSource)
			{
				const std::string original = details.Get(DetailKey::kSourceOriginal);
				if (sourceRel.empty() || !fs::is_regular_file(fs::u8path(original), ec))
				{
					error = "the original source file is not available: " + (original.empty() ? std::string("(none recorded)") : original);
					return 0;
				}
			}
		}
		const std::string original = details.Get(DetailKey::kSourceOriginal);
		const std::vector<SDescriptor::SVariant> oldVariants = desc.variants;
		const STranscodeOps o = ops;

		auto result = std::make_shared<STranscodeJobResult>();
		auto work = [=](const SOpContext& ctx) -> SOpResult
		{
			SOpResult out;
			out.status = EOpStatus::Failed;
			std::string exe;
			std::string why;
			if (!CFfmpegService::Get() || !CFfmpegService::Get()->WaitForExe(ctx.cancel, exe, why))
			{
				out.status = ctx.Cancelled() ? EOpStatus::Cancelled : EOpStatus::Failed;
				out.message = why;
				return out;
			}
			auto discard = [&]()
			{
				for (const fs::path& p : result->created)
					RemoveQuiet(p);
				result->created.clear();
				RemoveQuiet(result->stage);
				RemoveQuiet(result->stripStage);
				result->stage.clear();
				result->stripStage.clear();
				result->variants.clear();
				result->newSourceRel.clear();
			};
			auto failWith = [&](const SOpResult& r, const std::string& prefix) -> SOpResult
			{
				discard();
				SOpResult f;
				f.status = r.status == EOpStatus::Cancelled || ctx.Cancelled() ? EOpStatus::Cancelled : EOpStatus::Failed;
				f.message = f.status == EOpStatus::Cancelled ? std::string("cancelled") : prefix + r.message;
				return f;
			};
			auto track = [&](const fs::path& p)
			{
				std::error_code e;
				if (!fs::exists(p, e))
					result->created.push_back(p);
			};

			const int variantTotal = o.changeVariants ? static_cast<int>(o.variants.size()) : 0;
			const int steps = (o.recompress ? 1 : 0) + variantTotal;
			float at = 0.f;
			const float span = steps > 0 ? 1.f / static_cast<float>(steps) : 1.f;
			std::string variantSource = AssetsAbsolute(mainRel);
			int videoHeight = 0;
			SEncodeOptions enc;
			enc.crf = o.crf;
			enc.height = o.height;

			if (o.recompress)
			{
				std::string srcRel = sourceRel;
				std::string srcAbs;
				std::error_code e;
				const bool separateSource = !srcRel.empty() && _stricmp(srcRel.c_str(), masterRel.c_str()) != 0;
				if (separateSource)
				{
					srcAbs = AssetsAbsolute(srcRel);
					if (o.refreshSource)
					{
						std::string copyError;
						ctx.Log("copying the original file over the source copy");
						if (!CopyFileReplace(fs::u8path(original), fs::u8path(srcAbs), copyError))
						{
							SOpResult f;
							f.message = copyError;
							return failWith(f, "");
						}
					}
				}
				else
				{
					// no separate source copy (a plain WebM import, or a PNG sequence): the current video becomes the source copy,
					// so that later re-encodes start from it again instead of from an already re-encoded file
					srcRel = folder + name + "_source.webm";
					srcAbs = AssetsAbsolute(srcRel);
					ctx.Log("the current video becomes the source copy: " + srcRel);
					track(fs::u8path(srcAbs));
					std::string copyError;
					if (!CopyFileReplace(fs::u8path(AssetsAbsolute(masterRel)), fs::u8path(srcAbs), copyError))
					{
						SOpResult f;
						f.message = copyError;
						return failWith(f, "");
					}
					result->newSourceRel = srcRel;
				}
				if (o.refreshSource || result->newSourceRel.size() > 0)
				{
					std::string sha;
					if (!Sha256File(srcAbs, sha, ctx.cancel))
					{
						SOpResult f;
						f.message = "cannot hash the source copy";
						return failWith(f, "");
					}
					result->newSourceRel = srcRel;
					result->newSourceSha = sha;
					result->newSourceSize = std::to_string(static_cast<unsigned long long>(fs::file_size(fs::u8path(srcAbs), e)));
				}
				result->stage = fs::u8path(AssetsAbsolute(folder + name + ".stage.webm"));
				ctx.Log("re-encoding " + srcRel + " (VP9 / Opus, crf " + std::to_string(o.crf) + (o.height > 0 ? ", height " + std::to_string(o.height) : std::string(", source size")) + ")");
				const SOpResult encoded = FfmpegTranscode(exe, srcAbs, result->stage.u8string(), enc, SubContext(ctx, at, at + span));
				at += span;
				if (!encoded.Ok())
					return failWith(encoded, "FFmpeg failed: ");
				std::string probeError;
				result->mediaKnown = ProbeMediaFile(result->stage.u8string(), result->media, probeError, ctx.cancel);
				if (!result->mediaKnown)
				{
					SOpResult f;
					f.message = "cannot read the re-encoded video: " + probeError;
					return failWith(f, "");
				}
				videoHeight = result->media.height;
				variantSource = result->stage.u8string();
				result->recompressed = true;
				if (stripped)
				{
					result->stripStage = fs::u8path(AssetsAbsolute(folder + name + "_silent.stage.webm"));
					ctx.Log("strip audio of the new video (the descriptor plays the picture-only copy)");
					const SOpResult strippedResult = GetOfflineOps().StripAudio(result->stage.u8string(), result->stripStage.u8string(), SubContext(ctx, at, at));
					if (!strippedResult.Ok())
						return failWith(strippedResult, "strip audio failed: ");
					variantSource = result->stripStage.u8string();
				}
			}
			else
			{
				SMediaInfo mainInfo;
				std::string probeError;
				if (!ProbeMediaFile(variantSource, mainInfo, probeError, ctx.cancel))
				{
					SOpResult f;
					f.message = "cannot read the video: " + probeError;
					return failWith(f, "");
				}
				videoHeight = mainInfo.height;
			}

			if (o.changeVariants)
			{
				std::vector<int> heights = o.variants;
				std::sort(heights.begin(), heights.end(), std::greater<int>());
				heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
				SEncodeOptions venc;
				venc.crf = o.crf;
				for (int h : heights)
				{
					if (ctx.Cancelled())
						return failWith(SOpResult(), "");
					if (h >= videoHeight)
					{
						ctx.Log("variant " + std::to_string(h) + "p skipped: not below the video height (" + std::to_string(videoHeight) + ")");
						at += span;
						continue;
					}
					const std::string file = name + "_" + std::to_string(h) + "p.webm";
					const fs::path varPath = fs::u8path(AssetsAbsolute(folder + file));
					ctx.Log("variant " + std::to_string(h) + "p -> " + file);
					track(varPath);
					const SOpResult made = FfmpegMakeVariant(exe, variantSource, varPath.u8string(), h, venc, SubContext(ctx, at, at + span));
					at += span;
					if (!made.Ok())
						return failWith(made, "variant " + std::to_string(h) + "p failed: ");
					SDescriptor::SVariant v;
					v.file = file;
					v.height = h;
					SMediaInfo vinfo;
					std::string vError;
					if (ProbeMediaFile(varPath.u8string(), vinfo, vError, ctx.cancel) && vinfo.valid)
					{
						v.width = vinfo.width;
						v.height = vinfo.height;
					}
					result->variants.push_back(v);
				}
			}
			out.status = EOpStatus::Ok;
			return out;
		};

		auto onDone = [=](const SOpResult& jobResult)
		{
			auto finish = [&](bool ok, const std::string& text)
			{
				if (ok)
				{
					CryLogAlways("[CEVEditor] FFmpeg operation for '%s' finished: %s", name.c_str(), text.c_str());
					Notify(false, QObject::tr("Video asset"), QObject::tr("FFmpeg operation for %1 finished.").arg(QString::fromUtf8(name.c_str())));
				}
				else if (text == "cancelled")
				{
					CryLogAlways("[CEVEditor] FFmpeg operation for '%s' cancelled", name.c_str());
				}
				else
				{
					CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] FFmpeg operation for '%s' did not complete: %s", name.c_str(), text.c_str());
					Notify(true, QObject::tr("Video asset"), QObject::tr("FFmpeg operation for %1 did not complete: %2").arg(QString::fromUtf8(name.c_str()), QString::fromUtf8(text.c_str())));
				}
				if (onFinished)
					onFinished(ok);
			};
			auto discard = [&]()
			{
				for (const fs::path& p : result->created)
					RemoveQuiet(p);
				RemoveQuiet(result->stage);
				RemoveQuiet(result->stripStage);
			};
			CAsset* pAsset = CAssetManager::GetInstance()->FindAssetById(guid);
			if (pAsset == nullptr)
				pAsset = FindAssetByMetadata(metadataRel);
			if (pAsset == nullptr)
			{
				discard();
				CryLogAlways("[CEVEditor] FFmpeg operation for '%s': the asset is gone, the produced files were removed", name.c_str());
				if (onFinished)
					onFinished(false);
				return;
			}
			if (!jobResult.Ok())
			{
				discard();
				finish(false, jobResult.message);
				return;
			}

			std::string applyError;
			std::vector<std::string> files = AssetFileList(*pAsset);
			CDetails newDetails(*pAsset);
			SDescriptor newDesc;
			std::string descError;
			if (!LoadDescriptor(AssetsAbsolute(DescriptorRel(*pAsset)), newDesc, descError))
			{
				discard();
				finish(false, "cannot read the descriptor: " + descError);
				return;
			}
			std::vector<std::string> removedVariantFiles;

			if (result->recompressed)
			{
				// replace the video: nobody may hold it open (the preview player of the editor window, other jobs of the asset)
				if (!ReleaseAssetFiles(metadataRel, guid, 15000, applyError))
				{
					discard();
					finish(false, applyError);
					return;
				}
				std::error_code e;
				const fs::path master = fs::u8path(AssetsAbsolute(masterRel));
				fs::rename(result->stage, master, e);
				if (e)
				{
					discard();
					finish(false, "cannot replace " + masterRel + ": " + e.message() + " (is it open in a player?)");
					return;
				}
				result->stage.clear();
				if (!result->stripStage.empty())
				{
					const fs::path silent = fs::u8path(AssetsAbsolute(folder + name + "_silent.webm"));
					fs::rename(result->stripStage, silent, e);
					if (e)
						CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] cannot replace %s: %s", silent.u8string().c_str(), e.message().c_str());
					result->stripStage.clear();
				}
				if (o.removeOtherVariants)
				{
					for (const SDescriptor::SVariant& old : oldVariants)
					{
						const bool remade = std::any_of(result->variants.begin(), result->variants.end(), [&old](const SDescriptor::SVariant& v) { return _stricmp(v.file.c_str(), old.file.c_str()) == 0; });
						if (!remade)
							removedVariantFiles.push_back(old.file);
					}
				}
				else
				{
					// variants that stay are older than the new video now: the panel marks them as stale
				}
				FillMediaDetails(newDetails, result->media);
				if (!result->newSourceRel.empty())
				{
					newDetails.Set(DetailKey::kSourcePath, result->newSourceRel);
					newDetails.Set(DetailKey::kSourceSha256, result->newSourceSha);
					newDetails.Set(DetailKey::kSourceSize, result->newSourceSize);
					if (std::find_if(files.begin(), files.end(), [&](const std::string& f) { return _stricmp(f.c_str(), result->newSourceRel.c_str()) == 0; }) == files.end())
						files.push_back(result->newSourceRel);
				}
				newDetails.Set(DetailKey::kTranscoded, "1");
				SImportOptions settings = SImportOptions::FromSettingsString(newDetails.Get(DetailKey::kImportSettings));
				settings.recompress = true;
				settings.crf = o.crf;
				settings.height = o.height;
				newDetails.Set(DetailKey::kImportSettings, settings.ToSettingsString());
			}
			else if (o.removeOtherVariants)
			{
				for (const SDescriptor::SVariant& old : oldVariants)
				{
					const bool remade = std::any_of(result->variants.begin(), result->variants.end(), [&old](const SDescriptor::SVariant& v) { return _stricmp(v.file.c_str(), old.file.c_str()) == 0; });
					if (!remade)
						removedVariantFiles.push_back(old.file);
				}
			}

			if (o.changeVariants)
			{
				ApplyVariants(newDesc, result->variants, removedVariantFiles);
				std::string saveError;
				if (!SaveDescriptor(AssetsAbsolute(DescriptorRel(*pAsset)), newDesc, saveError))
					CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] cannot update the descriptor of '%s': %s", name.c_str(), saveError.c_str());
				for (const SDescriptor::SVariant& v : result->variants)
				{
					if (std::find_if(files.begin(), files.end(), [&](const std::string& f) { return _stricmp(f.c_str(), (folder + v.file).c_str()) == 0; }) == files.end())
						files.push_back(folder + v.file);
				}
				for (const std::string& removed : removedVariantFiles)
				{
					const std::string rel = folder + removed;
					files.erase(std::remove_if(files.begin(), files.end(), [&rel](const std::string& f) { return _stricmp(f.c_str(), rel.c_str()) == 0; }), files.end());
					std::error_code e;
					fs::remove(fs::u8path(AssetsAbsolute(rel)), e);
				}
				SImportOptions settings = SImportOptions::FromSettingsString(newDetails.Get(DetailKey::kImportSettings));
				settings.variants = VariantHeights(newDesc);
				newDetails.Set(DetailKey::kImportSettings, settings.ToSettingsString());
			}
			newDetails.Set(DetailKey::kImportState, "ready");
			if (!WriteAssetFilesAndDetails(*pAsset, &files, &newDetails))
			{
				finish(false, "cannot write the .cryasset");
				return;
			}
			finish(true, std::to_string(result->variants.size()) + " variant(s)" + (result->recompressed ? ", video re-encoded" : ""));
		};

		return pRunner->Submit(QObject::tr("FFmpeg: %1").arg(QString::fromUtf8(name.c_str())), work, onDone, AssetOwnerKey(guid));
	}

	// ---------------------------------------------------------------------------------------------------------------
	bool UpgradeToCueRoute(CAsset& asset, std::string& error)
	{
		const std::string name = asset.GetName().c_str();
		const std::string folder = FolderOf(asset.GetMetadataFile().c_str());

		// WAV / Ogg files of the asset: <name>.ext -> no language, <name>_<language>.ext -> language. WAV wins over Ogg.
		std::map<std::string, std::string> byLanguage; // language -> relative path
		for (const std::string& rel : AssetFileList(asset))
		{
			const bool wav = HasExt(rel, "wav");
			const bool ogg = HasExt(rel, "ogg");
			if (!wav && !ogg)
				continue;
			const std::string stem = StemOf(rel);
			std::string language;
			if (stem.size() > name.size() && _strnicmp(stem.c_str(), name.c_str(), name.size()) == 0 && stem[name.size()] == '_')
				language = stem.substr(name.size() + 1);
			else if (_stricmp(stem.c_str(), name.c_str()) != 0)
				continue;
			auto it = byLanguage.find(language);
			if (it == byLanguage.end() || (wav && !HasExt(it->second, "wav")))
				byLanguage[language] = rel;
		}
		if (byLanguage.empty())
		{
			error = "the asset has no WAV or Ogg file: export one first";
			return false;
		}

		std::vector<SAceSound> sounds;
		for (const auto& kv : byLanguage)
			sounds.push_back({ kv.first, AssetsAbsolute(kv.second) });
		const SAceResult ace = RegisterAssetSounds(name, sounds);
		if (!ace.ok)
		{
			error = "ACE registration failed: " + ace.error;
			return false;
		}

		const std::string descAbs = AssetsAbsolute(DescriptorRel(asset));
		SDescriptor desc;
		if (!LoadDescriptor(descAbs, desc, error))
			return false;
		desc.route = "cue";
		std::string defaultLanguage = desc.defaultLanguage;
		for (const SAceTriggers& t : ace.triggers)
		{
			if (t.language.empty())
			{
				desc.trigger = t.play;
				desc.pauseTrigger = t.pause;
				desc.resumeTrigger = t.resume;
				continue;
			}
			int index = desc.FindLanguage(t.language);
			if (index < 0)
			{
				SDescriptor::SLanguage lang;
				lang.name = t.language;
				desc.languages.push_back(lang);
				index = static_cast<int>(desc.languages.size()) - 1;
			}
			SDescriptor::SLanguage& lang = desc.languages[static_cast<size_t>(index)];
			lang.trigger = t.play;
			lang.pauseTrigger = t.pause;
			lang.resumeTrigger = t.resume;
			if (defaultLanguage.empty())
				defaultLanguage = t.language;
		}
		desc.defaultLanguage = defaultLanguage;
		bool hasUnnamedSound = false;
		for (const SAceTriggers& t : ace.triggers)
			hasUnnamedSound = hasUnnamedSound || t.language.empty();
		if (!hasUnnamedSound && !ace.triggers.empty())
		{
			// default triggers of the languages without their own: those of the default language. Always renewed:
			// after a rename the old asset name must not stay in them.
			for (const SAceTriggers& t : ace.triggers)
			{
				if (t.language.empty() || _stricmp(t.language.c_str(), defaultLanguage.c_str()) == 0)
				{
					desc.trigger = t.play;
					desc.pauseTrigger = t.pause;
					desc.resumeTrigger = t.resume;
					break;
				}
			}
		}
		if (!SaveDescriptor(descAbs, desc, error))
			return false;

		CDetails details(asset);
		details.Set(DetailKey::kRoute, desc.route);
		details.Set(DetailKey::kSdlFiles, JoinList(ace.copiedRel, ';'));
		details.Set(DetailKey::kAceLibrary, ace.libraryRel);
		std::vector<std::string> languageNames;
		for (const SDescriptor::SLanguage& l : desc.languages)
			languageNames.push_back(l.name);
		details.Set(DetailKey::kLanguages, JoinList(languageNames, ','));
		if (!WriteAssetFilesAndDetails(asset, nullptr, &details))
		{
			error = "cannot write the .cryasset";
			return false;
		}
		CryLogAlways("[CEVEditor] '%s' now uses the cue route (ACE library %s)", name.c_str(), ace.libraryRel.c_str());
		return true;
	}
}
