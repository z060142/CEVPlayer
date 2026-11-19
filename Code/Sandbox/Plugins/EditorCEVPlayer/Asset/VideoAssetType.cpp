#include "StdAfx.h"
#include "Asset/VideoAssetType.h"

#include <filesystem>

#include <AssetSystem/Asset.h>
#include <AssetSystem/AssetEditor.h>
#include <AssetSystem/AssetImportContext.h>
#include <AssetSystem/EditableAsset.h>
#include <PathUtils.h>

#include "Asset/VideoAssetMeta.h"
#include "Asset/VideoDescriptorIO.h"
#include "Asset/VideoImport.h"
#include "Audio/AceRegistrar.h"

namespace CEVEditor
{
	REGISTER_ASSET_TYPE(CVideoAssetType)

	namespace fs = std::filesystem;

	// Detail columns of the Asset Browser (names are what the user sees; the values come from the Details of the .cryasset).
	CItemModelAttribute CVideoAssetType::s_widthAttribute("Width", &Attributes::s_intAttributeType, CItemModelAttribute::StartHidden);
	CItemModelAttribute CVideoAssetType::s_heightAttribute("Height", &Attributes::s_intAttributeType, CItemModelAttribute::StartHidden);
	CItemModelAttribute CVideoAssetType::s_durationAttribute("Duration (s)", &Attributes::s_floatAttributeType, CItemModelAttribute::StartHidden);
	CItemModelAttribute CVideoAssetType::s_codecAttribute("Codec", &Attributes::s_stringAttributeType, CItemModelAttribute::StartHidden);
	CItemModelAttribute CVideoAssetType::s_alphaAttribute("Alpha", &Attributes::s_stringAttributeType, CItemModelAttribute::StartHidden);
	CItemModelAttribute CVideoAssetType::s_audioAttribute("Audio", &Attributes::s_stringAttributeType, CItemModelAttribute::StartHidden);
	CItemModelAttribute CVideoAssetType::s_routeAttribute("Audio route", &Attributes::s_stringAttributeType, CItemModelAttribute::StartHidden);
	CItemModelAttribute CVideoAssetType::s_trackNamesAttribute("Audio track names", &Attributes::s_stringAttributeType, CItemModelAttribute::StartHidden);
	CItemModelAttribute CVideoAssetType::s_stateAttribute("Import state", &Attributes::s_stringAttributeType, CItemModelAttribute::StartHidden);

	CAssetEditor* CVideoAssetType::Edit(CAsset* pAsset) const
	{
		// The name is the one the window is registered under (REGISTER_VIEWPANE_FACTORY in VideoEditor.cpp).
		return CAssetEditor::OpenAssetForEdit("CEV Video Editor", pAsset);
	}

	CryIcon CVideoAssetType::GetIconInternal() const
	{
		return CryIcon(":/EditorCEVPlayer/icons/assets_video.ico"); // Qt resource of this plug-in (EditorCEVPlayer.qrc)
	}

	std::vector<CItemModelAttribute*> CVideoAssetType::GetDetails() const
	{
		return {
			&s_widthAttribute, &s_heightAttribute, &s_durationAttribute, &s_codecAttribute, &s_alphaAttribute,
			&s_audioAttribute, &s_routeAttribute, &s_trackNamesAttribute, &s_stateAttribute };
	}

	QVariant CVideoAssetType::GetDetailValue(const CAsset* pAsset, const CItemModelAttribute* pDetail) const
	{
		if (pAsset == nullptr || pDetail == nullptr)
			return QVariant();
		const char* key = nullptr;
		if (pDetail == &s_widthAttribute) key = DetailKey::kWidth;
		else if (pDetail == &s_heightAttribute) key = DetailKey::kHeight;
		else if (pDetail == &s_durationAttribute) key = DetailKey::kDuration;
		else if (pDetail == &s_codecAttribute) key = DetailKey::kCodec;
		else if (pDetail == &s_audioAttribute) key = DetailKey::kAudioTracks;
		else if (pDetail == &s_routeAttribute) key = DetailKey::kRoute;
		else if (pDetail == &s_trackNamesAttribute) key = DetailKey::kTrackNames;
		else if (pDetail == &s_stateAttribute) key = DetailKey::kImportState;
		else if (pDetail == &s_alphaAttribute)
			return QVariant(QString(pAsset->GetDetailValue(DetailKey::kAlpha) == "1" ? "yes" : "no"));
		if (key == nullptr)
			return QVariant();
		return GetVariantFromDetail(pAsset->GetDetailValue(key), pDetail);
	}

	namespace
	{
		// The engine makes a Sound asset (<file>.cryasset) of the .wav / .ogg files under Assets. Those metadata files are
		// not Files of the video asset: when the files are renamed, moved or deleted they would stay behind and describe
		// files that are gone (the engine creates fresh ones for the new names / places by itself).
		void RemoveSoundAssetMetadata(const CAsset& asset)
		{
			for (const string& file : asset.GetFiles())
			{
				const std::string f = file.c_str();
				const size_t dot = f.find_last_of('.');
				if (dot == std::string::npos)
					continue;
				const std::string ext = f.substr(dot + 1);
				if (_stricmp(ext.c_str(), "wav") == 0 || _stricmp(ext.c_str(), "ogg") == 0)
				{
					std::error_code ec;
					fs::remove(fs::u8path(AssetsAbsolute(f + ".cryasset")), ec);
				}
			}
		}

		// Files are about to be renamed / moved / deleted: nobody may hold them open (preview player, background jobs).
		bool ReleaseForFileChange(const CAsset& asset, const char* what, int timeoutMs)
		{
			std::string error;
			if (ReleaseAssetFiles(asset.GetMetadataFile().c_str(), asset.GetGUID(), timeoutMs, error))
				return true;
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] cannot %s '%s': %s", what, asset.GetName().c_str(), error.c_str());
			return false;
		}
	}

	bool CVideoAssetType::MoveAsset(CAsset* pAsset, const char* szDestinationFolder, bool bMoveSourcefile) const
	{
		if (pAsset == nullptr || szDestinationFolder == nullptr)
			return false;
		if (!ReleaseForFileChange(*pAsset, "move", 10000))
			return false;
		RemoveSoundAssetMetadata(*pAsset);
		const std::string oldSourcePath = CDetails(*pAsset).Get(DetailKey::kSourcePath);

		// moves the metadata and every file of the asset (wav / ogg / webm too) into the folder
		if (!CAssetType::MoveAsset(pAsset, szDestinationFolder, bMoveSourcefile))
			return false;

		// the Details name the source copy by path: follow it (the descriptor names files relative to itself, the SDL_mixer
		// copies and the ACE library do not depend on the folder)
		if (!oldSourcePath.empty())
		{
			CDetails details(*pAsset);
			details.Set(DetailKey::kSourcePath, FolderOf(pAsset->GetMetadataFile().c_str()) + FileNameOf(oldSourcePath));
			WriteAssetFilesAndDetails(*pAsset, nullptr, &details);
		}
		return true;
	}

	bool CVideoAssetType::RenameAsset(CAsset* pAsset, const char* szNewName) const
	{
		if (pAsset == nullptr || szNewName == nullptr)
			return false;
		const std::string oldName = pAsset->GetName().c_str();
		const std::string newName = szNewName;
		const bool hadAce = !CDetails(*pAsset).Get(DetailKey::kAceLibrary).empty();
		if (oldName != newName)
		{
			if (!ReleaseForFileChange(*pAsset, "rename", 10000))
				return false;
			RemoveSoundAssetMetadata(*pAsset);
		}

		// Renames the files whose names start with the old asset name (CAssetType::RenameAsset); the content of the
		// descriptor and the Details still carry the old names afterwards.
		if (!CAssetType::RenameAsset(pAsset, szNewName))
			return false;

		bool ok = true;
		const std::string descAbs = AssetsAbsolute(DescriptorRel(*pAsset));
		SDescriptor desc;
		std::string error;
		if (LoadDescriptor(descAbs, desc, error))
		{
			RenameDescriptorFiles(desc, oldName, newName);
			if (!SaveDescriptor(descAbs, desc, error))
			{
				CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] rename: cannot rewrite %s: %s", descAbs.c_str(), error.c_str());
				ok = false;
			}
		}
		else
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] rename: cannot read %s: %s", descAbs.c_str(), error.c_str());
			ok = false;
		}

		CDetails details(*pAsset);
		std::string sourcePath = details.Get(DetailKey::kSourcePath);
		if (!sourcePath.empty())
		{
			// the copy of the source was renamed with the asset: keep the folder, replace the prefix of the file name
			const std::string folder = FolderOf(sourcePath);
			std::string file = FileNameOf(sourcePath);
			if (_strnicmp(file.c_str(), oldName.c_str(), oldName.size()) == 0)
				file = newName + file.substr(oldName.size());
			details.Set(DetailKey::kSourcePath, folder + file);
		}
		WriteAssetFilesAndDetails(*pAsset, nullptr, &details);

		if (hadAce)
		{
			// The SDL_mixer copies and the ACE library carry the old asset name (trigger names too): register again
			// under the new one. UpgradeToCueRoute removes nothing itself, so drop the old registration first.
			RemoveAssetSounds(oldName);
			if (!UpgradeToCueRoute(*pAsset, error))
			{
				CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] rename: ACE registration of '%s' not renewed: %s", newName.c_str(), error.c_str());
				ok = false;
			}
		}
		return ok;
	}

	void CVideoAssetType::PreDeleteAssetFiles(const CAsset& asset) const
	{
		// The files are deleted right after this (on the engine's file operation queue): the preview player and the jobs of
		// the asset must have let go of them. Deleting goes on in any case, the log says when something was still open.
		ReleaseForFileChange(asset, "release the files of", 5000);

		// The SDL_mixer copies and the ACE library are not Files of the asset: remove them with it.
		if (!CDetails(asset).Get(DetailKey::kAceLibrary).empty() || !CDetails(asset).Get(DetailKey::kSdlFiles).empty())
			RemoveAssetSounds(asset.GetName().c_str());

		// The engine makes a Sound asset (<file>.cryasset) of every .wav / .ogg under Assets: do not leave those behind
		// (the files themselves are deleted with the video asset).
		RemoveSoundAssetMetadata(asset);
	}
}
