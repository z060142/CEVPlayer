# M6a · Surface ID 登錄表、介面版本 5、tick 不假設執行緒、貼圖來源契約（query／hint 記錄）

對應：`docs/design/M6-ui-source.md` §3（契約全文與規則 1～10——照抄宣告，不得改欄位順序）、§4.1、§9（最後一點）、§10（`generation`、同名重建同 id）。模型：Opus。工作樹 `E:/coding crycry/cev-wt-m6a`，分支 `m6a`。引擎樹 `E:/coding crycry/CRYENGINE_Source_CEVPlayer_XTFrame`（Profile，已建置）。**不合併、不推送**；主模型收報告後合併。

動手前：以程式核對本單的每一條前提（檔名、函式、行為）；不符就停下回報，不自行改設計。先寫會失敗的測試。

## 要做的事

1. **登錄表**（`Module/Player/PlayerManager.*`）：`id → player`，不分大小寫；ID 規則 `[A-Za-z0-9_.:-]`、1～63 字元。`CreatePlayerForSurface(id)`：ID 已存在回 `nullptr` 並記錯誤（不是警告）；`FindPlayerBySurface(id)`。有 ID 的播放器其引擎貼圖名是 `$cev:<id>`（內部細節，尺寸改變、換檔、換語言、換變體都不變）；沒有 ID 的播放器維持現行自動命名。播放器銷毀時移除登錄。
2. **介面**（`Interface/CEVPlayer/IVideoPlugin.h`、`IVideoPlayer.h`，只在尾端附加虛擬函式；`kVideoPlayerInterfaceVersion` 4 → 5）：`IVideoPlugin::CreatePlayerForSurface`、`FindPlayerBySurface`；`IVideoPlayer::GetSurfaceId()`、`GetGeneration()`（uint32，貼圖 id／尺寸／排列改變時＋1——由播放器自己遞增，不以 id 是否改變判斷）、`GetPixelLayout()`（目前恆為 RGBA8）、`GetDecodeBackend()`（目前恆為 CPU／libvpx）。標頭註解與 `docs/design/M1-runtime-core.md` 裡「CryXTUI 引用本標頭」的句子改為：其他插件以 `docs/design/M6-ui-source.md` §3 的契約對接，本標頭給直接連結 CEVPlayer 的遊戲程式。既有的 `CreatePlayer(textureName)` 保留（本單不刪舊東西——那是 M6b）。
3. **tick 不假設執行緒**（§9 最後一點）：`CVideoPlayerImpl::Update`／`CPlayerManager::UpdateBeforeRender` 改為由呼叫者傳入 dt 的單一入口（今天的呼叫者仍是 `CPlugin::UpdateBeforeRender`，dt 取自現行的時間來源——行為不變）；入口內不直接讀 `gEnv->pTimer` 或主幀編號。只限主執行緒的引擎服務（entity／flow／Schematyc 事件派送、開檔）集中在可辨識的幾處並加註解標示「main thread only」，不必在本單搬動。不得改變今天的播放行為與事件順序。
4. **契約的匯出**（新檔 `Module/Contract/TexSource.{h,cpp}`；宣告照 §3 逐字，放在本倉自己的標頭，不引用任何其他插件的檔案）：`extern "C" __declspec(dllexport) const CryTexSourceApi* CryTexSource_v1(void)`，`version` = 2。
   - `query`：只讀「快照」——每個有 ID 的播放器在自己 tick 的結尾寫一份（textureId、width、height、flags、contentSerial、generation、layout=RGBA8、state、errorCode、timeSeconds、durationSeconds、loopCount；平面相關欄位為 0）。`contentSerial` 在像素更換與 `generation` 改變時都增加、永不減少。依呼叫者的 `size` 只寫入其範圍內的欄位。
   - `hint`：只記錄到以 `(name, consumer)` 為鍵的表（可見、像素大小、acceptLayouts、release）；依 §3 規則 5 以「來源自己的更新次數 120」淘汰；`consumer`＝0 視為一個只接受 RGBA8 的匿名消費端。**本單不依 hint 改變任何播放行為**（那是 M7／M8），只提供 `cev_surfaces` 主控台指令列出每個 surface 的快照與聚合後的 hint。
   - `open`／`close`／`control`：本單回 0／不做事（M7 實作）；函式指標必須存在。
   - 規則 10：契約函式內不呼叫任何引擎 API；快照與 hint 表以一把輕量鎖保護（之後載入期由渲染執行緒呼叫）。插件關閉後這些函式仍可呼叫：`query`／`open`／`control` 回 0，`hint`／`close` 不做事（靜態儲存、關閉旗標）。
5. **主控台**：`cev_play_surface <id> <path> [loop]`（以 ID 建立並播放，供驗收用）；`cev_surfaces`。

## 測試

- 模組內自測（新指令 `cev_contract_selftest`，比照 `cev_apitest` 的作法）：以 `GetModuleHandle`＋`GetProcAddress("CryTexSource_v1")` 取得自己的匯出（不經內部捷徑），驗證：version 2；未知名字 `query` 回 0；建立 surface 後 `query` 回 1 且欄位正確；較小的 `size`（v1 大小）只寫 v1 欄位、其後的位元組不動；播放時 `contentSerial` 遞增、暫停時不動；換檔（不同尺寸）後 `generation`＋1 且 `contentSerial` 增加、引擎貼圖名不變；重複 ID 被拒；ID 不分大小寫；`hint` 的累計、`release`、120 次更新淘汰、`consumer`＝0；銷毀後 `query` 回 0。
- `cev_apitest` 仍 33 項全過；`tools/smoke_test.sh` 仍 PASS；Core 的 `cevcore_selftest` 仍過（Core 若有改動）。
- 引擎內實測（`xtui_direct 0`，因為 CryXTUI 的 Direct 模式目前不畫 `engine://`——§10）：`cev_play_surface intro <1080p 素材>`，以 `xtui_bind_texture` 把 `$cev:intro` 綁進測試 UI（階段 0 的測試專案與腳本在 `E:/coding3/xt-bench/cev-s0/engine/`，可沿用），截圖存證；換檔成不同解析度後不重新綁定，再截圖（預期：CryXTUI v0.17 只在綁定時解析一次，畫面可能不更新——照實記錄，這是消費端要做的部分，不是本單的失敗）。

## 規則

- 建置：以 `tools/sync_sources.sh` 的等效方式只同步改過的檔進引擎樹（不要讓 SDK 目錄被刪掉重抄）；新增原始檔需要重產方案時照 `Tools/Integration/Build.ps1 -ConfigureOnly`，只增量建 `CEVPlayer` 目標；不重建渲染器、不改引擎與 CryXTUI 的原始碼。獨立建置（Core）的中間檔放 `T:/cev/m6a/`，做完刪。結果與截圖放 `E:/coding3/xt-bench/cev-m6a/`。
- 啟動引擎前確認沒有不是自己啟動的 `Sandbox.exe`／`GameLauncher.exe`；關閉 XTUI surface 後再 `quit`（否則關機崩潰——§10）。量測與啟動用階段 0 的 `run_launcher.py` 做法（Popen＋`SetProcessAffinityMask`、逾時）；不用 `cmd /c start … > log`。背景指令 10 分鐘沒輸出就檢查並改前景限時重跑。
- 最多 8 個平行建置工作。不 `git add -A`、不 `git stash`、不推送、不合併。不動 `E:\\coding2`、`E:\\coding4`。不碰不是自己啟動的行程。
- 程式、註解、提交訊息英文；提交訊息帶 `M6a`、結尾 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。
- 報告（繁體中文、精簡）：改了什麼（檔案清單）、每項測試的結果與證據、與本單的偏離、沒做到與沒測到的、引擎樹最後的狀態。
