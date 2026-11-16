#include "StdAfx.h"
#include "PluginEnv.h"
#include "LegacyPlayback.h"

#include <algorithm>

#include "Player/PlayerManager.h"

namespace CEVPlayer
{
	CLegacyPlayback::CLegacyPlayback(CPlayerManager& manager)
		: m_manager(manager)
	{
	}

	CLegacyPlayback::~CLegacyPlayback()
	{
		Shutdown();
	}

	CLegacyPlayback::SEntry* CLegacyPlayback::FindEntry(const IVideoPlayer* pPlayer)
	{
		if (m_main && m_main->pPlayer == pPlayer)
			return m_main.get();
		for (const std::unique_ptr<SEntry>& e : m_textures)
		{
			if (e->pPlayer == pPlayer)
				return e.get();
		}
		return nullptr;
	}

	const CLegacyPlayback::SEntry* CLegacyPlayback::FindTexture(const char* textureName) const
	{
		if (textureName == nullptr || textureName[0] == 0)
			return nullptr;
		const std::string normalized = NormalizeTextureName(textureName); // entries are stored under the normalized name
		for (const std::unique_ptr<SEntry>& e : m_textures)
		{
			if (e->textureName == normalized)
				return e.get();
		}
		return nullptr;
	}

	CLegacyPlayback::SEntry* CLegacyPlayback::FindTexture(const char* textureName)
	{
		return const_cast<SEntry*>(static_cast<const CLegacyPlayback*>(this)->FindTexture(textureName));
	}

	bool CLegacyPlayback::IsTextureVideoOwner(const char* textureName, const IVideoPlayerEventListener* pListener) const
	{
		const SEntry* const e = FindTexture(textureName);
		return e != nullptr && pListener != nullptr && e->pListener == pListener;
	}

	void CLegacyPlayback::Fire(SEntry& entry, EVideoPlayerEvents event)
	{
		// Copy first: the listener may call back into this layer and invalidate the entry.
		IVideoPlayerEventListener* const pListener = entry.pListener;
		if (pListener == nullptr)
			return;
		const std::string name = entry.videoName;
		pListener->OnVideoPlayerEvent(name.c_str(), event);
	}

	std::unique_ptr<CLegacyPlayback::SEntry> CLegacyPlayback::StartVideo(IVideoPlayerEventListener* pListener, const char* videoName, const char* textureName, bool looped)
	{
		std::string path = VIDEO_FOLDER;
		path += videoName;
		path += VIDEO_FORMAT_EXT;

		const std::string normalizedTexture = NormalizeTextureName(textureName);
		IVideoPlayer* const pPlayer = m_manager.CreatePlayer(textureName); // normalizes the same way
		if (pPlayer == nullptr)
			return nullptr;
		pPlayer->AddListener(this);
		pPlayer->SetLoop(looped);
		if (!pPlayer->Load(path.c_str()))
		{
			m_manager.DestroyPlayer(pPlayer);
			return nullptr;
		}
		pPlayer->Play();

		std::unique_ptr<SEntry> entry = std::make_unique<SEntry>();
		entry->pPlayer = pPlayer;
		entry->pListener = pListener;
		entry->videoName = videoName;
		entry->textureName = normalizedTexture;
		return entry;
	}

	void CLegacyPlayback::DestroyEntry(std::unique_ptr<SEntry>& entry)
	{
		if (entry && entry->pPlayer != nullptr)
		{
			entry->pPlayer->RemoveListener(this);
			m_manager.DestroyPlayer(entry->pPlayer);
			entry->pPlayer = nullptr;
		}
		entry.reset();
	}

	// ------------------------------------------------------------------------------------------------ main window
	void CLegacyPlayback::Play2DVideo(IVideoPlayerEventListener* pListener, const char* videoName, bool /*preload*/, bool looped, int audioTrack, bool isSkippable, bool canBePaused, bool /*isLevelLoading*/)
	{
		if (gEnv->pSystem->IsQuitting() || videoName == nullptr || videoName[0] == '\0')
			return;

		if (audioTrack > 0)
			LogDebug("audio track %d requested for <%s>: track selection is not supported yet, the first audio track plays", audioTrack, videoName);

		SQueued request;
		request.pListener = pListener;
		request.videoName = videoName;
		request.looped = looped;
		request.isSkippable = isSkippable;
		request.canBePaused = canBePaused;

		if (m_main)
		{
			LogWarning("Can't play 2D video because some video already playing. Video will be queued and played later");
			m_queue.push_back(request);
			return;
		}
		StartMain(request);
	}

	bool CLegacyPlayback::StartMain(const SQueued& request)
	{
		std::unique_ptr<SEntry> entry = StartVideo(request.pListener, request.videoName.c_str(), nullptr, request.looped);
		if (!entry)
		{
			LogError("Can't play 2D video <%s>", request.videoName.c_str());
			return false;
		}
		entry->is2D = true;
		entry->skippable = request.isSkippable;
		entry->canBePaused = request.canBePaused;
		entry->pPlayer->SetFullscreen(true, EVideoFit::Fill); // the old plug-in stretched the video over the window
		m_main = std::move(entry);
		return true;
	}

	void CLegacyPlayback::StartNextQueued()
	{
		// A queued video that cannot start (missing file, rejected) must not block the ones behind it.
		while (!m_main && !m_queue.empty())
		{
			const SQueued next = m_queue.front();
			m_queue.pop_front();
			StartMain(next);
		}
	}

	void CLegacyPlayback::Pause2DVideo()
	{
		if (m_main && m_main->pPlayer->GetState() == EVideoPlayerState::Playing)
			m_main->pPlayer->Pause();
		else
			LogWarning("Can't pause 2D video because no video in queue or video already paused");
	}

	void CLegacyPlayback::Resume2DVideo()
	{
		if (m_main && m_main->pPlayer->GetState() == EVideoPlayerState::Paused)
			m_main->pPlayer->Resume();
		else
			LogWarning("Can't resume 2D video because no video in queue or video already resumed");
	}

	void CLegacyPlayback::Stop2DVideo()
	{
		if (!m_main)
		{
			if (gEnv->pSystem != nullptr && !gEnv->pSystem->IsQuitting())
				LogWarning("Can't stop 2D video because no video in queue or video already stopped");
			return;
		}
		// OnStop of an explicit stop is delivered right here: the player is gone afterwards.
		std::unique_ptr<SEntry> entry = std::move(m_main);
		if (!entry->stopSent)
		{
			entry->stopSent = true;
			Fire(*entry, EVideoPlayerEvents::OnStop);
		}
		DestroyEntry(entry);
		StartNextQueued();
	}

	bool CLegacyPlayback::Is2DVideoPlayingNow() const
	{
		return m_main && m_main->pPlayer->GetState() == EVideoPlayerState::Playing;
	}

	void CLegacyPlayback::OnActionPause2DVideo(EInputState activationMode, float /*value*/)
	{
		if (activationMode == eIS_Pressed && m_main && m_main->canBePaused)
		{
			const EVideoPlayerState state = m_main->pPlayer->GetState();
			if (state == EVideoPlayerState::Playing)
				Pause2DVideo();
			else if (state == EVideoPlayerState::Paused)
				Resume2DVideo();
		}
	}

	void CLegacyPlayback::OnActionSkip2DVideo(EInputState activationMode, float /*value*/)
	{
		if (activationMode == eIS_Pressed && m_main && m_main->skippable)
		{
			Fire(*m_main, EVideoPlayerEvents::OnSkip);
			if (m_main && m_main->pPlayer->GetState() == EVideoPlayerState::Playing)
				Stop2DVideo();
		}
	}

	// ------------------------------------------------------------------------------------------------ textures
	bool CLegacyPlayback::PlayVideo(IVideoPlayerEventListener* pListener, const char* videoName, const char* textureName, bool /*preload*/, bool looped, const char* /*stopTexture*/)
	{
		if (gEnv->pSystem->IsQuitting() || videoName == nullptr || textureName == nullptr || textureName[0] == '\0')
			return false;

		LogDebug("PlayVideo(%p, %s, %s, %s)", static_cast<void*>(pListener), videoName, textureName, looped ? "looped" : "once");

		if (FindTexture(textureName) != nullptr)
		{
			LogWarning("Can't play texture video because owner already play video");
			return false;
		}
		std::unique_ptr<SEntry> entry = StartVideo(pListener, videoName, textureName, looped);
		if (!entry)
		{
			LogError("Can't play texture video <%s>", videoName);
			return false;
		}
		m_textures.push_back(std::move(entry));
		return true;
	}

	void CLegacyPlayback::PauseVideo(const char* textureName)
	{
		SEntry* const e = FindTexture(textureName);
		if (e == nullptr)
			return;
		e->autoPaused = false; // an explicit request: the game pause no longer owns this video's state
		if (e->pPlayer->GetState() == EVideoPlayerState::Playing)
			e->pPlayer->Pause();
	}

	void CLegacyPlayback::ResumeVideo(const char* textureName)
	{
		SEntry* const e = FindTexture(textureName);
		if (e == nullptr)
			return;
		e->autoPaused = false;
		if (e->pPlayer->GetState() == EVideoPlayerState::Paused)
			e->pPlayer->Resume();
	}

	void CLegacyPlayback::StopVideo(const char* textureName)
	{
		SEntry* const found = FindTexture(textureName);
		if (found == nullptr)
			return;
		std::unique_ptr<SEntry> entry;
		for (std::vector<std::unique_ptr<SEntry>>::iterator it = m_textures.begin(); it != m_textures.end(); ++it)
		{
			if (it->get() == found)
			{
				entry = std::move(*it);
				m_textures.erase(it);
				break;
			}
		}
		if (!entry->stopSent)
		{
			entry->stopSent = true;
			Fire(*entry, EVideoPlayerEvents::OnStop);
		}
		DestroyEntry(entry);
	}

	int CLegacyPlayback::GetTextureIDByTextureName(const char* textureName) const
	{
		const SEntry* const e = FindTexture(textureName);
		return e != nullptr ? e->pPlayer->GetTextureId() : 0;
	}

	void CLegacyPlayback::ClearTextureVideos()
	{
		std::vector<std::unique_ptr<SEntry>> entries;
		entries.swap(m_textures);
		for (std::unique_ptr<SEntry>& entry : entries)
		{
			if (!entry->stopSent)
			{
				entry->stopSent = true;
				Fire(*entry, EVideoPlayerEvents::OnStop);
			}
			DestroyEntry(entry);
		}
	}

	void CLegacyPlayback::PauseAllTextureVideos(bool pause)
	{
		for (const std::unique_ptr<SEntry>& e : m_textures)
		{
			const EVideoPlayerState state = e->pPlayer->GetState();
			if (pause)
			{
				if (state == EVideoPlayerState::Playing)
				{
					e->pPlayer->Pause();
					e->autoPaused = true;
				}
			}
			else if (e->autoPaused)
			{
				// Only what the game pause paused: a video the game paused on purpose stays paused.
				e->autoPaused = false;
				if (state == EVideoPlayerState::Paused)
					e->pPlayer->Resume();
			}
		}
	}

	// ------------------------------------------------------------------------------------------------ shared
	void CLegacyPlayback::RemoveVideoListener(IVideoPlayerEventListener* pListener)
	{
		if (pListener == nullptr)
			return;
		if (m_main && m_main->pListener == pListener)
			m_main->pListener = nullptr;
		for (SQueued& q : m_queue)
		{
			if (q.pListener == pListener)
				q.pListener = nullptr;
		}
		for (const std::unique_ptr<SEntry>& e : m_textures)
		{
			if (e->pListener == pListener)
				e->pListener = nullptr;
		}
	}

	void CLegacyPlayback::DetachListeners()
	{
		if (m_main)
			m_main->pListener = nullptr;
		for (SQueued& q : m_queue)
			q.pListener = nullptr;
		for (const std::unique_ptr<SEntry>& e : m_textures)
			e->pListener = nullptr;
	}

	void CLegacyPlayback::Shutdown()
	{
		m_queue.clear();
		if (m_main)
			DestroyEntry(m_main);
		for (std::unique_ptr<SEntry>& e : m_textures)
			DestroyEntry(e);
		m_textures.clear();
	}

	void CLegacyPlayback::FinishEntry(IVideoPlayer& player, bool sendStop)
	{
		std::unique_ptr<SEntry> entry;
		bool wasMain = false;
		if (m_main && m_main->pPlayer == &player)
		{
			entry = std::move(m_main);
			wasMain = true;
		}
		else
		{
			for (std::vector<std::unique_ptr<SEntry>>::iterator it = m_textures.begin(); it != m_textures.end(); ++it)
			{
				if ((*it)->pPlayer == &player)
				{
					entry = std::move(*it);
					m_textures.erase(it);
					break;
				}
			}
		}
		if (!entry)
			return;
		if (sendStop && !entry->stopSent)
		{
			entry->stopSent = true;
			Fire(*entry, EVideoPlayerEvents::OnStop);
		}
		DestroyEntry(entry);
		if (wasMain)
			StartNextQueued();
	}

	// ------------------------------------------------------------------------------------------------ IVideoPlayerListener
	void CLegacyPlayback::OnStarted(IVideoPlayer& player)
	{
		if (SEntry* const e = FindEntry(&player))
			Fire(*e, EVideoPlayerEvents::OnPlay);
	}

	void CLegacyPlayback::OnPaused(IVideoPlayer& player)
	{
		if (SEntry* const e = FindEntry(&player))
			Fire(*e, EVideoPlayerEvents::OnPause);
	}

	void CLegacyPlayback::OnResumed(IVideoPlayer& player)
	{
		if (SEntry* const e = FindEntry(&player))
			Fire(*e, EVideoPlayerEvents::OnResume);
	}

	void CLegacyPlayback::OnStopped(IVideoPlayer&)
	{
		// Only produced by Stop() on the player, which this layer never calls; Stop / end destroy the player.
	}

	void CLegacyPlayback::OnLooped(IVideoPlayer& player)
	{
		// The old plug-in reported every loop as a stop followed by a new start.
		if (SEntry* const e = FindEntry(&player))
		{
			Fire(*e, EVideoPlayerEvents::OnStop);
			if (SEntry* const again = FindEntry(&player))
				Fire(*again, EVideoPlayerEvents::OnPlay);
		}
	}

	void CLegacyPlayback::OnEnded(IVideoPlayer& player)
	{
		FinishEntry(player, true);
	}

	void CLegacyPlayback::OnError(IVideoPlayer& player, int /*errorCode*/, const char* /*message*/)
	{
		FinishEntry(player, false);
	}
}
