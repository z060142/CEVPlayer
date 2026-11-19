#include "StdAfx.h"
#include "Asset/VideoAssetImporter.h"

#include <QCoreApplication>
#include <QThread>
#include <QTimer>

#include <AssetSystem/AssetImportContext.h>
#include <IEditor.h>
#include <PathUtils.h>
#include <ThreadingUtils.h>

#include "Asset/ImportQueue.h"
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

		ESourceKind kind = ESourceKind::Webm;
		if (!ClassifySource(source, kind))
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] import of '%s' failed: this file type cannot be imported (%s)", source.c_str(), ImportExtensionsText().c_str());
			return {};
		}
		SImportOptions options;
		options.assetName = ctx.GetAssetName().c_str();
		// the engine already asked about overwriting for the original name (GetAssetNames / CanWrite); a name changed in
		// the dialog is checked by Ask (it prompts and may send the user back to the dialog)
		const std::string confirmedName = SanitizeAssetName(options.assetName);
		if (!CImportOptionsDialog::Ask(QString::fromUtf8(source.c_str()), options, folder, confirmedName, kind, false))
			return {}; // cancelled

		if (options.NeedsFfmpeg(kind))
		{
			// Minutes of ffmpeg do not belong on the engine's single import queue: the import queue (own jobs, one ffmpeg at a
			// time) does it and merges the asset itself, so this returns no asset. Fire and forget: nothing waits for the main thread.
			ThreadingUtils::PostOnMainThread([source, folder, options, confirmedName]() mutable
			{
				CImportQueue* const pQueue = CImportQueue::Get();
				if (pQueue == nullptr)
					return;
				const SImportPlan plan = PlanImport(source, folder, options);
				if (plan.ok && plan.exists && plan.assetName == confirmedName)
					options.overwrite = true; // the engine's own confirmation covers the original name
				const int batch = pQueue->BeginBatch(QObject::tr("Import %1").arg(QString::fromUtf8(source.c_str())));
				pQueue->Enqueue(batch, source, folder, options);
				pQueue->EndBatch(batch);
			});
			CryLogAlways("[CEVEditor] import of '%s' queued (FFmpeg)", source.c_str());
			return {};
		}

		// the plan (asset manager lookups) and the preparation of an overwrite run on the main thread: this function
		// runs on the engine's background queue. Waiting here is fine, the main thread does not wait for us.
		SImportPlan plan;
		std::string prepareError;
		bool prepared = true;
		auto planOnMain = [&]()
		{
			plan = PlanImport(source, folder, options);
			if (plan.ok && plan.exists)
			{
				if (plan.assetName == confirmedName)
					options.overwrite = true; // the engine's own confirmation covers the original name
				if (options.overwrite)
					prepared = PrepareOverwrite(plan, prepareError);
			}
		};
		if (QThread::currentThread() == QCoreApplication::instance()->thread())
			planOnMain();
		else
			ThreadingUtils::PostOnMainThread(planOnMain).get();
		if (plan.ok && plan.exists && !options.overwrite)
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] import of '%s' refused: %s", source.c_str(), plan.detail.c_str());
			return {};
		}
		if (!plan.ok || !prepared)
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] import of '%s' failed: %s", source.c_str(), plan.ok ? prepareError.c_str() : plan.error.c_str());
			return {};
		}

		SImportResult result = ImportVideoFile(source, plan, options);
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

	void CVideoAssetImporter::ReimportAsset(CAsset* pAsset)
	{
		if (pAsset == nullptr)
			return;
		std::string error;
		const std::string name = pAsset->GetName().c_str();
		const int result = ReimportWithStoredSettings(*pAsset, error, [name](bool ok)
		{
			if (ok)
				CryLogAlways("[CEVEditor] reimport of '%s' done (stored import settings)", name.c_str());
		});
		if (result == 0)
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] reimport of '%s' failed: %s", name.c_str(), error.c_str());
		else if (result == 2)
			CryLogAlways("[CEVEditor] reimport of '%s' started (FFmpeg job with the stored import settings)", name.c_str());
	}
}
