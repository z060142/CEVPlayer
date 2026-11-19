# CEVPlayer

CEVPlayer plays video inside CRYENGINE 5.7 games: full screen for intros, cutscenes and menu backgrounds, on
materials for screens and monitors placed in a level, and into named textures that a UI system can show as part of its
own layout. Videos can be transparent (VP9 with alpha), so they also work as animated overlays and effects on top of
the scene or the UI.

It is made of two plug-ins:

* **CEVPlayer** (runtime) decodes WebM - VP8 / VP9 video, Vorbis / Opus audio - with libvpx, or on the GPU through
  Media Foundation when you ask for it. Sound goes through the engine's audio system and stays in sync with the picture.
  A video can carry several named audio tracks, markers that fire events at given times, and lower-resolution variants
  that are picked for small screens. A video shown through a UI stops costing CPU and GPU time while that UI reports
  it hidden, and catches up when it is shown again.
* **EditorCEVPlayer** (Sandbox) makes video an asset type: import a file through the Asset Browser (other formats are
  converted to WebM with FFmpeg, which the editor can download), preview it with sound, and edit its
  tracks, markers and variants in a video editor window.

Game logic drives it with FlowGraph nodes, a Schematyc component, CryVS functions (Community Edition) or the C++
interface; two nodes - *play on screen* and *play on material* - cover the common cases without any setup.

The CRYENGINE Community Edition (CECE) has CEVPlayer built in; see
[Build and integration](#build-and-integration) for when you would integrate this repository yourself.

Licence: Boost Software License 1.0, see [LICENSE](LICENSE). Third-party libraries keep their own licences, see
[THIRD-PARTY.md](THIRD-PARTY.md). Changes: [CHANGELOG.md](CHANGELOG.md). Project tracking (Traditional Chinese):
[docs/PLAN.md](docs/PLAN.md); what still needs a person to look at it:
[docs/manual-acceptance.md](docs/manual-acceptance.md).

## Features

### Runtime (`Code/CryPlugins/CEVPlayer`)

* **Input**: WebM only - VP8 or VP9 video (VP9 with alpha as WebM `BlockAdditional`), Vorbis or Opus audio. No Libav /
  FFmpeg at run time: libwebm demuxer, libvpx, libvorbis, libopus. Other formats are converted by the editor on import.
* **Decoding**: libvpx on a shared pool of worker threads (`cev_decodeThreads`). Optional **hardware decode for VP9**
  through Media Foundation (the decoder comes from the Microsoft Store package "VP9 Video Extensions", not from Windows
  itself): one dedicated thread per player, its own D3D11 device, NV12 output read back into the same frame ring the
  software path uses. Per player `Decode = Auto | GPU | CPU` (global default `cev_decodeMode`): `Auto` uses the GPU
  when it is available and the video is at least `cev_decodeAutoMinHeight` (1080) pixels tall, `GPU` falls back to
  libvpx with one warning when no decoder can be created, `CPU` always uses libvpx. VP8 always decodes on the CPU. A
  second Media Foundation decoder handles the alpha stream; when it cannot be opened only the alpha goes through
  libvpx. The backend in use is reported (`DecodeBackend` query / node output, `cev_stats`).
* **Outputs** (several players at a time):
  * **Screen**: fullscreen on the top layer (`Fill`, `Fit` = keep aspect with the scene visible in the bars,
    `Letterbox` = keep aspect with black bars), optionally skippable by a key / mouse / gamepad button.
  * **Material**: a texture slot of a material (a screen in the level); empty material name = the entity's material.
  * **Surface**: a named surface for UIs. The **surface id** is chosen by the author (1 to 63 of
    `A-Z a-z 0-9 _ . : -`, case insensitive, unique; creating a second player with the same id fails). It stays the
    same when the file, audio track, variant or size changes. The engine texture is named `$cev:<id>`.
* **Texture-source contract for UI consumers**: `CEVPlayer.dll` exports `CryTexSource_v1` (contract version 2,
  specified in [docs/design/M6-ui-source.md](docs/design/M6-ui-source.md) §3; no shared header - a consumer copies the C
  declarations). A UI plug-in finds the export by scanning the loaded modules and then, per surface id:
  * `query` - texture id, picture size, flags (premultiplied, alpha, playing, colour planes multiplied by alpha),
    `contentSerial`, `generation`, layout and plane rectangles, colour matrix / range / chroma siting, state, error code,
    time, duration, loop count;
  * `hint` - per consumer: visible this frame, largest on-screen pixel size, accepted layouts;
  * `open` / `close` / `control` - start a video for a `<video src>` style element and send play / pause / seek / rate /
    loop / volume / mute intents.

  Layouts: `RGBA8` (premultiplied, the default) and the packed planar layouts `PACKED_YUV420` / `PACKED_YUVA420` (one
  `R8` texture holding Y, U, V [and A]; no colour conversion on the CPU, about half the upload) - used only while every
  consumer that sends hints accepts them, 4:2:0 with an even width only; switching layouts rebuilds the texture without
  reloading the video. Visibility hints throttle work: a surface nobody sees only decodes (no copy, conversion or
  upload), stops decoding after `cev_hiddenStopSeconds` (2 s) and catches up from the previous key frame when it becomes
  visible again (per player `PauseDecodeWhenHidden`, default on; screen and material outputs always count as visible).
  The size hints choose the variant when playback starts. The contract functions never call the engine; all engine work
  happens in the plug-in's own tick. Any UI can also use a surface without the contract, through the `TextureId` /
  `Width` / `Height` outputs of the nodes (RGBA8 only, no throttling). No consumer of the contract ships in this
  repository.
* **Audio routes** (per video, in the `.cevvideo` descriptor):
  * `embedded` - the audio track inside the WebM, through an SDL audio device (48 kHz; other rates are resampled);
  * `mixed` - audio tracks in external audio-only WebM files;
  * `cue` - a CryAudio trigger plays the sound and the picture follows its playback position. Exact sync needs the
    optional engine patch (SDL_mixer implementation only); without it the picture follows a clock.
* **Named audio tracks**: a descriptor lists tracks by name (`SetAudioTrack`, `cev_audiotrack`); a track may carry an
  optional `language` tag, and following the game language is opt-in (`followGameLanguage="1"`). Switching the track
  while playing reloads the media in place (same time, same state).
* **Markers** (named times, delivered as `Marker` events), **A/V offset**, **resolution variants** (chosen at playback
  start from the output size or the consumers' size hints; `cev_variant` overrides).
* **Script bindings** generated from one table (`Module/Bindings/ApiTable.cpp`: 19 controls, 25 queries, 8 events):
  FlowGraph nodes, a Schematyc component and, on the community engine, CryVS functions (see [Node sets](#node-sets)).
* **C++ API**: `Interface/CEVPlayer/IVideoPlugin.h`, `IVideoPlayer.h` (interface version 5) for game code that links
  CEVPlayer directly. Other plug-ins use the texture-source contract instead.

### Editor (`Code/Sandbox/Plugins/EditorCEVPlayer`)

* **Asset type Video** (`<name>.cevvideo` + `<name>.cevvideo.cryasset`). The asset is "imported": the source copy in the
  asset folder is registered as the source file, so the Asset Browser's **Reimport** works and keeps the stored import
  settings. Rename / move / delete take every file of the asset along (exports, SDL_mixer copies, ACE library).
* **Import**: drop a file on the Asset Browser or on the editor window, or `cev_editor_import`. WebM is copied as it is;
  MP4 / MOV / MKV / AVI and PNG sequences are transcoded to VP9 WebM with FFmpeg (optional re-compression, target
  height, frame rate, resolution variants, key frames every 2 s). Options dialog: asset name, audio track name of the
  embedded audio (default `main`), route, trigger, split audio, WAV / Ogg exports. Source status: size and modification
  time are compared, the SHA-256 only when one of them changed.
* **CEV Video Editor** window (*Tools > CEV Video Editor*): the engine's Asset Browser, a preview and an Info column.
  * **Preview**: decoded by CEVPlayerCore and painted by a plain Qt widget (no engine renderer, works without the
    runtime plug-in); checkerboard behind alpha, play / pause / scrub / frame step / loop / timecode, an **Audio track**
    list (switches in place), audio through Qt. The preview does not follow the game language.
  * **Info column** built on Sandbox facilities: streams and files (with "not shipped" marks), the descriptor in a
    property tree (route, A/V offset, **Audio tracks** block with Default track, Follow game language, per track Name /
    Source = Embedded, File or Trigger / file / start, pause and resume trigger with the engine's audio trigger picker /
    optional Language; markers), changes written immediately (no Apply button, Ctrl+S saves), variants table, FFmpeg
    status with "Open preferences", engine dialogs (`CSystemFileDialog`, `CQuestionDialog`), job details.
* **Operations** (background jobs, cancellable, shown in the notification center with progress): split audio to WebM,
  strip audio, export WAV (sample 0 = video second 0), export Ogg Vorbis, upgrade to the cue route (SDL_mixer copy and an
  Audio Controls Editor library with Play / Pause / Resume triggers per audio track), generate variants, re-compress.
* **FFmpeg** is an external process, needed only for transcoding. Detection order: the path in *Preferences >
  CEVPlayer/FFmpeg*, the downloaded copy, `PATH`. The editor can download a minimal LGPL build pinned by version and
  SHA-256 (release `ffmpeg-minimal-v8.0.3-cev1` of this repository's GitHub releases; the zip is verified before it is
  unpacked into `%LOCALAPPDATA%\CEVPlayer\ffmpeg\8.0.3-cev1`). Without FFmpeg, WebM import and all audio operations still
  work; a non-WebM import asks you to download or choose an `ffmpeg.exe`.

## Node sets

### FlowGraph

One drawer `CEVPlayer` with two ready-to-use nodes and the sub-drawers `UI` and `Advanced` (10 node classes). Enum
ports are drop-downs: `Fit` = Fill / Fit / Letterbox, `Decode` = Auto / GPU / CPU, `Output` = Surface / Screen /
Material. The ready-to-use and UI nodes own their player (Stop or the end of the graph destroys it).

| Node | Inputs | Outputs |
|---|---|---|
| `CEVPlayer:PlayOnScreen` | Play, Stop, Video, Fit, Loop, Skippable, Decode | Started, Ended, Skipped, Error, ErrorCode, ErrorMessage |
| `CEVPlayer:PlayOnMaterial` | Play, Stop, Video, Material (empty = the entity's), Slot, SubMaterial, Loop, Autoplay, Decode | Started, Ended, Error, ErrorCode, ErrorMessage |
| `CEVPlayer:UI:PlayToSurface` | Play, Stop, Video, SurfaceID, Loop, Autoplay, Decode, PauseDecodeWhenHidden | Ready, TextureId, Width, Height, Duration, HasAlpha, Ended, Error, ErrorCode, ErrorMessage |
| `CEVPlayer:UI:SurfaceInfo` | Get, SurfaceID | Exists, TextureId, Width, Height, PixelLayout, Premultiplied, HasAlpha, FrameSerial, State |
| `CEVPlayer:UI:SurfaceEvents` | SurfaceID | Ready, NewFrame, FrameSerial, Resized, TextureId, Width, Height, Closed |
| `CEVPlayer:Advanced:Open` | Open, Close, Video, Output, SurfaceID, Fit, Material, Slot, SubMaterial, Decode | Handle, Ready, Error, ErrorCode, ErrorMessage |
| `CEVPlayer:Advanced:Transport` | Handle, Play (also resumes), Pause, Stop, Seek, Seconds | Done, Error |
| `CEVPlayer:Advanced:Settings` | Handle, Loop, Rate, Volume, AudioTrack, Variant, Skippable, PauseDecodeWhenHidden (each port applies its own setting when triggered) | Done, Error |
| `CEVPlayer:Advanced:Info` | Get, Handle, EveryFrame | State, Time, Duration, Width, Height, HasAlpha, AudioTrack, Variant, DecodeBackend, FrameSerial, SurfaceId, Error |
| `CEVPlayer:Advanced:Events` | Handle (0 = stop listening) | Ready, Started, NewFrame, NewFrameSerial, Marker, MarkerName, MarkerTime, Looped, Ended, Skipped, Error, ErrorCode, ErrorMessage |

Frame serials are strings (no 2^31 wrap).

### Schematyc component

Component **Video Player** (category *Video*). Properties: Video, Output (Surface / Screen / Material), SurfaceID,
Material, Slot, Screen Fit, Audio Track (empty = automatic), Loop, Autoplay, Skippable, Decode, PauseDecodeWhenHidden.
Functions: one per entry of the API table (controls and queries, e.g. `Play`, `Pause`, `Seek`, `SetAudioTrack`,
`SetDecode`, `Time`, `AudioTrackName`, `DecodeBackend`). Signals: Ready, Started, NewFrame (serial), Marker (name,
time), Looped, Ended, Skipped, Error (code, message).

### CryVS (community engine only)

Compiled only when CMake finds the CryVS headers (the community fork); on the official 5.7.1 tree nothing of it exists.
Global functions in three namespaces (library path `Function::CEVPlayer::...`):

* `CEVPlayer`: `PlayOnScreen`, `PlayOnMaterial` (return a handle), `Stop`;
* `CEVPlayer::UI`: `PlayToSurface`, `StopSurface`, `SurfaceInfo`;
* `CEVPlayer::Advanced`: `Open`, `Close`, `Play`, `Pause`, `Stop`, `Seek`, `SetLoop`, `SetRate`, `SetVolume`,
  `SetAudioTrack`, `SetVariant`, `SetSkippable`, `SetPauseDecodeWhenHidden`, `Info` (several outputs).

Enums (Fit, Output, Decode) are registered as CryVS types. Players made by these functions belong to the script and are
destroyed when the level unloads or game mode ends. A component `VideoPlayer` with one member function per API table
entry (41) and one signal per event is registered as well. Registration happens at `ESYSTEM_EVENT_GAME_POST_INIT` and
again on every environment refresh.

## The `.cevvideo` descriptor

A small XML file next to the video; the editor writes it, it can also be written by hand. File names are relative to
the descriptor's folder.

```xml
<CEVVideo version="1" route="mixed" video="intro.webm" avOffsetMs="0">
  <AudioTracks default="dialogue" followGameLanguage="1">
    <Track name="dialogue" embedded="1" language="english"/>
    <Track name="dialogue_jp" file="intro_dialogue_jp.webm" language="japanese"/>
    <Track name="commentary" file="intro_commentary.webm"/>
  </AudioTracks>
  <Variants rule="smallestNotBelowOutput">
    <Variant file="intro_720.webm" width="1280" height="720"/>
    <Variant file="intro_540.webm" width="960" height="540"/>
  </Variants>
  <Markers>
    <Marker name="explosion" time="3.25"/>
  </Markers>
</CEVVideo>
```

* `route`: `embedded`, `mixed` or `cue`. For `cue`, a track names its CryAudio trigger instead of a file:
  `<Track name="music" trigger="Play_Intro_Music" pauseTrigger="Pause_Intro_Music" resumeTrigger="Resume_Intro_Music"/>`;
  `trigger` / `pauseTrigger` / `resumeTrigger` on the root element are defaults for tracks without their own.
* Track choice: the track set by `SetAudioTrack`, then - only with `followGameLanguage="1"` - the track whose
  `language` matches `g_languageAudio` / `g_language`, then `default`, then the first track. Without `<AudioTracks>` the
  embedded audio plays. Track names: 1 to 63 of `A-Z a-z 0-9 _ . -`, case insensitive, unique.
* The old `<Languages>` element is not read: it is logged as an error and the video plays its embedded audio.
* Parsing is defensive: an invalid attribute gets its default and a warning, a broken child element is skipped; malformed
  XML or a missing `video` attribute rejects the descriptor.

## Console variables and commands

### Runtime plug-in - for users

| Name | Default | Meaning |
|---|---|---|
| `cev_play <path> [loop]` | | play a `.webm` / `.cevvideo` fullscreen (test player) |
| `cev_stop`, `cev_pause`, `cev_resume`, `cev_seek <s>` | | control the `cev_play` player |
| `cev_play_surface <id> <path> [loop]` | | play into a surface player; without a path the surface player is destroyed |
| `cev_audiotrack <handle\|surface> [name]` | | choose the audio track of a player (script handle or surface id); without a name: list the tracks and the one in use |
| `cev_list` | | all players with state, time and upload count |
| `cev_surfaces` | | surfaces of the texture-source contract with their snapshot and aggregated consumer hints |
| `cev_info` | | interface version, capability bits, contract version, surfaces, players, API table size |
| `cev_volume` | 1.0 | master volume of all videos (multiplied with the player volume) |
| `cev_audio_enable` | 1 | 0 = no audio output (audio is not even decoded for players created afterwards) |
| `cev_muteOnFocusLoss` | 1 | mute video audio while the game window has no focus |
| `cev_disableEmbeddedAudio` | 0 | 1 = videos play silently and SDL audio is never initialised (set it in the `.cryproject` to cover the whole run) |
| `cev_decodeMode` | 0 | decoder of players without an explicit `Decode`: 0 Auto, 1 GPU (Media Foundation, CPU fallback), 2 CPU; applies at the next Load / Play from stopped |
| `cev_decodeAutoMinHeight` | 1080 | `Auto` uses the GPU for videos at least this tall |
| `cev_decodeThreads` | 0 | decode worker threads of the shared pool and libvpx threads per video; 0 = automatic |
| `cev_variant` | -1 | variant for players loaded afterwards: -1 automatic, 0 main video, n = n-th `<Variant>` |
| `cev_allowPlanar` | 1 | 0 = surfaces always stay RGBA8 even when every consumer accepts a packed planar layout |
| `cev_hiddenStopSeconds` | 2.0 | surfaces no consumer sees stop decoding after this many seconds; 0 = never |
| `cev_cueForceFallback` | 0 | 1 = the cue route ignores the optional engine patch and follows the video clock |
| `cev_stats` | 0 | 1 = log and draw a statistics line per player every second (timings, uploads, layout, decoder) |
| `cev_log` | 0 | 1 = verbose log |

### Runtime plug-in - development and tests

| Name | Meaning |
|---|---|
| `cev_apitest [path]` | self test of the API table (default `Videos/vp9_alpha.webm`), PASS / FAIL |
| `cev_flowtest [pathA] [pathB]` | self test of every FlowGraph node class in an in-memory graph |
| `cev_contract_selftest [pathA] [pathB]` | self test of the texture-source contract through the `CryTexSource_v1` export |
| `cev_cryvstest` | self test of the CryVS registrations and calls (community engine only) |
| `cev_run_script <file>` | runs console lines one per frame, immediately (`wait <seconds>`, `#` comments); use it instead of `exec` for test scripts |
| `cev_stats_file <path>\|off` | write the per-second `cev_stats` rows as CSV |
| `cev_hidden_bench`, `cev_bench_play`, `cev_bench_stop`, `cev_bench_hint`, `cev_show_surface`, `cev_probe_r8`, `cev_probe_sync` | measurement probes used for the design documents |

### Sandbox plug-in - for users

| Command | Meaning |
|---|---|
| `cev_editor_import <absSrc\|absPngFolder> <assetFolder> [name:<asset>] [track:<audio track>] [wav] [ogg] [split\|strip] [cue\|mixed] [overwrite] [crf:N] [height:PX] [variants:540,360] [fps:N] [recompress]` | import without UI. Refuses when an asset of that name exists, unless `overwrite`. Scripts must write `name:` (`name=` only works when typed, the console treats `a=b` as an assignment) |
| `cev_editor_import_batch <absFolderOrListFile> <assetFolder> [options]`, `cev_editor_import_cancel` | import many files on one queue (one ffmpeg at a time); cancel the running and queued imports |
| `cev_editor_variants <asset.cevvideo> <heights> [crf:N]`, `cev_editor_recompress <asset.cevvideo> [crf:N] [height:PX] [variants:a,b\|none] [refresh]` | generate variants / re-encode from the source copy (FFmpeg) |
| `cev_editor_ffmpeg_status [redetect]`, `cev_editor_ffmpeg_path <path\|->` | show the ffmpeg in use; set or clear the path stored in the preferences |
| `cev_editor_ffmpeg_download` | download, verify and install the pinned minimal FFmpeg (what the Download button does) |
| `cev_editor_open [<asset.cevvideo>]`, `cev_editor_close` | open / close the CEV Video Editor |
| `cev_editor_rename <asset> <newName>`, `cev_editor_move <asset> <folder>`, `cev_editor_delete <asset>` | asset manager operations (same code path as the Asset Browser) |

### Sandbox plug-in - development and tests

| Command | Meaning |
|---|---|
| `cev_editor_script <file>` | run console commands from the editor idle loop (`wait_seconds N`, `#` comments). `CEV_EDITOR_SCRIPT=<file>` starts one automatically (`CEV_EDITOR_SCRIPT_DELAY` seconds after the main frame, default 8). Sandbox never runs the console's deferred queue, so `exec` / `wait_seconds` of a normal cfg do not work there |
| `cev_editor_preview_test [png:<path>] [pause:<s>] [track:<name>] [close]` | self test of the preview of the open editor |
| `cev_editor_info_test [legacy:<asset>] [alt:<abs webm>] [trigger:<name>] [ffmpeg:<path>]` | self test of the Info column on the open (scratch) asset: property tree edit, save, Reimport, source check, FFmpeg preference |
| `cev_editor_info_grab png:<path>`, `cev_editor_preview_dock <float\|dock\|tab_other\|tab_preview>`, `cev_editor_frametime <s> [label]`, `cev_editor_status` | screenshots, docking test, frame time statistics, counters |
| `cev_editor_ffmpeg_install_test <zip> <sha256> [version]` | the download install path with a local zip |

## Repository layout

| Path | Content |
|---|---|
| `Code/CryPlugins/CEVPlayer/` | runtime plug-in: `Module/` (engine glue, bindings, contract), `Core/` (engine independent decoder and its self test; `Core/offline/` = CEVPlayerOffline and CEVPlayerFFmpeg for the editor), `Interface/` (public headers) |
| `Code/Sandbox/Plugins/EditorCEVPlayer/` | Sandbox plug-in |
| `Code/SDKs/CEVPlayer/` | prebuilt third-party libraries (Win64, MSVC 2022), see [THIRD-PARTY.md](THIRD-PARTY.md) |
| `cmake/modules/` | CMake modules for those libraries (copied into the engine's `Tools/CMake/modules/`) |
| `patches/` | engine patch series (plug-in registration) and the optional CryAudio patch, [patches/README.md](patches/README.md) |
| `tools/` | sync / patch / build / test media / smoke test / packaging scripts, `tools/ffmpeg/` (minimal FFmpeg build) |
| `docs/` | plan and decision log, design documents, task sheets, research notes, manual acceptance lists (Traditional Chinese) |

## Build and integration

Requirements: Windows 10/11 x64, Visual Studio 2022 (MSVC 14.4x, C++ desktop workload, ATL/MFC for Sandbox), a Windows
SDK, Git Bash for the scripts, a CRYENGINE 5.7.1 source tree or the community fork. Optional: "VP9 Video Extensions"
from the Microsoft Store for hardware decode; FFmpeg for transcoding in the editor (can be downloaded by the editor).

Nothing is built inside this repository: the sources are copied into an engine tree.

### Official CRYENGINE 5.7.1 source

```bash
# 1. copy plug-in sources, SDKs and CMake modules into the engine tree (one direction: edit here, sync there)
bash tools/sync_sources.sh /path/to/CRYENGINE_5.7.1          # --dry-run shows what it would do

# 2. register the plug-ins in the engine's CMake files (idempotent; guarded with EXISTS)
bash tools/apply_patches.sh /path/to/CRYENGINE_5.7.1
#    optional: playback position of a CryAudio trigger for the cue route (SDL_mixer implementation)
bash tools/apply_patches.sh /path/to/CRYENGINE_5.7.1 --with-optional

# 3. configure (once, and again whenever a CMakeLists.txt changed)
cd /path/to/CRYENGINE_5.7.1
Tools/CMake/Win32/bin/cmake.exe -S . -B win64 -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE=Tools/CMake/toolchain/windows/WindowsPC-MSVC.cmake \
  -DOPTION_ENGINE=ON -DOPTION_SANDBOX=ON -DOPTION_CRYMONO=OFF

# 4. build the plug-ins (Profile), e.g. through the wrapper script
CRYENGINE_SOURCE=/path/to/CRYENGINE_5.7.1 LOGTAG=A bash tools/build_baseline.sh Profile \
  'CRYENGINE\Sandbox\Plugins\EditorCEVPlayer' 'Project\Plugins\CEVPlayer'
```

Targets: `CEVPlayer` (runtime DLL, links the static `CEVPlayerCore`) and `EditorCEVPlayer` (links `CEVPlayerOffline`
and `CEVPlayerFFmpeg`). `tools/build_baseline.sh` injects `tools/msbuild/baseline_compat.props`, which points at
machine-specific ATL/MFC and toolset compatibility files of the development machine; adjust or drop it on another
machine (building the solution targets with plain MSBuild works where ATL/MFC is installed). `tools/deploy.sh <built bin>
<installed bin>` copies the DLLs into an installed engine (with backups). Details:
[docs/research/02-baseline-build.md](docs/research/02-baseline-build.md).

### Community Edition (CECE, CryENGINE-5-7-community)

CECE ([CryENGINE-5-7-community](https://github.com/z060142/CryENGINE-5-7-community)) has CEVPlayer built in: it is
part of the engine tree and is built with it, so most CECE users do nothing here. CECE releases take their CEVPlayer
from this repository and can be behind it. This section is for developers who want a newer
version of the plug-in than the one in their CECE: the steps below **replace** the built-in copy.

> **Warning for CECE 1.0:** its built-in CEVPlayer is the earlier version. After replacing it, levels, flow graphs and
> components made with the built-in nodes no longer load them and must be rebuilt with the [node sets](#node-sets) of
> this version (there are no aliases). In return the editor plug-in and the CryVS functions become available.

CECE releases after 1.0 that already contain this version do not have this break: updating them is only a matter of
taking newer sources.

The tree already has `Code/CryPlugins/CEVPlayer` and an unguarded `add_subdirectory(CEVPlayer/Module)`; the patch
series for it (`patches/community/`) only adds the Sandbox registration. `apply_patches.sh` detects the tree by file
hashes.

**Caveat:** `sync_sources.sh` replaces each destination directory as a whole (`rm -rf`, then copy), including
`Code/SDKs/CEVPlayer/`, and removes old flat copies under `Code/SDKs/<lib>`. If `Code/SDKs` (or a folder in it) is a
junction or symbolic link into a tree you do not want changed, do not run the script there: copy
`Code/CryPlugins/CEVPlayer`, `Code/Sandbox/Plugins/EditorCEVPlayer`, `Code/SDKs/CEVPlayer` and `cmake/modules/*.cmake`
by hand instead (see [docs/research/06-community-baseline.md](docs/research/06-community-baseline.md)). Configure and build
as above; only the CEVPlayer targets are verified on the fork. CryVS support is compiled automatically there. Test
projects on the fork need `sys_SchematycPlugin=0` and Sandbox needs `sys_asserts=0` (fork issues, see
[Limitations](#limitations-and-known-issues)).

### Use in a project

Add the runtime plug-in to the `.cryproject`:

```json
{ "type": "EPluginType::Native", "path": "CEVPlayer" }
```

The Sandbox plug-in is loaded from `bin/win_x64/EditorPlugins/EditorCEVPlayer.dll` automatically. The editor's preview
and import work without the runtime plug-in.

## Asset workflow

1. **Import**: drop a video on the Asset Browser (or on the editor window, which imports into the folder of the open
   asset), or `cev_editor_import`. If an asset of that name already exists you are asked before it is replaced.
2. The asset folder then holds `<name>.cevvideo` (runtime descriptor), the video the descriptor plays (`<name>.webm`, or
   for a transcoded import the WebM made from the source), the source copy, `<name>.cevvideo.cryasset` (editor metadata:
   source path, size, time and SHA-256, import settings, stream details) and the files the operations make
   (`<name>_<track>.webm|.wav|.ogg`, `<name>_silent.webm`, variants). All files are flat in one folder and start with the
   asset name, so rename / move / delete take them along.
3. **Edit** in *Tools > CEV Video Editor*: preview and Info column (see [Editor](#editor-codesandboxpluginseditorcevplayer)).
   If a background operation rewrites the descriptor while you have unsaved edits, your edits are merged with the new
   file (and you are told).
4. **Audio operations** run as background jobs with a progress notification and a cancel button. Quitting the editor
   cancels and joins them. Files a cancelled or failed job created from nothing are removed again.
5. **Cue route**: *Upgrade to cue route* copies the WAV / Ogg to `audio/sdlmixer/assets/cev/<name>/`, writes
   `audio/sdlmixer/ace/cev_<name>.xml` (Play_ / Pause_ / Resume_ trigger per audio track, shown by the Audio Controls
   Editor) and switches the descriptor. These two locations are not Files of the asset; delete and rename keep them in
   step. Wwise / FMOD: the Info column lists the manual steps.
6. **Engine behaviour**: the engine creates a Sound asset (`<file>.wav.cryasset`) for every `.wav` under Assets, also for
   the exports and the SDL_mixer copies. The video asset removes those on delete / rename / move; the engine creates new
   ones for the new names.

## Source file policy

The importer copies the source video into the asset folder; for a plain WebM import that copy **is** the video the game
plays. After *Strip audio*, a transcoded import or a re-compression the descriptor plays another file and the copy is
only a source file. The WAV / Ogg exports in the asset folder are editor side files too (the cue route plays its own
copies under `audio/sdlmixer`). What goes where:

* **Do not ship**: `*.cryasset`, `.cevpreview/`, and per asset the files the descriptor does not play (source copy after
  a strip or transcode, WAV / Ogg exports). The Info column marks them "not shipped".
* CRYENGINE 5.7.1 has no switch for "do not pack the source of an asset". `rc.exe` has `/exclude=` (wildcards) and
  `/exclude_listfile=` (exact paths relative to the source root). `tools/list_unshipped_cev_files.py <Assets>` writes the
  exact list from the `.cryasset` files and descriptors, `tools/pak_exclude_sources.txt` holds the wildcard patterns, and
  `tools/pak_cev_assets.sh <engine> <Assets> <out.pak>` runs rc.exe with both.
* **Also note**: the stock packaging job (`Tools/CryVersionSelector/rc_jobs/game_assets.xml`, used by the project
  release tool) only packs known extensions; `.cevvideo`, `.webm` and `.wav` are not among them. Pack the videos with
  `tools/pak_cev_assets.sh` (or add the same `<Job Zip=... Input="*.cevvideo;*.webm" Exclude=.../>` to your own copy of
  the job file) and ship the resulting pak next to the others.
* **Version control**: videos and WAV files are large binaries. Use Git LFS (or another large file store) for
  `Assets/**/*.webm`, `*.wav`, `*.ogg`, or keep the original sources outside the repository: the asset records the
  original path and hash, so Reimport works from anywhere and the copy can be restored. Never commit
  `<name>.cevvideo.cryasset` without its folder (it lists the files by relative path).
  Example `.gitattributes`: `Assets/**/*.webm filter=lfs diff=lfs merge=lfs -text` (same for `*.wav`, `*.ogg`).

## Tests

| Test | What | How |
|---|---|---|
| `cevcore_selftest` | decoder core: demux, decode, alpha, colour conversion, audio clock, descriptor plan, packed layout, backends, stress | standalone CMake build of `Code/CryPlugins/CEVPlayer/Core` (see the header of its `CMakeLists.txt`), clips from `tools/make_test_media.sh` |
| `cevoffline_selftest` | editor media tools: probe, remux, split / strip, WAV / Ogg export (alignment <= 1 sample), error paths | same build (`offline/`) |
| `cevffmpeg_selftest` | FFmpeg locator, runner, command builder, SHA-256, install; real transcodes with a full ffmpeg (`--ffmpeg`) | same build; `--real-download <dir>` additionally downloads the pinned release (network) |
| `cev_apitest`, `cev_flowtest`, `cev_contract_selftest`, `cev_cryvstest` | in-engine self tests (PASS / FAIL with check counts in the log) | GameLauncher with a test project; drive them with `+cev_run_script <file>` |
| `cev_editor_preview_test`, `cev_editor_info_test` | editor preview and Info column | Sandbox with `CEV_EDITOR_SCRIPT=<file>` |
| `tools/smoke_test.sh <engine-root> [--sandbox] [--exec "cmd;cmd"]` | start, run commands, quit | see [docs/research/03-smoke-test.md](docs/research/03-smoke-test.md) |

Standalone example (system CMake >= 3.14, an ffmpeg with libvpx, libvpx-vp9, libopus, libvorbis and libx264 for the
clips):

```bash
FFMPEG=/path/to/ffmpeg.exe FFPROBE=/path/to/ffprobe.exe bash tools/make_test_media.sh /tmp/cev_media
cmake -S Code/CryPlugins/CEVPlayer/Core -B /tmp/cev_build -G "Visual Studio 17 2022" -A x64 -DCEV_TEST_MEDIA_DIR=/tmp/cev_media
cmake --build /tmp/cev_build --config Release
/tmp/cev_build/Release/cevcore_selftest.exe --media /tmp/cev_media
/tmp/cev_build/offline/Release/cevoffline_selftest.exe --media /tmp/cev_media
/tmp/cev_build/offline/Release/cevffmpeg_selftest.exe --media /tmp/cev_media --ffmpeg /path/to/ffmpeg.exe
```

The in-engine tests expect the clips under `Videos/` of the test project's assets. Some items can only be checked by a
person; they are collected in [docs/manual-acceptance.md](docs/manual-acceptance.md).

## Limitations and known issues

* **Input is WebM only** at run time; other containers go through the editor's FFmpeg import.
* **Nothing advances during level-loading screens** yet. Playback while the engine's loading loop runs needs an engine
  mechanism that is being prepared elsewhere (milestone M10 in [docs/PLAN.md](docs/PLAN.md)).
* **Sandbox edit mode**: the engine does not tick plug-ins there, so in-level videos (screen, material, surface) do not
  advance outside game mode. The editor's own preview is not affected.
* **Hardware decode** exists only for VP9 and needs the "VP9 Video Extensions" package; it saves CPU time but costs more
  process memory than software decode (about 100 MB more for a 1080p30 video, several hundred MB for 2160p with alpha,
  measured on one NVIDIA GPU; [docs/design/M6-ui-source.md](docs/design/M6-ui-source.md) §14). It has been checked on one
  machine only (other GPU vendors, machines without the package and multi-GPU systems are untested; the fallback paths
  were exercised with test switches).
* **UI path**: the packed planar layouts, visibility throttling, size-based variants and `open` / `control` take effect
  only with a UI consumer that implements the texture-source contract; none ships in this repository. Without one, UIs
  use the RGBA8 texture from the node outputs.
* **Fullscreen rendering** goes through the engine's aux geometry (top layer); colour conversion for RGBA8 is done on the
  CPU. The plug-in ships no shaders.
* **Cue route**: exact lip-sync needs the optional patch (`patches/optional/02-cryaudio-trigger-position.patch`, SDL_mixer
  implementation only); otherwise (and with Wwise / FMOD / Adx2 / PortAudio) it follows a clock. Seeking a cue video
  moves the picture only.
* **Engine console `exec`** of a cfg file with four or more lines containing `wait_seconds` can hit an engine assert
  (`CryString.h:734`); the test commands use `cev_run_script` / `cev_editor_script` instead.
* An engine assert `CryThreadImpl_win32.h:98 m_recurseCounter == 0` was seen once in Sandbox during an import. It is in
  the engine's recursive lock; the plug-ins use no CRYENGINE locks, and 17 further imports did not reproduce it. Not
  attributed.
* **Editor**: Win64 Sandbox only; ACE libraries are written for SDL_mixer only; WAV / Ogg copies for the cue route are made
  on the main thread; paths longer than 260 characters are not supported.
* **Community fork**: only the CEVPlayer targets are built and tested there. Fork issues not caused by the plug-in: every
  quit crashes in `C3DEngine::UnloadLevel`, `sys_SchematycPlugin=1` crashes, `IFramework::RefreshEnv()` asserts, Sandbox
  needs `sys_asserts=0`.
* **Not yet verified by a person**: the FlowGraph nodes as they look and behave in the editor (incl. the PlayOnMaterial
  success path, Autoplay, Skipped / Marker outputs), the Schematyc component in a level, a CryVS script authored in the
  CryVS designer and run in game mode, preview audio and smoothness, the Info column and dialogs by mouse, hardware
  decode on other hardware. The full list is in [docs/manual-acceptance.md](docs/manual-acceptance.md).
* Windows only.

## Credits and maintenance

CEVPlayer was created by **Ilya Chernetsov** ([afrostalin/CEVPlayer](https://github.com/afrostalin/CEVPlayer),
2018 - 2020, CRYENGINE 5.3 - 5.6); his history and copyright notices are kept in this repository. This version is a
rewrite that continues the project under the same name and licence. It is maintained by
[z060142](https://github.com/z060142), independently of the original author - please send questions and changes
[here](https://github.com/z060142/CEVPlayer), not to him.
