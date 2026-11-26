# V1 · 並用驗證：XUI2DL 與 XTFrame 裝在同一個渲染器（2026-11-27）

模型：Opus。倉 `E:/coding crycry/CEVPlayer-rebuild-integration`，分支 `xui2dl`，**不推送**。這是**驗證單**：不改倉內任何程式（發現 `apply_xui2dl.py` 或 XUI2DL 的問題 → 記下、回報，不修）。在**共用樹** `E:/coding crycry/CRYENGINE_Source_CEVPlayer_XTFrame` 進行，結束時**必須還原**到開始時的狀態。XTFrame 主線已同意這個時段（期間它不建置、不換入）。

目的：使用者要求推送前確認「XUI2DL 與 XTFrame 互不依賴，可單獨運作，也可一起運作」。單獨運作已各自驗過；這張單驗「一起」。

## 不可碰

`RenderDll/XUI2D/`、`IXUIRenderer.h`、`Engine/Shaders` 下的 `XUI2D.cfx`、`Code/CryPlugins/CryXTUI`、`EditorXTUI`；`CryXTUI.dll`（eea9e46b25db55765057f01cfc542529ef036a07696c7ff715448baa9df61152）與 `EditorXTUI.dll`（d6d6b605ff9b72fe474067cf31b2ddea8d645c008b1bed1c91ebc076860e882d）不重建，前後核對。`Code/CryPlugins/CEVPlayer` 與 `CEVPlayer.dll` 這張單也不動（現為 L7b 狀態，3000ba2e…）。

## 步驟

1. **備份**（`E:/coding3/xt-bench/cev-v1/backup/`，附 SHA-256）：`bin/win_x64/CryRenderD3D11.dll` 與 `.pdb`（DLL 應為 aab72c2b9422b7b12378dc71a46cb0f1a25050f8db415b7b50b8fb00d8c78e20）；`apply_xui2dl.py` 會改到的每一個引擎檔（先以 `--check` 與讀腳本得知清單）；CMake 產生、會因重跑而改變的 solution／專案檔（至少渲染器的 `.vcxproj`、`.vcxproj.filters` 與相關的產生清單——先找出重跑 CMake 會動到哪些，備份它們）。
2. **套用**：`python EnginePatch/apply_xui2dl.py <共用樹>`。記錄完整輸出。**主線最想知道的**：每一個插入點是否乾淨（錨點唯一、沒有落在 XTFrame 補丁 01～19 改過的同一段）；逐一列出檔名、行號、插入點前後是否緊鄰 XTFrame 的改動（以該樹的 `// XTUI`／補丁標記或與官方 5.7.1 的差異判斷）。讀取迴圈那組掛點應整組略過。
3. **建置**：重跑 CMake（該樹既有的設定方式，見 `docs/tasks/L5.md` 與 README），只建 `CryRenderD3D11`。編譯或連結失敗 → 保留 log，直接跳到步驟 6 還原，回報。
4. **並用驗證**（測試專案與驅動沿用 `E:/coding3/xt-bench/cev-l7b/`、`cev-l6/project`；讀回加 `r_MouseUseSystemCursor 1`）：

| 項目 | 判定 |
|---|---|
| V1 後端選擇 | `cev_info`：`fullscreen backend: xui2d … loop=1`；log 的 `XUI2D_Patched` 仍為 0x3ff |
| V2 XUI2DL 不佔資源 | 整次執行（含全螢幕播放與一次關卡載入）log 沒有 `[XUI2DL] render resources created`；`cev_probe_xui2dl stats` 顯示沒有建立資源、`layersHeld 0`、`framesDrawn 0`（若該指令在 XUI2D 後端下會改變輸出路徑，只呼叫 `stats` 子指令） |
| V3 XUI2DL 的 api 存在但不被使用 | log 顯示渲染器匯出 `XUI2DL_GetApi`（插件探測到 api）且 caps 不含 `LOADTIME`(4)；插件仍選 XUI2D |
| V4 全螢幕與讀取 | L7b 的 H1（連續 map 三次）、H3（與 XTFrame 讀取 UI 並存兩次載入）：判定與 L7b 相同 |
| V5 XTFrame 的自測 | `xtui_planar_selftest` 26／0、`xtui_source_selftest` 197／0、讀取迴圈分層（cvar `xtui_loadmode_selftest_layer`）24／0。跑法照 `E:/coding3/xt-bench/ce-118/cfg/`、`scripts/`、`runs_shared/`（唯讀；說明在 `E:/coding3/CryengineXTUI/CE_XTUI/docs/08` 的 CE-117、CE-118 兩節與 `tools/integration/README.md`）。數字與基準不同 → **不修**，記現象與 log 路徑 |
| V6 CEVPlayer 回歸 | L5 的 X3、X4 暫停幀與 L7b 逐位元組相同；自測 70／65／248；`sys_asserts 1` 下斷言逐一列出（已知：`DriverD3D.cpp:3267`、`ResFile.cpp:508`） |
| V7 強制走 Lite（選做，有現成開關才做） | 若插件有不重編就能停用 XUI2D 後端的方式則測一次 Lite 在此樹能畫；沒有就標「未測」，不要為此加開關 |

5. **XTFrame 自測結果以外**的任何異常（崩潰、XUI2D 的 assert、畫面錯）：保留 log，照實寫。
6. **還原**（無論成功或失敗都要做）：`apply_xui2dl.py <共用樹> --remove`；`--remove` 不刪複製進去的檔，另行刪除 `Code/CryEngine/RenderDll/XUI2DL/` 與 `Engine/Shaders/HWScripts/CryFX/XUI2DL.cfx`（只刪這次由腳本複製進去的檔，先確認開始前它們不存在）；被改的引擎檔逐檔與備份 `cmp` 相同；換回備份的 `CryRenderD3D11.dll`／`.pdb` 並核對 SHA-256 為 aab72c2b…；CMake 產生檔回復到備份（或重跑 CMake 後確認渲染器專案不再含 XUI2DL 項目）——**列出任何殘留**（著色器快取裡的 XUI2DL 項目可留，照實列）。最後啟動一次確認：`cev_info` 的後端仍是 xui2d、log 無 XUI2DL 字樣。

## 規則

同 L5／L6（共用樹的啟動方式；樹忙碌檢查；只關自己啟動的行程；每次執行有逾時，卡住或崩潰的保留並列出）。不提交任何東西（這張單沒有程式變更）。證據放 `E:/coding3/xt-bench/cev-v1/`。

報告（繁體中文、精簡，寫在最後訊息裡）：套用腳本的完整結果與每個插入點相對於 XTFrame 改動的位置；建置結果；V1～V7 的判定與證據（註明像素、計數或讀碼）；XTFrame 三組自測的數字；還原的逐項核對與殘留清單；任何異常；沒做到的。

## V1b · 最終版插件在共用樹的編譯與回歸（2026-11-27）

模型：Opus。V1 已通過且共用樹已還原。這一步只做：把倉 `xui2dl` 的 HEAD 的插件原始碼同步到共用樹（逐檔 `cmp`）、只建 `CEVPlayer`（Profile）、換 `CEVPlayer.dll`（備份現有的 3000ba2e… 到 `E:/coding3/xt-bench/cev-v1b/backup/`）。不改倉、不提交、不碰 XTFrame 的任何檔與成品（三個 DLL 的 SHA-256 前後核對，同 V1）。共用樹沒有 Scaleform 執行時，Flash 掛點只驗「被拒且不崩潰」。

| 項目 | 判定 |
|---|---|
| W1 編譯 | 0 錯誤；警告照實列（含 `CEV_HAVE_XUI2D` 路徑與 S2 的 Flash 掛點程式） |
| W2 後端 | `cev_info`：`fullscreen backend: xui2d planar=1 loop=1` |
| W3 自測 | `cev_apitest` 72、`cev_flowtest` 69、`cev_contract_selftest` 248；`sys_asserts 1` 下斷言逐一列出（已知 `DriverD3D.cpp:3267`、`ResFile.cpp:508`） |
| W4 全螢幕與讀取 | L7b 的 H1、H3；L5 的 X3、X4 暫停幀與 L7b 逐位元組相同 |
| W5 XTFrame 自測 | planar 26／0、source 197／0、分層 24／0（跑法同 V1；數字不同只記不修） |
| W6 Flash 掛點 | `cev_attach_flash` 在沒有 Scaleform 的這棵樹上被拒、有 log、不崩潰；之後播放正常 |
| W7 Surface 經 XTFrame UI | L5a 的做法取一項：一支 4:2:0 影片經 UI 顯示為打包排列、畫面正確（像素） |

證據放 `E:/coding3/xt-bench/cev-v1b/`。報告繁體中文、精簡，寫在最後訊息裡：W1～W7 的判定與證據、最終 `CEVPlayer.dll` 的 SHA-256、任何異常。共用樹結束時留著新的 `CEVPlayer.dll` 與同步後的插件原始碼（這是預期的最終狀態）。
