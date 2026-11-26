# L7a · 同名貼圖重建的競態與探測讀回的寫檔錯誤（2026-11-27）

模型：Opus。倉 `E:/coding crycry/CEVPlayer-rebuild-integration`，分支 `xui2dl`（L6：`28df095`、`d42e77e`），**不推送**。只改插件（`Code/CryPlugins/CEVPlayer/Module/`）。只在**社群版樹** `E:/coding crycry/CryENGINE-5-7-community` 建置（只建 `CEVPlayer`）與執行；共用樹不碰。規則同 `docs/tasks/L3a.md`。規格找不到答案就停下回報。

## 背景（唯讀調查的結論，2026-11-27）

`E:/coding3/xt-bench/cev-l3c/logs/asserts_l3b_try1_crash.log`、`E:/coding3/xt-bench/cev-l3c/project_asserts/error.log`：渲染執行緒在 `CTexture::RT_CreateDeviceTexture`（`D3DTexture.cpp:300` → `DeviceResources.cpp:260`）存取違規。

- 播放器切換排列時（`VideoPlayerImpl.cpp` 約 :1620）在同一個呼叫裡先 `ReleaseTexture(true)` 再 `EnsureTexture()`，**用同一個貼圖名字**。
- 引擎的貼圖上傳命令（`CTexture::UpdateTextureRegion`，`D3DTexture.cpp:532`）在 lambda 內持有 `_smart_ptr<CTexture>`，於渲染執行緒執行後釋放。插件的釋放若先發生，最後一個引用就落在渲染執行緒，而引擎在渲染執行緒上歸零是立刻 `delete`（`Renderer.cpp:5144-5149`）。
- 同時主執行緒以名字查找（`FindOrRegisterTextureObject`，`Texture.cpp:334-357`，普通 `AddRef`）可能拿到正在被刪除的物件 → 「the created texture does not match」（`RenderBridge.cpp:174-178`）→ 之後排入的建立命令碰到已失效的物件。
- 這個時間窗在這條分支之前就存在（master 也有）。

## 1. 修正

裁決：**釋放舊貼圖之前，先讓渲染執行緒把已排入的命令跑完**，使插件的釋放成為最後一個引用（或至少不與渲染執行緒的釋放並行），名字在主執行緒同步註銷。

- 在 `RenderBridge` 加一個函式（名稱 `FlushBeforeTextureRelease()`）：主執行緒、且不在讀取迴圈回呼內時，呼叫引擎既有的 `IRenderer::FlushRTCommands(true, true, true)`；在渲染執行緒（讀取迴圈的 tick）上呼叫時什麼都不做（此時釋放與查找在同一條執行緒，沒有並行）。先確認 5.7.1 的 `IRenderer` 有這個公開函式與參數意義；沒有等效的公開呼叫就停下回報。
- 呼叫點：所有「釋放播放器貼圖之後會（立即或稍後）以同名重建」的路徑，在釋放之前呼叫一次——排列切換、尺寸改變的重建、`m_textureDeferred` 的讀取後重建。播放器銷毀與一般的 `Stop` 釋放也走同一個釋放函式的話一併涵蓋（多一次 flush 可接受）。逐一列出改了哪些呼叫點。
- 「the created texture does not match」那一行 log 加印實際拿到的值（id、寬、高、mips、類型、格式）與預期值。
- 不改引擎、不改 `EnginePatch/`。

## 2. 探測讀回的寫檔錯誤

`cev_probe_loadtime` 的迴圈內讀回（P4 子模式）以 RGB8 讀、卻當成每像素 4 位元組寫檔，PPM 內容錯位（L6 的代理以外部腳本 `E:/coding3/xt-bench/cev-l6/tools/decode_p4.py` 還原）。改成寫出正確的 PPM。

## 3. 驗證（社群版樹）

競態無法穩定重現，以壓力與回歸判定：

| 項目 | 判定 |
|---|---|
| R1 契約自測的 M8 段落連跑 | `cev_contract_selftest` 連續 30 次（同一行程內、`sys_asserts 1`），全部 248 PASS；log 無「does not match」、無崩潰 |
| R2 排列切換壓力 | 一支播放中的 Surface 播放器，`cev_allowPlanar` 0↔1 交替 200 次（每次間隔一到數幀；以 `cev_run_script` 既有的等待指令排程）；無「does not match」、無崩潰；結束後畫面正確（讀回） |
| R3 切換的代價 | R2 期間 `cev_stats` 的主執行緒幀時間：切換那一幀比平常多多少（毫秒），照實記錄 |
| R4 回歸 | L3c 的 C1、C4；L4 的 F1、F2、F6（讀回與 L3c 逐位元組相同）；自測 70／65／248 |
| R5 讀回 | 修正後的 P4 讀回檔可直接用一般看圖程式或 `PIL` 開啟，內容與 `decode_p4.py` 還原的舊做法一致（社群版樹的 P4 在 `xui2dl` 來源下若不可用，就以讀碼與一個離線的寫檔單元檢查替代，並註明） |

## 規則

同 L3a（社群版樹腳本不用 `quit`；每次執行都有逾時；卡住或崩潰的執行保留並列出；不碰共用樹；不 `git add -A`、不 `git stash`、不推送；英文程式與提交訊息）。提交：一個，訊息帶 `L7a`，結尾 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。證據放 `E:/coding3/xt-bench/cev-l7a/`。標「未編譯：共用樹」。

報告（繁體中文、精簡，寫在最後訊息裡）：提交；改了哪些呼叫點；R1～R5 的判定與證據；任何崩潰或逾時；偏離與沒做到的。
