# S2 · 把 Surface 播放器掛到 Scaleform（Flash UI）的 MovieClip（2026-11-27）

模型：Opus。倉 `E:/coding crycry/CEVPlayer-rebuild-integration`，分支 `xui2dl`（S1 探測：`b7959d9`），**不推送**。只在**社群版樹** `E:/coding crycry/CryENGINE-5-7-community` 建置（只建 `CEVPlayer`）與執行（S1 已把 `CryScaleformHelper.dll` 放進該樹的 `bin/win_x64/`，留在原處）。規則同 `docs/tasks/L3a.md`。先讀 `docs/tasks/S1-scaleform-probe.md` 與 S1 的證據 `E:/coding3/xt-bench/cev-s1/`（測試專案、腳本、驅動、`tools/`、探測指令 `cev_probe_flash` 的實作）。

使用者的原則（2026-11-27）：**只需要做到可以播放與控制；Scaleform 已停止維護，不為效能花心思，只提供可用的選項**。因此：不做 GPU 色彩轉換、不做 alpha 修正、不做任何最佳化。規格找不到答案就停下回報，不自行設計。

## S1 實測的事實（設計依據）

- `IUIElement::LoadTexIntoMc(movieClip, ITexture*)` 可把引擎貼圖顯示在 MovieClip，內容每幀跟著貼圖原地更新、不落後，色彩與全螢幕輸出逐位元組相同。
- 圖像尺寸在載入當下固定為貼圖尺寸；貼圖重建（影片換解析度）後 id 可能不變但尺寸不會跟上，須重新載入。
- Scaleform 只會取樣單張 RGBA 貼圖；打包排列顯示成紅色碎片。
- Scaleform 對貼圖持有引用；播放器銷毀後 Flash 停在最後一格，之後同名播放器會沿用同一個 id 而被無聲顯示。
- Scaleform 以直通 alpha 疊合，我方貼圖是預乘：半透明邊緣偏暗。**不處理，列為已知限制。**
- 引擎沒有 `CryScaleformHelper.dll` 時 Flash UI 不存在。

## 1. 介面

`Interface/IVideoPlayer.h`（介面版本 5 → 6，尾端追加，既有虛擬函式順序不動）：

```cpp
//! Shows this player's texture in a movie clip of a Flash UI element (Scaleform). The player must have a surface
//! (CreatePlayerForSurface); while attached it outputs RGBA8. movieClip is the instance path inside the element
//! (it must exist). Returns false when Scaleform / the element / the clip is not available. Main thread.
virtual bool AttachToFlash(const char* szUIElement, int instanceId, const char* szMovieClip) = 0;
//! Removes the texture from that clip (all clips of this player when szUIElement is null). Main thread.
virtual void DetachFromFlash(const char* szUIElement, int instanceId, const char* szMovieClip) = 0;
```

版本常數、版本檢查、`cev_apitest` 的版本斷言一併更新。

## 2. 行為

- **可用性**：`gEnv->pFlashUI` 為空、找不到元件（`GetUIElement(name)` 後取 `GetInstance(instanceId)`）、元件未初始化、或 clip 不存在 → 回傳 false 並記一行 log（說明哪一步）。不建立 clip（S1 探測的 `new:` 只留在探測指令）。
- **掛上**：記錄（元件名、instance、clip 路徑）於播放器；有貼圖時立刻 `LoadTexIntoMc`，還沒有貼圖時等貼圖建立後由主執行緒的 tick 補做。同一個（元件、instance、clip）重複掛上視為成功、不重複載入。一個播放器最多 4 個掛點，超過回傳 false。
- **排列**：有任何掛點的播放器，`DesiredLayout` 一律回 RGBA8（不論其他消費端的 `acceptLayouts`；先確認契約規定消費端必須能接受排列 0——`docs/design/M6-ui-source.md` §3；若契約允許消費端不接受 RGBA8，停下回報）。掛上時若目前是打包排列，照既有的排列切換路徑切回 RGBA8，之後才載入。
- **貼圖重建**：貼圖被重建或尺寸改變（既有的 `generation` 增加）後，主執行緒的下一次 tick 對每個掛點先 `UnloadTexFromMc`（以舊貼圖，若仍可得）再 `LoadTexIntoMc`（新貼圖）。讀取迴圈中（渲染執行緒的 tick）不呼叫 FlashUI；延到讀取後主執行緒的第一次 tick。
- **卸下的時機**（都在釋放貼圖**之前**，主執行緒）：`DetachFromFlash`、`Stop` 導致釋放貼圖、播放器銷毀、插件關閉。卸下時元件已不存在（被卸載）→ 略過、不報錯。釋放貼圖之前先卸下，再走 L7a 的 `FlushBeforeTextureRelease`。
- **元件被重新載入**（UI 重載、關卡切換後元件重建）：不偵測、不自動重掛；文件寫明要再呼叫一次 `AttachToFlash`。
- **控制**：播放、暫停、seek、音量等全部沿用既有的播放器介面與節點，這張單不新增控制。
- 顯示與否不影響節流：有掛點的播放器視為「可見」（不進入隱藏節流）。

## 3. 節點與指令

- FlowGraph 節點 `CEVPlayer:AttachToFlash`（放在既有節點集旁，命名與既有節點一致——先讀 README 的 Node sets 與既有節點的註冊方式）：輸入 `Attach`、`Detach`、`SurfaceId`（字串）、`UIElement`（字串）、`InstanceId`（整數，預設 0）、`MovieClip`（字串）；輸出 `Done`、`Failed`。以 `IVideoPlugin::FindPlayerBySurface` 找播放器。
- Schematyc 與 CryVS：不做（文件註明以 C++ 或 FlowGraph 使用）。
- 測試指令：`cev_attach_flash <surfaceId> <uiElement> <instanceId> <movieClip>`、`cev_detach_flash <surfaceId>`（列在 README 的 development and tests 指令表）。S1 的 `cev_probe_flash` 保留，但其載入／卸載改走新的掛點介面的地方不強求（保持可用即可）。
- `cev_info`／`cev_surfaces` 對有掛點的播放器加印 `flash: <n> clip(s)`。

## 4. 文件

- `README.md`：Features 的 Outputs 加一小項（Flash UI：把 Surface 顯示在 Scaleform 的 MovieClip；一句話說明定位是相容選項）；Node sets 加節點；指令表；Limitations 加一條（需要 Scaleform 執行時；RGBA8、CPU 轉換；尺寸＝貼圖尺寸，由作者自行縮放 clip；半透明邊緣偏暗；clip 必須在舞台範圍內；元件重載後需重新掛上；未以 lazy update 的元件測試）。
- `CHANGELOG.md` 未發佈區段加兩行（功能、介面版本 6）。
- 設計紀錄寫一頁 `docs/design/M12-scaleform-attach.md`（繁體中文；內容＝本單 §「S1 實測的事實」與 §2 的行為，附 S1 證據路徑）。

## 5. 驗證（社群版樹；沿用 S1 的測試專案與 `Crosshair` 元件；讀回加 `r_MouseUseSystemCursor 1`）

測試腳本裡 clip 可用 S1 探測指令的 `new:` 先建立，再以 `cev_attach_flash` 掛上。每一項註明像素驗證、計數佐證或讀碼。

| 項目 | 判定 |
|---|---|
| A1 掛上與播放 | `cev_attach_flash` 後影片出現在 clip、時間碼前進，與同幀 XUI2DL 1:1 圖層差 0；`cev_info` 印 `flash: 1 clip(s)` |
| A2 控制 | 暫停、繼續、seek、循環：Flash 中的畫面與播放器狀態一致 |
| A3 排列固定 | 掛上後以 `cev_bench_hint <id> 2` 要求打包：排列維持 rgba8，Flash 畫面正確；卸下後恢復可切換 |
| A4 掛上前是打包 | 先切成打包再掛上：自動切回 rgba8，畫面正確（不出現紅色碎片的幀——連續讀回檢查） |
| A5 換解析度 | 換播另一支解析度：下一幀（或數幀內）Flash 中的尺寸正確、不拉伸；log 有重新載入的一行 |
| A6 卸下與銷毀 | `cev_detach_flash` 後 clip 清空；掛著時直接銷毀播放器：clip 清空（不停在最後一格）；之後同名新播放器不會自己出現在 clip；`sys_asserts 1` 下 0 個斷言 |
| A7 失敗路徑 | 不存在的元件、不存在的 clip、沒有 surface 的播放器、第 5 個掛點：回傳 false 與對應 log，無崩潰 |
| A8 沒有 Scaleform | 暫時把 `bin/win_x64/CryScaleformHelper.dll` 改名（記 SHA-256，測完改回並核對）：`cev_attach_flash` 回報不可用、無崩潰、其餘播放正常 |
| A9 兩個播放器各掛一個、同一播放器掛兩個 | 都正常；各自卸下互不影響 |
| A10 讀取（`gfx_loadtimethread 1`，模擬 8 秒；讀取中換解析度一次） | 無死鎖、無崩潰；讀取後尺寸正確（重掛延到讀取後） |
| A11 FlowGraph 節點 | 節點已註冊（`cev_flowtest` 加檢查；既有 65 項的數字會增加，報告新數字）；節點邏輯以既有的 flow 測試方式呼叫一次 Attach／Detach |
| A12 回歸 | `cev_apitest`（版本 6；數字會變，報告新數字）、`cev_flowtest`、`cev_contract_selftest` 248；L3c 的 C1；L4 的 F1、F2、F6（讀回與 L7a 逐位元組相同） |

## 規則

同 L3a（社群版樹腳本不用 `quit`；每次執行都有逾時；樹忙碌檢查；只關自己啟動的行程；不碰共用樹；GameSDK 範例專案唯讀、Crytek 素材不進倉；不 `git add -A`、不 `git stash`、不推送；英文程式與提交訊息）。提交：功能一個、節點與指令一個、文件一個，訊息帶 `S2`，結尾 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。證據放 `E:/coding3/xt-bench/cev-s2/`。標「未編譯：共用樹、Sandbox 插件」「未測：lazy update 的元件、Sandbox、正式的 GameSDK 選單／HUD 元件、行程正常結束時 Scaleform 的解構」。

報告（繁體中文、精簡，寫在最後訊息裡）：提交；介面與行為的實際做法（掛點存哪、何時載入／卸下）；A1～A12 的判定與證據；自測的新數字；崩潰或卡住的執行；偏離與沒做到的。
