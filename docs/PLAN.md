# CEVPlayer rebuild — 專案追蹤

需求單：https://claude.ai/artifact/GxnbGNmndgCzmTodnTqS9r
分支：`rebuild`（從 master 5e91493 開出；第一個 commit cb2853d 套用 CECE e1326cf 差異）

本輪驗收底線：兩棵引擎樹都能編譯、插件載入與播放不崩潰。無法在本機實際觀看的效果（畫面、同步）記為「未驗證」。

## 環境（本機）

| 項目 | 位置 / 狀態 |
|---|---|
| 官方 5.7.1 | `D:\coding11\Cryengine_5.7.1`（86736a0） |
| CECE | `D:\coding11\CryENGINE-5-7-community`（e1326cf） |
| CE SDK 包 | `CRYENGINE_v5.7.0_SDKs.zip` 取自 `CRYTEK/CRYENGINE_ReadMe` release `5.7.0`（原腳本的 `SDKs` tag 已 404） |
| CMake | 引擎自帶 3.22：`Cryengine_5.7.1/Tools/CMake/Win32/bin/cmake.exe` |
| VS | 2022 17.14 |

## 里程碑

| 里程碑 | 狀態 | 備註 |
|---|---|---|
| M0 獨立倉、SDK、patch 01、腳本、兩樹編譯舊插件 | 編譯完成，冒煙測試待做 | 官方版整樹建置 0 錯誤；patch 01 + sync 後舊插件在官方版與 CECE 都建置成功，apply_patches 重跑為冪等。CECE 整樹有 fork 本身的 4 組錯誤（AssimpSDK、HPi/LodGenerator 缺 XT、GameSDK unity 重複定義），見 docs/research/02-baseline-build.md；依使用者指示以官方版為主線，CECE 只驗證 CEVPlayer 目標。 |
| M1 執行期核心（解碼層、渲染橋、使用端、計數、查詢接口） | 完成（第一階段路徑） | Core 自測 6000+ 項、模糊測試；CE 橋接冒煙 PASS；兩輪獨立審查已修。第一階段限制：全螢幕走 aux（最上層、直通 alpha 有黑邊）、CPU 轉色、載入畫面期間不推進。 |
| M2 快速路線聲音與腳本綁定 | 完成 | M2a：聲音時鐘、重取樣、.cevvideo、語言、標記點、禁用內嵌聲音；M2b：描述表（36 項）、29 個 FlowGraph 節點、Schematyc 元件、CryVS（只在 CECE 編譯，未實跑） |
| M3 規範路線（CryAudio 位置查詢，optional patch） | 完成 | optional patch 02（CryAudio 播放位置 + SDL_mixer 實作）；官方版套用後整樹 0 錯誤；cue 時鐘與 trigger 位置差 ±7 ms；未套 patch 退回影片時鐘 |
| M4 資產與編輯器 | 完成（畫面待手動驗收） | EditorCEVPlayer：資產類型、WebM 匯入、三欄視窗、QViewport 預覽、離線處理（分離音軌／WAV／Ogg，對齊 ≤0.3 取樣）、ACE 註冊、資產圖示、打包工具；手動驗收清單 docs/manual-acceptance-M4.md |
| M5 FFmpeg 轉碼 | 完成（UI 與 CI 待驗證） | 偵測／下載（等 release 才開放）／非 WebM 匯入／壓縮與解析度／多解析度子檔／批次；Sandbox 冒煙全過。未驗證：CI 實跑、精簡版 FFmpeg、實際網路下載、UI 手動操作（docs/manual-acceptance-M5.md） |
| 階段 0＋M6～M10 經由 UI 播放 | 階段 0、M6 完成 2026-11-19（M6a 登錄表與契約、M6b 節點整併、M6c CryVS）；M7 以後待做 | 設計在 docs/design/M6-ui-source.md：Surface ID 與登錄表、貼圖來源契約（不共用標頭）、舊節點刪除與兩個抽屜（M6）；可見性／變體／`open`（M7）；打包平面直通（M8）；Media Foundation 硬解，CPU 為 fallback，可選（M9）；關卡讀取期間推進等 XTFrame 主線的機制（M10） |

## 決策紀錄

- 2026-11-16 分支名用 `rebuild`。
- 2026-11-16 M0：repo 內 CMake 模組放 `cmake/modules/`（不用 `Tools/CMake/`）。patches 以 CRLF 產生並用 `.gitattributes` 排除換行正規化；`apply_patches.sh` 以去 CR 後的 blob hash 辨識引擎樹。
- 2026-11-16 CECE 整樹編不過（fork 本身問題）→ 依使用者指示以官方版為主線開發，CECE 只建 CEVPlayer 目標驗證。
- 2026-11-16 本機 VS 沒裝 ATL/MFC 且無法提權安裝 → 由 VS 套件解出到 D:/coding11/_tools/atlmfc，經 baseline_compat.props 引入（只有 Release MFC）。
- 2026-11-16 依使用者要求，CEVPlayer 的 5 個第三方 SDK 集中到 `Code/SDKs/CEVPlayer/`（偏離需求單的 `Code/SDKs/<名稱>`）。
- 2026-11-16 CE 插件預設無 /EHsc；CEVPlayerCore 單獨開 /EHsc，例外不跨出 Core API。
- 2026-11-16 M4 使用者決定：WAV/OGG 放影片資產資料夾內；原始影片複製進 Assets；預覽先用 QViewport，不穩定再改 XTFrame 子視窗。資產圖示以 image summon 產生（WEBM 字樣）。
- 2026-11-16 依使用者指示推上 GitHub（rebuild 分支）。
- 2026-11-19 使用者：經由 UI 播放為主線；舊節點全部刪除不留別名、新節點分「開箱即用／進階」兩個抽屜；硬解用 Media Foundation、CPU 解為 fallback 且可選；GPU 轉色由 UI 端（XTFrame）負責，CEVPlayer 不寫自己的著色器；素材只吃 WebM；渲染器 patch 先不做（硬解先走讀回並量測）；UI 的影片播放全部交給播放來源（XTFrame 主線裁決，CE 倉 docs/09 §9.10）。
- 2026-11-19 精簡版 FFmpeg：使用者要求在本機實際建出來（「哪樣好用哪樣用，這個不急」）。做法：沿用 `tools/ffmpeg/build_minimal.sh` 與釘死版本的發行包（可重現、授權檢查已寫好），不另外 clone 開發分支；本機沒有 MSYS2，以 scoop 裝到使用者目錄。排在階段 0 與 M6 之後、CPU 有空時做；發佈 release 與把雜湊釘進編輯器另外再問。
- 2026-11-19 編輯器兩項待排（使用者提出，尚未定順序）：Info 欄回歸引擎機制（重新匯入、偏好設定頁、引擎對話框、屬性面板、音訊觸發器選擇器、通知中心）；預覽改為 Core 解碼＋一般 Qt 元件繪製，不經引擎渲染器（描述檔邏輯搬進 Core 共用）。調查結論見 2026-11-19 的對話記錄，動工前寫成設計文件。
- 2026-11-19 使用者：建置中間檔放 RAM 碟 `T:`（32 GB，與 XTFrame 主線共用；重開機即清空），工作樹完成要清掉，成品不放 T:。本倉的用法：Core 獨立建置與自測、插件目標的中間檔、精簡版 FFmpeg 的工作目錄放 `T:/cev/<用途>/`（做完刪）；引擎整樹的建置目錄太大、放不進去，留在 SSD 並盡量只做增量建置；量測結果與要留的產物放 SSD。
- 2026-11-19 社群版引擎：`E:\coding crycry\CryENGINE-5-7-community`（GitHub main e1326cf），`Code/SDKs` 以目錄連結指向使用者提供的 `E:\coding2\CryENGINE-5-7-community\Code\SDKs`（含 AssimpSDK、XT_13_4——使用者：「之前編不過應該是 SDK 的問題」）。使用者本機 `E:\coding2` 那份在較新的提交 e881698（CryVS 更新，未推上 GitHub），CryVS 節點對哪一份做待確認。開工等 XTFrame 主線通知（使用者 2026-11-19）。
- 2026-11-19 M6 完成後的已知缺口：CryVS 腳本的實際執行未驗（腳本只能由 CryVS Designer 建立並在存檔時編譯；步驟在 M6c 的報告，待人工驗收）；社群版引擎每次關機在 `C3DEngine::UnloadLevel` 崩潰（不載入 CEVPlayer 也一樣）、`IFramework::RefreshEnv()` 會觸發 fork 自己的致命斷言；整合引擎樹的 CryXTUI Direct 模式不畫 `engine://`、關機偶發存取違規（已通知 XTFrame 主線）；引擎主控台 `exec` 四行以上含 `wait_seconds` 的 cfg 會觸發 `CryString.h:734` 斷言（引擎問題，測試改用 `cev_run_script`）。FlowGraph 未在執行期驗的：PlayOnMaterial 成功路徑、Autoplay、Skipped／Marker 輸出、Schematyc 元件、編輯器內的節點外觀。
