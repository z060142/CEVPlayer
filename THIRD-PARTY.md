# Third-party components

This file lists the third-party code that this repository contains or that the editor plug-in downloads. It records
versions, sources and where the licence texts are; it does not draw legal conclusions. CEVPlayer itself is under the
Boost Software License 1.0 ([LICENSE](LICENSE)).

## Prebuilt libraries in this repository (`Code/SDKs/CEVPlayer/`)

Static libraries for Windows x64, MSVC 2022 (v143), `/MD` (Release and Profile) and `/MDd` (Debug), linked into the
runtime plug-in (and through `CEVPlayerCore` into the editor plug-in). Each folder holds `include/`, `lib/win64/`, a
`VERSION` file (version, source URL, SHA-256 of the source archive, build flags) and the upstream licence files. All five
were built with `tools/build_sdks.sh` from the source archives listed below.

| Library | Version | Source archive (from `VERSION`) | Built with | Licence files in the tree |
|---|---|---|---|---|
| libvpx | 1.17.0 (tag v1.17.0) | `https://github.com/webmproject/libvpx/archive/refs/tags/v1.17.0.tar.gz` | vcpkg overlay port (vcpkg commit 8f363bdc), triplet x64-windows-static-md; decoders only (`--disable-vp8-encoder --disable-vp9-encoder`), NASM assembly | `Code/SDKs/CEVPlayer/libvpx/LICENSE`, `LICENSE-x86inc`, `PATENTS` |
| libwebm | 1.0.0.32 (tag libwebm-1.0.0.32) | `https://github.com/webmproject/libwebm/archive/refs/tags/libwebm-1.0.0.32.tar.gz` | CMake, `-DMSVC_RUNTIME=dll` (mkvparser + mkvmuxer + common) | `Code/SDKs/CEVPlayer/libwebm/LICENSE`, `PATENTS` |
| libogg | 1.3.6 | `https://downloads.xiph.org/releases/ogg/libogg-1.3.6.tar.xz` | CMake, static | `Code/SDKs/CEVPlayer/libogg/LICENSE` |
| libvorbis | 1.3.7 | `https://downloads.xiph.org/releases/vorbis/libvorbis-1.3.7.tar.xz` | CMake, static, against the libogg above | `Code/SDKs/CEVPlayer/libvorbis/LICENSE` |
| libopus | 1.6.1 | `https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz` | CMake, static, default x86 intrinsics with run-time CPU detection | `Code/SDKs/CEVPlayer/libopus/LICENSE` |

The exact SHA-256 values and compiler flags are in each `VERSION` file; `Code/SDKs/CEVPlayer/README.md` describes the
folder layout.

## Components used from the engine tree (not in this repository)

* **SDL2** (`Code/SDKs/SDL2` of the engine): the audio device of the `embedded` / `mixed` routes.
* The editor plug-in uses the engine's **Qt** (Sandbox) for the preview widget and its audio output.

These are part of the CRYENGINE source distribution and covered by its terms.

## Minimal FFmpeg (downloaded separately, not in this repository)

The editor plug-in can download a minimal FFmpeg build for transcoding on import. It is **not** linked into either
plug-in: the editor starts `ffmpeg.exe` as an external process. Any other `ffmpeg.exe` (configured in the preferences or
found on `PATH`) can be used instead; at run time no FFmpeg is used.

* Release: tag `ffmpeg-minimal-v8.0.3-cev1` of this repository's GitHub releases, asset
  `ffmpeg-minimal-8.0.3-cev1-win64.zip`, SHA-256 `b89f1dee75a7f19ed2fbcc80f72d718fe415515399bc7199a1a1e88cded64033`
  (pinned in `Code/CryPlugins/CEVPlayer/Core/offline/include/CEVOffline/FfmpegInstall.h`; the zip is verified before it
  is unpacked into `%LOCALAPPDATA%\CEVPlayer\ffmpeg\8.0.3-cev1`).
* Built with `tools/ffmpeg/build_minimal.sh` in MSYS2 MINGW64 from the source archives pinned (URL and SHA-256) in
  `tools/ffmpeg/versions.env`: FFmpeg 8.0.3, libvpx 1.15.0, opus 1.5.2, libogg 1.3.5, libvorbis 1.3.7, dav1d 1.5.3,
  zlib 1.4, all linked statically into `ffmpeg.exe`.
* Configuration: `--disable-everything` with an explicit list of demuxers (mov/mp4, matroska/webm, avi, image2),
  decoders (h264, hevc, prores, vp8, vp9, AV1 through dav1d, png, aac, mp3, opus, vorbis, flac, pcm), encoders
  (libvpx-vp9, libopus, libvorbis, pcm_s16le), muxers (webm, ogg, wav), filters (scale, format, aresample and the
  framework filters ffmpeg selects), parsers, bitstream filters and the file / pipe protocols; `--disable-gpl
  --disable-nonfree --disable-version3`. The build script refuses to package when `ffmpeg -version` reports gpl,
  nonfree or version3. The full list is in `docs/design/M5-ffmpeg.md` §7.
* Contents of the zip (flat): `ffmpeg.exe`, `LICENSE` (LGPL 2.1), `BUILD.txt` (source URLs and SHA-256, tool chain,
  configure arguments), the build script and `versions.env`, and `licenses/` with `FFmpeg-COPYING.LGPLv2.1`,
  `FFmpeg-LICENSE.md`, `libvpx-LICENSE`, `libvpx-PATENTS`, `opus-COPYING`, `libogg-COPYING`, `libvorbis-COPYING`,
  `dav1d-COPYING`, `zlib-LICENSE`.

## Test media

`tools/make_test_media.sh` generates synthetic clips with an `ffmpeg` the developer supplies; no third-party media is
stored in the repository.
