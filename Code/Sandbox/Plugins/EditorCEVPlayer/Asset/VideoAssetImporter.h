// Importer for dropped / imported .webm files (extension list grows in M5). The engine calls ImportAssets on its
// background queue; the options dialog is shown through CImportOptionsDialog::Ask (no waiting on the main thread from
// the main thread), the quick part of the import runs right there, the long operations run on the plug-in's own job runner.
#pragma once

#include <AssetSystem/AssetImporter.h>

namespace CEVEditor
{
	class CVideoAssetImporter : public CAssetImporter
	{
	public:
		DECLARE_ASSET_IMPORTER_DESC(CVideoAssetImporter);

		std::vector<string> GetFileExtensions() const override { return { "webm" }; }
		std::vector<string> GetAssetTypes() const override { return { "CEVVideo" }; }

	private:
		std::vector<string> GetAssetNames(const std::vector<string>& assetTypes, CAssetImportContext& ctx) override;
		std::vector<CAsset*> ImportAssets(const std::vector<string>& assetPaths, CAssetImportContext& ctx) override;
	};
}
