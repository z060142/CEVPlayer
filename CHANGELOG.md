# Changelog

## Unreleased - rebuild

The `rebuild` branch replaces the Libav based plug-in of `master` (CRYENGINE 5.3 - 5.6) with a rewrite for CRYENGINE
5.7.1 and adds a Sandbox plug-in. Details and the decision log: [docs/PLAN.md](docs/PLAN.md) and `docs/design/`.

### Runtime

* New decoder core `CEVPlayerCore` (engine independent static library, own self test): libwebm demuxer, libvpx VP8 / VP9
  (VP9 alpha), libvorbis / libopus, frame ring, audio clock as master, resampling to 48 kHz, shared decode pool.
  Libav / FFmpeg are no longer used at run time.
* Prebuilt third-party libraries under `Code/SDKs/CEVPlayer/` (libvpx 1.17.0, libwebm 1.0.0.32, libogg 1.3.6, libvorbis
  1.3.7, libopus 1.6.1) with build script `tools/build_sdks.sh`.
* Outputs: fullscreen (Fill / Fit / Letterbox, skippable), material texture slot, named surface (surface id, engine texture
  `$cev:<id>`). Several players at a time.
* `.cevvideo` runtime descriptor: audio route (`embedded`, `mixed`, `cue`), named audio tracks with optional language tag
  and opt-in `followGameLanguage`, A/V offset, resolution variants, markers.
* Cue route: picture follows the playback position of a CryAudio trigger (optional engine patch
  `patches/optional/02-cryaudio-trigger-position.patch`, SDL_mixer implementation), clock fallback otherwise.
* Optional hardware decode of VP9 through Media Foundation (VP9 Video Extensions), read back into the same frame ring,
  automatic fallback to libvpx; per player `Decode = Auto | GPU | CPU`, `cev_decodeMode`, `cev_decodeAutoMinHeight`.
* Console variables and commands `cev_*` (see the README); statistics `cev_stats`, `cev_stats_file`.
* C++ interface `IVideoPlugin` / `IVideoPlayer`, interface version 5 (`CreatePlayerForSurface`, `FindPlayerBySurface`,
  `GetSurfaceId`, `GetPixelLayout`, `GetDecodeBackend`).

### UI integration

* Surface id registry: author-chosen ids (1 to 63 of `A-Z a-z 0-9 _ . : -`, case insensitive), duplicates refused, the id
  survives file / track / variant / size changes.
* Texture-source contract: export `CryTexSource_v1`, contract version 2 (`query`, `hint`, `open`, `close`, `control`),
  specified in `docs/design/M6-ui-source.md` §3. Contract functions never call the engine.
* Consumer hints: visibility throttling (decode only while hidden, stop after `cev_hiddenStopSeconds`, catch-up from the
  previous key frame), variant choice from the on-screen size at playback start, `PauseDecodeWhenHidden`.
* Packed planar layouts `PACKED_YUV420` / `PACKED_YUVA420` (one `R8` texture) negotiated through `acceptLayouts`, RGBA
  conversion only when a consumer needs RGBA8, layout switches without reloading the video; `cev_allowPlanar`.
* Self test `cev_contract_selftest`.

### Nodes and scripting

* One binding table (`Module/Bindings/ApiTable.cpp`: 19 controls, 25 queries, 8 events) generates all script bindings.
* FlowGraph: drawer `CEVPlayer` with `PlayOnScreen` and `PlayOnMaterial`, sub-drawer `UI` (`PlayToSurface`,
  `SurfaceInfo`, `SurfaceEvents`), sub-drawer `Advanced` (`Open`, `Transport`, `Settings`, `Info`, `Events`); enum ports as
  drop-downs; frame serials as strings.
* Schematyc component `Video Player` (properties, one function per table entry, eight signals).
* CryVS (community engine only): global functions in `CEVPlayer`, `CEVPlayer::UI`, `CEVPlayer::Advanced`, enum types, a
  `VideoPlayer` component with member functions and signals, re-registration on environment refresh; self test
  `cev_cryvstest`.
* Self tests `cev_apitest`, `cev_flowtest`; development script runner `cev_run_script`.

### Editor

* New Sandbox plug-in `EditorCEVPlayer`: asset type Video (`.cevvideo` + `.cryasset`), rename / move / delete of all files
  of an asset, the asset is imported (Asset Browser Reimport keeps the import settings), source status by size / time
  and SHA-256.
* Import of WebM, and with FFmpeg of MP4 / MOV / MKV / AVI and PNG sequences (re-compression, target height, frame rate,
  variants, key frame interval); batch import; options dialog.
* CEV Video Editor window: preview decoded by CEVPlayerCore in a plain Qt widget with audio and an audio track list; Info
  column with a property tree for the descriptor (audio tracks, engine audio trigger picker, markers), files with "not
  shipped" marks, variants, FFmpeg status; engine dialogs; preference page `CEVPlayer/FFmpeg`.
* Background jobs with progress notifications: split / strip audio, WAV and Ogg export, upgrade to the cue route with an
  Audio Controls Editor library, generate variants, re-compress.
* FFmpeg as an external process: detection, pinned download of a minimal LGPL build (release
  `ffmpeg-minimal-v8.0.3-cev1`, SHA-256 checked before unpacking).
* Editor test commands `cev_editor_*` and the editor script runner (`CEV_EDITOR_SCRIPT`).

### Tooling

* `tools/sync_sources.sh`, `tools/apply_patches.sh` with the engine patch series in `patches/` (vanilla 5.7.1 and the
  community fork, tree detection by file hashes), `tools/build_baseline.sh`, `tools/build_engine.sh`, `tools/deploy.sh`.
* `tools/build_sdks.sh` (third-party libraries), `tools/ffmpeg/build_minimal.sh` + `versions.env` (minimal FFmpeg),
  `tools/make_test_media.sh` (synthetic test clips), `tools/smoke_test.sh`, packaging helpers `tools/pak_cev_assets.sh`,
  `tools/list_unshipped_cev_files.py`.
* Standalone self tests `cevcore_selftest`, `cevoffline_selftest`, `cevffmpeg_selftest`.

### Breaking changes versus the old `master`

* Supported engine: CRYENGINE 5.7.1 (and the community fork). The 5.3 - 5.6 code and the Libav SDK (`_CustomSDKs`) are
  gone.
* Repository layout: sources moved from `Module/` and `Interface/` at the root to `Code/CryPlugins/CEVPlayer/`; the
  plug-in is copied into an engine tree with `tools/sync_sources.sh` and registered by the patch series.
* The legacy FlowGraph nodes (`CEVPlayer:FlashVideoPlayer`, `CEVPlayer:TextureVideoPlayer`), the
  `TextureVideoPlayerComponent`, all `vp_*` console commands and variables and the `OutTextureName` /
  `OutputTextureName` ports are removed **without aliases**. Levels and flow graphs that use them must be rebuilt with the
  new nodes.
* `IVideoPlugin::PlayVideoToMainWindow`, `PlayVideoToTexture`, `PauseVideo`, `ResumeVideo`, `StopVideo` and
  `IVideoPluginListeners.h` are removed; the interface is `IVideoPlugin` / `IVideoPlayer` with
  `kVideoPlayerInterfaceVersion` 5.
* Within the branch's own history: the descriptor's `<Languages>` element is replaced by `<AudioTracks>` (a descriptor
  with `<Languages>` is logged as an error and plays its embedded audio), and the API names changed from language to
  audio track (`SetLanguage` -> `SetAudioTrack`, query `Language` -> `AudioTrack`, console `cev_audiotrack`, editor
  import option `track:`, `.cryasset` details `audioTrackNames`). The bindings `TextureName`, `InterfaceVersion` and
  `Caps` are gone (`cev_info` shows the last two).
* Input at run time is WebM only.
