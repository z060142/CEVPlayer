// The tool window "CEV Video Editor": a CAssetEditor (docking container with the engine's Asset Browser on the left,
// already filtered to the asset type of the editor; double click / selection opens the asset), the preview in the
// middle (CVideoPreviewPanel) and the info / operations panel on the right (CVideoInfoPanel).
#pragma once

#include <memory>
#include <string>

#include <QPointer>

#include <AssetSystem/AssetEditor.h>

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
			SMediaInfo media;
			bool mediaKnown = false;
			SSourceStatus source;
		};
		const SState& State() const { return m_state; }
		bool HasAsset() const { return m_stateValid; }

		// ---- edits from the panels (main thread) ----
		void SetDescriptor(const SDescriptor& descriptor);   //!< marks the asset modified; saved by Save
		void ReloadPreview();
		//! Replaces the file of the source copy again from the recorded original (preview player is released first).
		bool ReimportSource(std::string& error);
		void RunAudioOps(const SAudioOps& ops);
		void UpgradeToCue();
		void RefreshAssetState();                              //!< re-reads Details, descriptor and the file list
		static CVideoEditor* Current();                        //!< the last window that opened an asset (console command, tests)

		//! Opens the dropped / selected .webm files: options dialog, import on a job, merge on the main thread.
		void ImportFiles(const QStringList& files);

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
		bool LoadState(CAsset& asset);
		void StartBackgroundChecks();
		void PushToPreview();

		SState m_state;
		QPointer<CVideoPreviewPanel> m_pPreview;
		QPointer<CVideoInfoPanel> m_pInfo;
		bool m_stateValid = false;
		int m_generation = 0;   //!< bumped on open / close: results of older jobs are dropped
	};
}
