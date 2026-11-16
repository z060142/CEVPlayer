// CEVPlayer public player API (interface version 1).
//
// This header is the contract for other plug-ins (CryXTUI, game code, ...): it contains no CRYENGINE includes
// and no implementation details. Obtain the factory through IVideoPlugin (CEVPlayer/IVideoPlugin.h):
//
//     IVideoPlugin* pPlugin = gEnv->pSystem->GetIPluginManager()->QueryPlugin<CEVPlayer::IVideoPlugin>();
//     CEVPlayer::IVideoPlayer* pPlayer = pPlugin->CreatePlayer("$my_video");
//     pPlayer->AddListener(&myListener);
//     pPlayer->SetLoop(true);
//     pPlayer->Load("Videos/intro.webm");   // asynchronous
//     pPlayer->Play();                      // may be called before the file is ready: starts when it is
//     ...
//     pPlugin->DestroyPlayer(pPlayer);
//
// Threading: every method of IVideoPlayer, IVideoPlugin::CreatePlayer / DestroyPlayer and the listener
// callbacks run on the game's main thread. Listener callbacks are dispatched at one fixed point of the frame
// (the end of the plug-in's UpdateBeforeRender step), never from inside a call you make. It is legal to call
// player methods, RemoveListener or DestroyPlayer from inside a callback.
//
// Pixel format: the texture is RGBA8 with premultiplied alpha (IsPremultiplied() is always true).

#pragma once

#include <cstdint>

struct ITexture;
struct IMaterial;

namespace CEVPlayer
{
	//! Version of this header. Bumped when a virtual is appended / a struct is extended.
	constexpr uint32_t kVideoPlayerInterfaceVersion = 1;

	//! Bits returned by IVideoPlugin::GetVideoPlayerCaps().
	enum EVideoPlayerCaps : uint32_t
	{
		eVideoCap_Alpha           = 1u << 0, //!< files with an alpha channel (VP8/VP9 BlockAdditional alpha) are supported
		eVideoCap_Premultiplied   = 1u << 1, //!< the output texture is premultiplied RGBA (always set)
		eVideoCap_MultiResolution = 1u << 2, //!< resolution changes inside one stream (not supported yet: 0)
	};

	enum class EVideoPlayerState : uint8_t
	{
		Stopped = 0, //!< at time 0, not running (a loaded Stopped player shows its first frame; also: no media yet)
		Playing,
		Paused,
		Ended,       //!< reached the end and is not looping
		Loading,     //!< Load() is in progress (IsLoaded() == false)
		Error,       //!< Load() failed, see the OnError callback
	};

	//! How the fullscreen output places the picture on the screen.
	enum class EVideoFit : uint8_t
	{
		Fill = 0,   //!< stretch to the whole screen, aspect ratio is not kept
		Fit,        //!< keep the aspect ratio, whole picture visible, the scene stays visible in the bars
		Letterbox,  //!< like Fit, with opaque black bars
	};

	struct IVideoPlayer;

	//! Callbacks, all optional. See the threading note at the top of this file.
	struct IVideoPlayerListener
	{
		virtual ~IVideoPlayerListener() {}

		//! The file is parsed, the first frame is decoded, size / duration / texture are valid.
		virtual void OnReady(IVideoPlayer& player) { (void)player; }
		//! Playback started (Play() from Stopped / Ended, or the deferred Play() after Load()).
		virtual void OnStarted(IVideoPlayer& player) { (void)player; }
		//! A new picture was uploaded to the texture (at most once per frame and player).
		virtual void OnNewFrame(IVideoPlayer& player, uint64_t frameSerial) { (void)player; (void)frameSerial; }
		//! A loop iteration completed and playback restarted at 0.
		virtual void OnLooped(IVideoPlayer& player) { (void)player; }
		//! Reached the end of a non-looping video.
		virtual void OnEnded(IVideoPlayer& player) { (void)player; }
		//! Loading failed (errorCode is a cev::Status value, message is a static string).
		virtual void OnError(IVideoPlayer& player, int errorCode, const char* message) { (void)player; (void)errorCode; (void)message; }
		// ---- appended in version 1 as defaulted callbacks (used by the legacy wrappers) ----
		virtual void OnPaused(IVideoPlayer& player) { (void)player; }
		virtual void OnResumed(IVideoPlayer& player) { (void)player; }
		virtual void OnStopped(IVideoPlayer& player) { (void)player; }
	};

	//! One player = one video file = one RGBA texture. Create / destroy through IVideoPlugin.
	struct IVideoPlayer
	{
		virtual ~IVideoPlayer() {}

		// ---- media ----
		//! Starts loading `path` (any CryPak path, e.g. "Videos/intro.webm"). Returns false when the request is
		//! rejected immediately (empty path, file does not exist); OnError is then also sent. Replaces a
		//! previously loaded video (the texture is kept when the size does not change).
		virtual bool Load(const char* path) = 0;
		//! Stops, releases the media and the texture. The player object stays valid.
		virtual void Unload() = 0;
		virtual const char* GetPath() const = 0;

		// ---- control ----
		virtual void Play() = 0;    //!< from Stopped / Paused / Ended; queued while loading
		virtual void Pause() = 0;
		virtual void Resume() = 0;
		virtual void Stop() = 0;    //!< back to time 0, shows the first frame
		virtual void Seek(double seconds) = 0;
		virtual void SetLoop(bool loop) = 0;
		virtual void SetRate(float rate) = 0;     //!< playback speed (0 < rate <= 16)
		virtual void SetVolume(float volume) = 0; //!< 0..1, multiplied with the global volume (vp_audio_volume)

		// ---- queries ----
		virtual EVideoPlayerState GetState() const = 0;
		virtual bool   IsLoaded() const = 0;      //!< ready (size, duration and texture are valid)
		virtual bool   IsLooping() const = 0;
		virtual double GetTime() const = 0;       //!< seconds
		virtual double GetDuration() const = 0;   //!< seconds, 0 until loaded
		virtual int    GetWidth() const = 0;      //!< 0 until loaded
		virtual int    GetHeight() const = 0;
		virtual bool   HasAlpha() const = 0;
		virtual bool   IsPremultiplied() const = 0; //!< always true
		virtual bool   HasAudio() const = 0;
		virtual uint64_t GetFrameSerial() const = 0; //!< counts uploaded pictures, usable as a "changed" test

		// ---- output: texture ----
		//! Texture name as registered with the renderer (e.g. "$cev_video_3"); may be used in .mtl files and
		//! Flash. The default is generated; use IVideoPlugin::CreatePlayer(name) to choose it.
		virtual const char* GetTextureName() const = 0;
		virtual int         GetTextureId() const = 0;   //!< renderer texture id, 0 while there is no texture
		virtual ITexture*   GetTexture() const = 0;     //!< nullptr while there is no texture

		// ---- output: fullscreen (stage 1: renderer aux 2D image on the top layer) ----
		virtual void SetFullscreen(bool enable, EVideoFit fit = EVideoFit::Letterbox) = 0;
		virtual bool IsFullscreen() const = 0;

		// ---- output: material slot ----
		//! Binds the texture to a texture slot (EEfResTextures value, e.g. 0 = diffuse) of `pMaterial`
		//! (subMaterial < 0: the material itself, otherwise that sub-material). Several bindings are allowed;
		//! they are applied as soon as the texture exists and removed by UnbindMaterial / Unload / destruction.
		//! The player holds a reference on the material while it is bound.
		virtual bool BindMaterial(IMaterial* pMaterial, int subMaterial = -1, int textureSlot = 0) = 0;
		virtual void UnbindMaterial(IMaterial* pMaterial) = 0; //!< all slots of this material; nullptr = all materials

		// ---- events ----
		virtual void AddListener(IVideoPlayerListener* pListener) = 0;
		virtual void RemoveListener(IVideoPlayerListener* pListener) = 0;
	};
}
