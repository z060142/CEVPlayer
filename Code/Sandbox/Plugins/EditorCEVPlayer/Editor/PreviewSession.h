// Owns the IVideoPlayer of a preview (texture output only, no fullscreen): the only place in the editor that holds an
// IVideoPlayer*. Created, used and destroyed on the main thread. Player callbacks only set flags that the owner reads
// from its per-frame Tick, nothing happens inside a callback.
#pragma once

#include <string>

#include <CrySystem/ISystem.h>
#include <CEVPlayer/IVideoPlayer.h>

namespace CEVEditor
{
	enum class EPreviewAudio
	{
		AsAuthored,    //!< load the .cevvideo as it is (route, language, triggers)
		EmbeddedTrack, //!< play the video file with its own audio, through a temporary descriptor
		Muted,         //!< as authored, volume 0
	};

	//! Deletes the leftovers of an earlier run in Assets/.cevpreview (temporary descriptors of the "embedded track" preview).
	void RemoveStalePreviewFiles();

	class CPreviewSession final : public CEVPlayer::IVideoPlayerListener, public ISystemEventListener
	{
	public:
		CPreviewSession();
		~CPreviewSession() override;
		CPreviewSession(const CPreviewSession&) = delete;
		CPreviewSession& operator=(const CPreviewSession&) = delete;

		//! False when the CEVPlayer runtime plug-in is not loaded (the window then says so).
		static bool RuntimeAvailable();

		//! Loads the descriptor (path relative to the game root, as the runtime plug-in expects) and creates the player on
		//! first use. `videoRelPath` / `avOffsetMs` are only needed for EEmbeddedTrack (temporary descriptor).
		bool Load(const std::string& descriptorRel, EPreviewAudio audio, const std::string& videoRelPath, double avOffsetMs);
		//! Destroys the player (and the temporary descriptor). Safe to call repeatedly.
		void Release();

		CEVPlayer::IVideoPlayer* Player() const { return m_pPlayer; }
		bool HasPlayer() const { return m_pPlayer != nullptr; }

		//! Cleared by the reader.
		bool TakeReady() { const bool v = m_ready; m_ready = false; return v; }
		bool TakeError(std::string& message) { if (!m_error) return false; message = m_errorMessage; m_error = false; return true; }
		uint32_t FramesSeen() const { return m_frames; }

		// IVideoPlayerListener
		void OnReady(CEVPlayer::IVideoPlayer&) override { m_ready = true; }
		void OnError(CEVPlayer::IVideoPlayer&, int code, const char* message) override;
		void OnNewFrame(CEVPlayer::IVideoPlayer&, uint64_t) override { ++m_frames; }

		// ISystemEventListener: the runtime plug-in destroys every player at FULL / FAST_SHUTDOWN, after that the pointer
		// must not be touched (not even to remove the listener).
		void OnSystemEvent(ESystemEvent event, UINT_PTR wparam, UINT_PTR lparam) override;

	private:
		bool m_listening = false;
		CEVPlayer::IVideoPlayer* m_pPlayer = nullptr;
		bool m_ready = false;
		bool m_error = false;
		std::string m_errorMessage;
		uint32_t m_frames = 0;
		std::string m_tempDescriptorAbs;
		std::string m_tempDescriptorRel;
	};
}
