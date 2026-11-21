# CEVPlayer third-party SDKs

Prebuilt libraries used only by CEVPlayer, grouped in this parent folder and kept apart from the engine's own `Code/SDKs/*`.

| Folder | Family | Purpose |
|---|---|---|
| `libvpx/` | WebM Project | VP8 / VP9 video decoding (decoders only) |
| `libwebm/` | WebM Project | WebM (Matroska) container parsing |
| `libogg/` | Xiph.org | Ogg packets (required by libvorbis) |
| `libvorbis/` | Xiph.org | Vorbis audio decoding |
| `libopus/` | Xiph.org | Opus audio decoding |

- Each folder contains `include/`, `lib/win64/release/` (Profile and Release, `/MD`), `lib/win64/debug/` (`/MDd`), `VERSION` (version, source URL, SHA-256, build flags) and the license files.
- Rebuild: `bash tools/build_sdks.sh` (writes into this folder).
- Engine integration: `tools/sync_sources.sh <engine-root>` copies this whole folder to `<engine>/Code/SDKs/CEVPlayer/`; the CMake modules `Tools/CMake/modules/<lib>.cmake` locate each library at `${SDK_DIR}/CEVPlayer/<lib>`.
- SDL2 is not included here: the engine's own `Code/SDKs/SDL2` is used.
