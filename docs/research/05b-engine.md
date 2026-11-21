# 05b · 階段 0 引擎內量測（S0-B，2026-11-19）

引擎樹 `E:/coding crycry/CRYENGINE_Source_CEVPlayer_XTFrame`（5.7.1 + CEVPlayer + CryXTUI v0.17.0，Profile，DX11）。
量測專案與結果：`E:/coding3/xt-bench/cev-s0/engine/`（`project/CEVS0.cryproject`、`run_launcher.py`、`analyze_sync.py`、`logs/`、`shots/`、`csv/`）。
GameLauncher 一律以 Popen 啟動後 `SetProcessAffinityMask(0xFFF)`，視窗 1280×720，無關卡。

## 加入的量測碼（只供量測，未改播放路徑）

- Core `Stats`：`copyMs`（WriteFrame）、`colorMs`（ConvertSlot）；`convertMs` 仍為兩者和（語意不變）。
- `CVideoPlayerImpl::UploadFrame`：計時 `RenderBridge::UpdateTexture`（主執行緒），`TakeUploadTiming`、`LastUploadBytes`。
- `cev_stats` 行加 copy／color／uploadMs；`cev_stats_file <path>|off`：每秒每播放器一列 CSV，含行程 WS／private、DXGI `QueryVideoMemoryInfo`（adapter 0，本行程 local）。開檔時寫一列 player=0 的基準。
- 開發指令：`cev_probe_r8 <w> <h>`、`cev_probe_sync 1|0`、`cev_bench_play <path> <texture> [loop]`、`cev_bench_stop`（`Console.cpp` 的 `DevProbe`；每幀由 `CVideoPlugin::UpdateBeforeRender/AfterRender` 呼叫，關機在 `ShutdownBridge`）。
- 沒有新增原始檔（避免重新產生方案）。

## 1. `eTF_R8` 動態貼圖：可用

讀碼（推論）：`UploadToVideoMemory` 的 2D 同步路徑走 `CTexture::GetOrCreate2DTexture(name, w, h, mips, flags, nullptr, eSrcFormat)`——**`eTFDst` 參數在此路徑被忽略**，目的格式＝來源格式；`DeviceFormats` 有 `eTF_R8 → DXGI_FORMAT_R8_UNORM`，`IsInPlaceFormat` 含 R8；`RT_UpdateTextureRegion` 只檢查來源與目的 typeless 格式相同，列寬以 `TextureDataSize(USize,1,…, m_eDstFormat)` 算（緊密、1 B/px）。非渲染執行緒呼叫時先整塊複製進 render command（`LevelLoadingThread_defer`，video thread 啟用時加 `MainThread_defer`）。

實測（`logs/r8.log`、`logs/sync.log`）：

| 尺寸 | 建立 | 物件 | 每幀 `UpdateTextureInVideoMemory(eTF_R8)` 主執行緒耗時 |
|---|---|---|---|
| 1920×1620 | id>0，src=dst=`R8`，mips 1，loaded | `xtui_textures` 列 `R8` | avg 0.35–0.41 ms，max 0.58 ms（3,110,400 B） |
| 3840×3240 | 同上 | 同上 | avg 1.53 ms，max 1.78 ms（12,441,600 B） |
| 1000×750（非 2 的冪、非 4 的倍數高） | 同上 | `$CevProbeR8 1000x750 R8` | avg 0.02 ms |

截圖：`shots/r8_1920x1620.png`、`r8_3840x3240.png`、`r8_1000x750.png`——aux（`AuxGeomTexturePS`）把 R8 顯示為**紅色通道**（G=B=0），圖樣與移動的亮條正確、每幀更新。
執行期沒有 assert／警告。每次結束都有一條 `ResFile.cpp:508 !m_Dir.size()` assert——**沒開探測的 smoke／apitest 也有**，是引擎自帶的關機 assert，與 R8 無關。
附帶觀察：`RemoveTexture` 後以同名立刻重建會拿回同一個 id（395）；建立後立刻讀 `GetDeviceDataSize()` 仍是舊值（裝置貼圖在渲染執行緒建立）。

## 2. 同一幀還是晚一幀：同一幀（Layer 模式）；Direct 模式下 `engine://` 圖完全不顯示

做法：`cev_probe_sync 1` 建 64×64 RGBA8，在 `UpdateBeforeRender`（與播放器上傳同一步）以 `UpdateTextureInVideoMemory` 填入編碼主幀序號（8 色循環）的純色；同步驟 aux 畫 A 塊、`UpdateAfterRender` aux 畫 B 塊、aux 以該貼圖畫 C 塊；測試 UI（`project/src/probe/probe.xt`，`xtframe-pack` 打包）`<img src="engine://vid">`，`xtui_bind_texture probe vid $CevProbeSync 1`；`xtui_screenshot`（無損 back buffer）後以 `analyze_sync.py` 取各塊顏色。專案外掛順序 CryDefaultEntities, CEVPlayer, CryXTUI。

| 條件 | 截圖數 | B=A | C=A | UI=A |
|---|---|---|---|---|
| 無上限 FPS（~8000 fps），`xtui_direct 0` | 6 | 6 | 6 | **6** |
| `sys_MaxFPS 60`，`xtui_direct 0`，截圖間隔 1–3 幀 | 30 | 30 | 30 | **30** |
| `xtui_direct 1`（預設） | 6+12+20 | 全部 | 全部 | **0：img 不畫（顯示其後的灰底）** |
| `xtui_gpu 0`（SW blit 路徑） | 6 | 6 | 6 | 0：img 不畫 |

截圖：`shots/var_direct0_1.png`（四塊同色）、`var_direct1_1.png`、`sync_c_03.png`（Direct：灰框內無圖）、`var_gpu0_1.png`。
結論：CEVPlayer 在 `UpdateBeforeRender` 送出的貼圖更新與 CryXTUI Layer 模式的 UI 繪製在**同一個顯示幀**生效；aux 在 BeforeRender 與 AfterRender 畫的也在同一幀。
**v0.17.0 預設的 Direct target 模式下，綁定的引擎貼圖根本沒有出現在畫面上**（`xtui_list` 顯示 reused 21256/21258：UI 幾乎每幀重用快取帶）——這是消費端（CryXTUI）的問題，與 T-746 外部貼圖內容版本有關；本單未查其原因（不在本倉）。另一顆外掛順序（CryXTUI 在前）與載入期 video thread（`MainThread_defer`）未測。

附帶：surface 開著直接 `quit`，關機時 0xC0000005（`logs/sync.log`、`quit_b`、`quit_c`；`quit_b` 開了 surface 但未綁貼圖也會）；先 `xtui_close` 再 quit 則乾淨（`sync2`、`variants`）。只開 surface 不開探測的兩次（`quit_a`、`quit_a2`）卡在第一個 `wait_seconds` 沒有進展（一次逾時被殺、一次 0xCFFFFFFF 結束），**原因未查**——未能確認崩潰是否與 CEVPlayer 無關。

## 3. 引擎內基準

`cev_bench_play Videos/<f>.webm $CevBench 1`（具名貼圖、非全螢幕、不顯示），`sys_MaxFPS 60`，循環播放 21 s（素材 10 s，S0-A 產生），`cev_stats_file`。表為 wall>2.5 s 各列平均／各列 p99 最大；記憶體為對基準列的最大增量（`csv/*.csv`、`logs/bench_*.log`）。

| 素材 | 解碼 ms | 複製 ms | 轉色 ms | 上傳 ms（主執行緒） | 上傳/s | B/幀 | dropped | ΔWS MB | Δprivate MB | ΔGPU MB |
|---|---|---|---|---|---|---|---|---|---|---|
| 1080p30 | 1.59 / 2.64 | 0.35 / 0.72 | 4.28 / 5.61 | 1.63 / 3.02 | 30 | 8,294,400 | 0 | 110 | 118 | 3 |
| 1080p30 alpha | 2.42 / 3.96 | 0.68 / 1.27 | 5.37 / 9.32 | 1.66 / 4.19 | 30 | 8,294,400 | 0 | 141 | 144 | 3 |
| 1080p60 | 1.62 / 2.74 | 0.38 / 0.93 | 4.37 / 8.22 | 1.83 / 3.63 | 60 | 8,294,400 | 25 | 139 | 142 | 3 |
| 1080p60 alpha | 2.55 / 5.02 | 0.78 / 2.13 | 5.46 / 9.05 | 1.97 / 5.41 | 60 | 8,294,400 | 17 | 153 | 162 | 3 |
| 2160p30 | 7.24 / 12.06 | 2.43 / 4.85 | 17.17 / 27.11 | 8.09 / 14.40 | 30 | 33,177,600 | 0 | 420 | 447 | 27 |
| 2160p30 alpha | 10.18 / 17.60 | 3.66 / 7.84 | 21.61 / 36.79 | 7.68 / 16.14 | **21–28** | 33,177,600 | 0 | 573 | 607 | 27 |

- 主執行緒上傳的成本與位元組成正比（約 0.2 ms/MB）：2160p 每幀 8 ms，60 fps 遊戲裡佔掉半幀。對照 R8 打包平面同尺寸（3840×3240＝12.4 MB）1.5 ms。
- 2160p30 alpha 跟不上（ring 平均 1.0、avOffset 約 1 s，上傳 21–28/s）。
- 1080p60 有 dropped（17–25，主要在起播與循環點）。
- GPU 記憶體：DXGI 的本行程 local usage 只增加 3／27 MB（約為 RGBA 貼圖 8.3／33 MB 的量級；計數方式未對照驅動工具，僅供參考）。RAM 增量遠大於 §2 推算的 ring 46–54 MB（1080p 110–160 MB；含解碼器、demux 緩衝、render command 複本等，未拆分）。
- 量測期間 FPS 上限 60；解碼執行緒數為自動。

## 4. 既有自測

- `cev_apitest`（預設 `Videos/vp9_alpha.webm`）與 `cev_apitest Videos/vp9_1080p30_alpha.webm`：**PASS（33 checks, 0 failed）**（`logs/apitest.log`）。
- `tools/smoke_test.sh <engine> --project …/CEVS0.cryproject --exec "cev_play Videos/vp8_vorbis.webm;wait_seconds 3;cev_list;cev_stop" --expect "player 1 ready"`：**PASS**（`logs/smoke/Game.log`）。測試專案 `D:/coding11/_testproject/CEVTest` 不存在，另建 `CEVS0`（CryDefaultEntities、CEVPlayer、CryXTUI；`tools/make_test_media.sh` 產生的小素材）。smoke 腳本本身不釘核（只是功能測試）。

## 對設計文件的修正建議

1. §5.2／§8：`eTF_R8` 已驗可用（含 3840×3240、非 2 的冪），移出風險。另記：`UploadToVideoMemory` 同步路徑忽略 `eTFDst`；同名重建會重用 id，`generation` 不能只靠 id 判斷。
2. §8「是否同一幀」：改為「Layer 模式實測同一幀（36/36）」；新增風險：**CryXTUI v0.17.0 的 Direct target 模式（預設）不顯示 `engine://` 綁定的貼圖**，SW 路徑也不顯示——消費端（CE-86／T-746）需處理，否則 M6 驗收「UI 寫 ID 就出畫面」在預設設定下不會過。
3. §2 資料路徑：把「未量」換成本表；ring 推算 46–54 MB 與實測 RAM 增量不符（1080p 110–160 MB、2160p 420–600 MB），建議 M8 前拆分量測。
4. §5／§6 的優先序：2160p 主執行緒上傳 8 ms、轉色 17–22 ms 是最大成本；打包平面（M8）可把上傳降到約 1/2.7、省掉轉色——支持 M8 先於 M9。2160p alpha 目前即時播放不成立，應在文件中註明上限。
5. §5.5：`cev_stats` 量測欄位與 `cev_stats_file` 已有雛形（本分支），M7 可沿用欄位名。
