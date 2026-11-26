# L3b · XUI2DL 的讀取迴圈（二）：迴圈內繪製全螢幕圖層（2026-11-27）

模型：Opus。倉 `E:/coding crycry/CEVPlayer-rebuild-integration`，分支 `xui2dl`（L3a：`f4ad71c`、`7c67ecc`），**不推送**。只在**社群版樹** `E:/coding crycry/CryENGINE-5-7-community` 建置與執行；共用樹唯讀。先讀 `docs/tasks/L3a.md`（含規則）、`docs/design/M11-xui2dl.md` §5、§7、`docs/design/M6-ui-source.md` §9.2、調查報告 `E:/coding3/xt-bench/cev-xui2dl/L0-survey.md` 的 D.1、D.2、D.4 與「執行緒與鎖的限制」。L3a 的證據與驅動在 `E:/coding3/xt-bench/cev-l3a/`。

L3a 的九點偏離全部定案照做（探測經管理員的掛鉤、`LoadtimeBeforeStop` 的替身回呼、名稱 `SetLoadtimeUpdate` 等）。規格找不到答案就停下回報。

## 1. 引擎端（`EnginePatch/`）

- 新掛點：讀取迴圈內、渲染器的讀取鎖（`m_rdldLock`）**之內**、原版 `LoadtimeRender` 呼叫之後、`RT_PresentFast()` 之前，呼叫 `XUI2DL::RT_LoadtimeRender()`。套用腳本照 L3a 的規則處理（有補丁 17 的樹不套用這一組）。
- `RT_LoadtimeRender()`：
  - 目前的保留快照為空、或渲染資源尚未建立、或著色器未就緒 → 什麼都不做（**不在迴圈內建立資源或編譯著色器**；調查報告 C.2 提到引擎在迴圈內首次編譯著色器有已知問題）。
  - 否則畫出快照，走與主路徑相同的繪製函式。迴圈不清後緩衝：**沒有別的回呼在畫**（`m_pLoadtimeCallback` 為空，或是 L3a 的替身）時，先把後緩衝清成不透明黑再畫；有 Flash 回呼時不清，直接疊在它上面。
  - 快照的取得：渲染執行緒在與主執行緒 `SetFrame` 交換指標相同的鎖下取一個引用；用完的釋放遵守「最後一個引用不得在 render loading thread 上釋放」（既有的延後機制）。
  - 幾何上傳後重設命令列表的串流快取（既有做法）；迴圈中不得呼叫會斷言的主執行緒幀序號函式（調查報告 D.4）。
- 主路徑的繪製命令維持「讀取中略過」。
- `XUI2DLStats` **尾端**追加 `uint64_t loopFramesDrawn;`（迴圈內實際畫了至少一個圖層的次數）；`GetStats` 既有的「只寫 `min(呼叫端 size, sizeof)`」規則讓舊呼叫端不受影響。兩份 `XUI2DLApi.h` 同步。
- 資源在進迴圈前就要存在：`LoadtimeAutoStart` 進入迴圈之前，若目前快照非空而資源尚未建立，先在渲染執行緒建立並等著色器預熱完成（最多等 2 秒，逾時就放棄，迴圈內不畫並記一行 log）。

## 2. 插件端

- 全螢幕播放器在讀取中執行的條件：後端是 `XUI2DL`、讀取來源是 `xui2dl`（L3a）、`cev_loadtime 1`、沒有材質綁定。`RunsDuringLoad()` 與 `LoadtimeTick` 的既有規則（只更新貼圖、不派發事件、dt 夾限）不變。後端是 `XUI2D` 時維持現狀（不執行；另一張單）。
- 讀取迴圈期間保留清單不變：`LoadtimeTick` 不得呼叫 `FullscreenOutput::Present`／`SetFrame`。全螢幕播放器在迴圈期間不得重建貼圖或切換排列（先讀 M10 對 Surface 播放器在迴圈內「排列切換／貼圖重建」的既有處理；全螢幕比照。既有處理不足以保證時停下回報）。
- 讀取期間才開始的全螢幕播放器，要到讀取結束後才會出現——寫進 `docs/design/M11-xui2dl.md` §9 的已知限制。
- `cev_probe_xui2dl stats` 加印 `loopFramesDrawn`。

## 3. 驗證（社群版樹）

模擬讀取的做法同 L3a（`e2level`＋`cev_probe_loadtime 1 <stall ms>`）。**迴圈內的畫面證據**：主執行緒在讀取中被卡住，探測的 `read` 讀不到；改由驅動在讀取期間從行程外擷取遊戲視窗（L3a 的驅動已有在逾時前截圖的做法，沿用同一個擷取方式，在 8 秒的讀取期間每秒擷取一張）。螢幕關閉導致擷取不到內容時，照實註明「未以像素驗證」，改以 `loopFramesDrawn` 與貼圖的 `contentSerial` 佐證，不得把推論寫成已驗證。

| 項目 | 判定 |
|---|---|
| B1 全螢幕影片跨讀取（RGBA 與打包各一次，8 秒） | 讀取期間擷取的畫面有影片且內容隨時間前進（時間碼）；`loopFramesDrawn` 隨迴圈增加；媒體時間與真實時間的差在 M10 容許內；讀取結束後畫面連續、不追趕 |
| B2 Letterbox 黑邊 | 迴圈內的畫面黑邊為黑（沒有殘留讀取前的畫面） |
| B3 帶 alpha 的全螢幕影片 | 迴圈內透明處為黑（已清除），無殘影 |
| B4 `cev_loadtime 0` | 迴圈不因我方而啟動；讀取期間的畫面照實記錄 |
| B5 沒有全螢幕圖層、只有 Surface 播放器 | 行為同 L3a 的 T2；`loopFramesDrawn` 不增加；我方不清後緩衝 |
| B6 資源尚未建立就讀取（啟動後第一次送圖層緊接著讀取） | 不崩潰；迴圈內要嘛正常畫（預熱趕上）、要嘛不畫並有那一行 log |
| B7 連續 6 次載入；讀取結束後立刻銷毀全螢幕播放器；讀取中播完的不循環影片 | 無死鎖、無崩潰；銷毀後畫面清空；`Ended` 事件在讀取後發出一次 |
| B8 回歸 | L3a 的 T2、T3；L4 的 F1、F2、F6；自測 70／65／248；`sys_asserts 1` 下斷言逐一列出 |
| B9 套用腳本 | 社群版樹 `--check` 為 applied；共用樹副本試跑：讀取迴圈的兩組掛點都略過；`--remove` 後逐位元組相同 |

沒有 Flash 讀取畫面的素材：「疊在 Flash 讀取畫面之上」標「未測」。

## 規則

同 L3a（含：社群版樹腳本不用 `quit`；每次執行都有逾時，卡住被殺的執行要保留並列出；不碰共用樹；不 `git add -A`、不 `git stash`、不推送）。提交：引擎端一個、插件端（含 M11 §9 那一行）一個，訊息帶 `L3b`，結尾 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。證據放 `E:/coding3/xt-bench/cev-l3b/`。未做的標「未編譯：共用樹」「未測：Flash 讀取畫面、非同步關卡讀取、Sandbox、XUI2D 後端的讀取迴圈」。

報告（繁體中文、精簡，寫在最後訊息裡）：提交；新掛點的位置；B1～B9 的判定與證據路徑，**每一項註明是像素驗證還是計數佐證**；任何逾時或卡住的執行；偏離與沒做到的。
