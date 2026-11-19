# 人工驗收清單（rebuild 分支，2026-11-19 準備併入 master 時整理）

自動化測試（`cevcore_selftest`、`cevoffline_selftest`、`cevffmpeg_selftest`、`cev_apitest`、`cev_flowtest`、`cev_contract_selftest`、`cev_cryvstest`、`cev_editor_preview_test`、`cev_editor_info_test`）證明的是「程式路徑走得通、數值對、不崩潰」。下面是**自動化沒有涵蓋、需要人眼或滑鼠確認**的項目，取自各任務報告的「未測／需人工確認」與 `docs/PLAN.md` 的決策紀錄。每項寫了步驟與預期；結果請直接在本檔打勾或寫備註。

標記：**〔受阻〕**＝要等其他專案，目前無法驗收。

## 0. 共同準備

1. 官方版引擎樹（Profile）已同步並建置 `CEVPlayer`、`EditorCEVPlayer`（README「Build and integration」）。
2. 測試專案的 `.cryproject` 載入 `CEVPlayer`；`Assets/Videos/` 放 `tools/make_test_media.sh` 產生的影片（至少 `vp9_alpha.webm`、`vp8_vorbis.webm`、`vp9_1080p30_opus.webm`），以及 `tools/test_descriptors/tracks_test.cevvideo` 與它用到的 `audio_music.webm`、`audio_commentary.webm`。
3. 啟動參數建議 `+sys_assert_dialogues 0`。查 log 時搜尋 `[CEVPlayer]`、`[CEVEditor]`。
4. 社群版引擎的項目另外需要 `sys_SchematycPlugin=0`（專案 console_variables）與 Sandbox 的 `sys_asserts=0`；每次離開都會在 `C3DEngine::UnloadLevel` 崩潰（fork 的既有問題，與本插件無關）。

## 1. FlowGraph 節點（編輯器內的外觀與行為）

- [ ] **抽屜結構**：開一個關卡，在任一實體上開 Flow Graph，按右鍵加節點。預期：只有一個 `CEVPlayer` 抽屜，裡面直接是 `PlayOnScreen`、`PlayOnMaterial`，以及子抽屜 `UI`（`PlayToSurface`、`SurfaceInfo`、`SurfaceEvents`）與 `Advanced`（`Open`、`Transport`、`Settings`、`Info`、`Events`）。沒有舊的 `FlashVideoPlayer`／`TextureVideoPlayer`。
- [ ] **埠名與說明**：逐一放上 10 個節點，對照 README「Node sets」的表，埠名一致、滑鼠停在埠上有說明文字。
- [ ] **列舉下拉**：`PlayOnScreen` 的 `Fit` 是 Fill／Fit／Letterbox 下拉；各節點的 `Decode` 是 Auto／GPU／CPU；`Advanced:Open` 的 `Output` 是 Surface／Screen／Material。不需要填數字。
- [ ] **PlayOnScreen**：`Game:Start` → `Play`，Video=`Videos/vp8_vorbis.webm`。進遊戲模式（Ctrl+G）。預期：全螢幕播放、有聲音、`Started` 觸發；播完 `Ended` 觸發。三種 Fit 各試一次，畫面比例符合描述。
- [ ] **Skipped**：同上，`Skippable`＝勾、`Loop`＝勾；播放中按任一鍵或滑鼠鍵。預期：影片停止、`Skipped` 觸發（可接 `Debug:Log` 看）。
- [ ] **PlayOnMaterial 成功路徑**：場景中放一個平面物件，給它一個材質；節點 Material 留空（＝實體自己的材質）、Slot 0，`Play`。預期：物件表面出現影片、`Started` 觸發，`Stop` 後恢復原貼圖。再試指定 Material 名稱與 SubMaterial。
- [ ] **Autoplay**：`PlayOnMaterial`／`UI:PlayToSurface` 的 `Autoplay` 勾選、不接 `Play`。進遊戲模式。預期：自動開始播放。
- [ ] **Marker 輸出**：用一個帶 `<Markers>` 的描述檔（例如在 `tracks_test.cevvideo` 加 `<Markers><Marker name="m1" time="1.0"/></Markers>`），以 `Advanced:Open`（Output=Screen）→ `Transport:Play`，`Events` 接同一個 Handle。預期：約 1 秒時 `Marker` 觸發，`MarkerName`＝m1、`MarkerTime`＝1.0。
- [ ] **UI 抽屜**：`UI:PlayToSurface`（SurfaceID＝`menu_bg`）→ `Ready` 帶出 TextureId／Width／Height；`UI:SurfaceInfo` 的 `Get` 讀到同樣的值；`UI:SurfaceEvents` 的 `NewFrame` 持續觸發、`Stop` 後 `Closed` 觸發。
- [ ] **Sandbox 編輯模式**：不進遊戲模式時影片不前進——這是引擎不 tick 插件的既有行為，確認「不前進」而不是崩潰或錯誤即可。

## 2. Schematyc 元件（官方版）

- [ ] 在關卡放一個實體，加元件 **Video Player**（分類 Video）。屬性：Video＝`Videos/vp9_alpha.webm`、Output＝Material、Autoplay＝勾。進遊戲模式。預期：該實體的材質上播放有 alpha 的影片。
- [ ] Output 改 Screen、Screen Fit 改 Letterbox、Skippable 勾：全螢幕播放並可按鍵跳過。
- [ ] Output 改 Surface、SurfaceID＝`hud_vid`：主控台 `cev_surfaces` 列出 `hud_vid`。
- [ ] 建一個 Schematyc 實體類別使用此元件：在 `Ready`／`Ended`／`Marker` 訊號上接 Log，確認訊號有到；呼叫 `SetAudioTrack`（用 `tracks_test.cevvideo`，名稱 `commentary`）後聲音切換。
- [ ] Audio Track 屬性填 `music`：播放時用的是 music 音軌（`cev_audiotrack <surface>` 可查）。

## 3. CryVS 腳本（社群版引擎）

`cev_cryvstest` 已證明登錄與以 GUID 直接呼叫都正確；但 CryVS 腳本只能在 CryVS Designer 建立、存檔時編譯，**實際的腳本執行沒有驗過**。

- [ ] 用社群版 Sandbox 開測試專案（`sys_SchematycPlugin=0`、`sys_asserts=0`）。
- [ ] 在 CryVS Designer 新建一個腳本類別。左側函式庫中應看得到 `CEVPlayer`、`CEVPlayer::UI`、`CEVPlayer::Advanced` 三組全域函式，以及元件 `VideoPlayer`。
- [ ] 在類別的開始事件（Start／Construction 一類的訊號）接 `CEVPlayer::UI::PlayToSurface`（Video＝`Videos/vp9_alpha.webm`、SurfaceID＝`cryvs_test`），再接一個 Log 節點輸出回傳的 Handle。列舉參數（Fit、Output、Decode）應是下拉。
- [ ] 存檔（觸發編譯）；把此類別放到關卡中的實體上；進遊戲模式。預期：log 有 Handle 值（非 0），`cev_surfaces` 列出 `cryvs_test`，離開遊戲模式後該 surface 被收掉。
- [ ] 加入 `VideoPlayer` 元件並在其 `Ready` 訊號接 Log：預期訊號有到。
- [ ] 試 `CEVPlayer::PlayOnScreen` 與 `CEVPlayer::Stop`：全螢幕播放、Stop 後消失。

## 4. 編輯器預覽（E2）

- [ ] **聲音與流暢度**：開 *Tools > CEV Video Editor*，選 `vp8_vorbis` 資產並播放。預期：聲音清楚、沒有斷續；畫面流暢不掉幀；暫停／繼續／拖動時間軸／逐格前進後退都正確；alpha 影片背後是棋盤格。
- [ ] **Audio track 下拉**：開一個有多音軌的資產（描述檔有 `<AudioTracks>`），播放中切換音軌。預期：在原位置、原播放狀態換成新音軌，沒有爆音。
- [ ] **用滑鼠拖曳 dock**：把預覽面板拖成浮動、拖回停駐、拖到別的分頁各 3 次。預期：預覽不變黑、不凍結、不崩潰（自動化只用 `cev_editor_preview_dock` 經由 docking manager 移動）。
- [ ] **主視口的成本**：預覽播放 1080p 時 Sandbox 的主視口不明顯變卡（E2 量測：不開預覽 2.32 ms／幀、預覽播放 2.97 ms／幀，只量過一次）。

## 5. Info 欄與 Sandbox 機制（E1、A1）

- [ ] **Asset Browser 右鍵 Reimport**：在 Asset Browser 對一個影片資產按右鍵。預期：有 **Reimport**，執行後沿用原本的匯入設定（音軌名、路線、匯出選項）重新匯入，沒有再跳選項對話框以外的錯誤。
- [ ] **偏好設定頁**：*Edit > Preferences*（或 *Tools > Preferences*）有 **CEVPlayer/FFmpeg** 頁，可以設定 ffmpeg 路徑；Info 欄的「Open preferences」會打開這一頁。
- [ ] **property tree 外觀**：Info 欄描述檔區塊是引擎的 property tree：route、A/V offset、Markers、**Audio tracks** 區塊（上方 Default track 下拉與 Follow game language 核取方塊；每列 Name、Source＝Embedded／File／Trigger、對應欄位、Language）。改值後立即寫入（沒有 Apply 鍵），Ctrl+S 存檔；檔名標題出現修改記號。
- [ ] **音訊觸發器選擇器**：把某音軌的 Source 改成 Trigger，按 start／pause／resume trigger 欄的選擇鍵。預期：跳出引擎（ACE）的音訊觸發器選擇器，選好後欄位填入名稱。
- [ ] **外部音檔選擇**：Source＝File 時檔案欄可以用檔案選擇器挑 WebM。
- [ ] **舊描述檔**：開一個含 `<Languages>` 的舊描述檔資產。預期：log 有錯誤、Audio tracks 為空，不崩潰。
- [ ] **工作的通知**：執行一個背景操作（例如 Export WAV）與一個 FFmpeg 轉碼（例如 *Re-compress...*）。預期：右下角通知中心出現進度通知，結束時變成完成／失敗；取消時顯示已取消。

## 6. FFmpeg 對話框（滑鼠操作）

- [ ] **下載對話框**：讓系統找不到 ffmpeg（偏好設定清空、PATH 上沒有、`%LOCALAPPDATA%\CEVPlayer\ffmpeg` 刪掉），在 Info 欄按「Download minimal build」或 *Set up FFmpeg...*。預期：進度對話框、可取消；完成後狀態為 ready／8.0.3／downloaded copy。網路被擋時訊息說明防火牆、給出網址與 SHA-256，並指向 *Preferences > CEVPlayer/FFmpeg*。
- [ ] **「需要 FFmpeg」對話框**：在同樣沒有 ffmpeg 的狀態下，把一個 MP4 拖進 Asset Browser。預期：詢問要下載還是指定 `ffmpeg.exe`（引擎的 `CQuestionDialog`）；選指定後用 `CSystemFileDialog` 選檔。
- [ ] 選擇後匯入繼續並完成。

## 7. 硬體解碼（M9）

本機（RTX 4070 Ti）已實測；以下是其他環境：

- [ ] **其他廠牌顯示卡**（AMD、Intel）：裝好 VP9 Video Extensions，`cev_decodeMode 1`，`cev_play Videos/vp9_1080p30_opus.webm`，`cev_stats 1`。預期：stats 行的 `decode=` 顯示 GPU 後端、`hw=1`，畫面正確；VP8 影片仍顯示 CPU。
- [ ] **沒有 VP9 Video Extensions 的機器**：同上。預期：播放器退回 CPU，log 有一次警告，播放正常。
- [ ] **多顯示卡**：確認硬解用的是引擎所在的那張卡（設計假設「DXGI 第一張卡＝引擎的卡」未驗）。
- [ ] 工作管理員的 GPU「Video Decode」引擎使用率在硬解時上升（未量過）。

## 8. 經由 UI 播放的完整路徑 〔受阻〕

- [ ] **〔受阻：等 UI 消費端實作契約〕**打包平面排列、可見性節流、依畫面大小選變體、`open`／`control` 只有在 UI 插件實作 `CryTexSource_v1` 契約時才生效；本倉沒有消費端。`cev_contract_selftest` 以模擬消費端驗過來源端。消費端完成後驗：UI 上的影片與 RGBA8 路徑逐像素差 ≤ 2 級、隱藏時 `cev_stats` 的 copy／color／upload 為零、切換排列時聲音不中斷。
- [ ] **〔受阻：等引擎的載入期機制（M10）〕**關卡讀取畫面期間影片前進。目前不前進是已知限制。

## 9. 先前從未執行的 M4／M5 清單

下列兩份清單寫成後**沒有人實際走過**。內容部分已過時（預覽已不是 `QViewport`、Info 欄改為 property tree、「音訊語言」改為「音軌名」、FFmpeg 路徑改存偏好設定頁——見各檔開頭的註記），請以本檔第 4～6 節為準，其餘步驟照原清單：

- [ ] [`manual-acceptance-M4.md`](manual-acceptance-M4.md)：資產類型、匯入、改名／搬移／刪除、分離音訊、WAV／Ogg、升級 cue route、打包工具。
- [ ] [`manual-acceptance-M5.md`](manual-acceptance-M5.md)：FFmpeg 區段、非 WebM 匯入、壓縮與解析度、變體、批次、PNG 序列。

## 10. 其他未實測（E1 報告）

- [ ] FFmpeg 轉出的資產做 Reimport（Asset Browser 右鍵）。
- [ ] 改名／搬移資產後，來源檔的登記跟著更新（Reimport 仍找得到來源）。
- [ ] 預覽外部音軌檔（`file=`）與 pak 內的影片。
