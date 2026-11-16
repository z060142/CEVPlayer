#include "StdAfx.h"
#include "Asset/VideoAssetImporter.h"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>

#include <AssetSystem/AssetImportContext.h>
#include <IEditor.h>
#include <PathUtils.h>
#include <ThreadingUtils.h>

#include "Asset/VideoImport.h"
#include "Editor/ImportOptionsDialog.h"

namespace CEVEditor
{
	REGISTER_ASSET_IMPORTER(CVideoAssetImporter)

	std::vector<string> CVideoAssetImporter::GetAssetNames(const std::vector<string>&, CAssetImportContext& ctx)
	{
		// The name the asset will get (the engine checks these paths for overwriting before ImportAssets is called).
		ctx.OverrideAssetName(string(SanitizeAssetName(ctx.GetAssetName().c_str()).c_str()));
		const std::string folder = NormalizeFolder(ctx.GetOutputDirectoryPath().c_str());
		return { string((folder + ctx.GetAssetName().c_str() + ".cevvideo.cryasset").c_str()) };
	}

	std::vector<CAsset*> CVideoAssetImporter::ImportAssets(const std::vector<string>&, CAssetImportContext& ctx)
	{
		const std::string source = ctx.GetInputFilePath().c_str();
		const std::string folder = ctx.GetOutputDirectoryPath().c_str();

		SImportOptions options;
		options.assetName = ctx.GetAssetName().c_str();
		if (!CImportOptionsDialog::Ask(QString::fromUtf8(source.c_str()), options))
			return {}; // cancelled

		SImportResult result = ImportVideoFile(source, folder, options);
		if (!result.ok)
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] import of '%s' failed: %s", source.c_str(), result.error.c_str());
			return {};
		}
		CryLogAlways("[CEVEditor] imported video asset '%s' (%s)", result.assetName.c_str(), result.metadataRel.c_str());

		if (options.NeedsAudioOps() || options.route == "cue")
		{
			// The drop handler merges the returned asset on the main thread after this function returns. The audio
			// operations need the asset in the asset manager, so they start a little later (main thread, Qt timer).
			const std::string metadataRel = result.metadataRel;
			const std::string name = result.assetName;
			ThreadingUtils::PostOnMainThread([metadataRel, name, options]()
			{
				QTimer::singleShot(500, QCoreApplication::instance(), [metadataRel, name, options]() { StartPostImportOps(metadataRel, name, options); });
			});
		}
		return { result.pAsset };
	}
}
