// The tool window "CEV Video Editor": a CAssetEditor (docking container with the engine's Asset Browser on the left,
// already filtered to the asset type of the editor; double click / selection opens the asset), the preview in the
// middle (CVideoPreviewPanel) and the info / operations panel on the right (CVideoInfoPanel).
#pragma once

#include <memory>
#include <string>

#include <QPointer>

#include <AssetSystem/AssetEditor.h>
#include <CryExtension/CryGUID.h>

#include "Asset/VideoDescriptorIO.h"
#include "Asset/VideoAssetMeta.h"
#include "Asset/VideoImport.h"
#include "Offline/IOfflineOps.h"

namespace CEVEditor
{
	class CVideoPreviewPanel;
	class CVideoInfoPanel;

	class CVideoEditor : public CAssetEditor
	{
		Q_OBJECT
	public:
		static constexpr const char* kEditorName = "CEV Video Editor"; // = REGISTER_VIEWPANE_FACTORY name = CVideoAssetType::Edit

		//! REGISTER_VIEWPANE_FACTORY creates the pane with `new CVideoEditor()`: a default constructor is required.
		CVideoEditor();
		~CVideoEditor() override;

		const char* GetEditorName() const override { return kEditorName; }

		struct SState
		{
			CryGUID guid;
			std::string assetName;
			std::string metadataRel;
			std::string descriptorRel;     //!< Files[0], relative to Assets
			std::string descriptorAbs;
			std::string folderRel;         //!< with trailing slash
			std::string videoRel;          //!< the descriptor's main video, relative to Assets
			std::string sourceCopyRel;     //!< Details sourcePath
			std::vector<std::string> files;   //!< Files of the asset (relative to Assets), .cevvideo first
			CDetails details;
			SDescriptor descriptor;        //!< edited copy (saved by OnSaveAsset)
			SDescriptor baseDescriptor;    //!< what the file contained when it was last read / written (merge base)
			SMediaInfo media;
			bool mediaKnown = false;
			SSourceStatus source;
		};
		const SState& State() const { return m_state; }
		bool HasAsset() const { return m_stateValid; }
		//! True when the in-memory descriptor differs from the file (unsaved edits).
		bool HasUnsavedDescriptor() const { return m_stateValid && !DescriptorsEqual(m_state.descriptor, m_state.baseDescriptor); }
		//! Bumped whenever the descriptor in the state was replaced by something other than the user's own edit
		//! (open, reload from disk, merge): the info panel refills its tables then.
		int DescriptorRevision() const { return m_descriptorRevision; }

		// ---- edits from the panels (main thread) ----
		void SetDescriptor(const SDescriptor& descriptor);   //!< marks the asset modified; saved by Save
		void ReloadPreview();
		//! The engine's reimport of the asset (CAsset::Reimport -> CVideoAssetImporter::ReimportAsset, stored import settings).
		//! False when the asset has no registered source file (PNG sequence) or is read-only.
		bool Reimport();
		//! Checks the source file again (size / time, hash when they changed); the result arrives with StateChanged.
		void RecheckSource();
		//! After a reimport: media info and source status are read again, the preview reloads.
		void RefreshAfterReimport();
		void RunAudioOps(const SAudioOps& ops);
		//! FFmpeg operations (variants / re-encode) on a job; the window refreshes when it is done.
		void RunTranscodeOps(const STranscodeOps& ops);
		//! The dialogs of the operation buttons (they ask for FFmpeg first when none is usable).
		void GenerateVariantsDialog();
		void RecompressDialog();
		void UpgradeToCue();
		void RefreshAssetState();                              //!< re-reads Details, descriptor and the file list (unsaved edits are merged)
		static CVideoEditor* Current();                        //!< the last window that opened an asset (console command, tests)
		//! The preview panel of this window (null while it is not created / was closed).
		CVideoPreviewPanel* PreviewPanel() const;
		//! The info panel of this window (null while it is not created / was closed).
		CVideoInfoPanel* InfoPanel() const;

		//! Opens the dropped / selected files (webm mp4 mov mkv avi, folders of PNG files): options dialog, then the import queue
		//! (one file at a time on a job, merge on the main thread). Several files: one dialog, one batch.
		void ImportFiles(const QStringList& files);

		//! Destroys the preview player of this window when it shows the asset (files are about to be replaced / renamed /
		//! deleted); the next state refresh loads it again.
		void ReleasePlayerForAsset(const std::string& metadataRel);

	signals:
		void StateChanged();
		void PreviewPanelChanged();

	protected:
		bool OnOpenAsset(CAsset* pAsset) override;
		void OnCloseAsset() override;
		bool OnSaveAsset(CEditableAsset& editAsset) override;
		void OnDiscardAssetChanges(CEditableAsset& editAsset) override;
		void OnInitialize() override;
		void OnCreateDefaultLayout(CDockableContainer* pSender, QWidget* pAssetBrowser) override;
		void dragEnterEvent(QDragEnterEvent* pEvent) override;
		void dropEvent(QDropEvent* pEvent) override;

	private:
		bool LoadState(CAsset& asset, bool keepUnsavedEdits);
		void StartBackgroundChecks();
		void PushToPreview();
		void OnAssetChangedHook(CAsset& asset, int changeFlags);
		void QueueRefresh();
		std::string PreviewKey() const;

		SState m_state;
		QPointer<CVideoPreviewPanel> m_pPreview;
		QPointer<CVideoInfoPanel> m_pInfo;
		bool m_stateValid = false;
		bool m_refreshQueued = false;
		bool m_forcePreviewReload = false;
		int m_generation = 0;   //!< bumped on open / close: results of older jobs are dropped
		int m_descriptorRevision = 0;
		std::string m_pushedPreviewKey;
	};
}
