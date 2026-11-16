#include "StdAfx.h"
#include "Editor/VideoEditor.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <set>

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QIcon>
#include <QStringList>
#include <QTimer>

#include <AssetSystem/Asset.h>
#include <AssetSystem/EditableAsset.h>
#include <DockingSystem/DockableContainer.h>
#include <DragDrop.h>
#include <Notifications/NotificationCenter.h>
#include <PathUtils.h>

#include "Editor/ImportOptionsDialog.h"
#include "Editor/InfoPanel.h"
#include "Editor/PreviewPanel.h"
#include "Jobs/JobRunner.h"
#include "Offline/MediaInfo.h"

namespace CEVEditor
{
	REGISTER_VIEWPANE_FACTORY(CVideoEditor, "CEV Video Editor", "Tools", false)

	namespace
	{
		CVideoEditor* g_pCurrent = nullptr;
		std::vector<CVideoEditor*> g_instances;     // every live window (the pane is not unique)
		std::set<std::string> g_importsRunning;     // metadata paths of the imports that are on a job right now

		void Notify(bool warning, const QString& title, const QString& text)
		{
			if (GetIEditor() == nullptr || GetIEditor()->GetNotificationCenter() == nullptr)
				return;
			if (warning)
				GetIEditor()->GetNotificationCenter()->ShowWarning(title, text);
			else
				GetIEditor()->GetNotificationCenter()->ShowInfo(title, text);
		}

		bool IsWebm(const QString& path)
		{
			return path.endsWith(QStringLiteral(".webm"), Qt::CaseInsensitive);
		}

		// size + modification time of a file as text: what makes the preview reload when a file was replaced
		std::string StampOf(const std::string& abs)
		{
			std::error_code ec;
			const std::filesystem::path p = std::filesystem::u8path(abs);
			const auto time = std::filesystem::last_write_time(p, ec);
			const long long t = ec ? 0 : static_cast<long long>(time.time_since_epoch().count());
			ec.clear();
			const long long size = static_cast<long long>(std::filesystem::file_size(p, ec));
			return std::to_string(t) + ":" + std::to_string(ec ? -1LL : size);
		}

		bool SameMetadata(const std::string& a, const std::string& b)
		{
			return _stricmp(a.c_str(), b.c_str()) == 0;
		}
	}

	void ReleaseEditorsOfAsset(const std::string& metadataRel)
	{
		const std::vector<CVideoEditor*> copy = g_instances; // a release can queue work, never edits the list
		for (CVideoEditor* const pEditor : copy)
			pEditor->ReleasePlayerForAsset(metadataRel);
	}

	CVideoEditor::CVideoEditor()
		: CAssetEditor("CEVVideo") // the asset type name, see CVideoAssetType::GetTypeName
	{
		setAcceptDrops(true);
		setWindowIcon(QIcon(QStringLiteral(":/EditorCEVPlayer/icons/assets_video.png")));
		g_instances.push_back(this);
	}

	CVideoEditor::~CVideoEditor()
	{
		g_instances.erase(std::remove(g_instances.begin(), g_instances.end(), this), g_instances.end());
		if (g_pCurrent == this)
			g_pCurrent = nullptr;
		++m_generation; // results of running jobs are dropped
	}

	CVideoEditor* CVideoEditor::Current()
	{
		return g_pCurrent;
	}

	void CVideoEditor::OnInitialize()
	{
		RegisterDockableWidget("Preview", [this]()
		{
			CVideoPreviewPanel* const pPanel = new CVideoPreviewPanel();
			m_pPreview = pPanel;
			m_pushedPreviewKey.clear(); // a new panel has no player yet
			PushToPreview();
			Q_EMIT PreviewPanelChanged();
			return pPanel;
		}, true, false);
		RegisterDockableWidget("Info", [this]()
		{
			CVideoInfoPanel* const pPanel = new CVideoInfoPanel(this);
			m_pInfo = pPanel;
			return pPanel;
		}, true, false);
	}

	void CVideoEditor::OnCreateDefaultLayout(CDockableContainer* pSender, QWidget* pAssetBrowser)
	{
		// left: the asset browser (the engine's, filtered to the video asset type); middle: preview; right: info
		QWidget* const pPreview = pSender->SpawnWidget("Preview", pAssetBrowser, QToolWindowAreaReference::VSplitRight);
		pSender->SpawnWidget("Info", pPreview, QToolWindowAreaReference::Right);
	}

	bool CVideoEditor::LoadState(CAsset& asset, bool keepUnsavedEdits)
	{
		SState st;
		st.guid = asset.GetGUID();
		st.assetName = asset.GetName().c_str();
		st.metadataRel = asset.GetMetadataFile().c_str();
		st.descriptorRel = DescriptorRel(asset);
		st.descriptorAbs = AssetsAbsolute(st.descriptorRel);
		st.folderRel = FolderOf(st.metadataRel);
		st.files = AssetFileList(asset);
		st.details = CDetails(asset);
		st.sourceCopyRel = st.details.Get(DetailKey::kSourcePath);
		std::string error;
		if (!LoadDescriptor(st.descriptorAbs, st.descriptor, error))
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING, "[CEVEditor] cannot open '%s': %s", st.assetName.c_str(), error.c_str());
			if (!m_stateValid)
				Notify(true, tr("Video asset"), tr("Cannot open %1: %2").arg(QString::fromUtf8(st.assetName.c_str()), QString::fromUtf8(error.c_str())));
			return false;
		}
		st.baseDescriptor = st.descriptor;
		// the descriptor names its video relative to its own folder (or relative to the game root)
		st.videoRel = st.folderRel + st.descriptor.video;
		std::error_code ec;
		if (!std::filesystem::exists(std::filesystem::u8path(AssetsAbsolute(st.videoRel)), ec) && std::filesystem::exists(std::filesystem::u8path(AssetsAbsolute(st.descriptor.video)), ec))
			st.videoRel = st.descriptor.video;
		// keep what the background checks already know when the same asset is refreshed
		const bool sameAsset = m_stateValid && m_state.guid == st.guid;
		if (sameAsset)
		{
			st.media = m_state.media;
			st.mediaKnown = m_state.mediaKnown;
			st.source = m_state.source;
		}
		// Unsaved descriptor edits survive a reload that was caused by somebody else writing the file (an audio job, a
		// rename): three way merge of the edits with the new file content.
		bool merged = false;
		if (sameAsset && keepUnsavedEdits && !DescriptorsEqual(m_state.descriptor, m_state.baseDescriptor))
		{
			const SDescriptor result = MergeDescriptors(m_state.baseDescriptor, m_state.descriptor, st.descriptor);
			merged = !DescriptorsEqual(result, m_state.descriptor);
			st.descriptor = result;
		}
		m_state = std::move(st);
		m_stateValid = true;
		++m_descriptorRevision;
		if (merged)
		{
			CryLogAlways("[CEVEditor] '%s': the descriptor changed on disk, your unsaved edits were merged with it", m_state.assetName.c_str());
			Notify(false, tr("Video asset"), tr("%1: the descriptor was changed on disk by an operation; your unsaved edits were merged with it. Save to keep both.").arg(QString::fromUtf8(m_state.assetName.c_str())));
		}
		return true;
	}

	std::string CVideoEditor::PreviewKey() const
	{
		return m_state.descriptorRel + "|" + m_state.videoRel + "|" + std::to_string(m_state.descriptor.avOffsetMs) + "|"
			+ StampOf(m_state.descriptorAbs) + "|" + StampOf(AssetsAbsolute(m_state.videoRel));
	}

	void CVideoEditor::PushToPreview()
	{
		if (!m_pPreview)
			return;
		if (!m_stateValid)
		{
			m_pushedPreviewKey.clear();
			m_pPreview->Clear();
			return;
		}
		// the player plays the files on disk: only reload when one of them changed (or the player was released)
		const std::string key = PreviewKey();
		if (!m_forcePreviewReload && key == m_pushedPreviewKey)
			return;
		m_forcePreviewReload = false;
		m_pushedPreviewKey = key;
		CVideoPreviewPanel::SSource source;
		source.descriptorRel = m_state.descriptorRel;
		source.videoRel = m_state.videoRel;
		source.avOffsetMs = m_state.descriptor.avOffsetMs;
		source.fps = m_state.mediaKnown ? m_state.media.fps : m_state.details.GetDouble(DetailKey::kFps);
		m_pPreview->SetSource(source);
	}

	void CVideoEditor::StartBackgroundChecks()
	{
		CJobRunner* const pRunner = CJobRunner::Get();
		if (pRunner == nullptr)
			return;
		const int generation = m_generation;
		QPointer<CVideoEditor> self(this);
		const std::string owner = AssetOwnerKey(m_state.guid); // renames / deletes of the asset wait for these jobs (they hold files open)
		const std::string videoAbs = AssetsAbsolute(m_state.videoRel);
		const std::string copyAbs = m_state.sourceCopyRel.empty() ? std::string() : AssetsAbsolute(m_state.sourceCopyRel);
		const std::string original = m_state.details.Get(DetailKey::kSourceOriginal);
		const std::string sha = m_state.details.Get(DetailKey::kSourceSha256);

		// stream information of the main video (reads the container index: off the main thread)
		auto media = std::make_shared<SMediaInfo>();
		auto mediaOk = std::make_shared<bool>(false);
		pRunner->Submit(tr("Read media info"), [videoAbs, media, mediaOk](const SOpContext& ctx)
		{
			std::string error;
			*mediaOk = ProbeMediaFile(videoAbs, *media, error, ctx.cancel);
			SOpResult r;
			r.status = *mediaOk ? EOpStatus::Ok : EOpStatus::Failed;
			r.message = *mediaOk ? std::string() : "cannot read " + videoAbs + ": " + error;
			return r;
		}, [self, generation, media, mediaOk](const SOpResult&)
		{
			if (!self || self->m_generation != generation)
				return;
			self->m_state.media = *media;
			self->m_state.mediaKnown = *mediaOk;
			if (*mediaOk && self->m_pPreview)
				self->m_pPreview->SetFps(media->fps);
			Q_EMIT self->StateChanged();
		}, owner);

		// source copy / original file against the hash recorded at import time (hashes whole files)
		auto status = std::make_shared<SSourceStatus>();
		pRunner->Submit(tr("Check source file"), [copyAbs, original, sha, status](const SOpContext& ctx)
		{
			*status = CheckSourceStatus(copyAbs, original, sha, ctx.cancel);
			SOpResult r;
			r.status = EOpStatus::Ok;
			return r;
		}, [self, generation, status](const SOpResult&)
		{
			if (!self || self->m_generation != generation)
				return;
			self->m_state.source = *status;
			Q_EMIT self->StateChanged();
		}, owner);
	}

	bool CVideoEditor::OnOpenAsset(CAsset* pAsset)
	{
		++m_generation;
		m_stateValid = false;
		m_state = SState();
		m_pushedPreviewKey.clear();
		if (!LoadState(*pAsset, false))
			return false;
		g_pCurrent = this;
		// files written by a job or a rename: the window follows (merging unsaved edits)
		pAsset->signalChanged.Connect(this, &CVideoEditor::OnAssetChangedHook);
		PushToPreview();
		StartBackgroundChecks();
		CryLogAlways("[CEVEditor] opened video asset '%s'", m_state.assetName.c_str());
		Q_EMIT StateChanged();
		return true;
	}

	void CVideoEditor::OnAssetChangedHook(CAsset& asset, int changeFlags)
	{
		if ((changeFlags & eAssetChangeFlags_Files) == 0 || &asset != GetAssetBeingEdited())
			return;
		QueueRefresh();
	}

	void CVideoEditor::QueueRefresh()
	{
		if (m_refreshQueued)
			return;
		m_refreshQueued = true;
		QPointer<CVideoEditor> self(this);
		// later, from the event loop: never inside the asset manager's notification
		QTimer::singleShot(0, this, [self]()
		{
			if (!self)
				return;
			self->m_refreshQueued = false;
			if (self->m_stateValid && self->GetAssetBeingEdited() != nullptr)
				self->RefreshAssetState();
		});
	}

	void CVideoEditor::OnCloseAsset()
	{
		++m_generation;
		if (CAsset* const pAsset = GetAssetBeingEdited())
			pAsset->signalChanged.DisconnectObject(this); // the base class connects its own handler again for the next asset
		if (m_pPreview)
			m_pPreview->Clear(); // destroys the preview player (asset switch / close)
		m_pushedPreviewKey.clear();
		m_stateValid = false;
		m_state = SState();
		++m_descriptorRevision;
		Q_EMIT StateChanged();
	}

	void CVideoEditor::OnDiscardAssetChanges(CEditableAsset&)
	{
		if (CAsset* const pAsset = GetAssetBeingEdited())
		{
			if (LoadState(*pAsset, false))
			{
				PushToPreview();
				Q_EMIT StateChanged();
			}
		}
	}

	bool CVideoEditor::OnSaveAsset(CEditableAsset& editAsset)
	{
		CAsset* const pAsset = GetAssetBeingEdited();
		if (!m_stateValid || pAsset == nullptr)
			return false;

		// A job may have rewritten the file since it was read (and the refresh may still be queued): merge, never overwrite.
		{
			SDescriptor disk;
			std::string readError;
			if (LoadDescriptor(m_state.descriptorAbs, disk, readError) && !DescriptorsEqual(disk, m_state.baseDescriptor))
			{
				m_state.descriptor = MergeDescriptors(m_state.baseDescriptor, m_state.descriptor, disk);
				CryLogAlways("[CEVEditor] '%s': the descriptor changed on disk since it was read, merged before saving", m_state.assetName.c_str());
			}
		}
		std::string error;
		if (!SaveDescriptor(m_state.descriptorAbs, m_state.descriptor, error))
		{
			CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_ERROR, "[CEVEditor] cannot save '%s': %s", m_state.assetName.c_str(), error.c_str());
			Notify(true, tr("Video asset"), tr("Cannot save %1: %2").arg(QString::fromUtf8(m_state.assetName.c_str()), QString::fromUtf8(error.c_str())));
			return false;
		}
		m_state.baseDescriptor = m_state.descriptor;
		++m_descriptorRevision;
		// the files stay as they are (the asset's current list: a job may have added some); only the Details that mirror
		// the descriptor change, on top of the current ones
		std::vector<string> files;
		for (const std::string& f : AssetFileList(*pAsset))
			files.emplace_back(f.c_str());
		editAsset.SetFiles(files);
		CDetails details(*pAsset);
		details.Set(DetailKey::kRoute, m_state.descriptor.route);
		std::vector<std::string> languageNames;
		for (const SDescriptor::SLanguage& l : m_state.descriptor.languages)
			languageNames.push_back(l.name);
		details.Set(DetailKey::kLanguages, JoinList(languageNames, ','));
		m_state.details = details;
		editAsset.SetDetails(details.ToPairs());
		CryLogAlways("[CEVEditor] saved '%s'", m_state.assetName.c_str());
		PushToPreview();
		Q_EMIT StateChanged();
		return true;
	}

	void CVideoEditor::SetDescriptor(const SDescriptor& descriptor)
	{
		if (!m_stateValid)
			return;
		m_state.descriptor = descriptor;
		if (CAsset* const pAsset = GetAssetBeingEdited())
			pAsset->SetModified(!DescriptorsEqual(m_state.descriptor, m_state.baseDescriptor) || pAsset->IsModified());
		Q_EMIT StateChanged();
	}

	void CVideoEditor::ReloadPreview()
	{
		m_forcePreviewReload = true;
		PushToPreview();
	}

	void CVideoEditor::RefreshAssetState()
	{
		CAsset* const pAsset = GetAssetBeingEdited();
		if (pAsset == nullptr)
			return;
		if (LoadState(*pAsset, true))
		{
			PushToPreview();
			Q_EMIT StateChanged();
		}
	}

	void CVideoEditor::ReleasePlayerForAsset(const std::string& metadataRel)
	{
		if (!m_stateValid || !SameMetadata(m_state.metadataRel, metadataRel) || !m_pPreview)
			return;
		m_pPreview->ReleasePlayer();
		m_forcePreviewReload = true;
		// the operation that asked for this changes the asset (which refreshes the window); if it does not, come back anyway
		QPointer<CVideoEditor> self(this);
		QTimer::singleShot(1500, this, [self]()
		{
			if (self && self->m_stateValid && self->GetAssetBeingEdited() != nullptr)
				self->RefreshAssetState();
		});
	}

	bool CVideoEditor::ReimportSource(std::string& error)
	{
		CAsset* const pAsset = GetAssetBeingEdited();
		if (pAsset == nullptr)
		{
			error = "no asset opened";
			return false;
		}
		// the copy is replaced: nobody may hold it open (preview player, background checks, audio jobs)
		if (!ReleaseAssetFiles(m_state.metadataRel, m_state.guid, 15000, error))
			return false;
		const bool ok = ReimportVideo(*pAsset, error);
		m_state.mediaKnown = false;
		m_state.source = SSourceStatus();
		++m_generation;
		m_forcePreviewReload = true;
		if (LoadState(*pAsset, true))
		{
			PushToPreview();
			StartBackgroundChecks();
			Q_EMIT StateChanged();
		}
		return ok;
	}

	void CVideoEditor::RunAudioOps(const SAudioOps& ops)
	{
		CAsset* const pAsset = GetAssetBeingEdited();
		if (pAsset == nullptr)
			return;
		QPointer<CVideoEditor> self(this);
		std::string error;
		if (SubmitAudioOps(*pAsset, ops, error, [self](bool) { if (self) self->RefreshAssetState(); }) == 0)
			Notify(true, tr("Video asset"), tr("Cannot start the operation: %1").arg(QString::fromUtf8(error.c_str())));
	}

	void CVideoEditor::UpgradeToCue()
	{
		CAsset* const pAsset = GetAssetBeingEdited();
		if (pAsset == nullptr)
			return;
		std::string error;
		if (!UpgradeToCueRoute(*pAsset, error))
			Notify(true, tr("Video asset"), tr("Cannot switch to the cue route: %1").arg(QString::fromUtf8(error.c_str())));
		else
			Notify(false, tr("Video asset"), tr("The asset now uses the cue route."));
		RefreshAssetState();
	}

	void CVideoEditor::ImportFiles(const QStringList& files)
	{
		CJobRunner* const pRunner = CJobRunner::Get();
		if (pRunner == nullptr)
			return;
		for (const QString& file : files)
		{
			if (!IsWebm(file))
				continue;
			// (re-read each time: the asset browser of this window may have been used while a dialog was open)
			const std::string folder = m_stateValid ? m_state.folderRel : std::string("Videos/");
			SImportOptions options;
			options.assetName = SanitizeAssetName(QFileInfo(file).completeBaseName().toUtf8().constData());
			if (!CImportOptionsDialog::Ask(file, options, folder)) // asks again when the name is taken
				continue;
			const std::string source = file.toUtf8().constData();
			// main thread: asset manager lookups and the release of files that are about to be replaced
			const SImportPlan plan = PlanImport(source, folder, options);
			if (!plan.ok)
			{
				Notify(true, tr("Video asset"), tr("Import failed: %1").arg(QString::fromUtf8(plan.error.c_str())));
				continue;
			}
			if (plan.exists && !options.overwrite)
			{
				Notify(true, tr("Video asset"), tr("Import refused: %1").arg(QString::fromUtf8(plan.detail.c_str())));
				continue;
			}
			if (g_importsRunning.count(plan.metadataRel) != 0)
			{
				Notify(true, tr("Video asset"), tr("An import of %1 is already running.").arg(QString::fromUtf8(plan.assetName.c_str())));
				continue;
			}
			std::string prepareError;
			if (plan.exists && !PrepareOverwrite(plan, prepareError))
			{
				Notify(true, tr("Video asset"), tr("Import failed: %1").arg(QString::fromUtf8(prepareError.c_str())));
				continue;
			}
			g_importsRunning.insert(plan.metadataRel);
			auto result = std::make_shared<SImportResult>();
			const std::string metadataRel = plan.metadataRel;
			pRunner->Submit(tr("Import %1").arg(QFileInfo(file).fileName()), [source, plan, options, result](const SOpContext& ctx)
			{
				*result = ImportVideoFile(source, plan, options, ctx.cancel);
				SOpResult r;
				r.status = result->ok ? EOpStatus::Ok : (ctx.Cancelled() ? EOpStatus::Cancelled : EOpStatus::Failed);
				r.message = result->error;
				return r;
			}, [result, options, metadataRel](const SOpResult& r)
			{
				g_importsRunning.erase(metadataRel);
				if (r.Ok())
				{
					FinishImport(*result, options);
				}
				else
				{
					delete result->pAsset; // never merged
					result->pAsset = nullptr;
					Notify(true, QObject::tr("Video asset"), QObject::tr("Import failed: %1").arg(QString::fromUtf8(r.message.c_str())));
				}
			}, plan.guid.IsNull() ? std::string() : AssetOwnerKey(plan.guid));
		}
	}

	void CVideoEditor::dragEnterEvent(QDragEnterEvent* pEvent)
	{
		auto pData = CDragDropData::FromMimeData(pEvent->mimeData());
		if (pData->HasFilePaths())
		{
			const QStringList paths = pData->GetFilePaths();
			if (std::any_of(paths.begin(), paths.end(), IsWebm))
			{
				CDragDropData::ShowDragText(this, tr("Import"));
				pEvent->acceptProposedAction();
				return;
			}
		}
		CAssetEditor::dragEnterEvent(pEvent);
	}

	void CVideoEditor::dropEvent(QDropEvent* pEvent)
	{
		auto pData = CDragDropData::FromMimeData(pEvent->mimeData());
		if (pData->HasFilePaths())
		{
			const QStringList paths = pData->GetFilePaths();
			if (std::any_of(paths.begin(), paths.end(), IsWebm))
			{
				pEvent->acceptProposedAction();
				// The options dialog is modal: not inside the drop handler (the drag source, e.g. Explorer, waits for it).
				QPointer<CVideoEditor> self(this);
				QTimer::singleShot(0, this, [self, paths]()
				{
					if (self)
						self->ImportFiles(paths);
				});
				return;
			}
		}
		CAssetEditor::dropEvent(pEvent);
	}
}
