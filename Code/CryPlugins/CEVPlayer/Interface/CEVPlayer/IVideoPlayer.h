// CEVPlayer public player API (interface version 5).
//
// This header is for game code that links CEVPlayer directly. Other plug-ins connect through the texture source
// contract of docs/design/M6-ui-source.md section 3 (exported function CryTexSource_v1) instead of this header.
// It contains no CRYENGINE includes and no implementation details. Obtain the factory through IVideoPlugin (CEVPlayer/IVideoPlugin.h):
//
//     IVideoPlugin* pPlugin = gEnv->pSystem->GetIPluginManager()->QueryPlugin<CEVPlayer::IVideoPlugin>();
//     CEVPlayer::IVideoPlayer* pPlayer = pPlugin->CreatePlayer("$my_video");
//     pPlayer->AddListener(&myListener);
//     pPlayer->SetLoop(true);
//     pPlayer->Load("Videos/intro.webm");   // asynchronous; also "Videos/intro.cevvideo" (runtime descriptor)
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
//
// Lifetime: IVideoPlayer* belongs to the plug-in. It is valid from CreatePlayer until DestroyPlayer or until the
// plug-in shuts down (system events ESYSTEM_EVENT_FULL_SHUTDOWN / FAST_SHUTDOWN, or the module is unloaded). At
// shutdown all players are destroyed (no callbacks are sent) and every IVideoPlayer* and IVideoPlayerListener
// registration is gone: never use a player pointer after that, and make CreatePlayer's nullptr result (returned
// once shutdown began) a normal case. A player whose playback fails later (texture refused, the stream changes its
// format) goes to EVideoPlayerState::Error and sends OnError; it stops presenting but the object stays valid until
// you destroy it.
//
// Textures: the renderer keeps a texture alive for as long as something drew it through its 2D aux geometry
// (fullscreen output), also after the player is gone. Prefer the generated names for fullscreen players: they are
// reused, so repeated playback does not grow the texture count. A texture name chosen by you must not be used by
// an engine texture (render targets / non RGBA8 textures are refused, the player fails with OnError), and the
// video must not be larger than the renderer's maximum texture size (OnError).


#pragma once

#include <cstdint>

struct ITexture;
struct IMaterial;

namespace CEVPlayer
{
	//! Version of this header. Bumped when a virtual is appended / a struct is extended.
	//! 3: IVideoPlayerListener::OnMarker, IVideoPlayer::SetLanguage / GetLanguage, Load() accepts .cevvideo.
	//! 4: IVideoPlayer::SetVariant / GetVariant (per player variant choice of a .cevvideo descriptor).
	//! 5: IVideoPlugin::CreatePlayerForSurface / FindPlayerBySurface, IVideoPlayer::GetSurfaceId / GetGeneration /
	//!    GetPixelLayout / GetDecodeBackend. M6b (same version, never released before): the legacy IVideoPlugin
	//!    virtuals were removed.
	constexpr uint32_t kVideoPlayerInterfaceVersion = 5;

	//! IVideoPlayer::GetPixelLayout(). Values match the CTS_LAYOUT_* values of the texture source contract.
	enum class EVideoPixelLayout : uint32_t
	{
		RGBA8 = 0, //!< one RGBA8 texture, premultiplied (the only layout so far)
	};

	//! IVideoPlayer::GetDecodeBackend().
	enum class EVideoDecodeBackend : uint32_t
	{
		Cpu = 0, //!< libvpx on the CPU (the only backend so far)
	};

	//! Bits returned by IVideoPlugin::GetVideoPlayerCaps().
	enum EVideoPlayerCaps : uint32_t
	{
		eVideoCap_Alpha           = 1u << 0, //!< files with an alpha channel (VP8/VP9 BlockAdditional alpha) are supported
		eVideoCap_Premultiplied   = 1u << 1, //!< the output texture is premultiplied RGBA (always set)
		eVideoCap_MultiResolution = 1u << 2, //!< resolution changes inside one stream (not supported yet: 0)
		eVideoCap_Variants        = 1u << 3, //!< IVideoPlayer::SetVariant / GetVariant work (interface version 4)
	};

	enum class EVideoPlayerState : uint8_t
	{
		Stopped = 0, //!< at time 0, not running (a loaded Stopped player shows its first frame; also: no media yet)
		Playing,
		Paused,
		Ended,       //!< reached the end and is not looping
		Loading,     //!< Load() is in progress (IsLoaded() == false)
		Error,       //!< Load() failed or playback failed later (OnError was sent); the object stays valid
	};

	//! How the fullscreen output places the picture on the screen.
	enum class EVideoFit : uint8_t
	{
		Fill = 0,   //!< stretch to the whole screen, aspect ratio is not kept
		Fit,        //!< keep the aspect ratio, whole picture visible, the scene stays visible in the bars
		Letterbox,  //!< like Fit, with opaque black bars
	};

	//! SetVariant() argument that hands the choice back to the cvar cev_variant (the default).
	constexpr int kVariantFollowCvar = -2;

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
		//! Loading failed, or a loaded video failed during playback (errorCode is a cev::Status value, message is a
		//! static string). The player is in the Error state afterwards.
		virtual void OnError(IVideoPlayer& player, int errorCode, const char* message) { (void)player; (void)errorCode; (void)message; }
		// ---- appended in version 1 as defaulted callbacks ----
		virtual void OnPaused(IVideoPlayer& player) { (void)player; }
		virtual void OnResumed(IVideoPlayer& player) { (void)player; }
		virtual void OnStopped(IVideoPlayer& player) { (void)player; }
		// ---- appended in version 3 ----
		// COMPATIBILITY: the plug-in is the CALLER of these virtuals. An IVideoPlayerListener implementation built
		// against a header older than the plug-in's (before version 3 = without OnMarker) has a shorter vtable than
		// the plug-in expects, and the plug-in then calls a slot that does not exist (undefined behaviour, typically a
		// crash). There is no run time check (the plug-in cannot see which header a listener was built with):
		// rebuild every IVideoPlayerListener implementer against the current header, at the latest when
		// kVideoPlayerInterfaceVersion grows past the version it was built with; compare it with
		// IVideoPlugin::GetVideoPlayerInterfaceVersion() at start up. The opposite direction is safe (virtuals are
		// only appended, never inserted or removed: a listener built against a newer header works with an older plug-in,
		// which simply never calls the callbacks it does not know).
		//! The playback position reached a marker of the .cevvideo descriptor. `time` is the marker's media
		//! timestamp in seconds (not the time of the dispatch). Markers fire again after a loop and after a seek
		//! backwards; a seek forwards skips the markers it jumps over.
		virtual void OnMarker(IVideoPlayer& player, const char* name, double time) { (void)player; (void)name; (void)time; }
	};

	//! One player = one video file = one RGBA texture. Create / destroy through IVideoPlugin.
	struct IVideoPlayer
	{
		virtual ~IVideoPlayer() {}

		// ---- media ----
		//! Starts loading `path` (any CryPak path, e.g. "Videos/intro.webm" or the runtime descriptor
		//! "Videos/intro.cevvideo": video file, languages, variants, markers). Returns false when the request is
		//! rejected immediately (empty path, file does not exist, unusable descriptor); OnError is then also sent.
		//! Replaces a previously loaded video (the texture is kept when the size does not change).
		virtual bool Load(const char* path) = 0;
		//! Stops, releases the media and the texture. The player object stays valid.
		virtual void Unload() = 0;
		virtual const char* GetPath() const = 0;

		// ---- control ----
		virtual void Play() = 0;    //!< from Stopped / Paused / Ended; queued while loading
		virtual void Pause() = 0;
		virtual void Resume() = 0;
		//! Back to time 0. The texture shows the first frame; a player in fullscreen output (SetFullscreen) stops
		//! drawing until the next Play() (so a stopped or skipped video does not stay on the screen).
		virtual void Stop() = 0;
		virtual void Seek(double seconds) = 0;
		virtual void SetLoop(bool loop) = 0;
		virtual void SetRate(float rate) = 0;     //!< playback speed, clamped to 0.25 .. 4 (rate <= 0 / NaN ignored)
		virtual void SetVolume(float volume) = 0; //!< 0..1, multiplied with the global volume cev_volume

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
		//! Texture name as registered with the renderer (e.g. "$tv"); may be used in .mtl files and Flash. The
		//! default is generated ("$cev_video_<w>x<h>_<k>", final once IsLoaded(), reused by later players); use
		//! IVideoPlugin::CreatePlayer(name) to choose it.
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

		// ---- appended in version 3 (languages) ----
		//! Selects the audio language by the name used in the .cevvideo descriptor (case insensitive). Without a
		//! call the language comes from the cvars g_languageAudio / g_language, then the descriptor default, then
		//! the embedded audio track. Allowed at any time: while a video plays, the media is reloaded in place and
		//! continues at the same position (a short glitch is possible). nullptr / empty = back to the default
		//! choice. A name the descriptor does not list is ignored with a warning; videos without a descriptor
		//! have no languages.
		virtual void SetLanguage(const char* name) = 0;
		//! The language in use (descriptor name), empty for a video without languages (the embedded track).
		virtual const char* GetLanguage() const = 0;

		// ---- appended in version 4 (variants) ----
		//! Chooses the variant of a .cevvideo descriptor for this player: kVariantFollowCvar (default) = the cvar
		//! cev_variant decides, -1 = automatic (smallest variant not below the screen height when fullscreen, else the
		//! main video), 0 = main video, n = the n-th <Variant> of the descriptor. Applies to the next Load(); while a
		//! descriptor video is loaded the media is reloaded in place and continues at the same position (a short
		//! glitch is possible). Videos without variants ignore it.
		virtual void SetVariant(int variant) = 0;
		//! Variant in use: 0 = main video (also without a descriptor), n = n-th <Variant>.
		virtual int GetVariant() const = 0;

		// ---- appended in version 5 (surface ids) ----
		//! Surface id given to IVideoPlugin::CreatePlayerForSurface, "" for players made by CreatePlayer.
		virtual const char* GetSurfaceId() const = 0;
		//! Counts changes of the texture (texture id, size or layout). It is advanced by the player itself, also
		//! when the renderer hands out the same id again; usable as a "fetch the texture again" test.
		virtual uint32_t GetGeneration() const = 0;
		virtual EVideoPixelLayout GetPixelLayout() const = 0;     //!< always RGBA8 so far
		virtual EVideoDecodeBackend GetDecodeBackend() const = 0; //!< always Cpu so far
	};
}
