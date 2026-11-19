# CEVPlayer

WebM video for CRYENGINE 5.7 (VP8 / VP9, alpha, Vorbis / Opus audio): a runtime plug-in (**CEVPlayer**) and a Sandbox
plug-in (**EditorCEVPlayer**) with a video asset type, an importer and a tool window.

The original plug-in is by **Ilya Chernetsov** ([afrostalin/CEVPlayer](https://github.com/afrostalin/CEVPlayer), Libav based,
CRYENGINE 5.3 - 5.6). This branch (`rebuild`) is a rewrite of the decoding, rendering and audio layers for CRYENGINE 5.7.1
and adds the editor tooling; the plug-in keeps its name. The original FlowGraph nodes, the TextureVideoPlayer component,
the `vp_*` console commands and the `IVideoPlugin::PlayVideoTo*` functions were removed in M6b without aliases (levels and
flow graphs that use them must be rebuilt with the nodes below). Licence:
Boost Software License 1.0, see [LICENSE](LICENSE). Third-party libraries (libvpx, libwebm, libogg, libvorbis, libopus,
SDL2 from the engine) keep their own licences, see `Code/SDKs/CEVPlayer/*/`.

Status: **stage 1**, see [Limitations](#limitations-of-stage-1). Project tracking: [docs/PLAN.md](docs/PLAN.md).

## Features

Runtime (`Code/CryPlugins/CEVPlayer`)

* WebM playback without Libav / FFmpeg: libwebm demuxer, libvpx (VP8 / VP9, with alpha), libvorbis / libopus.
* Players on screen (fullscreen, letterbox / fill), on a material slot or in a named **surface** that UIs show by its
  surface id, several at a time, shared decode pool.
* Three audio routes per video (`.cevvideo` descriptor): `embedded` (audio track inside the WebM, SDL audio device),
  `mixed` (external audio WebM files per audio track) and `cue` (a CryAudio trigger plays the sound, the picture follows
  its playback position; needs the optional engine patch for exact sync, otherwise it falls back to a clock).
* Named audio tracks, markers, A/V offset and variants (resolution steps) in the descriptor. Tracks are chosen by
  name (`SetAudioTrack`); a track may carry an optional `language` tag, and following the game language is opt-in:

  ```xml
  <CEVVideo version="1" route="mixed" video="intro.webm">
    <AudioTracks default="dialogue" followGameLanguage="0">
      <Track name="dialogue" embedded="1" language="english"/>
      <Track name="commentary" file="intro_commentary.webm"/>
      <Track name="music" trigger="Play_Intro_Music" pauseTrigger="Pause_Intro_Music" resumeTrigger="Resume_Intro_Music"/>
    </AudioTracks>
  </CEVVideo>
  ```

  Choice: the track set by `SetAudioTrack`, then - only with `followGameLanguage="1"` - the track whose `language`
  matches `g_languageAudio` / `g_language`, then `default`, then the first track; without `<AudioTracks>` the embedded
  audio plays. Track names: 1 to 63 of `A-Z a-z 0-9 _ . -`, case insensitive, unique. The pre-A1 `<Languages>` element
  is not read (an error in the log; the video plays its embedded audio).
* FlowGraph nodes in one top-level folder `CEVPlayer` (PlayOnScreen, PlayOnMaterial) with the sub-folders `UI`
  (PlayToSurface, SurfaceInfo, SurfaceEvents) and `Advanced` (Open, Transport, Settings, Info, Events); a Schematyc component `Video Player`; a
  script API table (`Module/Bindings/ApiTable.cpp`, one function per entry); the C++ player API (`IVideoPlugin`,
  `IVideoPlayer`) and the texture source contract export `CryTexSource_v1` (docs/design/M6-ui-source.md).

Editor (`Code/Sandbox/Plugins/EditorCEVPlayer`)

* Asset type **Video** (`<name>.cevvideo` + `<name>.cevvideo.cryasset`), rename / move / delete handle every file of the
  asset, the SDL_mixer copies and the ACE library.
* Importer for `.webm` (drop on the Asset Browser or on the tool window, or console): options dialog, source copied into
  Assets, SHA-256 of the source, "source updated, reimport needed" detection.
* Tool window **Tools > CEV Video Editor**: asset browser, preview (checkerboard, alpha, play / pause / scrub / frame
  step / loop / timecode, audio mode), info panel (streams, files, descriptor editing with audio tracks and markers), job list.
* Operations (background jobs, cancellable): split audio to WebM, strip audio, export WAV (sample 0 = video second 0),
  export Ogg Vorbis, upgrade to the cue route (SDL_mixer copy + Audio Controls Editor library with Play / Pause / Resume
  triggers per audio track).

## Repository layout

| Path | Content |
|---|---|
| `Code/CryPlugins/CEVPlayer/` | runtime plug-in: `Module/` (engine glue), `Core/` (engine independent decoder, `Core/offline/` = CEVPlayerOffline for the editor), `Interface/` (public headers) |
| `Code/Sandbox/Plugins/EditorCEVPlayer/` | Sandbox plug-in (asset type, importer, tool window, jobs, ACE writer) |
| `Code/SDKs/CEVPlayer/` | prebuilt third-party libraries (Win64, MSVC 2022) |
| `cmake/modules/` | CMake find modules for those libraries (copied into `Tools/CMake/modules/`) |
| `patches/` | the engine patch series (plug-in registration) and optional patches (CryAudio trigger position) |
| `tools/` | sync / patch / build / smoke test / packaging scripts |
| `docs/` | design notes (`design/`), research (`research/`), manual acceptance list (`manual-acceptance-M4.md`) |

## Build

Supported engine trees: CRYENGINE 5.7.1 (`vanilla`, the reference) and the community fork CECE (only the CEVPlayer
targets are verified there). Windows, Visual Studio 2022, Git Bash. Nothing in this repository is built inside the
repository: the sources are copied into an engine tree.

```bash
# 1. copy plug-in sources, SDKs and CMake modules into the engine tree (one direction: edit here, sync there)
bash tools/sync_sources.sh /d/coding11/Cryengine_5.7.1

# 2. register the plug-ins in the engine's CMake files (idempotent; guarded with EXISTS)
bash tools/apply_patches.sh /d/coding11/Cryengine_5.7.1
#    optional: playback position of a CryAudio trigger for the cue route (SDL_mixer implementation)
bash tools/apply_patches.sh /d/coding11/Cryengine_5.7.1 --with-optional

# 3. configure (once, and again whenever a CMakeLists.txt changed)
cd /d/coding11/Cryengine_5.7.1
Tools/CMake/Win32/bin/cmake.exe -S . -B win64 -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE=Tools/CMake/toolchain/windows/WindowsPC-MSVC.cmake \
  -DOPTION_ENGINE=ON -DOPTION_SANDBOX=ON -DOPTION_CRYMONO=OFF

# 4. build the two plug-ins (Profile); the wrapper adds the MFC / STL compatibility props this machine needs
cd /d/coding11/CEVPlayer
CRYENGINE_SOURCE=/d/coding11/Cryengine_5.7.1 LOGTAG=A bash tools/build_baseline.sh Profile \
  'CRYENGINE\Sandbox\Plugins\EditorCEVPlayer' 'Project\Plugins\CEVPlayer'
```

Expected: 0 errors, 0 warnings. `tools/deploy.sh <built bin> <installed bin>` copies the DLLs into an installed engine
(with backups). Details and the CECE variant: [docs/research/02-baseline-build.md](docs/research/02-baseline-build.md),
[patches/README.md](patches/README.md). Rebuilding the third-party libraries: `tools/build_sdks.sh`.

Use in a project: add the runtime plug-in to the `.cryproject`

```json
{ "type": "EPluginType::Native", "path": "CEVPlayer" }
```

The Sandbox plug-in is loaded from `bin/win_x64/EditorPlugins/EditorCEVPlayer.dll` automatically. Without the runtime
plug-in the tool window still imports assets but cannot preview.

## Console variables and commands

Runtime plug-in

| Name | Meaning |
|---|---|
| `cev_play <path> [loop]` / `cev_stop` / `cev_pause` / `cev_resume` / `cev_seek <s>` | play and control a fullscreen test video (`.webm` or `.cevvideo`) |
| `cev_audiotrack <handle\|surface> [name]` | choose the audio track of a player (script handle or surface id) by its descriptor name; without a name: list the tracks and the one in use |
| `cev_list` | list all players (state, time, uploads) |
| `cev_play_surface <id> <path> [loop]`, `cev_surfaces` | play into a surface player; list the surfaces of the texture source contract |
| `cev_info` | interface version, capability bits, contract version, number of surfaces and players |
| `cev_apitest [path]`, `cev_flowtest [pathA] [pathB]`, `cev_contract_selftest` | self tests of the API table, the FlowGraph nodes and the texture source contract |
| `cev_run_script <file>` | development: runs console lines one per frame, immediately (`wait <seconds>`, `#` comments) |
| `cev_stats` | 1 = log / draw decode statistics per player |
| `cev_log`, `cev_decodeThreads` | verbose log; decode worker threads (0 = automatic) |
| `cev_audio_enable`, `cev_muteOnFocusLoss`, `cev_disableEmbeddedAudio` | audio switches |
| `cev_variant` | variant of a `.cevvideo` (-1 = automatic) |
| `cev_cueForceFallback` | 1 = cue route ignores the optional engine patch |
| `cev_volume` | master volume of all videos |

Sandbox plug-in (automation and tests; the commands run on the main thread)

| Command | Meaning |
|---|---|
| `cev_editor_import <absSrc> <folder> [name:<asset>] [track:<audio track>] [wav] [ogg] [split\|strip] [cue\|mixed] [overwrite]` | import without UI. Refuses (log line, nothing touched) when an asset of that name exists, unless `overwrite` is given. `name=` also works when typed in the console; scripts need `name:` because the console treats `a=b` as a variable assignment |
| `cev_editor_open [<asset.cevvideo>]` | open the tool window (on that asset) |
| `cev_editor_close` | close the tool windows |
| `cev_editor_rename <asset.cevvideo> <newName>`, `cev_editor_move <asset.cevvideo> <folder>`, `cev_editor_delete <asset.cevvideo>` | asset manager operations (same code path as the Asset Browser) |
| `cev_editor_status` | one log line: viewports, running jobs, offline library |
| `cev_editor_script <file>` | run a list of console commands from the editor idle loop (`wait_seconds N`, `#` comments). The environment variable `CEV_EDITOR_SCRIPT=<file>` starts one automatically (`CEV_EDITOR_SCRIPT_DELAY` seconds after the main frame, default 8). Sandbox never runs the console's deferred queue, so `exec` / `wait_seconds` of a normal cfg do not work there |

## Asset workflow

1. **Import**: drop a `.webm` on the Asset Browser (or on the tool window, which imports into the folder of the open
   asset), or `cev_editor_import`. The options dialog asks for the asset name, the audio track name of the embedded audio (default `main`), audio
   route, trigger name, "split audio" (keep the embedded track / play a picture-only copy) and WAV / Ogg exports. If an
   asset of that name already exists you are asked before it is replaced (and sent back to the dialog if you decline).
2. The asset folder then holds `<name>.cevvideo` (runtime descriptor), `<name>.webm` (copy of the imported file, the video
   the descriptor plays), `<name>.cevvideo.cryasset` (editor metadata: source path, SHA-256, import settings, stream
   details) and the files the operations make (`<name>_<track>.webm|.wav|.ogg`, `<name>_silent.webm`). All files are
   flat in one folder and start with the asset name, so rename / move / delete take them along.
3. **Edit** in *Tools > CEV Video Editor*: preview, descriptor (route, A/V offset, audio tracks with Default track / Follow game language, triggers, markers; *Apply to
   descriptor*, then Save with Ctrl+S). If a background operation rewrites the descriptor while you have unsaved edits,
   your edits are merged with the new file (and you are told).
4. **Audio operations** run as background jobs; the Jobs list shows progress and a cancel button. Quitting the editor
   cancels and joins them. Files a cancelled or failed job created from nothing are removed again.
5. **Cue route**: *Upgrade to cue route* copies the WAV / Ogg to `audio/sdlmixer/assets/cev/<name>/`, writes
   `audio/sdlmixer/ace/cev_<name>.xml` (Play_ / Pause_ / Resume_ trigger per audio track, the Audio Controls Editor shows
   them) and switches the descriptor. These two locations are not Files of the asset; delete and rename keep them in step.
   Wwise / FMOD: the info panel lists the manual steps.
6. **Known engine behaviour**: the engine creates a Sound asset (`<file>.wav.cryasset`) for every `.wav` under Assets,
   also for the exports and the SDL_mixer copies. The video asset removes those on delete / rename / move; the engine
   creates new ones for the new names.

## Source file policy

The importer copies the source video into the asset folder; for a plain import that copy **is** the video the game plays.
After *Strip audio* (or later re-encodes) the descriptor plays another file and `<name>.webm` is only a source file. The
WAV / Ogg exports in the asset folder are editor side files too (the cue route plays its own copies under
`audio/sdlmixer`). What goes where:

* **Do not ship**: `*.cryasset`, `.cevpreview/`, and per asset the files the descriptor does not play (source copy after
  a strip, WAV / Ogg exports). The info panel marks them "not shipped".
* CRYENGINE 5.7.1 has no switch for "do not pack the source of an asset". `rc.exe` has `/exclude=` (wildcards) and
  `/exclude_listfile=` (exact paths relative to the source root). `tools/list_unshipped_cev_files.py <Assets>` writes the
  exact list from the `.cryasset` files and descriptors, `tools/pak_exclude_sources.txt` holds the wildcard patterns, and
  `tools/pak_cev_assets.sh <engine> <Assets> <out.pak>` runs rc.exe with both.
* **Also note**: the stock packaging job (`Tools/CryVersionSelector/rc_jobs/game_assets.xml`, used by the project
  release tool) only packs known extensions; `.cevvideo`, `.webm` and `.wav` are not among them. Pack the videos with
  `tools/pak_cev_assets.sh` (or add the same `<Job Zip=... Input="*.cevvideo;*.webm" Exclude=.../>` to your own copy of the
  job file) and ship the resulting pak next to the others.
* **Version control**: videos and WAV files are large binaries. Use Git LFS (or another large file store) for
  `Assets/**/*.webm`, `*.wav`, `*.ogg`, or keep the original sources outside the repository: the asset records the
  original path and hash, so *Reimport source* works from anywhere and the copy can be restored. Never commit
  `<name>.cevvideo.cryasset` without its folder (it lists the files by relative path).
  Example `.gitattributes`: `Assets/**/*.webm filter=lfs diff=lfs merge=lfs -text` (same for `*.wav`, `*.ogg`).

## Limitations of stage 1

* Input is **WebM only** (VP8 / VP9, Vorbis / Opus). Other containers, re-compression, resolution variants and
  sub-videos come with the FFmpeg stage (the import dialog shows those fields disabled).
* Rendering: fullscreen video is drawn through the engine's aux geometry (top layer, alpha blending with a thin dark
  edge), colour conversion is done on the CPU, no video is advanced during a loading screen. The preview in the tool window
  uses a Character Tool style `QViewport` and draws the video texture in two passes for premultiplied alpha. A shader
  based path needs an engine patch and is not part of this stage.
* Cue route: exact lip-sync needs the optional patch (`patches/optional/02-cryaudio-trigger-position.patch`, SDL_mixer
  implementation only); otherwise (and with Wwise / FMOD / Adx2 / PortAudio) it follows a clock. Seeking a cue video moves
  the picture only.
* The embedded-audio route needs a 48 kHz audio device (SDL); other rates are resampled.
* Editor: Win64 Sandbox only; ACE libraries are written for SDL_mixer only; WAV / Ogg copies for the cue route are made on
  the main thread (large files take a moment); re-importing the source also runs on the main thread; paths longer than
  260 characters are not supported (plain Win32 paths).
* Windows only; the CECE tree is checked for compiling the two plug-ins, not for running (its own Sandbox / launcher
  have start-up problems, see docs/research/03-smoke-test.md).

## Tests

* `tools/smoke_test.sh <engine-root> [--sandbox] [--exec "cmd;cmd"]` starts the launcher or Sandbox on a minimal project,
  runs commands, quits and reports PASS / FAIL ([docs/research/03-smoke-test.md](docs/research/03-smoke-test.md)).
  `tools/smoke_close_main.ps1` closes a Sandbox that has floating tool windows (the older watcher cannot).
* Sandbox scripts (`CEV_EDITOR_SCRIPT`) drive the editor plug-in; the manual checks for what a script cannot see are in
  [docs/manual-acceptance-M4.md](docs/manual-acceptance-M4.md).
* `Code/CryPlugins/CEVPlayer/Core/offline/tests` is the self test of the offline library (alignment <= 1 sample).

## Contact

Original plug-in: Ilya Chernetsov, chernecoff@gmail.com, [GitHub issues](https://github.com/afrostalin/CEVPlayer/issues).
