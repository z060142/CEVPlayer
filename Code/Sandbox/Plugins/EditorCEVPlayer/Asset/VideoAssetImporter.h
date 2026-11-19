// Importer for dropped / imported .webm .mp4 .mov .mkv .avi files (NOT .png: textures use it; a folder of PNG files is imported through
// the editor window / the console). The engine calls ImportAssets on its background queue; the options dialog is shown through
// CImportOptionsDialog::Ask (no waiting on the main thread from the main thread). A plain WebM copy runs right there; everything that
// needs FFmpeg (other formats, re-encode, variants) is handed to the import queue (own jobs) so the engine queue never blocks, and
// returns no asset (the queue merges it when its job is done). The long operations run on the plug-in's own job runner.
#pragma once

#include <AssetSystem/AssetImporter.h>

namespace CEVEditor
{
	class CVideoAssetImporter : public CAssetImporter
	{
	public:
		DECLARE_ASSET_IMPORTER_DESC(CVideoAssetImporter);

		std::vector<string> GetFileExtensions() const override { return { "webm", "mp4", "mov", "mkv", "avi" }; }
		std::vector<string> GetAssetTypes() const override { return { "CEVVideo" }; }

	private:
		std::vector<string> GetAssetNames(const std::vector<string>& assetTypes, CAssetImportContext& ctx) override;
		std::vector<CAsset*> ImportAssets(const std::vector<string>& assetPaths, CAssetImportContext& ctx) override;
		//! The engine's Reimport (Asset Browser, CAsset::Reimport): no dialog, the import settings stored in the Details are used
		//! again (the default implementation would import afresh and lose them, AssetImporter.cpp). See ReimportWithStoredSettings.
		void ReimportAsset(CAsset* pAsset) override;
	};
}
