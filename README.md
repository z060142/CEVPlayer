# CEVPlayer

Video playback for CRYENGINE 5.7 games: full screen, on materials, and inside UI layouts. Videos can be transparent.

## What it does

* Plays WebM (VP8 / VP9 video, Vorbis / Opus audio) with sound in sync, also during level loading screens.
* Shows video full screen, on a material in the level, or as a named texture that a UI system displays.
* Is driven by FlowGraph nodes, a Schematyc component, CryVS functions or C++.
* Adds a Video asset type to Sandbox: import (other formats are converted with FFmpeg), preview and edit.

## Requirements

* Windows 10 / 11 x64 and Visual Studio 2022 with the C++ desktop workload (plus ATL and MFC for the Sandbox plug-in).
* The source of CRYENGINE 5.7.1 or of the [Community Edition](https://github.com/z060142/CryENGINE-5-7-community).

There are no prebuilt plug-in binaries; the third-party libraries are included prebuilt.

## Build

```bash
bash tools/setup_engine.sh /path/to/engine --xui2dl
```

This copies the plug-in into the engine tree, patches and builds it. Add `--dry-run` first to see what it would do.

## Use

Add the plug-in to your `.cryproject`:

```json
{ "type": "EPluginType::Native", "path": "CEVPlayer" }
```

Then place a `CEVPlayer:PlayOnScreen` or `CEVPlayer:PlayOnMaterial` node in a flow graph and pick a `.webm` file.

## Documentation

* [MANUAL.md](MANUAL.md) - everything: features, nodes, console commands, asset workflow, build details, limitations.
* [UI-SOURCE.md](UI-SOURCE.md) - for UI plug-in authors: how to show a CEVPlayer video in your own UI system.
* [CHANGELOG.md](CHANGELOG.md) - changes, including what broke since the old version.

## Licence and credits

Boost Software License 1.0, see [LICENSE](LICENSE); third-party components are listed in
[THIRD-PARTY.md](THIRD-PARTY.md).

Created by Ilya Chernetsov ([afrostalin/CEVPlayer](https://github.com/afrostalin/CEVPlayer)); this version is a rewrite
maintained by [z060142](https://github.com/z060142), so please send questions here and not to him.
