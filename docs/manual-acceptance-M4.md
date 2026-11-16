# M4 手動驗收清單：EditorCEVPlayer（Sandbox 影片資產編輯器）

對象：官方版 CRYENGINE 5.7.1（`D:/coding11/Cryengine_5.7.1`，Profile 建置）。自動化冒煙測試只證明「外掛載入、視窗能開關、匯入產生檔案、不崩潰」；以下項目需要人眼確認畫面與操作手感。建議總時間約 15 分鐘。

## 0. 準備

1. 建置並部署：`bash tools/sync_sources.sh /d/coding11/Cryengine_5.7.1`，重新 configure，建置 `EditorCEVPlayer`、`CEVPlayer`、`Sandbox`（Profile）。
2. 測試專案 `D:/coding11/_testproject/CEVTest`（`.cryproject` 已載入 CEVPlayer 外掛）。用 Sandbox 開啟：
   `Cryengine_5.7.1/bin/win_x64/Sandbox.exe -project D:/coding11/_testproject/CEVTest/CEVTest.cryproject`
3. 測試影片：`D:/coding11/_testproject/CEVTest/Assets/Videos/` 內有 `vp9_alpha.webm`（有 alpha）、`vp8_vorbis.webm`（有聲音）、`av_sync_flash_beep.webm`、`opus_preskip.webm`。**請先把要匯入的檔案複製到 Assets 以外的資料夾**（例如 `D:/coding11/_testproject/src_media/`），再拖進去，才看得出「複製進 Assets」的行為。
4. 查看 log：專案目錄的 `Sandbox` 啟動 log，搜尋 `[CEVEditor]`。啟動後應有 `[CEVEditor] plugin loaded`。

預期：Sandbox 正常啟動，沒有錯誤對話框。

## 1. 開啟工具視窗

1. 選單 `Tools` → `CEV Video Editor`（若找不到，檢查 log 是否有 plugin loaded）。
2. 視窗是三欄：左 = Asset Browser（只列影片資產）、中 = 預覽（棋盤格背景 + 傳輸列）、右 = 資訊與操作。
3. 嘗試把視窗拖成浮動、再拖回停駐、切換分頁各 3 次；預覽區不得變黑、不得凍結、Sandbox 不得崩潰。（log 會出現 `viewport created / destroyed` 成對的行；視窗重新停駐會重建預覽視口，這是預期行為。）
4. 關閉視窗再從選單重開 3 次。

預期：每次都能開、關；沒有崩潰；Windows 事件檢視器沒有新的顯示驅動錯誤（NVIDIA event 153）。

## 2. 匯入（拖放）

1. 在左欄 Asset Browser 先切到某個資料夾（例如新增 `Videos/m4test`）。
2. 從檔案總管把 `vp9_alpha.webm`（在 Assets 以外的資料夾）拖進 Asset Browser。
3. 先出現引擎的匯入對話框（輸出資料夾、型別勾選），按確定；接著出現「Import video」選項視窗：資產名稱、音訊語言、音訊路線（embedded / mixed / cue）、觸發器名稱、分離音訊三態、WAV、Ogg、品質；壓縮／解析度／子檔區為灰色（FFmpeg 階段才可用）。
4. 全部用預設值按 OK。

預期：
- 資料夾內出現：`vp9_alpha.webm`（原始檔的**複本**）、`vp9_alpha.cevvideo`、`vp9_alpha.cevvideo.cryasset`；Asset Browser 列出一個 Video 資產。
- 用文字編輯器開 `.cryasset`：`Details` 內有 `sourcePath`（相對路徑）、`sourceOriginalPath`（原始絕對路徑）、`sourceSha256`、`importSettings`、`width/height/duration/codec/alpha`。
- 選項視窗按 Cancel：不產生任何檔案。
- 也可以把 `.webm` 直接拖進 CEV Video Editor 視窗中間或右欄（等同匯入，資料夾 = 目前開啟資產的資料夾）。

## 3. 預覽：播放、拖曳、逐格、循環

1. 在左欄雙擊剛匯入的資產（或在 Asset Browser 選取後雙擊）。
2. 中欄應顯示影片畫面，右欄顯示資訊。
3. 按 `Play`：畫面播放，時間碼（`時:分:秒:格`）與滑桿隨之前進；按 `Pause` 暫停；按 `Stop` 回到 0 秒。
4. 暫停時拖曳滑桿：畫面即時跳到對應位置（不播放）。
5. 暫停時按 `|<` / `>|`：每次前進／後退剛好一格，時間碼每次變化 1 格（影片是 30 fps 或 25 fps 時以格為單位）。
6. 勾選 `Loop` 後播到結尾應自動從 0 重來；取消後結束停住。
7. `Fit` / `100%` 切換：100% 時影片以原始大小置中（影片比視窗大時維持縮放）。
8. 在 Asset Browser 雙擊另一個影片資產：預覽換成新影片，沒有殘留舊畫面；關閉工具視窗後 log 沒有錯誤。

預期：以上都能操作，無延遲卡頓、無崩潰。

## 4. 棋盤格與 alpha

1. 用有 alpha 的 `vp9_alpha.webm` 的資產：透明區域應透出灰白棋盤格（格子約 16 像素，淺灰／深灰）。
2. 半透明邊緣（羽化處）應該自然混合，不應出現「發黑的邊緣」（這是兩次繪製的預乘合成；若邊緣明顯變暗或發白請記錄截圖）。
3. 用沒有 alpha 的影片（`vp8_vorbis.webm`）：整個畫面不透明，棋盤格只在影片矩形外圍不出現（影片外是深灰背景）。
4. 影片上下方向正確（不能是倒的）。

預期：棋盤格與 alpha 正確；方向正確。若不正確，截圖並附 log 的 `preview draws texture id ...` 一行回報。

## 5. 資訊欄

開啟資產後右欄應顯示：
1. 標題 = 資產名稱；影片資訊：編碼（vp8 / vp9）、解析度、fps、長度、alpha、音軌（編碼、取樣率、聲道、起始偏移、codec delay）、路線。
2. 「檔案」表：每個檔案的角色（descriptor / video / source copy / sound file …）與狀態（大小或 `MISSING`）。刪掉資料夾裡的 `.webm` 後重開資產，該列應顯示 `MISSING`（紅字）。
3. 來源狀態（綠 = 最新）：把原始檔（Assets 以外的那一份）用別的影片覆蓋後重新開啟資產，應出現橘色 `Source updated, reimport needed`。按 `Reimport source` 後恢復 `Source up to date`，預覽換成新內容。
4. 描述檔編輯：改 `A/V offset`、增減語言列與標記點，按 `Apply to descriptor`，再 `Ctrl+S` 儲存；用文字編輯器開 `.cevvideo` 應看到變更；預覽重新載入。
5. 資產改名（Asset Browser 右鍵 Rename）：資料夾內所有以舊名開頭的檔案一起改名，`.cevvideo` 內的 `video=` / `file=` 同步改成新名；改名後雙擊仍能播放。

## 6. 操作按鈕（離線函式庫已接上）

按鈕在右欄「Operations」。長工作在背景執行，下方「Jobs」清單顯示進度，Log 顯示訊息，可 `Cancel selected job`。

1. `Export WAV`：資料夾內出現 `<名稱>_<語言>.wav`（48 kHz / 16 bit，第 0 個取樣 = 影片 0 秒）；檔案表出現該檔。
2. `Export Ogg`：出現 `<名稱>_<語言>.ogg`。
3. `Split audio to WebM`：出現 `<名稱>_<語言>.webm`（只有聲音）；描述檔語言列有 `file=`，路線變 `mixed`。
4. `Strip audio`：出現 `<名稱>_silent.webm`（無聲畫面），描述檔 `video=` 改指向它。
5. `Upgrade to cue route`（需要先有 WAV 或 Ogg）：
   - `Assets/audio/sdlmixer/assets/cev/<名稱>/` 內有 WAV／Ogg 副本；
   - `Assets/audio/sdlmixer/ace/cev_<名稱>.xml` 產生（每個語言 Play_/Pause_/Resume_ 三個 trigger）；
   - 描述檔 `route="cue"` 並寫入 trigger 名稱；
   - 開 `Tools` → `Audio Controls Editor`，應看到 `cev_<名稱>` 資料庫與 trigger（若 ACE 視窗事先開著，請先儲存 ACE 再執行）。**請確認 ACE 中事件連到的檔案路徑正確**（寫入的 `path="cev/<名稱>"` 沒有結尾斜線；若 ACE 顯示為遺失，回報 ACE 實際存檔的 path 寫法）。
   - 預覽「Audio: as authored」應由 CryAudio trigger 發聲（Profile 建置才有 `AUDIO_REFRESH`；Release 要重啟）。
6. 預覽下拉選單：`Audio: embedded track`（用影片內的聲音，經暫時描述檔）、`Audio: muted`。
7. 匯入時在選項視窗勾選 WAV / Ogg / 分離音訊 / 路線 cue：匯入完成後同樣自動執行上述操作，完成時右下角通知。
8. 刪除資產（Asset Browser 右鍵 Delete）：資料夾內所有檔案、`audio/sdlmixer/assets/cev/<名稱>/`、ACE 的 `cev_<名稱>.xml` 一併移除。

已知現象：引擎會替 Assets 下的每個 `.wav` / `.ogg` 自動建立 Sound 資產（資產內 WAV／Ogg 是檔案，同時也會冒出 Sound 資產）；影片資產的檔案表標示了這些檔案。影片資產改名、搬移、刪除時會一併移除這些 `<檔案>.cryasset`（引擎會替新名稱／新位置重新建立）。

## 6b. 審查後新增的行為（請人工確認）

1. **同名匯入**：匯入時把資產名稱改成一個已存在的資產名 → 出現「已存在，要取代嗎？」（預設否）；按否回到選項視窗；按是才覆蓋（保留該資產的其他檔案與 GUID）。拖進編輯器視窗的匯入走同一條路。
2. **未儲存的描述檔編輯**：在資訊欄改語言／標記點，按 `Apply to descriptor`（先不要存檔），再按 `Export WAV` 等操作。作業完成後你的編輯仍在，並出現通知「descriptor was changed on disk … merged」；Save 後兩邊的改動都在 `.cevvideo` 裡。
3. **表格編輯不被洗掉**：在語言表打字（未按 Apply）時，背景檢查（來源雜湊）完成不會把表格重設。
4. **作業中改名／刪除**：開始一個長的 `Export Ogg`，立刻在 Asset Browser 改名或刪除該資產 → 作業被取消、檔案不留殘骸、改名／刪除成功。預覽中的資產改名、搬移後預覽自動重新載入。
5. **搬移資產**：把資產拖到另一個資料夾 → 所有檔案一起搬，資訊欄的「source copy」路徑跟著更新，雙擊仍可播放。
6. **還原視窗**：開著 `CEV Video Editor`（含浮動）關閉 Sandbox，再啟動：視窗還原為空的（不會自動載入上次資產），之後雙擊資產可以預覽；上次的資產被刪除也不影響。
7. **關閉 Sandbox 時作業仍在跑**（匯入一支很長的影片並勾選 WAV + Ogg 後立刻關閉）：Sandbox 在一兩秒內退出，不卡住。

## 7. 主控台指令（自動化用）

在 Sandbox 主控台（`~`）：
- `cev_editor_import <原始檔絕對路徑> <Assets 內資料夾>`：不經 UI 匯入（可再加 `name=` `lang=` `wav` `ogg` `split` `strip` `cue`；腳本裡用 `name:` `lang:`，因為主控台把 `a=b` 當成變數設定；同名資產預設拒絕並記一行 log，加 `overwrite` 才覆蓋）。
- `cev_editor_rename <資產.cevvideo> <新名稱>`、`cev_editor_move <資產.cevvideo> <資料夾>`、`cev_editor_delete <資產.cevvideo>`：走 Asset Manager（與 Asset Browser 相同路徑）。
- `cev_editor_open <資產.cevvideo>`：開啟工具視窗並載入資產；不帶參數只開視窗。
- `cev_editor_close`、`cev_editor_status`（log 一行：視口建立／銷毀數、工作數）、`cev_editor_script <檔>`。

## 8. 回報格式

每項標記 通過／失敗；失敗請附：重現步驟、截圖、`[CEVEditor]` 開頭的 log 行、Windows 事件檢視器是否有顯示驅動錯誤。
