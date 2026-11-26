# S1 · 探測：把影片貼圖放進 Scaleform（Flash UI）的 MovieClip（2026-11-27）

模型：Opus。倉 `E:/coding crycry/CEVPlayer-rebuild-integration`，分支 `xui2dl`，**不推送**。只在**社群版樹** `E:/coding crycry/CryENGINE-5-7-community` 建置（只建 `CEVPlayer`）與執行。規則同 `docs/tasks/L3a.md`。

這是**探測單**：目的是回答「可不可行、有哪些坑」，不是做成品功能。只加一個探測指令（`Module/Console.cpp`，與既有的 `cev_probe_*` 同類），不加公開介面、不加節點、不改引擎、不改 `EnginePatch/`。規格找不到答案就停下回報。

## 背景（讀碼得出，未實測）

- `IUIElement::LoadTexIntoMc(const char* movieClip, ITexture*)`（`CryCommon/CrySystem/Scaleform/IFlashUI.h:1537`；實作 `CryAction/FlashUI/FlashUIElement.cpp:1569-1605`）對 MovieClip 呼叫 `loadMovie("img://TEXID:<id>")`。
- Scaleform 的影像載入器以 id 取引擎貼圖並包成 `GImageInfoTextureXRender`（`CrySystem/Scaleform/SharedStates.cpp:595-625`、`GImageInfo_Impl.cpp:150-215`）。
- 先例：`Code/GameSDK/GameDll/Nodes/ModelToHudFlowNodes.cpp:113`。
- CEVPlayer 的 Surface 播放器輸出引擎貼圖 `$cev:<id>`；沒有 UI 消費端時排列是 RGBA8（預乘 alpha），每幀原地更新。

## 素材

GameSDK 範例專案（**唯讀**，不得在其中啟動或寫入任何檔）：`E:/coding crycry/cryengine-gamesdk-sample-project/5.7.1/gamesdk/gamedata.pak`，內含 `Libs/UI/*.gfx`（`Menus_Startmenu.gfx`、`HUD_2D.gfx`、`HUD_3D.gfx`、`USM_Player.gfx`、`Menus_Dialogs.gfx`…）與 `Libs/UI/UIElements/*.xml`（`MainMenu.xml` 宣告了 MovieClip `Cursor`、`HolderMC`；`HUD.xml` 有 `SBItem`；`USMPlayer.xml`）。pak 內檔名用反斜線，Python 的 `zipfile.read` 會拒絕，需自行依 local header 解壓。

做一個我方的測試專案（放 `E:/coding3/xt-bench/cev-s1/project/`，以先前的 e2level 測試專案為底，見 `E:/coding3/xt-bench/cev-l3c/`），把需要的 `Libs/UI` 檔案以散檔複製進去（只複製測試用得到的；這些是 Crytek 的範例素材，只留在測試資料夾，不得進倉）。選一個能在沒有 GameSDK 遊戲 DLL 的情況下顯示出來的 UI 元件；元件的事件／函式缺少遊戲端無妨，只要影片能被看到。**沒有任何一個元件顯示得出來時停下回報**（寫明每個元件卡在哪）。

## 探測指令

`cev_probe_flash <uiElement> <movieClip 路徑> <surfaceId>`：取得該 UI 元件（必要時先讓它可見），對該 MovieClip 呼叫 `LoadTexIntoMc`，貼圖取自該 Surface 播放器目前的引擎貼圖。`cev_probe_flash off` 呼叫 `UnloadTexFromMc`。印出貼圖 id、尺寸、格式。MovieClip 路徑可以是 XML 未宣告的實例路徑（`GetOrCreateMovieClipDesc`）；先讀碼確認。

## 要回答的問題（每題附 log 行、讀回圖或檔案:行；註明像素驗證、計數佐證或讀碼）

讀回畫面用既有的 `cev_probe_xui2dl read` 或引擎截圖（加 `r_MouseUseSystemCursor 1`）。

1. **顯示得出來嗎**：影片出現在 MovieClip 的位置；影像正立還是上下顛倒；尺寸如何決定（MovieClip 大小、貼圖大小、還是載入當下的寬高）。
2. **會動嗎**：播放中連續讀回，時間碼前進；暫停時不變。原地更新的內容是否每幀反映（有無落後或要 `ForceLazyUpdate` 才更新；FlashUI 的 lazy update 是否讓畫面停住）。
3. **色彩**：與同一幀經全螢幕輸出（XUI2DL）的讀回比對——每通道平均差、最大差；是否偏亮／偏暗（sRGB）或紅藍對調。
4. **透明**：帶 alpha 的影片（預乘）疊在 UI 上，邊緣有無黑邊或亮邊；與預期（預乘 source-over）的差異。Scaleform 把這張貼圖當直通還是預乘 alpha（讀碼＋像素）。
5. **貼圖重建**：`cev_allowPlanar` 與此無關（沒有消費端時固定 RGBA8）；改以換播另一支不同解析度的影片觸發重建。Flash 端變成什麼（舊畫面、空白、崩潰）；重新呼叫 `LoadTexIntoMc` 後是否恢復；新舊 id 是否相同。
6. **引用與銷毀**：Scaleform 是否對引擎貼圖持有引用（讀 `GImageInfoTextureXRender`、`InitTextureFromTexId`）。播放器銷毀而 Flash 仍顯示時發生什麼（黑、白、殘留、崩潰）；先 `off` 再銷毀是否乾淨。`sys_asserts 1` 下的斷言列出。
7. **排列**：若同一個 Surface 之後有 UI 消費端要求打包排列（以 `cev_probe_fullscreen surface:<id>` 讓它切成打包），Flash 端顯示什麼（預期是錯的畫面——照實記錄，這決定成品要不要把「被 Flash 使用」當成只接受 RGBA8 的消費端）。
8. **讀取畫面**：Flash 元件在關卡讀取期間仍顯示時（`gfx_loadtimethread 1`，以 `cev_probe_loadtime` 模擬 8 秒讀取），影片是否繼續前進、有無崩潰或死鎖。做不到（元件在讀取中不顯示）就寫明原因，不強求。
9. **成本**：1080p RGBA8 影片經 Flash 顯示時 `cev_stats` 的轉換與上傳時間；與全螢幕打包路徑同一支影片的對照（只記數字）。
10. **多個**：同一張貼圖載入兩個 MovieClip、兩個播放器各載入一個，是否都正常。

任何崩潰、卡住的執行保留並列出，不要只重跑。

## 規則

同 L3a（啟動方式；社群版樹腳本不用 `quit`；每次執行都有逾時；樹忙碌檢查；只關自己啟動的行程；不碰共用樹；不 `git add -A`、不 `git stash`、不推送；英文程式與提交訊息）。提交：一個（只有探測指令），訊息帶 `S1`，結尾 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。證據放 `E:/coding3/xt-bench/cev-s1/`。

報告（繁體中文、精簡，寫在最後訊息裡）：用了哪個 UI 元件與 MovieClip、測試專案怎麼組的；十題各自的結論與證據；讀碼得到的 Scaleform 端事實（檔案:行）；做成品功能時必須處理的事項清單；崩潰或卡住的執行；沒做到的。

## 補充（2026-11-27，使用者裁決後）

- 社群版樹沒有 Scaleform 執行時（`CryScaleformHelper.dll` 載入失敗）。使用者同意測試：從安裝版引擎 `S:/Crytek/cryengine-57-lts/5.7.1/bin/win_x64/` **複製** `CryScaleformHelper.dll`（以及引擎載入時實際要求的其他 `CryScaleform*.dll`，以 log 為準，只複製需要的）到社群版樹的 `bin/win_x64/`。S: 的安裝版唯讀。複製了哪些檔、SHA-256 記在 `E:/coding3/xt-bench/cev-s1/scaleform_dlls.txt`。這些是專有檔：確認它們不在社群版樹的 git 追蹤範圍內（`git -C <樹> status --short -- bin` 照實記錄；若會被當成未追蹤檔列出，照實寫，不要改該樹的 `.gitignore`），**測完留在原處**（使用者之後要自己看），在報告列出以便日後移除。
- 載入即崩潰或 Scaleform 仍初始化失敗（與 fork 不相容）：保留 log，移除複製的檔，停下回報。
- 使用者的原則：**這條路只需要做到可以播放與控制，不為效能花心思**。因此第 9 題（成本）刪除；第 8 題（讀取畫面）降為「順手能測就測」。其餘題目不變。
