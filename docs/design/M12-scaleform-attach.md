# M12 · 把 Surface 播放器掛到 Scaleform（Flash UI）的 MovieClip（2026-11-27）

任務單：`docs/tasks/S2-scaleform-attach.md`。探測：`docs/tasks/S1-scaleform-probe.md`，證據 `E:/coding3/xt-bench/cev-s1/`（測試專案、腳本、讀回、log）；本功能的驗證證據 `E:/coding3/xt-bench/cev-s2/`。

## 1. 定位

使用者原則（2026-11-27）：Scaleform 已停止維護，這條路只需要做到**可以播放與控制**，只提供可用的選項，不為效能花心思。因此不做 GPU 色彩轉換、不做 alpha 修正、不做任何最佳化。對外以 C++（`IVideoPlayer::AttachToFlash` / `DetachFromFlash`，介面版本 6）與 FlowGraph 節點 `CEVPlayer:AttachToFlash` 提供；Schematyc 與 CryVS 不提供。播放、暫停、seek、音量等控制沿用既有的播放器介面與節點。

## 2. S1 實測的事實（設計依據）

- `IUIElement::LoadTexIntoMc(movieClip, ITexture*)` 可把引擎貼圖顯示在 MovieClip，內容每幀跟著貼圖原地更新、不落後，色彩與全螢幕輸出逐位元組相同。
- 圖像尺寸在載入當下固定為貼圖尺寸；貼圖重建（影片換解析度）後 id 可能不變但尺寸不會跟上，須重新載入。
- Scaleform 只會取樣單張 RGBA 貼圖；打包排列顯示成紅色碎片。
- Scaleform 對貼圖持有引用；播放器銷毀後 Flash 停在最後一格，之後同名播放器會沿用同一個 id 而被無聲顯示。
- Scaleform 以直通 alpha 疊合，我方貼圖是預乘：半透明邊緣偏暗。**不處理，列為已知限制。**
- 引擎沒有 `CryScaleformHelper.dll` 時 Flash UI 不存在。
- 完全落在舞台範圍外的 clip 會被剔除；行程外的視窗擷取在本機是黑的，驗證一律用引擎內讀回。

## 3. 行為

- **可用性**：`gEnv->pFlashUI` 為空、找不到元件（`GetUIElement(name)` 後取 `GetInstance(instanceId)`）、元件未初始化、或 clip 不存在 → 回傳 false 並記一行 log（說明哪一步）。不建立 clip。播放器必須有 surface（`CreatePlayerForSurface`），否則回傳 false。
- **掛上**：記錄（元件名、instance、clip 路徑）於播放器；有貼圖時立刻 `LoadTexIntoMc`，還沒有貼圖時等貼圖建立後由主執行緒的 tick 補做。同一個（元件、instance、clip）重複掛上視為成功、不重複載入。一個播放器最多 4 個掛點，超過回傳 false。
- **排列**：有任何掛點的播放器，`DesiredLayout` 一律回 RGBA8（不論其他消費端的 `acceptLayouts`）。掛上時若目前是打包排列，照既有的排列切換路徑切回 RGBA8，之後才載入。
  - 契約的確認（`M6-ui-source.md` §3，讀文件）：契約沒有一句明文「消費端必須接受排列 0」，但 RGBA8 是無條件的退路——規則 7「其他內容由來源轉換或以 RGBA8 提供」、規則 5「**所有**活著的消費端都接受某個平面排列時才用它」（否則 RGBA8）、`acceptLayouts` 的「0 means RGBA8 only」，以及 M8 規定非 4:2:0／奇數寬只能輸出 RGBA8。契約不存在「沒有可用排列」的狀態，所以消費端實際上必須能取樣 RGBA8。據此照任務單實作；若主模型認為需要明文，契約 §3 應補一句。
- **貼圖重建**：貼圖被重建或尺寸改變後，主執行緒的下一次 tick 對每個掛點先 `UnloadTexFromMc`（以舊貼圖，若仍可得）再 `LoadTexIntoMc`（新貼圖）。讀取迴圈中（渲染執行緒的 tick）不呼叫 FlashUI；延到讀取後主執行緒的第一次 tick。
- **卸下的時機**（都在釋放貼圖之前，主執行緒）：`DetachFromFlash`、釋放貼圖的 `Unload`（現行實作的 `Stop` 不釋放貼圖，只回到第一格）、播放器銷毀、插件關閉。卸下時元件已不存在（被卸載）→ 略過、不報錯。釋放貼圖之前先卸下，再走 L7a 的 `FlushBeforeTextureRelease`。
- **元件被重新載入**（UI 重載、關卡切換後元件重建）：不偵測、不自動重掛；要再呼叫一次 `AttachToFlash`。關卡切換時 FlashUI 會卸載沒有 `force_no_unload` 旗標的元件（`CryAction/FlashUI/FlashUI.cpp` 的 `ESYSTEM_EVENT_LEVEL_UNLOAD`）。
- **節流**：有掛點的播放器視為「可見」（不進入隱藏節流）。

## 4. 實作（`Module/Player/VideoPlayerImpl.*`）

- **掛點存放**：`CVideoPlayerImpl::m_flash`（`SFlashClip` 的陣列，上限 `kMaxFlashClips` = 4）。每個掛點記元件名、instance、clip 路徑，以及「這個 clip 目前載入的是哪一張貼圖」（`loaded`、`loadedSerial`、`loadedTextureId`）與「最後一次為哪一張貼圖嘗試過載入」（`handledSerial`）。
- **貼圖的身分**：`m_textureSerial` 在 `EnsureTexture` 每建立一張貼圖時加一。引擎可能把同一個 id 再發給新貼圖（S1），所以重建的判斷用這個序號，不用 id。它與契約的 `generation` 由同一組事件推動（建立／重建貼圖），但立即生效，不必等 `PublishSnapshot` 折算。
- **載入**（`UpdateFlashClips`）：在主執行緒的 tick 開頭（`TickBody`，在 `m_releasePending` 的釋放之後）與 `AttachToFlash` 的結尾呼叫；只在有 RGBA8 貼圖時做事。對 `handledSerial` 不等於目前序號的掛點：先卸下（若有載入），再找元件的既有 instance（以 `GetInstances` 列舉，不會建立新 instance）、確認已初始化且 clip 存在，然後 `LoadTexIntoMc`。元件或 clip 已不存在時記一行 log，該張貼圖不再重試（直到下一次重建或再次 `AttachToFlash`）。因此貼圖在 tick N 建立／重建時，載入發生在 tick N+1；`AttachToFlash` 當下已有貼圖則立即載入。
- **卸下**（`UnloadFlashClip`）：找既有 instance；元件、instance 或 clip 已不存在則略過；貼圖用「目前的貼圖（序號相同時）」，否則以舊 id 向渲染器查（`EF_GetTextureByID`），查不到就略過。
- **釋放貼圖之前**：`ReleaseTexture(false)`（`Unload`、銷毀、插件關閉的釋放）在 `FlushBeforeTextureRelease` 之前卸下所有已載入的掛點。這個呼叫若發生在渲染執行緒（讀取迴圈的 tick，例如讀取中 `Unload` 的延後釋放、或契約 `close` 銷毀播放器），不呼叫 FlashUI：貼圖的釋放延到讀取後主執行緒的第一次 tick（`m_releasePending`），讀取中被銷毀的播放器進入 `m_zombies`（`CPlayerManager::DestroyPlayer`），由主執行緒先卸下再釋放。`ReleaseTexture(true)`（排列切換、尺寸重建）不卸下，下一次 tick 以舊貼圖卸下再載入新貼圖。
- **排列**：`DesiredLayout` 的「強制 RGBA8」條件加上 `!m_flash.empty()`（不是消費端驅動的切換，不受 30 tick 的保持限制）。`AttachToFlash` 在目前是打包貼圖、且不在關卡讀取中時直接呼叫既有的 `UpdateLayout()`，然後載入。
- **可見**：`ApplyVisibility` 把有掛點的播放器當成可見（與材質輸出相同）。
- **讀取迴圈**：有掛點的播放器照樣在讀取迴圈中 tick（`RunsDuringLoad` 不變）；FlashUI 只在主執行緒呼叫（`FlashCallsAllowed()` = 主執行緒且 `gEnv->pFlashUI` 存在），而主執行緒在讀取中不 tick 播放器。
- **主控台與節點**：`cev_attach_flash <surfaceId> <uiElement> <instanceId> <movieClip>`、`cev_detach_flash <surfaceId>`（卸下該播放器所有掛點）；`cev_info`／`cev_surfaces` 對有掛點的播放器印 `flash: <n> clip(s)`。FlowGraph 節點 `CEVPlayer:AttachToFlash` 以 `IVideoPlugin::FindPlayerBySurface` 找播放器（唯一不經 API 表的節點），`Detach` 在 `UIElement` 為空時卸下全部。S1 的 `cev_probe_flash` 保留原樣（獨立的探測清單，不經掛點介面）。

## 5. 已知限制

需要 Scaleform 執行時；掛著時固定 RGBA8、CPU 轉換；尺寸＝貼圖尺寸，由作者自行縮放 clip；半透明邊緣偏暗；clip 必須在舞台範圍內；元件重載後需重新掛上；每個播放器最多 4 個 clip。重建後到重新載入之間（貼圖重建的那一幀到下一次 tick）Flash 仍以舊的圖像資訊取樣新貼圖，可能有一兩幀尺寸不對；重新載入的那一幀 clip 可能空白一幀（S2 讀回所見）。

未測：lazy update 的元件、Sandbox、正式的 GameSDK 選單／HUD 元件、行程正常結束時 Scaleform 的解構（社群版樹的 `quit` 本身會崩潰）。未編譯：共用樹、Sandbox 插件。
