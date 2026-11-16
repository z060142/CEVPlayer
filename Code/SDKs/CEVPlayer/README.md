# CEVPlayer 第三方 SDK

CEVPlayer 專用的預編譯函式庫，集中在這個母資料夾，與引擎原有的 `Code/SDKs/*` 分開。

| 資料夾 | 系列 | 用途 |
|---|---|---|
| `libvpx/` | WebM 專案 | VP8 / VP9 影像解碼（只含解碼器） |
| `libwebm/` | WebM 專案 | WebM（Matroska）容器解析 |
| `libogg/` | Xiph.org | Ogg 封包（libvorbis 需要） |
| `libvorbis/` | Xiph.org | Vorbis 聲音解碼 |
| `libopus/` | Xiph.org | Opus 聲音解碼 |

- 每個資料夾：`include/`、`lib/win64/release/`（Profile 與 Release，/MD）、`lib/win64/debug/`（/MDd）、`VERSION`（版本、來源 URL、SHA-256、編譯旗標）、授權檔。
- 重建：`bash tools/build_sdks.sh`（輸出到這裡）。
- 引入引擎：`tools/sync_sources.sh <engine-root>` 把整個資料夾複製到 `<engine>/Code/SDKs/CEVPlayer/`，CMake 模組 `Tools/CMake/modules/<lib>.cmake` 以 `${SDK_DIR}/CEVPlayer/<lib>` 定位。
- SDL2 不在這裡：沿用引擎自帶的 `Code/SDKs/SDL2`。
