# M5 手動驗收清單：FFmpeg 轉檔（EditorCEVPlayer）

> 2026-11-19 註記：本清單從未執行。之後的變動：FFmpeg 路徑改存在編輯器偏好設定頁 `CEVPlayer/FFmpeg`（`editor.json` 只在第一次啟動讀入一次），Info 欄的 FFmpeg 區塊只顯示狀態與「Open preferences」；最小版已發佈並釘進程式（`ffmpeg-minimal-v8.0.3-cev1`），下載鍵可用。與本清單衝突處以 [`manual-acceptance.md`](manual-acceptance.md) 第 5～6 節為準。

對象：官方版 CRYENGINE 5.7.1（`D:/coding11/Cryengine_5.7.1`，Profile 建置）。自動化冒煙測試（`docs/design/M5-ffmpeg.md` §12）證明「偵測、轉檔、變體、批次、取消、拒絕訊息、不崩潰」；以下項目需要人眼確認對話框、操作手感與畫面。M4 清單（`manual-acceptance-M4.md`）仍然適用；本清單只列 M5 新增的部分。建議總時間約 20 分鐘。

## 0. 準備

1. 建置並部署（同 M4 §0）；Sandbox 用 `D:/coding11/_testproject/CEVTest` 開啟。
2. 系統上要有一個**完整版** ffmpeg（例如 `winget install Gyan.FFmpeg`）。**第一輪請讓它不在 PATH 上**，才看得到「找不到 → 詢問」的流程。
3. 測試媒體（repo 外的 `D:/coding11/_testproject/src_media/`）：`m5_h264.mp4`（H.264 + AAC 1080p 5 秒）、`m5_prores4444.mov`（帶 alpha）、`batch/`（b1.mp4、b2.mkv、b3.avi）、`png_seq/`（`frame_1000.png` 起算的 PNG 序列，帶 alpha）。可用 `ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=30:duration=5 ...` 自己產生。
4. 設定檔：`%LOCALAPPDATA%\CEVPlayer\editor.json`（只有 `ffmpegPath`）。測試前可先刪除它。

## 1. FFmpeg 區段（資訊欄）

1. 開 `Tools` → `CEV Video Editor`，右欄捲到 **FFmpeg** 區段。
2. 完全找不到 ffmpeg 時：顯示橘字 `FFmpeg not available: ...`，說明「只有需要 FFmpeg 的操作被停用」；`Download minimal build` 按鈕是**灰的**，工具提示與下方一行寫著「尚未發佈最小版，請手動選擇 ffmpeg」（目前釘住的版本是 `unreleased`）。
3. 按 `Locate...` 選一個**不是 ffmpeg 的檔案**（例如 `notepad.exe`）：顯示無法使用的原因（`Not usable: ...`）；選一個**缺少 libvpx-vp9 / libopus 的 ffmpeg**（若有）：列出 `missing encoder:...`。
4. 選正確的 `ffmpeg.exe`：綠字 `FFmpeg ready: <版本> (configured)`，下方有路徑、缺少的建議項與匯入項（完整版應為 nothing）。
5. 關閉再開啟 Sandbox：路徑被記住（`editor.json`），不需要再選；`Re-detect` 重新偵測。
6. 把路徑改成不存在的檔案，`Re-detect`：變橘字，說明原因；PATH 上若有可用的 ffmpeg，會改用它（來源 `system`）。

## 2. 匯入非 WebM（拖放）

1. FFmpeg **不可用**時，把 `m5_h264.mp4` 拖進 Asset Browser：引擎匯入對話框 → 「Import video」對話框；FFmpeg 群組顯示橘字「FFmpeg is needed ...」與 `Set up FFmpeg...`。按 OK：彈出**詢問視窗**（找不到 + 原因 + 只有 `Choose ffmpeg.exe...` 與 Cancel；下載被停用的原因寫在視窗裡）。選 Cancel：回到對話框，沒有產生任何檔案。
2. 選好 ffmpeg 後重做：對話框 FFmpeg 群組為綠字；設 `Quality (CRF)` 34、`Output height` 保持、勾選 540p 與 360p，勾選 Export a WAV，OK。
3. 右欄 Jobs 清單出現 `Import m5_h264`，有進度；可按 `Cancel selected job`（見 §5）。完成後 Asset Browser 出現資產。
4. 資料夾內：`m5_h264.webm`（轉出的）、`m5_h264_source.mp4`（原檔複本）、`m5_h264_540p.webm`、`m5_h264_360p.webm`、`m5_h264_<語言>.wav`、`.cevvideo`、`.cevvideo.cryasset`。
5. 開資產：預覽正常、聲音正常；資訊欄 **Variants** 表列出兩個變體（尺寸、`up to date`、檔案大小）；Files 表的 `m5_h264_source.mp4` 角色是「source copy - original file, not shipped」；`.cevvideo` 用文字編輯器看到 `<Variants rule="smallestNotBelowOutput">`。
6. 單獨拖入 **PNG 檔**到 Asset Browser：**不會**被影片匯入器接走（仍是貼圖）。
7. 把 `png_seq` **資料夾**拖進 CEV Video Editor 視窗：出現 PNG 序列的匯入對話框（有 Frame rate），OK 後得到 `png_seq` 資產；資訊欄來源列寫 `PNG sequence ... (not copied into the project)`。
8. 一次把 `batch/` 內三個檔案拖進 CEV Video Editor 視窗：只出現**一次**對話框（資產名稱灰掉，有 `Replace assets that already exist`）；三個作業**依序**（一次一個）跑；結束後右下通知與 log 有逐檔結果摘要。

## 3. alpha

1. 匯入 `m5_prores4444.mov`：預覽的棋盤格透出（半透明），資訊欄 `Alpha: yes`。
2. 匯入 `png_seq`：同上。
3. 匯入 `m5_h264.mp4`：`Alpha: no`。

## 4. 變體與重新壓縮（資訊欄 Operations）

1. `Generate variants...`：選 720p，OK：Jobs 有進度，完成後 Variants 表多一列，`.cevvideo` 同步；已存在的變體不動。
2. 高度不低於影片高度的選項是灰的（影片 1080p：1080 / 1440 / 2160 灰）。
3. `Re-compress / resize...`：CRF 40、高度 720，現有變體預先勾選，OK。完成後：主影片變小（Files 表大小），解析度 1280x720（資訊欄），**GUID 不變**（`.cryasset` 開頭的 `Guid`），Details 的 `importSettings` 有 `crf=40;height=720`，Variants 表只剩 360 / 540 中**低於 720** 的項目，皆為 `up to date`。
4. 做完 §4.3 之後再做一次 `Re-compress / resize...`（CRF 36、高度保持）：它會從 `m5_h264_source.mp4` 重新編碼（不是從上一次的輸出），畫面不會比第一次更糊。
5. 舊的 M4 資產（`.webm` 複本、沒有 `_source`）按 `Re-compress / resize...`：Log 顯示「the current video becomes the source copy」，產生 `<名稱>_source.webm`。
6. 原始檔改過後按 `Reimport source`（轉檔過的資產）：Jobs 重新轉檔（原檔複製過來 + 以儲存的設定重編）。
7. Variants 表的 `stale` 標示：重新壓縮但**不**重做某個變體（改用 console `cev_editor_recompress ... ` 預設會重做；用 `Re-compress` 取消勾選會移除）。若手動把主影片換成較新的檔，變體應顯示橘字 `stale: older than the main video`。
8. 重新壓縮進行中（預覽開著）不得出現檔案被占用的錯誤；完成後預覽自動重載新影片。

## 5. 取消與關閉

1. 匯入 `m5_long.mp4`（約 40 秒 1080p）時按 `Cancel selected job`：數秒內結束，Jobs 顯示 cancelled，資料夾裡**沒有**半成品（沒有 `.tmp`、`.stage.webm`、沒有 `.cryasset`）。
2. 匯入進行中直接關閉 Sandbox：數秒內關閉、沒有崩潰、沒有殘留 ffmpeg 行程（工作管理員）、沒有半成品。
3. 批次進行中取消（console `cev_editor_import_cancel`）：目前這個取消，後面排隊的全部丟棄，摘要寫明。

## 6. Console 與設定（可選）

`cev_editor_ffmpeg_status`、`cev_editor_ffmpeg_path <路徑|->`、`cev_editor_import <來源> <資料夾> crf:32 height:720 variants:540,360`、`cev_editor_import_batch <資料夾或清單檔> <資產資料夾>`、`cev_editor_variants`、`cev_editor_recompress`：行為見 `docs/design/M5-ffmpeg.md` §11。

## 7. 遊戲端

GameLauncher 載入匯入的資產（`cev_play Videos/m5test/m5_h264.cevvideo`）：`cev_variant 0` 播主影片、`cev_variant 1` 播第一個變體、`-1`（自動）在視窗化時播主影片；log 的 `<Videos/...webm>` 對應所選檔案。
