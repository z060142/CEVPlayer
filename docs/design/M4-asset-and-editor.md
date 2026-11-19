# M4 資產與編輯器（不需 FFmpeg 的部分）— 設計（給實作 session）

依據：需求單「資產與格式」「編輯器工具」「三個部分的分工」；研究 `docs/research/04-editor-research.md`（以下簡稱「研究」）。
底線：官方版 Sandbox 可編譯、啟動、開關工具視窗、匯入 WebM 不崩潰；CECE 驗 EditorCEVPlayer 與 CEVPlayer 目標可編譯。畫面正確性以手動驗收清單交給使用者。

## 使用者決定（2026-11-16）

1. **WAV / OGG 放在影片資產資料夾內**，列為該影片資產的檔案（`Files`），搬移、改名、刪除一起動。
   - 已知副作用：CE 會替 Assets 下每個 .wav/.ogg 自動建立 Sound 資產。處理：工具視窗與影片資產資訊欄把這些檔案視為影片資產的一部分；自動產生的 Sound 資產記錄為已知現象（若能在 `CAssetType` / importer 層避免——例如這些檔案先寫成暫存名再由我們的資產登記——就避免，做不到就文件說明）。
   - SDL_mixer impl 只掃 `audio/sdlmixer/assets/`：另外**複製一份**到那裡並建立 ACE trigger；影片資產搬移／刪除／重新產生時同步更新或移除那份副本。
2. **原始影片複製進 Assets**（同 tif→dds 慣例），與影片資產放在一起；`.cryasset` 的 `Details` 記錄原始檔相對路徑與雜湊。
   - 原始檔不得進 pak：5.7.1 沒有內建排除規則 → 提供打包排除清單（`tools/` 下的說明與範例設定），並在資訊欄標示「原始檔，不發行」。版本控制建議 LFS 或排除，寫在 README。
   - 雜湊變動時資訊欄提示「來源已更新，需要重新導入」。
3. **預覽視窗：先用 Character Tool 的 `QViewport`**（`SignalRender` + 編輯模式更新），不穩定再退到 XTFrame 的子視窗 + swap chain 做法（研究 §4）。

## 程式分層

```
Code/CryPlugins/CEVPlayer/Core/            CEVPlayerCore（執行期，既有）
Code/CryPlugins/CEVPlayer/Core/offline/    CEVPlayerOffline（新靜態庫，不碰 CE；只給編輯器連結）
Code/Sandbox/Plugins/EditorCEVPlayer/      編輯器插件
```

### M4a — CEVPlayerOffline（CE 無關，可單獨自測）
- 依賴 CEVPlayerCore、libwebm（mkvmuxer）、libogg、libvorbis + vorbisenc（新增 `libvorbisenc` CMake 目標）、libopus。
- 功能：
  - `ProbeMedia(path)` → 編碼、解析度、幀率、時長、alpha、各音軌（codec、取樣率、聲道、起始偏移、pre-skip / codec delay）。
  - `ExtractAudioWebm(src, trackIndex, dst)`：重新封裝、不重新編碼（研究 §6；時間戳不改，執行期以第一個音訊封包為零點已處理）。
  - `ExportWav(src, trackIndex, dst)`：解碼 → 重取樣 48 kHz / 16-bit / 聲道同原音軌 → 補靜音或裁切讓第 0 個取樣 = 影片 0 秒（誤差 ≤ 1 取樣，需求單驗收）。
  - `ExportOggVorbis(src, trackIndex, dst, quality)`：來源是 Vorbis 且無起始偏移 → 無損重新封裝；其他情況 → 解碼、對齊、vorbisenc 編碼。
  - `StripAudio(src, dst)`（完全外部：影片只留畫面）。
  - 全部可取消（`std::atomic<bool>*`）、回報進度（callback）、回 `Status` + 錯誤訊息字串，不拋例外。
- 自測：對 `test_media_full` 的片子跑上述每個功能，驗證輸出可被 CEVPlayerCore 重新載入、取樣數正確、WAV 與內嵌音軌對齊 ≤ 1 取樣（含 `opus_preskip`、`audio_start_offset`）、Ogg 可被 libvorbisfile 讀回、取消立即生效。

### M4b — EditorCEVPlayer
- 骨架：`CryEditorPlugin`、`REGISTER_VIEWPANE_FACTORY`；CMake 以 `if(NOT TARGET CEVPlayerCore)` 防重複（同時修 Module/CMakeLists 的同一處，研究 §7）。
- 資產類型 `CVideoAssetType`：副檔名 `cevvideo`（主檔 = 執行期描述檔），`CanAutoRepairMetadata()` = false；覆寫 `RenameAsset` 改寫 `.cevvideo` 內的 `video=` / `file=`；所有衍生檔平放在同一資料夾（研究 §2）。
- 匯入 `CVideoAssetImporter`（副檔名 `webm`）：在背景佇列執行，**不得在主執行緒 `.get()` PostOnMainThread**（研究 §3 死結）；選項視窗（壓縮、解析度、子檔在 M5 才可用→灰掉；生成 WAV、分離音訊三態、語言、路線 + trigger 名稱）；把原始檔複製進資產資料夾、寫 `.cevvideo` 與 `.cryasset`（Details：原始檔相對路徑、SHA-256、匯入設定）。
- 工具視窗 `CVideoAssetEditor : CAssetEditor`：三欄 —— 左 Asset Browser（只顯示影片資產，雙擊開啟）、中預覽、右資訊與操作；覆寫 `OnSaveAsset`。
- 預覽 `CVideoPreviewViewport`：`QViewport`，render callback 先畫棋盤格、再以兩次 aux push 做預乘合成（研究 §4：黑色頂點色 + AlphaBlended，再白色 + AlphaAdditive）；每幀取 `GetTextureId()`；播放／暫停／拖曳時間軸／逐格（`Seek(t ± 1/fps)`）／循環／時間碼；聲音切換 SDL 與 CryAudio trigger（暫時描述檔）。視窗重建處理 `WinIdChange`、不在 idle 通知內建立或刪除。
- 資訊欄與操作：研究列出的資訊；操作 = 轉換 WAV、分離音訊、生成 Ogg、重新導入、升級成規範路線、編輯語言對應、影音偏移、標記點（標記點與語言對應的編輯 UI 可先做最小可用版）。長工作走自己的 job runner（背景執行緒、進度、取消、失敗保留完整日誌）。
- ACE：寫 `Assets/audio/sdlmixer/ace/<影片名>.xml`（一個影片一個 library），送 `ESYSTEM_EVENT_AUDIO_REFRESH`；Wwise / FMOD 顯示手動步驟清單。

## 驗收（M4，本輪）
1. CEVPlayerOffline 自測全過（含對齊 ≤ 1 取樣）。
2. 官方版：EditorCEVPlayer + CEVPlayer + Sandbox 建置 0 錯誤 0 警告；CECE：兩個插件目標 0 錯誤。
3. Sandbox 冒煙：啟動 → 開工具視窗 → 以 console / 自動化指令匯入一支 WebM（不經 UI 拖曳；提供 `cev_editor_import <src> <dstFolder>` 之類的指令）→ 產生 WAV / 分離音訊 / Ogg / ACE → 關閉，無崩潰、log 無錯誤。
4. 手動驗收清單（`docs/manual-acceptance-M4.md`）：使用者在 Sandbox 實際操作的步驟與預期畫面。

## 2026-11-19 修訂（E1、E2；本節優先於上文的對應段落）

- 預覽不再是引擎的 `QViewport`：`CVideoPreviewWidget`（一般 `QWidget`）直接畫 `CEVPlayerCore` 解出的畫面，聲音走 `QAudioOutput`；不經引擎渲染器、不需要執行期插件（任務單 `docs/tasks/E2.md`）。描述檔的判斷邏輯在 Core 的 `DescriptorPlan.h`，執行期與預覽共用。
- 資產是「匯入而來」：`IsImported()`＝true，資產資料夾內的來源複本登記為來源檔，重新匯入走引擎的 `CAsset::Reimport`（`CVideoAssetImporter::ReimportAsset` 沿用存下來的匯入設定）；來源狀態平常只比大小與修改時間。描述檔以 property tree 編輯（語言的 trigger 用引擎的音訊觸發器選擇器），沒有 Apply 鍵；對話框用引擎的 `CSystemFileDialog`／`CQuestionDialog`；使用者觸發的工作有 `CProgressNotification`；「Open folder」鍵已移除（任務單 `docs/tasks/E1.md`）。
