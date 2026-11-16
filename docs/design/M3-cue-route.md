# M3 規範路線（cue）— 設計（給實作 session）

依據：需求單「聲音 → 規範路線」「相容規則」「三個部分的分工 → 規範路線聲音」；研究 `docs/research/01-open-questions.md` §7。
底線：官方版主線、CECE 驗 CEVPlayer 目標；**不套 optional patch 時兩邊照常編譯與執行**，規範路線退回影片時鐘並記警告。

## 引擎 patch（optional）：`patches/optional/02-cryaudio-trigger-position.patch`

- 位置：`patches/optional/`（不在主系列；`apply_patches.sh` 加 `--with-optional` 才套；BASE_FILES 一併記錄）。community 套不上時放 `patches/community/optional/`。
- 規則（照 XTFrame-CRYENGINE patch 02）：只加不改；新虛擬函式一律附加在 vtable 尾端、帶預設實作；不在既有類別加成員（需要狀態時放 impl 內部新類別或外部表）。
- 內容：
  1. `CryCommon/CryAudio/IObject.h`：尾端追加
     `virtual bool GetTriggerPlaybackPosition(ControlId const triggerId, float& outSeconds) const { return false; }`
     （預設「不支援」；呼叫端只在探測成功後呼叫）。
  2. CryAudioSystem 內部 `Impl::IObject`（impl 介面）尾端追加對應虛擬，預設回 false；`CObject` 實作公開版本時轉給 impl。注意執行緒：CryAudio 的請求在音訊執行緒處理；查詢必須是 thread-safe 的讀取（impl 端以 atomic 維護位置），不得在主執行緒觸碰音訊執行緒的容器。
  3. SDL_mixer impl：在 event instance 開始播放的 channel 上以 `Mix_RegisterEffect` 計數已處理位元組（atomic），換算秒數；instance 結束或停止時清除。只支援 SDL_mixer；Wwise / FMOD / PortAudio 走預設「不支援」。
  4. 探測符號：`CryAudioSystem.dll` 匯出 C 符號 `CEVPlayer_AudioPatched`（位元遮罩，bit 0 = 播放位置查詢）。插件以 `GetModuleHandle("CryAudioSystem.dll")` + `GetProcAddress` 判斷；沒有就不呼叫新 vtable 槽，記錄一次。
- 編譯驗證：套 patch 後重建 `CryAudioSystem`、`CryAudioImplSDLMixer`（以及因標頭變動受影響的模組——至少整個 CryAudio 資料夾與 CEVPlayer；能的話整樹重建一次證明標頭相容）。未套 patch 的樹也要能編譯 CEVPlayer。

## 插件端

- `route="cue"`：影片只放畫面；每語言一個 trigger（描述檔 `<Language trigger="...">`，沒有語言專屬就用 `<CEVVideo trigger="...">` 作預設——若需要就擴充描述檔格式並更新 `VideoDescriptor`）。
- 播放：建立一個 CryAudio 物件（或用全域物件），`ExecuteTriggerWithCallbacks` 註冊開始／結束回呼。**收到「聲音已開始」才開始推進畫面**（逾時 2 秒就退回影片時鐘並警告）。
- 時鐘 `CueClock : cev::IClock`：
  - 探測成功 → 主執行緒每次 `NowSeconds()` 讀 `GetTriggerPlaybackPosition`，加上平滑（位置更新的顆粒度可能是 callback 區塊），單調不倒退。
  - 探測失敗或 impl 回 false → 影片時鐘（InternalClock），兩邊同時開始，記一次警告。
- 控制轉發：Pause / Resume / Stop 對應 trigger 的停止或暫停能力（查 5.7 SDL_mixer impl 的 event 動作類型；有 pause/resume 就用，沒有就 Stop + 從頭不可行時記為限制）；Seek 在 cue 路線只能做到「畫面跳、聲音停到下次 Play」或重新觸發（記為限制，不得崩潰）；Loop：每圈重新觸發 trigger。
- 影音偏移：描述檔 `avOffsetMs` → `PlayerCore::SetAvOffset`（同時套用到 M2a 的 embedded / mixed）。
- 專案開關 `cev_disableEmbeddedAudio` 已在 M2a 完成；cue 路線不受它影響（cue 不用 SDL 裝置）。
- M2a 遺留：外部語言音軌的 PlayerCore 改用 `PlayerConfig::decodeVideo = false`。

## 測試專案

- `D:/coding11/_testproject/CEVTest` 設定音訊 impl 為 SDL_mixer（`s_ImplName CryAudioImplSDLMixer` 或 .cryproject 對應欄位），手寫最小的 ACE 控制檔與資產：把 `av_sync_flash_beep` 的音軌轉成 48 kHz WAV 放進 `audio/sdlmixer/assets/`，建立 trigger `cev_test_beep`；描述檔 `cue_beep.cevvideo` 指向無聲版影片 + 該 trigger。
- 冒煙：未套 optional patch → 播 cue 片：log 顯示 trigger 執行、「聲音已開始」回呼、時鐘 = internal（警告）；套 patch 後重建 CryAudioSystem + SDL_mixer impl → 時鐘 = cue，`cev_list` 顯示 trigger 位置與影片時間的差 < 1 幀。

## 實作記錄（M3 完成時）

- **patch**：`patches/optional/02-cryaudio-trigger-position.patch`（10 個檔，兩棵樹基底雜湊相同、無 community 變體），細節與執行緒論證見 `patches/README.md` 的「Optional patches」。`tools/apply_patches.sh --with-optional` 才套用。
- **插件**：`Module/Audio/CueRoute.{h,cpp}`：`CCueClock`（`cev::IClock`）就是 trigger 的 transport。PlayerCore 照常呼叫 Resume / Pause / Seek（loop = `Seek(0)`），時鐘把它轉成 CryAudio 請求：
  - Resume / 從 0 起的 Seek：`ExecuteTriggerWithCallbacks`（旗標 `CallbackOnExternalOrCallingThread | SubsequentCallbackOnExternalThread`，回呼在主執行緒的 `ExternalUpdate`，`pUserData` 帶 run id，過期的回呼忽略）；收到 `TriggerExecuted` 之前畫面不前進（2 秒逾時 → 影片時鐘 + 警告；`requestResult == Failure` = 沒有這個 trigger → 同樣退回並警告）。
  - 探測 `CEVPlayer_AudioPatched` 成功且插件用修補過的 `IObject.h` 編譯 → 時間 = trigger 播放位置，與行進估計混合（時間常數 0.12 s，領先 > 0.25 s 立刻對齊，落後 > 0.25 s 等聲音），單調；否則影片時鐘 + 一次警告。`cev_cueForceFallback 1` 強制後者。
  - Pause / Resume：描述檔 `pauseTrigger` / `resumeTrigger`（SDL_mixer 的 event `type="pause" / "resume"` 動作；它們對**同一物件上同 sample 的播放中實例**生效）。沒有 pauseTrigger 時聲音停不下來（記一次警告；resume 時畫面追上聲音）。
  - Seek：只有 `t≈0`（loop / replay / stop 後重播）會重新觸發；其他目標停掉聲音，畫面無聲繼續（記一次警告）。Rate 對聲音無效（固定 1x）。
  - 描述檔：`<CEVVideo trigger= pauseTrigger= resumeTrigger=>` 為預設，`<Language trigger= …>` 覆寫；mixed 路線的 trigger-only 語言也走 cue。
  - 銷毀 / 卸載 / 關機 / 關卡卸載（`ESYSTEM_EVENT_LEVEL_UNLOAD`）：StopTrigger、移除 listener、ReleaseObject；`gEnv->pAudioSystem` 為空就不碰。
- `avOffsetMs` 現在對所有路線生效（`PlayerConfig::avOffsetSeconds`）；外部語言音檔的 PlayerCore 用 `decodeVideo = false`；影片先 Ready 而外部音檔還在載入時，Tick 會等它（5 秒逾時才放棄並警告），不再當成失敗。
- **限制**：位置是估計值（延遲常數 = 一個輸出緩衝 4096 frames，實際裝置佇列可能更深，用 `avOffsetMs` 校正）；`GetTriggerPlaybackPosition` 在 SDL_mixer 取「物件上最近開始、仍在播放」的實例，不認 `triggerId`（每個播放器一個物件一個 trigger 即可）；Wwise / FMOD / PortAudio 回 false（退回影片時鐘）；音量（SetVolume）對 cue 無效；cue 不能 seek 到中間；在社群樹（未修補）建置的插件即使引擎已修補也走影片時鐘。
- 測試資產：`tools/make_cue_test_assets.sh`（WAV 與無聲影片由 ffmpeg 產生、不進版控）、`tools/test_audio/cev_test.xml`、`tools/test_descriptors/cue_beep.cevvideo`。
