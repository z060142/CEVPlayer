// Asset type "CEVVideo": the main file is the runtime descriptor <name>.cevvideo, the video / audio files of the asset are
// the other Files of the .cryasset, all flat in one folder (names start with the asset name).
#pragma once

#include <AssetSystem/AssetType.h>

namespace CEVEditor
{
	class CVideoAssetType : public CAssetType
	{
	public:
		DECLARE_ASSET_TYPE_DESC(CVideoAssetType);

		const char* GetTypeName() const override { return "CEVVideo"; }          // written into .cryasset: never change
		const char* GetUiTypeName() const override { return QT_TR_NOOP("Video"); }
		const char* GetFileExtension() const override { return "cevvideo"; }
		bool CanBeEdited() const override { return true; }
		CAssetEditor* Edit(CAsset* pAsset) const override;
		// E1: the source copy next to the asset (Details sourcePath) is registered as the engine's source file (SetSourceFile), so the
		// Asset Browser offers Reimport and CVideoAssetImporter::ReimportAsset runs with the stored import settings.
		bool IsImported() const override { return true; }
		// MUST be false: otherwise the asset generator runs rc.exe for every .cevvideo without a .cryasset (docs/research/04 §2.5).
		bool CanAutoRepairMetadata() const override { return false; }
		bool RenameAsset(CAsset* pAsset, const char* szNewName) const override;
		bool MoveAsset(CAsset* pAsset, const char* szDestinationFolder, bool bMoveSourcefile) const override;
		void PreDeleteAssetFiles(const CAsset& asset) const override;
		std::vector<CItemModelAttribute*> GetDetails() const override;
		QVariant GetDetailValue(const CAsset* pAsset, const CItemModelAttribute* pDetail) const override;
		QColor GetThumbnailColor() const override { return QColor(150, 100, 220); }
		CryIcon GetIconInternal() const override;

		static CItemModelAttribute s_widthAttribute;
		static CItemModelAttribute s_heightAttribute;
		static CItemModelAttribute s_durationAttribute;
		static CItemModelAttribute s_codecAttribute;
		static CItemModelAttribute s_alphaAttribute;
		static CItemModelAttribute s_audioAttribute;
		static CItemModelAttribute s_routeAttribute;
		static CItemModelAttribute s_languagesAttribute;
		static CItemModelAttribute s_stateAttribute;
	};
}
