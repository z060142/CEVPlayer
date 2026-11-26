# L7b · XUI2D 後端：讀取迴圈中的資源世代（2026-11-27）

模型：Opus。倉 `E:/coding crycry/CEVPlayer-rebuild-integration`，分支 `xui2dl`（接在 L7a 之後），**不推送**。在**共用樹** `E:/coding crycry/CRYENGINE_Source_CEVPlayer_XTFrame` 建置（只建 `CEVPlayer`）與執行；不改 XTFrame 的任何檔與成品。先讀 `docs/tasks/L6.md`（含規則）、`docs/design/M11-xui2dl.md` §7.1、L6 的實作 `Module/Render/FullscreenXUI2D.*`（`CLoopDraw`、凍結幀）、L6 的證據與重現 `E:/coding3/xt-bench/cev-l6/`（`scripts/repro_gen.txt`、`logs/repro_gen.log`、測試專案 `cev-l6/project`、驅動與分析工具）。只改插件與文件。規格找不到答案就停下回報；XUI2D 行為與下述不符 → 最小重現、回報，不修、不繞過。

## 對方給的規則（XTFrame 主線讀碼得出，未實測）

- 已有關卡時再載入：卸關卡使 XUI2D 的資源世代 +2，發生在 `LEVEL_LOAD_START_LOADINGSCREEN`（迴圈啟動）**之前**，同一個 `map` 指令內、主執行緒中間沒有幀。從 LOADINGSCREEN 到讀取結束，引擎路徑不會再變（例外：有人主動呼叫 `IXUIRenderer::ReleaseResources`）。
- 世代改變後：所有貼圖句柄失效（含 `WrapEngineTexture` 的；以舊句柄取樣為白）。screen surface id、其 order／viewport、`IXUILoadFrame*` 仍有效，不必重取。
- `Begin` 只比對呼叫端傳入的世代與目前值，不檢查幀內的句柄：傳新世代卻帶舊句柄不會被拒，會畫白。
- `GetResourceGeneration()` 任何執行緒可呼叫。`WrapEngineTexture` 可在讀取迴圈的回呼內呼叫（加鎖的表插入；對方自己也這樣做）。這一條**取代** L6 規則 4 中「迴圈中不 wrap」的部分，僅限於世代改變後的重新 wrap。

## 1. 要做的（對方的做法 A）

- `CLoopDraw::LoadtimeUpdate` 每圈讀 `GetResourceGeneration()`。與凍結幀的世代不同時，在回呼內：以凍結幀記下的引擎貼圖 id 重新 `WrapEngineTexture`、重建要送的繪製內容（幾何與參數不變，只換句柄與世代），之後以新世代 `Begin`。重建只在世代改變的那一圈做一次。
- 凍結幀因此要保存「每個圖層的引擎貼圖 id」而不只是句柄（現況若只存句柄，補上）。
- 重新 wrap 失敗（回傳無效句柄）或引擎貼圖 id 已失效（`GetByID` 為空；以對方介面能得知的方式判斷，不能得知就以引擎的公開查詢）：該圖層這次讀取不畫，記一行 log（每次讀取一次），其餘圖層照畫。
- 讀取後主執行緒的第一次 `Present`：照既有的世代處理路徑（L5 已有「世代改變就重新 wrap」）；確認迴圈內重新 wrap 得到的句柄與主執行緒的表不會重複釋放或外洩（迴圈內建立的句柄交回主執行緒那份表管理，或在 `End` 之後由主執行緒丟棄重建，擇一，寫明選了哪個與理由）。
- `docs/design/M11-xui2dl.md` §7.1 補上這一段規則與出處。

## 2. 驗證（共用樹）

| 項目 | 判定 |
|---|---|
| H1 `repro_gen`（連續 map 三次，無 UI） | 三次載入每一圈都 Begin 成功、refused 0；迴圈內讀回是影片不是白或黑（像素）；log 有一行世代改變後重建的訊息（第 2、3 次） |
| H2 L6 的 G2（RGBA 與打包，各連續兩次載入） | 兩次都通過 L6 的判定（每圈 Execute、每秒約 24 幀、時間差、讀取後不追趕與連續） |
| H3 L6 的 G3（有 XTFrame 讀取 UI，兩次載入） | 兩次都是左半 UI 色塊、右半影片；`xtui_loadmode_clear` 的顏色兩次都不出現（像素） |
| H4 對方要的事實 | 卸關卡後我方的引擎貼圖 id 是否仍有效（重新 wrap 後取樣到的是影片即有效）。照實寫 |
| H5 主動釋放的例外 | 讀取之外：播放全螢幕影片時下 `xui2d_release_resources`（若該指令存在），下一幀恢復顯示（L5 已有的路徑，回歸）。迴圈內不測 |
| H6 L6 的 G6（連續 7 次載入、讀取後立刻銷毀、讀取中播完） | 無死鎖、無崩潰、無 XUI2D 的 assert；每次載入迴圈都有畫 |
| H7 回歸 | L6 的 G4、G5；L5 的 X2～X4（暫停幀逐位元組相同）；自測 70／65／248；`sys_asserts 1` 下斷言逐一列出 |
| H8 社群版樹（同一份原始碼，只建 `CEVPlayer`） | L3c 的 C1；自測 70／65／248 |

每一項註明像素驗證或計數佐證。讀回像素的測試加 `r_MouseUseSystemCursor 1`。

## 規則

同 L6。提交：插件一個、文件一個，訊息帶 `L7b`，結尾 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。證據放 `E:/coding3/xt-bench/cev-l7b/`。未做的標「未測：Scaleform 讀取畫面同時在跑、非同步關卡讀取、Sandbox、D3D12／Vulkan」。

報告（繁體中文、精簡，寫在最後訊息裡）：提交；重建做在哪、迴圈內建立的句柄如何交接；H1～H8 的判定與證據；H4 的事實；L6 報告中「3 begun」那三圈的世代值是哪來的（若查得到）；任何與規則不符的 XUI2D 行為；逾時或卡住的執行；偏離與沒做到的。
