# M1 執行期核心 — 設計（給實作 session）

依據：需求單「執行期架構」「沿用 XTFrame 的設計與優化」「相容規則」，以及 `docs/research/01-open-questions.md` 的結論。
本輪的驗收底線：兩棵樹（官方 5.7.1 為主線；CECE 若能編也要編）可編譯、GameLauncher / Sandbox 載入插件與播放不崩潰。

## 分層與目錄

```
Code/CryPlugins/CEVPlayer/
  Interface/CEVPlayer/        公開標頭，給直接連結 CEVPlayer 的遊戲程式（其他插件以 docs/design/M6-ui-source.md §3 的契約對接）
    IVideoPlugin.h            舊接口，保留（相容層）
    IVideoPluginListeners.h   舊接口，保留
    IVideoPlayer.h            新：核心 API（播放器控制、查詢、事件、能力與版本）
  Core/                       CEVPlayerCore 靜態庫，不 include 任何 CE 標頭
    CMakeLists.txt            可由 CE 樹內 add_subdirectory，也可單獨用一般 CMake 建（給自測）
    include/CEVCore/...       Status、IByteSource、IClock、PlayerCore、FrameRing、Stats ...
    src/...
    tests/cevcore_selftest.cpp  獨立自測程式（一般 CMake，不經 CE）
  Module/                     CE 橋接（CEVPlayer.dll）
```

## CEVPlayerCore（解碼層）

規則（照 XTFrame 14 §14.3/§14.4）：公開函式回傳 `Status`（enum class + 可選訊息碼），不拋例外、不印訊息；不用裸 new/delete（`std::unique_ptr` / `std::make_unique`）；不用 `std::map` / `unordered_map`；穩定播放時配置次數 = 0。

元件：
- `IByteSource`：`Read(offset, dst, size)`、`Length()`。CE 端以 CryPak 實作；自測端以 stdio 實作。可整檔預載。
- `WebmDemuxer`：libwebm mkvparser；列出影像軌（VP8/VP9、寬高、幀率、alpha 標記 = BlockAdditional ID 1 / `AlphaMode`）、音軌（Vorbis/Opus、CodecPrivate、CodecDelay、SeekPreRoll、起始時間）；依 cue 做 seek。
- `VideoDecoder`：libvpx VP8/VP9，可設執行緒數；alpha 用第二個解碼器解 BlockAdditional。輸出寫進 `FrameRing` 的預配置槽，不另配記憶體。**寬高不裁成 4 的倍數**（處理奇數寬高與 stride）。
- `AudioDecoder`：Vorbis（libvorbis 封包 API）與 Opus（libopus），輸出 float interleaved PCM，處理 pre-skip / codec delay / 起始時間，讓第 0 個取樣對齊影片 0 秒。M1 只需解得出且不崩潰；輸出裝置在 M2。
- `FrameRing`：固定 N 槽（預設 4），每槽 Y/U/V/(A) 平面 + pts；Load 時一次配好。生產者 = 解碼 worker，消費者 = 主執行緒。無鎖 SPSC 或短鎖皆可，但不得在持鎖時解碼。
- `DecodePool`：取代舊的「單一執行緒每 1 ms 輪詢」。固定數量 worker，`condition_variable` 有工作才喚醒；每個播放器一個解碼工作，環形緩衝滿了就讓出。
- `IClock`：`double NowSeconds()` + `Start/Pause/Resume/Seek/SetRate`。M1 只實作 `InternalClock`（高精度計時器 × rate）。之後 SDL 已播取樣數、CryAudio trigger 位置各是一種實作。
- `PlayerCore`：狀態機 Stopped / Playing / Paused / Ended（對應 XTFrame `MediaState`），Load（非同步：背景解出第一幀後才變 Ready）、Play、Pause、Resume、Stop、Seek、SetLoop、SetRate。`Update()` 由主執行緒呼叫：問時鐘 → 從 ring 取「該呈現的幀」；落後超過一幀就丟幀（計入 framesDropped）；超前就保留目前幀。回傳「是否有新幀」。
- `Stats`：每幀一筆（decodeMs、convertMs、uploadBytes、framesDropped、ring 占用、allocs），240 筆環形，avg / max / p99；關閉時每處一個分支。

自測（`tests/cevcore_selftest.cpp`）：以 `tools/make_test_media.sh` 產生的片子跑：VP9 1080p、VP9 alpha、VP8+Vorbis、VP9+Opus。驗證：解出幀數 = 片子幀數、alpha 片有 A 平面、穩定播放期間配置次數 = 0（測試程式覆寫 `operator new` 計數）、seek / loop / 結束狀態正確、無崩潰。

## CE 橋接（Module/）

- **更新點**：`EnableUpdate(EUpdateStep::BeforeRender, true)` → `UpdateBeforeRender()`（同 XTFrame）。Sandbox 編輯模式另以 `eNotify_OnIdleUpdate` 或同等機制推進（若做不到，M1 允許只在遊戲模式推進）。不用 `OnPostUpdate`。
- **關卡載入**：`ESYSTEM_EVENT_LEVEL_LOAD_START_LOADINGSCREEN` → 所有播放器的時鐘暫停、不呼叫任何渲染器函式；`LEVEL_LOAD_END` / `LEVEL_GAMEPLAY_START` → 恢復並重對時間。載入期間真正推進畫面要靠第二階段 patch（研究 Q2），M1 只保證不當機。
- **渲染橋（第一階段，無引擎 patch）**：
  - 每個播放器一張 RGBA 貼圖：`IRenderer::UploadToVideoMemory(nullptr, w, h, eTF_R8G8B8A8, eTF_R8G8B8A8, 0, false, FILTER_BILINEAR, 0, "$name", FT_NOMIPS)`（名稱照給的保留，`$` 開頭可被材質與 Flash UI 引用）。建立與釋放都在主執行緒、不在關卡載入期間。
  - 轉色在解碼 worker 上以 CPU 做 YUV(A)→RGBA **預乘 alpha**（BT.601 / BT.709 依檔案標記，limited range），寫進預配置的 RGBA 槽；主執行緒只在 `PlayerCore::Update()` 回報新幀時呼叫一次 `UpdateTextureInVideoMemory`。暫停、結束、沒換幀 → 0 次上傳。
  - 探測：`GetRenderType()` → 渲染器 DLL 名 → `GetModuleHandle` + `GetProcAddress("CEVPlayer_Patched")`。M1 沒有 patch，必然走第一階段，log 一次。之後第二階段的 GPU 轉色走這個開關。
- **使用端**：
  - 全螢幕：第一階段只能用 `IRenderAuxImage` / aux 2D quad 畫在最上層，支援填滿 / 等比 / 黑邊（letterbox）。這是需求單「不用 Aux」的已知例外，第二階段以自己的 pass 取代；全部 aux 呼叫集中在一個檔案（`Module/Render/FullscreenOutput*.cpp`），方便第二階段替換。
  - 材質欄位：把貼圖綁到指定材質的指定貼圖槽（`IMaterial` → `SShaderItem` → `SEfResTexture`）。
  - 具名貼圖：以 `$name` 註冊（見上）。
- **查詢接口**（`Interface/CEVPlayer/IVideoPlayer.h`）：介面版本 + 能力位元（alpha、premultiplied、多解析度）；每個播放器可查：貼圖 ID/ITexture*、寬高、hasAlpha、premultiplied（恆 true）、狀態（MediaState 對應）、目前時間、總長、「新幀就緒」事件（listener 介面）、控制（Play/Pause/Seek/Loop/Rate）。不得出現 XTFrame 專用程式碼。
- **相容**：舊的 `IVideoPlugin`（PlayVideoToMainWindow / PlayVideoToTexture / Pause / Resume / Stop）、FlowGraph 節點、`TextureVideoPlayerComponent` 在 M1 改成新播放器上的薄包裝，名稱與介面不變，確保舊專案仍可編譯與播放。舊的 `Video/`、`Render/Frame*`、`Core/DecoderThread` 等被新核心取代後刪除。
- **聲音（M1）**：SDL2 輸出暫時沿用舊做法但修掉兩個崩潰風險：不在每個播放器呼叫 `SDL_Init`/`SDL_Quit`（改 `SDL_InitSubSystem(SDL_INIT_AUDIO)` / `SDL_QuitSubSystem` 全插件一次、延遲到第一個需要聲音的播放器），避免關掉引擎自己的 SDL 音訊。完整的時鐘對聲音、混音、Opus 輸出在 M2。
- **cvar**：`cev_stats 0/1`（效能計數，預設關）、`cev_decodeThreads`、`cev_log`。

## 驗收（M1，本輪）

1. `CEVPlayerCore` 自測全部通過（含配置次數 = 0）。
2. 官方 5.7.1 樹：套 patch 01 → sync → 建 `CEVPlayer` 與 GameLauncher / Sandbox，零錯誤。CECE 同上（若 CECE 本身能編）。
3. 程式碼審查（另一個 agent）通過：執行緒安全（ring、listener 派發只在主執行緒）、渲染呼叫都在主執行緒、載入期間無渲染呼叫、無裸 new/delete、無 std::map。
4. 冒煙測試：GameLauncher 以 Blank 專案載入插件、以 console 指令播放測試片數秒後退出，無崩潰、log 無錯誤（能做到就做；畫面正確性不驗證）。
