# M2 快速路線聲音與腳本綁定 — 設計（給實作 session）

依據：需求單「聲音 → 時鐘介面、快速路線」「資產與格式 → 執行期描述檔（只取執行期部分）」「腳本與 API」。
底線同 M1：官方版主線、CECE 驗 CEVPlayer 目標；不崩潰、不卡住；冒煙測試 PASS。
分兩段實作：**M2a 聲音**、**M2b 綁定**，依序進行（同一棵引擎樹建置，避免互相干擾）。

## M2a 聲音（快速路線 = embedded / mixed）

### 時鐘
- Core 已有 `IClock` 與 `PlayerConfig::externalClock`。新增 `AudioClock`：時間 = 已被 SDL 回呼取走的取樣數 ÷ 輸出取樣率 − 裝置延遲估計（`SDL_GetQueuedAudioSize` 不適用 callback 模式，延遲用 `obtained.samples / freq` 估），乘 rate 由重取樣處理。
- 播放器有聲音且聲音路徑可用 → 時鐘 = AudioClock；沒有聲音、`cev_audio_enable 0`、專案禁用快速路線聲音、或聲音裝置開不起來 → InternalClock。
- 暫停、跳轉、循環、rate 一律經時鐘來源轉發（AudioClock 的 Seek = 清空音訊 ring + 設定基準時間）。
- 畫面落後 > 1 幀丟幀、超前就等（Core 已實作）；影音偏差列入 stats（`avOffsetMs`）。

### 重取樣
- Core 新增 `Resampler`（無配置的串流式重取樣，品質至少線性內插，建議 4～8 tap 窗口 sinc 或三次內插），把任意來源取樣率轉成 48 kHz，並承擔 rate（0.5～2.0）。初始化時配好緩衝，穩定播放 0 配置。
- 聲道：1→2 複製；2 原樣；>2 降混到立體聲（標準係數）。

### 混音與遊戲狀態
- 全插件一個 SDL 裝置（M1 已有），多播放器在 callback 內混音，每播放器音量 × 主音量 cvar `cev_volume`（0～1）。
- 連動：遊戲暫停（`ESYSTEM_EVENT_GAME_PAUSED/RESUMED`）→ 暫停插件自動暫停的播放器；視窗失焦（`ESYSTEM_EVENT_CHANGE_FOCUS`）→ 依 `cev_muteOnFocusLoss`（預設 1）靜音；
- 專案層級開關 cvar `cev_disableEmbeddedAudio`（預設 0，可在 .cryproject 設）：開啟時帶內嵌音軌的影片靜音播放、記一次警告、**整段運行不初始化 SDL 音訊子系統**。

### 外部音訊與多語言（mixed）
- 執行期描述檔（資產主檔，隨遊戲發行）：與影片同名的 `<name>.cevvideo`（XML，用 CE 的 `gEnv->pSystem->LoadXmlFromFile`），M2 只讀，寫入在 M4 編輯器。格式：

```xml
<CEVVideo version="1" route="embedded|mixed|cue" video="intro.webm" avOffsetMs="0">
  <Languages default="english">
    <Language name="english" embedded="1"/>                 <!-- 用內嵌音軌 -->
    <Language name="japanese" file="intro.japanese.webm"/>   <!-- 外部只有音軌的 WebM -->
    <Language name="german" trigger="Play_Intro_DE"/>        <!-- cue 路線，M3 -->
  </Languages>
  <Variants rule="smallestNotBelowOutput">                 <!-- M4/M5 產生，M2 只讀並選用 -->
    <Variant file="intro_720.webm" width="1280" height="720"/>
  </Variants>
  <Markers>
    <Marker name="explosion" time="3.25"/>
  </Markers>
</CEVVideo>
```

- Load 接受 `.webm` 或 `.cevvideo`；直接給 `.webm` = 沒有描述檔的 embedded。
- 語言選擇：播放器 `SetLanguage(name)`；預設取 CE 的音訊語言 cvar（`g_languageAudio`，沒有就 `g_language`），找不到就用描述檔 default，再不行用內嵌軌。
- 外部音訊用第二個 `PlayerCore`（只解音訊）餵混音器；影片 PlayerCore 的時鐘 = 外部音訊的 AudioClock。外部檔只支援 WebM（Ogg 記為未來）。mixed 沒有外部檔時播內嵌軌。
- 標記點：到達時發 `OnMarker(name, time)` 事件（主執行緒派發，附影片時間戳）。
- route="cue" 在 M2 視為「沒有聲音、影片時鐘」並記警告（M3 實作）。

### 驗收（M2a）
- 44.1 kHz Vorbis（vp8_vorbis）有聲音；Opus 照常；`av_sync_flash_beep` 以 Core 自測驗證：AudioClock 下閃光幀呈現時間與嗶聲開始取樣的偏差 < 1 幀（以模擬的 callback 取樣推進計算，不需真聲卡）。
- `cev_disableEmbeddedAudio 1`：log 證明 SDL 音訊子系統未初始化。
- 外部語言檔：`audio_en.webm` / `audio_ja.webm` 搭配某支片的描述檔，切語言不崩潰。
- 穩定播放 0 配置（Core 自測含重取樣路徑）。

## M2b 綁定（描述表、FlowGraph、Schematyc、CryVS、相容層）

### 描述表（唯一真實來源）
- `Module/Bindings/ApiTable.h/.cpp`：每個控制、查詢、事件只描述一次 —— 固定 GUID（寫死在原始碼，一經發佈不改）、名稱、參數（型別 + 名稱）、回傳、是否非同步、說明、對應的一般 C++ 函式指標（無反射、無動態分派前置）。
- 控制：Load、Play、Pause、Resume、Stop、Seek、SetLoop、SetRate、SetLanguage、SetVolume、SetOutput（fullscreen / material slot / named texture）、SetVariant。
- 查詢：State、Time、Duration、Width、Height、TextureId、HasAlpha、IsPremultiplied、Variant、Language、InterfaceVersion、Caps。
- 事件：Ready、Started、NewFrame、Marker、Looped、Ended、Skipped、Error。
- 「被跳過」：可跳過的影片在玩家按鍵（沿用舊插件 InputDispatcher 的做法）時 Stop 並發 Skipped。

### FlowGraph
- 新節點一對一對應描述表（`CEVPlayer:Video:*`），從表自動註冊；節點以播放器 handle（int）串接。
- 舊節點（2DVideoPlayer / TextureVideoPlayer / FlashVideoPlayer）保留名稱與接口，UI 名稱標「(deprecated)」。

### Schematyc
- 新元件「Video Player」（新 GUID）：屬性 = 影片路徑（`.webm`/`.cevvideo`，用檔案選擇器）、輸出模式（螢幕 / 材質欄位 + slot / 具名貼圖）、語言、循環、自動播放、可否跳過。聲音路線由描述檔決定。
- 函式與 signal 從描述表註冊；掛在 `ESYSTEM_EVENT_REGISTER_SCHEMATYC_ENV`。舊 `TextureVideoPlayerComponent` 保留。

### CryVS（只在 CECE）
- CMake：`if(EXISTS "${CRYENGINE_DIR}/Code/CryEngine/CryCommon/CryVS")` → 定義 `CEV_HAVE_CRYVS=1` 並加入 `Module/Bindings/CryVSEnv.cpp`；官方版不編、不 include 任何 CryVS 標頭。
- 執行期：`gEnv->pCryVS` 為空就不註冊。依 `docs/research/01-open-questions.md` Q5：在 `ESYSTEM_EVENT_GAME_POST_INIT` 註冊，並接 `gEnv->pCryVS->Signals().envRefresh` 在每次 Refresh 後重註冊；元件 / 函式 / signal / 型別都從描述表產生。action 的結束回報機制尚待確認 → M2 只註冊同步函式與 signal，非同步 action 列為未完成。

### 驗收（M2b）
- 官方版與 CECE 的 CEVPlayer 目標 0 錯誤 0 警告；官方版建置不引用任何 CryVS 標頭（以 `/showIncludes` 或 grep 建置 log 驗證）。
- 冒煙：GameLauncher 載入後 log 顯示新 FlowGraph 節點與 Schematyc 元件註冊成功；Sandbox 啟動關閉 PASS。
- 描述表 GUID 重複檢查（啟動時 assert/log）。

## 2026-11-19 修訂（A1；音軌就是音軌，本節優先於上文的對應段落）

- 描述檔：`<Languages>`／`<Language>` 改為 `<AudioTracks default="<名字>" followGameLanguage="0|1">`，每條 `<Track name="…" embedded="1" | file="…" | trigger="…" [pauseTrigger= resumeTrigger=] [language="…"]>`。`name` 是作者自取的識別字（`[A-Za-z0-9_.-]`，1～63，不分大小寫，不重複；不合法或重複的條目略過並警告），`language` 是選用標記。舊的 `<Languages>` 不讀：記一則錯誤（指出改用 `<AudioTracks>`），影片照常以內嵌聲音播放。
- 選軌（Core `ResolveDescriptorAudio`）：(1) `SetAudioTrack(name)`（名字不存在：警告一次、視為未指定）；(2) 只有 `followGameLanguage="1"` 時，`g_languageAudio`、再 `g_language`，比對各音軌的 `language` 標記；(3) `default`；(4) 第一條；沒有音軌時是內嵌聲音。`followGameLanguage` 預設 0，不讀任何遊戲語言 cvar。
- 介面（不留舊名）：`IVideoPlayer::SetAudioTrack/GetAudioTrack`（介面版本仍為 5）；描述表 `SetAudioTrack`（參數 `Track`）、查詢 `AudioTrack`、`AudioTrackCount`、`AudioTrackName(Index)`、`AudioTrackLanguage(Index)`；`SetLanguage`（EB59C123-…）與 `Language`（B7A77A6C-…）的 GUID 退役（`ApiTable.h` 退役清單，自檢會擋）。FlowGraph `Settings` 的 `AudioTrack` 埠、`Info` 的 `AudioTrack` 輸出；Schematyc 屬性 `Audio Track`（成員 id `'atrk'`）、CryVS 屬性與 `CEVPlayer::Advanced::SetAudioTrack`（全域函式 GUID 沿用原 SetLanguage 者）。主控台 `cev_lang` 改為 `cev_audiotrack <handle|surface> [name]`。
