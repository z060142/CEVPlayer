// Compatibility layer for the old IVideoPlugin API (PlayVideoToMainWindow / PlayVideoToTexture, Pause / Resume /
// Stop), the old FlowGraph nodes and the TextureVideoPlayerComponent: thin wrappers over IVideoPlayer.
//
// Behaviour kept from the old plug-in: "Videos/<name>.webm" paths, one main window video at a time with a queue
// for the next ones, skip / pause input actions, one video per output texture name, texture videos are removed
// when they end / stop and on level unload, listener events OnPlay / OnPause / OnResume / OnStop / OnSkip.
// Differences: OnPlay / OnPause / OnResume (and OnStop at the end of a video) are delivered at the plug-in's fixed
// dispatch point (end of UpdateBeforeRender) instead of from inside the calls, OnStop of an explicit Stop call is
// still synchronous; a looped video reports OnStop + OnPlay per loop; the audio track number is ignored (the
// first audio track plays, M1).
#pragma once

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <CEVPlayer/IVideoPlayer.h>
#include <CEVPlayer/IVideoPluginListeners.h>
#include <CryInput/IInput.h>

namespace CEVPlayer
{
	class CPlayerManager;

	class CLegacyPlayback final : public IVideoPlayerListener
	{
	public:
		explicit CLegacyPlayback(CPlayerManager& manager);
		~CLegacyPlayback() override;

		// ---- main window ----
		void Play2DVideo(IVideoPlayerEventListener* pListener, const char* videoName, bool preload = false, bool looped = false, int audioTrack = 0, bool isSkippable = false, bool canBePaused = false, bool isLevelLoading = false);
		void Pause2DVideo();
		void Resume2DVideo();
		void Stop2DVideo();
		bool Is2DVideoPlayingNow() const;
		void OnActionPause2DVideo(EInputState activationMode, float value);
		void OnActionSkip2DVideo(EInputState activationMode, float value);

		// ---- textures ----
		void PlayVideo(IVideoPlayerEventListener* pListener, const char* videoName, const char* textureName, bool preload = false, bool looped = false, const char* stopTexture = nullptr);
		void PauseVideo(const char* textureName);
		void ResumeVideo(const char* textureName);
		void StopVideo(const char* textureName);
		int GetTextureIDByTextureName(const char* textureName) const;

		// ---- both ----
		//! Removes the listener everywhere (main window video, queued videos, texture videos). Call it from the
		//! destructor of every listener: events are delivered later than the call that caused them.
		void RemoveVideoListener(IVideoPlayerEventListener* pListener);
		void ClearTextureVideos();
		void PauseAllTextureVideos(bool pause);
		//! Destroys every player this layer owns (shutdown).
		void Shutdown();

		// ---- IVideoPlayerListener ----
		void OnStarted(IVideoPlayer& player) override;
		void OnPaused(IVideoPlayer& player) override;
		void OnResumed(IVideoPlayer& player) override;
		void OnStopped(IVideoPlayer& player) override;
		void OnLooped(IVideoPlayer& player) override;
		void OnEnded(IVideoPlayer& player) override;
		void OnError(IVideoPlayer& player, int errorCode, const char* message) override;

	private:
		struct SEntry
		{
			IVideoPlayer* pPlayer = nullptr;
			IVideoPlayerEventListener* pListener = nullptr;
			std::string videoName;   // as passed by the caller, reported to the listener
			std::string textureName; // texture videos only
			bool is2D = false;
			bool skippable = false;
			bool canBePaused = false;
			bool stopSent = false;
		};
		struct SQueued
		{
			IVideoPlayerEventListener* pListener = nullptr;
			std::string videoName;
			bool looped = false;
			bool isSkippable = false;
			bool canBePaused = false;
		};

		SEntry* FindEntry(const IVideoPlayer* pPlayer);
		SEntry* FindTexture(const char* textureName);
		std::unique_ptr<SEntry> StartVideo(IVideoPlayerEventListener* pListener, const char* videoName, const char* textureName, bool looped);
		void Fire(SEntry& entry, EVideoPlayerEvents event);
		void FinishEntry(IVideoPlayer& player, bool sendStop);
		void DestroyEntry(std::unique_ptr<SEntry>& entry);
		void StartNextQueued();

		CPlayerManager& m_manager;
		std::unique_ptr<SEntry> m_main;
		std::deque<SQueued> m_queue;
		std::vector<std::unique_ptr<SEntry>> m_textures;
	};
}
