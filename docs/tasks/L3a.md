# L3a · XUI2DL 的讀取迴圈（一）：迴圈驅動與播放器 tick（2026-11-27）

模型：Opus。倉 `E:/coding crycry/CEVPlayer-rebuild-integration`，分支 `xui2dl`（目前 `f0cd971` 之後的文件提交），**不推送**。只在**社群版樹** `E:/coding crycry/CryENGINE-5-7-community` 建置與執行。共用樹 `E:/coding crycry/CRYENGINE_Source_CEVPlayer_XTFrame` **唯讀**（它含 XTFrame 的「補丁 17」，是這張單要移植的來源）。

先讀：`docs/design/M11-xui2dl.md` §7；`docs/design/M6-ui-source.md` §9、§9.2（M10 的讀取期間規則與七條定案，含「絕不在 tick 鎖內呼叫註冊／註銷」）；`docs/tasks/M10.md`；`E:/coding3/xt-bench/cev-xui2dl/L0-survey.md` 的 D 節（迴圈本體、補丁 17 改了哪些地方、執行緒與鎖的限制，含檔案:行）。

規格找不到答案就**停下回報，不自行設計**。這張單只做「迴圈會自己開、插件的播放器在迴圈裡被 tick」；**迴圈內的全螢幕繪製是下一張單（L3b），這張單不做**。

## 背景

原版引擎的讀取迴圈只有一個回呼槽（`StartLoadtimeFlashPlayback`），而且沒有人註冊時迴圈根本不開。XTFrame 的補丁 17 加了「多個回呼的註冊表＋有註冊者時由渲染器在關卡讀取時自動進出迴圈」。CEVPlayer 的 M10 就靠它在讀取期間推進播放器（`IRenderer::AddLoadtimeCallback`）。沒有 XTFrame 的引擎沒有補丁 17，所以 XUI2DL 要自帶同樣機制的最小版本。

## 1. 引擎端（`EnginePatch/`）

**移植來源**（共用樹，唯讀）：補丁 17 在 `RenderDll/Common/RenderThread.h:89-118`、`RenderThread.cpp:35-172, 821-827`、`XRenderD3D9/D3DRenderThread.cpp:348-473`、`XRenderD3D9/DriverD3D.cpp:5241-5250`、`DriverD3D.h:630-637` 的改動（行號以調查報告 D.3 為準，動手前核對）。以共用樹與社群版樹同名檔案的差異確認哪些行屬於補丁 17。

**要做的**（語意與補丁 17 相同，差別只有：只有一個註冊者、名字與入口屬於 XUI2DL、不改 `IRenderer.h`）：

- `XUI2DLApi.h`：`XUI2DLApi` 結構**尾端**追加一個成員，並加能力位元（`XUI2DL_CAP_LOADTIME = 4` 已保留）：
  ```c
  /* Loading-loop callback. pfnUpdate runs on the render thread once per loading-loop iteration, outside the
     renderer's loading lock, before that iteration is presented. dtSeconds is the real time since the previous
     iteration (0 on the first one). Main thread only; never call it from inside the callback. Passing NULL
     unregisters and returns only when the callback is not running. Returns 1 on success, 0 when refused. */
  int (*SetLoadtimeUpdate)(void (*pfnUpdate)(void* pUser, float dtSeconds), void* pUser);
  ```
  `XUI2DL_API_VERSION` 不變；舊的呼叫端以 `size` 判斷有沒有這個成員。插件那份標頭同步，兩份逐位元組相同。
- 迴圈呼叫點：在渲染執行緒讀取迴圈裡、原版呼叫 `m_pLoadtimeCallback->LoadtimeUpdate` 的位置（鎖外）之後，呼叫 `XUI2DL::RT_LoadtimeUpdate(dt)`。
- 自動進出迴圈：有註冊者、不是編輯器、`r_MultiThreaded > 0`、不是非同步關卡讀取、迴圈沒在跑時，在「關卡讀取畫面開始」的系統事件進入迴圈；在「讀取結束／讀取錯誤／快速關機」的系統事件離開。條件、等待迴圈啟動的方式、與 Flash 回呼並存的規則（迴圈已在跑時加入、只有註冊者時 `Stop` 也要能停）全部照補丁 17 的做法移植。
- 註銷的保證（返回時回呼不在執行中）與「回呼內不得註冊／註銷」的檢查，照補丁 17。
- `caps` 加上 `XUI2DL_CAP_LOADTIME`。
- `apply_xui2dl.py`：新增的掛點一樣以錨點插入、包在 `// XUI2DL begin`／`end` 之間、可重跑、可 `--remove`、找不到錨點不寫任何檔。**若目標樹已有補丁 17（偵測 `RenderThread.h` 內的 `LoadtimeCallbacks`），這一組讀取迴圈的掛點不套用**，並在 `--check` 的輸出註明（這種樹走 XUI2D，不用 XUI2DL 的迴圈驅動）；此時 `caps` 不得含 `XUI2DL_CAP_LOADTIME`（以一個由套用腳本決定的編譯定義控制）。
- 新程式放在 `XUI2DL.cpp`（或新檔 `XUI2DLLoadtime.cpp`，行數多於 250 時分檔）；引擎既有檔案只加呼叫與最少的膠合。檔頭的來源聲明照 L2 的格式（這一段同樣衍生自 CE_XTUI）。

## 2. 插件端

- `Module/Render/RenderBridge.*`：`LoadtimeAvailable()` 目前只認補丁 17（編譯期 `CEV_HAVE_LOADTIME_CALLBACKS`＋執行期 `XUI2D_Patched & 128`）。改成回報來源：`None`／`Patch17`／`XUI2DL`（XUI2DL 的 api 有 `SetLoadtimeUpdate` 成員且 `caps & XUI2DL_CAP_LOADTIME`）。兩者都有時用 `Patch17`。
- `Module/Player/PlayerManager.cpp` 的 `UpdateLoadtimeRegistration()` 與回呼：註冊／註銷依來源分派到補丁 17 的 `Add/RemoveLoadtimeCallback` 或 XUI2DL 的 `SetLoadtimeUpdate`。**所有 M10 的規則不變**：決定在鎖內、呼叫在鎖外；`cev_loadtime 0` 停用；`LoadtimeTick(dt)` 的 dt 夾限與內容不變。
- `cev_info`／`cev_surfaces` 的 `loadtime:` 那一行加印來源（`source=patch17|xui2dl|none`）。
- `cev_probe_loadtime`：目前整個探測包在 `CEV_HAVE_LOADTIME_CALLBACKS` 內，社群版樹編不進去。改成兩種來源都可用（它用補丁 17 的 `ILoadtimeCallback` 的部分，依來源改走對應的註冊；`P4` 的迴圈內繪製與讀回在 XUI2DL 來源下若做不到就印「not available with source=xui2dl」，不要硬做）。
- 全螢幕播放器維持現狀（不在讀取中執行）。

## 3. 驗證（社群版樹，Profile；套用腳本重跑後增量建 `CryRenderD3D11` 與 `CEVPlayer`；插件檔逐檔 `cmp`）

關卡：先前用過的 `e2level`（做法見 `E:/coding3/xt-bench/cev-l2/` 的 D1、D3）。真實讀取很短，所以用 `cev_probe_loadtime 1 <stall ms>` 在讀取中讓主執行緒多停幾秒（M10 的做法，見 `docs/tasks/M10.md` 與 `E:/coding3/xt-bench/cev-m10/`）。

| 項目 | 判定 |
|---|---|
| T1 可用性 | `cev_info`：`loadtime: available=1 … source=xui2dl`；`cev_probe_xui2dl stats` 或 log 顯示 caps 含 4 |
| T2 Surface 播放器在讀取中前進 | 讀取前 `cev_play_surface` 播一支循環影片；模擬 8 秒讀取；以探測每圈的契約快照判斷：`contentSerial` 持續前進、媒體時間與真實時間的差在 M10 的容許範圍內、讀取結束後不追趕。兩次載入 |
| T3 `cev_loadtime 0` | 同樣的讀取：播放器在讀取中不前進（與補丁前行為相同），沒有進入迴圈的 log |
| T4 沒有註冊者 | 沒有任何播放器時載入關卡：不進迴圈（log 無自動進入的訊息）、讀取正常完成 |
| T5 連續 6 次載入（有播放器） | 每次都正常進出迴圈；無死鎖（每次讀取在逾時內完成）；無崩潰 |
| T6 讀取中的控制 | 探測的 `control:` 子模式送 pause／play（F2 的做法）：state 與 `contentSerial` 的反應同 F2 的 H1 |
| T7 讀取中註冊狀態改變 | 讀取期間播放器播完（不循環的短片）→ 不崩潰、不死鎖；讀取結束後狀態正確 |
| T8 回歸 | L4 的 F1、F2、F6（全螢幕路徑沒被動到）；自測 70／65／248；`sys_asserts 1` 下斷言逐一列出 |
| T9 套用腳本 | 在社群版樹 `--check` 為 applied（含新的掛點）；在 `E:/coding3/xt-bench/cev-l3a/dryrun/` 放一份共用樹相關檔案的副本試跑：讀取迴圈的掛點被略過並有說明，其餘照常；`--remove` 後與原檔逐位元組相同 |

沒有 Flash 讀取畫面的素材時，「與 Flash 回呼並存」標「未測」，並說明讀碼的依據。

## 規則

同 L1、L1b、L2、L4（啟動方式；社群版樹腳本不用 `quit`，由驅動逾時結束；樹忙碌檢查；殺不掉的行程 → 停下回報；不碰共用樹的任何檔；不 `git add -A`、不 `git stash`、不推送；英文程式與提交訊息；不改登錄檔與系統設定）。**讀取迴圈的死鎖會讓行程卡住**：每次執行都要有逾時，逾時被殺的那一次要保留 log 並在報告列出，不要只重跑。提交：引擎端一個、插件端一個，訊息帶 `L3a`，結尾 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。證據放 `E:/coding3/xt-bench/cev-l3a/`。未做的標「未編譯：共用樹」「未測：迴圈內全螢幕繪製、與 Flash 讀取畫面並存、非同步關卡讀取、Sandbox」。

報告（繁體中文、精簡，寫在最後訊息裡）：提交；移植了補丁 17 的哪些部分（來源檔案:行 → 我方位置）與刻意不移植的部分；套用腳本在社群版樹新增的每一處；T1～T9 的判定與證據路徑；任何逾時或卡住的執行；偏離與沒做到的。
