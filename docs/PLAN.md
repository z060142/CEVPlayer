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
| M0 獨立倉、SDK、patch 01、腳本、兩樹編譯舊插件 | 進行中 | |
| M1 執行期核心（解碼層、渲染橋、使用端、計數、查詢接口） | 未開始 | |
| M2 快速路線聲音與腳本綁定 | 未開始 | |
| M3 規範路線（CryAudio 位置查詢，optional patch） | 未開始 | |
| M4 資產與編輯器 | 未開始 | |
| M5 FFmpeg 轉碼 | 未開始 | |

## 決策紀錄

- 2026-11-16 分支名用 `rebuild`。
