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
| M2 快速路線聲音與腳本綁定 | 完成，審查中 | M2a：聲音時鐘、重取樣、.cevvideo、語言、標記點、禁用內嵌聲音；M2b：描述表（36 項）、29 個 FlowGraph 節點、Schematyc 元件、CryVS（只在 CECE 編譯，未實跑） |
| M3 規範路線（CryAudio 位置查詢，optional patch） | 進行中 | 設計：docs/design/M3-cue-route.md |
| M4 資產與編輯器 | 未開始 | |
| M5 FFmpeg 轉碼 | 未開始 | |

## 決策紀錄

- 2026-11-16 分支名用 `rebuild`。
- 2026-11-16 M0：repo 內 CMake 模組放 `cmake/modules/`（不用 `Tools/CMake/`）。patches 以 CRLF 產生並用 `.gitattributes` 排除換行正規化；`apply_patches.sh` 以去 CR 後的 blob hash 辨識引擎樹。
- 2026-11-16 CECE 整樹編不過（fork 本身問題）→ 依使用者指示以官方版為主線開發，CECE 只建 CEVPlayer 目標驗證。
- 2026-11-16 本機 VS 沒裝 ATL/MFC 且無法提權安裝 → 由 VS 套件解出到 D:/coding11/_tools/atlmfc，經 baseline_compat.props 引入（只有 Release MFC）。
- 2026-11-16 依使用者要求，CEVPlayer 的 5 個第三方 SDK 集中到 `Code/SDKs/CEVPlayer/`（偏離需求單的 `Code/SDKs/<名稱>`）。
- 2026-11-16 CE 插件預設無 /EHsc；CEVPlayerCore 單獨開 /EHsc，例外不跨出 Core API。
