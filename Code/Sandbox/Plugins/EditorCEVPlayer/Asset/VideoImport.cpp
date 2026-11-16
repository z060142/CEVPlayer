#include "StdAfx.h"
#include "Asset/VideoImport.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
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
		return JoinSettings({
			{ "language", language }, { "route", route }, { "trigger", trigger }, { "split", splitName },
			{ "wav", wav ? "1" : "0" }, { "ogg", ogg ? "1" : "0" }, { "oggQuality", quality } });
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
	SImportResult ImportVideoFile(const std::string& absSrc, const std::string& folderRel, const SImportOptions& options)
	{
		SImportResult r;
		const fs::path src = fs::u8path(absSrc);
		std::error_code ec;
		if (!fs::is_regular_file(src, ec))
		{
			r.error = "source file not found: " + absSrc;
			return r;
		}
		if (!HasExt(absSrc, "webm"))
		{
			r.error = "only .webm files can be imported (other formats come with the FFmpeg stage): " + absSrc;
			return r;
		}
		const std::string folder = NormalizeFolder(folderRel);
		if (!SafeFolder(folder))
		{
			r.error = "invalid destination folder: " + folderRel;
			return r;
		}
		const std::string name = options.assetName.empty() ? SanitizeAssetName(src.stem().u8string()) : SanitizeAssetName(options.assetName);
		r.assetName = name;
		r.metadataRel = folder + name + ".cevvideo.cryasset";
		{
			string reason;
			if (!CAssetType::IsValidAssetPath(r.metadataRel.c_str(), reason))
			{
				r.error = std::string("invalid asset path ") + r.metadataRel + ": " + reason.c_str();
				return r;
			}
		}

		SMediaInfo info;
		std::string probeError;
		if (!ProbeMediaFile(absSrc, info, probeError))
		{
			r.error = "cannot read the video: " + probeError;
			return r;
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
		if (!CopyFileReplace(src, dstDir / fs::u8path(videoName), r.error))
			return r;
		std::string sha;
		if (!Sha256File((dstDir / fs::u8path(videoName)).u8string(), sha))
		{
			r.error = "cannot hash the copied video";
			return r;
		}

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
		std::string error;
		if (!SaveDescriptor((dstDir / fs::u8path(descriptorName)).u8string(), desc, error))
		{
			r.error = error;
			return r;
		}

		CDetails details;
		details.Set(DetailKey::kSourcePath, folder + videoName);
		details.Set(DetailKey::kSourceOriginal, absSrc);
		details.Set(DetailKey::kSourceSha256, sha);
		details.Set(DetailKey::kSourceSize, std::to_string(static_cast<unsigned long long>(fs::file_size(src, ec))));
		details.Set(DetailKey::kImportSettings, options.ToSettingsString());
		details.Set(DetailKey::kImportState, "ready");
		FillMediaDetails(details, info);
		details.Set(DetailKey::kRoute, desc.route);
		std::vector<std::string> languageNames;
		for (const SDescriptor::SLanguage& l : desc.languages)
			languageNames.push_back(l.name);
		details.Set(DetailKey::kLanguages, JoinList(languageNames, ','));

		CAsset* pAsset = FindAssetByMetadata(r.metadataRel);
		if (pAsset == nullptr)
			pAsset = new CAsset(kTypeName, CryGUID::Create(), name.c_str());
		CAssetImportContext ctx;
		CEditableAsset edit = ctx.CreateEditableAsset(*pAsset);
		edit.SetMetadataFile(r.metadataRel.c_str());
		edit.SetFiles({ string((folder + descriptorName).c_str()), string((folder + videoName).c_str()) });
		edit.SetDetails(details.ToPairs());
		if (!edit.WriteToFile())
		{
			r.error = "cannot write " + r.metadataRel;
			return r;
		}
		r.pAsset = pAsset;
		r.ok = true;
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
		std::string copyRel = details.Get(DetailKey::kSourcePath);
		if (copyRel.empty())
			copyRel = folder + name + ".webm";
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
			for (const SStep& s : steps)
			{
				if (!s.wanted)
					continue;
				if (ctx.Cancelled())
				{
					last.status = EOpStatus::Cancelled;
					last.message = "cancelled";
					return last;
				}
				ctx.Log(std::string(s.what) + " -> " + *s.rel);
				const std::string dstAbs = AssetsAbsolute(*s.rel);
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
					r.message = std::string(s.what) + " failed: " + r.message;
					return r;
				}
				produced->push_back({ kind, *s.rel });
				ctx.Progress(static_cast<float>(++done) / static_cast<float>(total));
			}
			return last;
		};

		auto onDone = [metadataRel, produced, opsCopy, name, onFinished](const SOpResult& result)
		{
			CAsset* const pAsset = FindAssetByMetadata(metadataRel);
			if (pAsset == nullptr)
			{
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

		return pRunner->Submit(QObject::tr("Audio operations: %1").arg(QString::fromUtf8(name.c_str())), work, onDone);
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
		if (desc.trigger.empty() && !ace.triggers.empty())
		{
			// default triggers of the languages without their own: those of the default language
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
