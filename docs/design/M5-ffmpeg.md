# M5 FFmpeg 整合（外部處理程序、最小 LGPL 版本、下載與驗證）— 設計

依據：需求單「FFmpeg 邊界」「最小版 FFmpeg」。本文件對應 `Code/CryPlugins/CEVPlayer/Core/offline/` 的新目標 `CEVPlayerFFmpeg`、`tools/ffmpeg/`、`.github/workflows/ffmpeg-minimal.yml`。
底線：執行期外掛（CEVPlayer）永遠不連結 FFmpeg，也不連結 `CEVPlayerFFmpeg`；FFmpeg 只以**外部處理程序**執行，絕不連結 libav*。

## 1. 邊界：什麼需要 FFmpeg

| 功能 | 需要 FFmpeg？ | 由誰完成 |
|---|---|---|
| 播放（WebM VP8/VP9 + Vorbis/Opus） | 否 | CEVPlayerCore |
| 分離音訊（WebM 音軌抽出、去除音軌） | 否 | CEVPlayerOffline（`ExtractAudioWebm` / `StripAudio`） |
| WAV 匯出、Ogg Vorbis 匯出 | 否 | CEVPlayerOffline（`ExportWav` / `ExportOggVorbis`） |
| 匯入非 WebM（mp4/mov/mkv/avi/PNG 序列） | **是** | CEVPlayerFFmpeg：`TranscodeToWebm` |
| 重新壓縮、改解析度 | **是** | 同上 |
| 產生子檔（360p / 720p 變體） | **是** | CEVPlayerFFmpeg：`MakeVariant` |

沒有 FFmpeg 時，編輯器仍可匯入 WebM、分離音訊、輸出 WAV/OGG；只有上表「是」的項目會灰掉並說明原因。

## 2. 程式分層

```
Core/offline/include/CEVOffline/
  FfmpegRunner.h    啟動程序、引數引號、-progress 解析、取消與逾時
  FfmpegLocator.h   尋找 ffmpeg、能力檢查（-encoders/-decoders/-muxers/-demuxers）
  FfmpegCommands.h  來源探測、引數建構（純函式）、執行計畫（含兩段式、暫存檔、清理）
  FfmpegInstall.h   釘住的版本與 SHA-256、SHA-256 實作、下載介面、驗證與解壓
Core/offline/src/ffmpeg/*.cpp
Core/offline/tests/cevffmpeg_selftest.cpp
```

- `CEVPlayerFFmpeg` 是獨立的靜態庫，連結 `CEVPlayerOffline`（共用 `Result` / `Control`），Windows 專用（`CreateProcessW`、WinHTTP）；解析類函式（引號、進度、能力、引數建構）本身可攜。
- 獨立建置時一律編譯；在 CRYENGINE 的 CMake 樹內為 `EXCLUDE_FROM_ALL`，只有編輯器插件 `target_link_libraries(<插件> PRIVATE CEVPlayerFFmpeg)` 時才會建。
- 規則同 Offline.h：不丟例外、錯誤以 `Result{status, message}` 回報、路徑與引數一律 UTF-8、進度與取消用 `Control`（回呼只在呼叫端執行緒執行）。
- 既有 API 完全不變（僅新增檔案與 CMake 目標）。

## 3. 偵測（FfmpegLocator）

順序：**設定的路徑 → 我們下載的副本 → 系統 PATH**。

- 設定路徑與下載資料夾可以是檔案或資料夾（資料夾找 `ffmpeg.exe`，其次 `bin/ffmpeg.exe`）。PATH 會列出所有 `ffmpeg.exe`，逐一檢查，不是只看第一個。
- 每個候選執行 `-version`、`-encoders`、`-decoders`、`-muxers`、`-demuxers`，解析名稱集合（muxer/demuxer 名稱以逗號拆開，例如 `mov,mp4,m4a,...`）。
- **最低需求**（`MinimumRequirement()`）：編碼器 `libvpx-vp9`、`libopus`，封裝器 `webm`。不足就視為「不存在」，記入 `LocateResult::rejected`，原因文字列出缺少項目（`missing encoder:libopus, muxer:webm`），然後繼續找下一個候選。
- 建議項（`RecommendedRequirement()`）：`libvorbis`、`pcm_s16le`、`ogg`、`wav`。匯入項（`ImportRequirement()`）：demuxer mov/mp4/matroska/webm/avi/image2；decoder h264、hevc、prores、vp8、vp9、libvpx-vp9、libdav1d、png、aac、mp3、opus、vorbis、flac、pcm_s16le。缺少者只列在 `CapabilityReport` 供 UI 顯示（例如「此 ffmpeg 不能匯入 HEVC」），不影響是否可用。
- 同時回報版本字串與 `--enable-gpl` / `--enable-nonfree` 旗標（資訊用：外部程序不構成連結；我們自己的下載版保證沒有）。
- 探測透過可注入的 `ProbeExec`，因此「順序、不足、不是 ffmpeg、無法執行」都能用假資料夾與罐頭輸出測試。

使用者介面流程（編輯器端，M5 後續）：`Locate()` 找不到 → 詢問「下載我們的最小版？」（顯示大小、來源、授權）→ 同意則 `DownloadAndInstallFfmpeg`；拒絕或失敗 → 讓使用者手動選擇路徑（選到的路徑再過一次能力檢查，不足就顯示缺少項目）。

## 4. 執行（FfmpegRunner）

- `CreateProcessW`：`CREATE_NO_WINDOW`；stdin/stdout/stderr 三條管線，只繼承這三個 handle（`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`）；放進 **Job Object**（`KILL_ON_JOB_CLOSE`），因此「終止」就是整個處理程序樹。
- 引數：UTF-8 轉寬字元，引號規則與 `CommandLineToArgvW` 相容（`QuoteWindowsArg`；含空白、引號、結尾反斜線、emoji、`%PATH%` 等皆在自測中經真實子程序往返驗證）。
- 兩條讀取執行緒持續排空 stdout/stderr（不會因管線塞滿而卡死）；stderr 完整保留（上限預設 4 MB，超過時保留頭尾並插入標記），失敗時整份日誌回傳給 UI。
- 進度：`-progress pipe:1` 的 `key=value` 區塊，以 `out_time_us`（無則 `out_time_ms`，兩者單位其實都是微秒）除以已知時長；開頭的負值/`N/A` 忽略；`progress=end` 才回報 1.0（計畫層級在檔案就位後才回 1.0）。分段到達的位元組、`\r\n`、雜訊行皆可處理。
- 取消：`Control::cancel` → 先對 stdin 寫 `q`（讓 ffmpeg 自行收尾）→ 等 `cancelGraceMs`（預設 500 ms）→ 仍存活則終止 Job。回傳 `Status::Cancelled`。
- **不會永遠卡住**：`noProgressTimeoutMs`（預設 60 s 沒有任何 stdout/stderr 位元組 → 判定卡死並終止）、`totalTimeoutMs`（可選的總時限）。逾時回 `Status::IoError`，`RunResult::timedOut = true`。
- 回傳 `RunResult`：結果、結束碼、是否取消/逾時/被終止、最後的進度時間與速度、stdout（非進度模式）、完整 stderr、耗時。

## 5. 指令建構（FfmpegCommands）

純函式，回傳引數向量，不執行任何東西（可單元測試）：

- `BuildTranscodeToWebm(src, dst, options) → TranscodePlan`
- `BuildMakeVariant(src, dst, height, options)`：高度固定、寬度 `-2`（隨比例並保持偶數）；來源是帶 alpha 的 WebM 時，輸入加 `-c:v libvpx-vp9`（ffmpeg 原生 vp9 解碼器會丟掉 WebM 的 alpha；實測確認）。
- 選項：`crf`（預設 32）、位元率（0＝純固定品質 `-b:v 0`）、寬/高（只給一邊則另一邊 `-2`）、`AlphaMode`（Auto/Preserve/Discard）、Opus 位元率、執行緒數、`twoPass`、`cpuUsed`、PNG 序列 `inputFrameRate`、顏色矩陣。
- alpha：來源有 alpha（ProRes 4444、PNG 序列、alpha WebM）→ `format=yuva420p` + `-auto-alt-ref 0`；否則 `yuv420p`。來源 alpha 由 `ProbeSource`（解析 `ffmpeg -i` 的 stderr：像素格式含 `a`、或 WebM 串流標籤 `alpha_mode=1`）決定。
- 顏色：預設以 `scale=…:out_color_matrix=bt709` 轉換並標記 `-colorspace/-color_primaries/-color_trc bt709`（RGB 來源不會被 swscale 預設的 BT.601 染色）。
- 只使用最小版有的功能：濾鏡只有 `scale`、`format`；兩段式的第一段以 `webm` 封裝器寫入 `NUL`（不需要 `null` 封裝器）。
- 輸出一律寫到 `<dst>.tmp`（`-f webm` 明確指定格式），成功才改名覆蓋 `<dst>`；失敗、取消、逾時：刪除 `.tmp` 與兩段式 log，既有的 `<dst>` 不受影響。
- `RunTranscodePlan` 依權重合成進度（兩段式 0.45 / 0.55）；`TranscodeToWebm` / `MakeVariant` = 探測來源 → 填入 alpha/音訊/時長 → 建構 → 執行。

## 6. 下載（FfmpegInstall）

- 外掛寫死 `kFfmpegMinimalVersion` 與 `kFfmpegMinimalSha256`。目前為 `"unreleased"` 與空字串：下載停用，`FfmpegDownloadDisabledReason()` 回傳說明（「尚未發佈最小版，請手動選擇 ffmpeg」）。第一次 CI 發佈後把 workflow 摘要印出的兩個值貼進標頭。
- URL：`https://github.com/z060142/CEVPlayer/releases/download/ffmpeg-minimal-v<版本>/ffmpeg-minimal-<版本>-win64.zip`。版本字串只允許 `[A-Za-z0-9._-]`（會進入路徑與 URL）。
- 抓取經 `IFfmpegFetcher` 介面：`WinHttpFetcher`（系統元件，零額外相依；跟隨重新導向——GitHub 發佈資產會 302 到 CDN；只允許 https，loopback 才允許 http）與 `LocalFileFetcher`（自測 / 離線安裝）。
- **先驗雜湊再解壓**：自己實作的 SHA-256（FIPS 180-4，附標準向量測試）。**雜湊不符 → 拒絕**（`Status::InvalidData`，訊息含實際與預期值），不解壓、刪除下載檔、保留舊的安裝。期望雜湊為空或格式錯誤也拒絕（絕不安裝未驗證的檔案）。
- 解壓：呼叫 Windows 10 1803+ 內建的 bsdtar（`%SystemRoot%\System32\tar.exe -xf`，可讀 zip），**不引入任何第三方程式碼**（不 vendoring miniz；若日後需要支援更舊的系統再考慮 miniz，MIT 授權可用）。只用 System32 的 tar，不走 PATH（避免被 Git 的 GNU tar 或惡意程式取代）。
- 安全：解到 `<安裝資料夾>.staging`；檢查沒有連結（symlink）、所有路徑都在 staging 內；找 `ffmpeg.exe`（深度 ≤ 3；位於 `bin/` 則以其上一層為根）；通過後才整個改名為安裝資料夾。`../` 的 zip-slip 條目由 bsdtar 拒絕，自測以手工壓縮的惡意 zip 驗證沒有檔案寫到目標外。
- 位置：`DefaultFfmpegInstallRoot()` = `%LOCALAPPDATA%\CEVPlayer\ffmpeg`，安裝資料夾 `<root>\<版本>`（也就是 `LocateOptions::downloadedDir`）。屬於使用者／編輯器工具資料夾，**不在專案內、不進 pak**。
- 安裝後仍由 `Locate()` 再做一次能力檢查。

## 7. 最小版 FFmpeg（CI 建置）

範圍（`--disable-everything` 後逐項啟用）：

| 類別 | 內容 |
|---|---|
| demuxer | mov/mp4、matroska/webm、avi、image2（PNG 序列） |
| decoder | h264、hevc、prores（含 4444）、vp8、vp9（原生 + libvpx）、AV1（libdav1d）、png、aac、mp3、opus、vorbis、flac、pcm |
| encoder | libvpx-vp9、libopus、libvorbis、pcm_s16le |
| muxer | webm、ogg、wav |
| filter | scale、format、aresample（另有 ffmpeg.c 自己 select 的 aformat、null、anull、trim、atrim 等框架濾鏡，由 configure 自動帶入） |
| 其他 | parser h264/hevc/aac/aac_latm/mpegaudio/vp8/vp9/av1/opus/vorbis/flac/png；bsf null、vp9_superframe(_split)、extract_extradata；protocol file、pipe（`-progress pipe:1` 需要） |

- 授權：`--disable-gpl --disable-nonfree --disable-version3`；FFmpeg 本體 LGPL-2.1+，外部函式庫 libvpx / libopus / libogg / libvorbis / dav1d 皆為 BSD；png 解碼器需要的 zlib（zlib 授權）取自 MinGW 工具鏈（`mingw-w64-x86_64-zlib`，靜態連結，授權檔放進 `licenses/zlib-LICENSE`）。建置腳本在打包前檢查 `ffmpeg -version` 的 configuration 不含 gpl/nonfree/version3、`-L` 為 LGPL，並檢查範圍內的編解碼器確實存在。
- 建置腳本 `tools/ffmpeg/build_minimal.sh`（可在本機 MSYS2 MINGW64 執行）+ `tools/ffmpeg/versions.env`（所有來源的版本、URL、SHA-256 釘死：FFmpeg 8.0.3、libvpx 1.15.0、opus 1.5.2、libogg 1.3.5、libvorbis 1.3.7、dav1d 1.5.3；來源下載後先驗雜湊再解開）。
- 組件名稱已對照 FFmpeg 8.0.3 原始碼確認存在（`allcodecs.c`、`allformats.c`、`allfilters.c`、`parsers.c`、`bitstream_filters.c`、`protocols.c`、`configure` 的 `ffmpeg_select`）；libvpx / dav1d / opus / libvorbis 的 configure 選項也對照過原始碼。
- 打包內容（zip 根目錄平放）：`ffmpeg.exe`、`LICENSE`（LGPL-2.1）、`licenses/`（各元件授權）、`BUILD.txt`（來源 URL + SHA-256、工具鏈版本、configure 引數）、建置腳本與 `versions.env`。**建置腳本與來源版本隨二進位一起發佈**（LGPL 要求與可重現性）。
- 發佈檔名 `ffmpeg-minimal-<FFMPEG_VERSION>-cev<CEV_BUILD_REV>-win64.zip`，另附 `.sha256`。

### 工作流程 `.github/workflows/ffmpeg-minimal.yml`

- 觸發：手動（`workflow_dispatch`，可勾選 publish）與 tag `ffmpeg-minimal-v*`（tag 必須等於 `versions.env` 算出的版本，否則失敗）。
- build 工作（windows-latest + MSYS2 MINGW64，`msys2/setup-msys2@v2`）：解析版本 → 快取下載 → 安裝工具鏈（toolchain、nasm、meson、ninja、pkgconf、zip）→ `build_minimal.sh` → **之後**才安裝參考用的完整 ffmpeg（避免其函式庫混入建置）→ `smoke_test_minimal.sh`（用與外掛相同的引數轉 MP4/PNG alpha/ProRes 4444/alpha 變體，用 ffprobe 驗證；並確認最小版不能編 H.264）→ 計算 SHA-256 寫入工作摘要（附可直接貼進標頭的常數）→ 上傳 artifact。
- publish 工作（ubuntu-latest，只在 tag 或勾選 publish 時）：驗證 `.sha256` → `gh release create/upload` 到本 fork（`z060142/CEVPlayer`）的 Releases。

### 實際執行紀錄（2026-11-19，本機）

- **建置腳本與 smoke test 已在本機實跑過；GitHub Actions 工作流程本身仍未跑過。** 環境：Windows 11；MSYS2 2026-09-27 基底用 scoop 裝在使用者目錄；MINGW64：gcc 16.2.0、binutils 2.47、nasm 3.02、meson 1.12.1、ninja 1.13.2、pkgconf 3.0.11、zlib 1.4；`--jobs 6`；工作目錄在 RAM 碟，峰值約 300 MB；完整跑一次約 14～20 分鐘。
- 第一次實跑需要的修正（範圍、授權旗標與釘死的版本都沒變）：
  - 套件清單加 `perl`（libvpx 的 configure 需要）。
  - 拿掉 `--disable-postproc`（FFmpeg 8.0 已移除 libpostproc，該選項不存在）。
  - 加 `--enable-zlib`：png 解碼器需要 zlib，而 `--disable-autodetect` 會讓 png 被**靜默**拿掉。
  - 比對 exe 輸出前先去掉 CR（Windows 輸出 CRLF，跨行的「GNU Lesser General Public License」原本比對不到）。
  - smoke test：ffprobe 輸出同樣去 CR；`alpha_mode` 改為不分大小寫並容許 `TAG:` 前綴（重編碼的 alpha WebM 帶的是 `ALPHA_MODE`）。
- 結果：
  - `-version` 的 configuration 不含 gpl/nonfree/version3；`-L` 為 LGPL 2.1+。
  - 編解碼器、muxer、demuxer、filter 清單與上表一致。
  - DLL 相依只有 bcrypt、KERNEL32、msvcrt、SHELL32。
  - `smoke_test_minimal.sh` 對參考 ffmpeg 7.1.1（scoop）與 `mingw-w64-x86_64-ffmpeg` 9.0.2 都 PASS。
  - 另外手動轉檔（帶 `-g 15`）：H.264 MP4、PNG 序列各轉成 VP9(+Opus) WebM，60 幀中 4 個關鍵幀；ProRes 4444 轉成 VP9 alpha，以 libvpx 解碼確認 alpha 正確。
  - 產物：`ffmpeg-minimal-8.0.3-cev1-win64.zip`（本機，未發佈）。兩次建置的 ffmpeg.exe 雜湊不同（未做位元級可重現）。
- `cevffmpeg_selftest`（MSVC 建置）對最小版：523 項檢查中 8 項失敗。失敗的全是它**用受測 ffmpeg 自己產生測試來源**的步驟（lavfi、libx264、prores_ks 都不在最小版範圍內），不是最小版的缺陷；這支自測仍需要完整版 ffmpeg。
- 本機注意事項（只限那台機器，與 CI 無關）：本機安全軟體擋掉 MSYS2 自帶的 curl。因此 pacman 改用 `XferCommand` = Windows `curl.exe`，來源仍只有官方 MSYS2 鏡像；六個來源 tarball 先用 Windows curl 下載到 `<work>/downloads`，由腳本的 `fetch()` 依 `versions.env` 重新驗證 SHA-256。

## 8. 自測 `cevffmpeg_selftest`

```
cevffmpeg_selftest [--media <dir>] [--ffmpeg <ffmpeg.exe>] [--only <子字串>] [--out <dir>] [--keep]
                   [--fetch-url <url> --fetch-sha256 <hex>]
```

第一部分（不需要 ffmpeg）：SHA-256 標準向量與填充邊界；引號（含 `CommandLineToArgvW` 與真實子程序往返）；`-progress` 解析（任意分段、byte-by-byte、雜訊）；能力解析（真實 ffmpeg 8 排版逐字、完整版、最小版、不足、損壞輸出）；定位順序（假資料夾 + 假探測：設定路徑優先、不足被跳過並說明、`bin/` 版面、PATH 多個、不是 ffmpeg、全部不可用）；引數建構（預設、縮放、alpha、音訊、PNG 序列、兩段式、變體）；來源探測解析（WebM alpha、ProRes 4444、PNG 序列、MP4、無法讀取）；執行器（以自身為子程序：進度、非零結束碼、無法啟動、卡死逾時、總時限、取消、已取消旗標、12 MB 洪水輸出）；安裝器（手工 zip：巢狀 / `bin/` / 平放、**雜湊不符拒絕**、竄改、空雜湊、無 ffmpeg.exe、非 zip、zip-slip、經 `LocalFileFetcher` 的下載安裝與取消）。

第二部分（用真實 ffmpeg：`--ffmpeg` 或 PATH 上的）：產生 MP4 H.264+AAC、帶 alpha 的 PNG 序列、ProRes 4444；轉成 WebM（VP9 + Opus）後用 `ProbeMedia`（幀數、尺寸、alpha 旗標、音軌）與 `PlayerCore`（載入、逐幀解碼、解碼錯誤 0、alpha 平面的實際數值）驗證；兩段式與縮放；360p / 180p / 奇數尺寸變體（alpha 保留）；失敗（缺檔、垃圾檔、src==dst、既有輸出在失敗後不變）；中文與空白路徑；取消（單段與兩段式：取消到返回 < 3 s、`.tmp` 與 log 清除、檔案已釋放）；卡死程序的逾時。

WinHTTP 以 `--fetch-url` 對本機 `python -m http.server`、真實 https 站台與 GitHub 重新導向手動驗證過。

## 9. 驗收對照

| 驗收項 | 狀態 |
|---|---|
| 沒有 FFmpeg 時，播放 / 分離音訊 / WAV 不受影響 | CEVPlayerFFmpeg 為獨立目標，Core / Offline 不依賴它 |
| 偵測順序與能力不足視為不存在 | 自測（假資料夾 + 真實 PATH） |
| 下載的 FFmpeg SHA-256 不符 → 拒絕 | 自測（`LocalFileFetcher`、`InstallFfmpegFromZip`） |
| ProRes 4444 / PNG 序列 alpha 保留 | 自測（PlayerCore 讀回 alpha 數值） |
| 取消立即生效並移除半成品 | 自測 |
| 最小版在 CI 建置並發佈到 Releases | 工作流程已寫好，**尚未在 CI 實跑** |


---

# 編輯器端實作紀錄（EditorCEVPlayer，M5 第二部分）

## 10. 設計與實作

### 10.1 程式結構（`Code/Sandbox/Plugins/EditorCEVPlayer/`）

| 檔案 | 內容 |
|---|---|
| `Settings/EditorSettings.*` | 使用者層級設定檔 `%LOCALAPPDATA%\CEVPlayer\editor.json`（目前只有 `ffmpegPath`；`QSaveFile` 原子寫入，保留其他鍵）。環境變數 `CEV_EDITOR_SETTINGS=<檔>` 覆寫位置（測試不碰使用者的設定）。 |
| `FFmpeg/FfmpegService.*` | `CFfmpegService`：偵測狀態快取（Unknown / Detecting / Ready / Absent，含路徑、版本、來源、缺少的功能、被拒絕的候選與原因）、`Locate` 在 job 執行緒執行（主畫面就緒時惰性啟動，結果快取；`Re-detect` 重跑）、下載 job、互動流程 `EnsureUsableModal`、`WaitForExe`（job 執行緒用，等偵測結束）。另有 `FfmpegTranscode` / `FfmpegMakeVariant` 包裝（把庫的 `Result` + 日誌尾端轉成編輯器的 `SOpResult`）。 |
| `Asset/ImportQueue.*` | `CImportQueue`：所有需要 FFmpeg 的匯入、編輯器視窗的匯入、批次匯入的**單一佇列**，一次一個檔案（一次一個 ffmpeg），每個檔案一個 `CJobRunner` job，逐檔結果與批次摘要，`CancelAll`。 |
| `Asset/VideoImport.*` | `ClassifySource`、`SImportOptions` 新欄位、`PlanImport`（來源種類、PNG 序列偵測、被取代的檔案）、`ImportVideoFile`（複製來源、轉檔、探測、變體、描述檔、`.cryasset`，失敗或取消時刪除本次新建的檔案）、`SubmitTranscodeOps`（變體 / 重新壓縮 / 重新匯入）。 |
| `Editor/ImportOptionsDialog.*` | 匯入對話框新增 FFmpeg 群組（狀態、`Set up FFmpeg...`、WebM 的「重新編碼」勾選、CRF、輸出高度、變體清單、PNG 序列的幀率）；批次模式。 |
| `Editor/TranscodeDialog.*` | `CVariantPicker`（2160/1440/1080/720/540/360 + 其他高度）與「Generate variants...」/「Re-compress / resize...」對話框。 |
| `Editor/InfoPanel.*` | FFmpeg 區段（狀態、路徑、版本、缺少項目、被拒絕的候選、設定的路徑、下載是否可用；`Locate...`、`Download minimal build`、`Re-detect`）、Variants 表（存在 / MISSING / stale）、兩個新按鈕。 |
| `Jobs/JobRunner.*` | 新增 `WaitIdle`（結束時等所有 job 的工作函式返回）。 |

唯一對 Core/offline 的改動（**僅新增**）：`WebmTranscodeOptions::inputStartNumber`（預設 -1，行為不變）會在 `-i` 前加 `-start_number N`，供 PNG 序列使用（image2 只會自己試前 5 個編號，`frame_1000.png` 起算的序列不給 `-start_number` 就讀不到）。`FfmpegCommands.h/.cpp` 各幾行；該庫的自測沒有重跑。

### 10.2 執行緒規則（沿用審查後的規則）

- 資產管理器只在主執行緒碰：`PlanImport`（佇列輪到該檔案時）、job（只做檔案 I/O、ffmpeg、探測）、完成回呼在主執行緒 `FinishImport` / 套用結果。沒有任何主執行緒對 `PostOnMainThread` 的 `.get()`；資產管理器匯入器（引擎背景佇列）對需要 FFmpeg 的匯入只 `PostOnMainThread` 後**立即返回空陣列**（引擎佇列不被數分鐘的 ffmpeg 占住；資產由佇列自己合併）。
- 所有 job 走 `CJobRunner`，帶 owner 標籤（資產 GUID；偵測 = `"ffmpeg"`，下載 = `"ffmpeg-download"`）；取消旗標傳進庫的 `Control`（先對 ffmpeg stdin 寫 `q`，500 ms 後終止 Job Object）。
- 結束：`eNotify_OnQuit` 先呼叫佇列 `CancelAll(permanent)`，再 `CJobRunner::CancelAll`，再 **`WaitIdle(8 s)`**。冒煙測試發現 Sandbox 在該通知後可能直接結束行程而不卸載外掛，沒有等待時被取消的 job 來不及刪除半成品（留下 `.stage.webm.tmp` 與來源複本）；加入等待後乾淨。
- 失敗 / 取消：`ImportVideoFile` 與 `SubmitTranscodeOps` 追蹤「本次從無到有建立的檔案」，失敗時刪除（既有檔案不刪）；庫自己負責 `.tmp` 與兩段式 log。取代既有資產時，轉檔先寫 `<名稱>.stage.webm`（庫再寫 `.stage.webm.tmp`），最後才改名覆蓋，所以預覽播放器不會在長時間轉檔中途重新打開要被取代的檔案。

### 10.3 檔案配置與 Details

- WebM 且不重編：同 M4（`<name>.webm` = 來源複本）。
- mp4 / mov / mkv / avi：原檔複製為 `<name>_source.<副檔名>`（來源，不進 pak：`tools/list_unshipped_cev_files.py` 會列出「資產有、描述檔沒播」的檔案），轉出 `<name>.webm`（VP9 + Opus，來源有 alpha 則 yuva420p，由庫的 `AlphaMode::Auto` 決定）。
- WebM + 重編：原檔複製為 `<name>_source.webm`，轉出 `<name>.webm`。
- PNG 序列（資料夾；單一 `.png` 檔**不**註冊給匯入器，貼圖照舊）：不複製（只記錄資料夾），`ffmpeg -framerate N -start_number first`；最長的連續編號從最小編號起算，缺號會警告並只用連續的部分；路徑含 `%` 拒絕。
- 變體：`<name>_<高度>p.webm`，來源 = 描述檔的主影片（`Strip audio` 後是 `_silent.webm`，所以變體也沒聲音，與主影片一致）；高度不低於影片者略過並記錄；寬高由探測實際檔案取得；描述檔 `<Variants rule="smallestNotBelowOutput">`，依高度遞增。覆蓋匯入（`overwrite`）時舊變體與舊來源複本（名稱不同時）移除。
- 新 Details：`transcoded`（1 = `<name>.webm` 由來源複本 / PNG 序列轉出）、`sourceKind`（webm / video / png-sequence）、`importSettings` 增加 `recompress`、`crf`、`height`、`variants`、`fps`。
- 音訊操作（分離 / WAV / Ogg / 靜音）一律以 `<name>.webm` 為來源（轉檔後那才有 Opus 音軌；`sourcePath` 可能是 mp4）。

### 10.4 操作

- **Generate variants...**：選高度（影片高度以上的灰掉）後跑 job，完成後（主執行緒）更新描述檔 / Files / Details；已存在的變體不動，重做會覆蓋。
- **Re-compress / resize...**：來源 = 儲存的來源複本；沒有（舊的 WebM 資產、PNG 序列）時**目前的影片先複製為 `<name>_source.webm`** 並成為來源（再次重編不會一層層劣化）。輸出先寫 stage 檔，完成後在主執行緒 `ReleaseAssetFiles`（關預覽播放器、取消資產的其他 job）再改名取代，GUID 不變；若描述檔播的是 `_silent.webm`，一併重新去除音軌。Details 的尺寸 / 編碼 / alpha 與 `importSettings` 更新。對話框列出的變體 = 完成後應有的集合（未勾選者連檔案一起移除）。
- **Reimport source**（轉檔過的資產）：改走同一個 job（先把原檔複製過來，再以儲存的設定重編並重做現有變體）；沒轉檔的資產維持 M4 行為。
- 資訊欄 Variants 表：存在 / MISSING；**stale** = 變體檔比 `<name>.webm` 舊（超過 2 秒）。

### 10.5 FFmpeg 缺少時的流程

- 偵測：設定路徑、下載副本、PATH（`Locate` 順序不變）。
- 互動（匯入對話框按 OK、`Generate variants...`、`Re-compress...`）：`EnsureUsableModal`。下載可用時提供「Download minimal build」與「Choose ffmpeg.exe...」（含授權 / 來源 / 雜湊驗證說明，下載有進度與取消）；`FfmpegDownloadDisabledReason()` 非空（目前釘住 `unreleased`）時視窗顯示原因，**只**提供手動選擇。選到的檔案再過一次能力檢查，不足就顯示缺少項目 / 被拒絕的原因。SHA-256 不符：庫拒絕並不解壓，訊息含實際與預期雜湊（視窗與 log 都會顯示）。
- 非互動（console）：Absent 時拒絕並印出原因與設定方式；偵測仍進行中則 job 等待結果（`WaitForExe`）。

## 11. Console 指令

| 指令 | 說明 |
|---|---|
| `cev_editor_ffmpeg_status [redetect]` | 一行（加被拒絕候選行）：`ffmpeg status: ready, path ..., version ..., source ...; missing recommended: ...; missing for import: ...; configured path: ...` 或 `absent (...)`，以及下載是否啟用。 |
| `cev_editor_ffmpeg_path <路徑 或 ->` | 設定（`-` 清除）持久的 ffmpeg 路徑（檔案或資料夾）並重新偵測；結果以 status 行記錄。 |
| `cev_editor_import <來源> <資料夾> [選項]` | 來源可為 webm / mp4 / mov / mkv / avi 或 PNG 資料夾。選項（`=` 在腳本裡會被當成 cvar 設定，一律可用 `:`）：`name:` `lang:` `wav` `ogg` `split` `strip` `cue` `mixed` `overwrite`，FFmpeg 階段 `crf:N` `height:PX` `variants:540,360` `fps:N` `recompress`（`crf` / `height` 隱含 `recompress`）。純 WebM 複製同步執行；其餘進佇列，結果記為 `import ok: <來源> -> <asset>` / `import failed: ...`。 |
| `cev_editor_import_batch <資料夾或清單檔> <資產資料夾> [選項] [sequences]` | 資料夾 = 其中所有影片檔（不遞迴；`sequences`：子資料夾視為 PNG 序列）；清單檔 = 每行一個路徑，`#` 註解。一個佇列，一次一個，結尾 `batch '...' finished: N file(s): ok a, failed b, cancelled c` 加逐檔結果。 |
| `cev_editor_import_cancel` | 取消執行中的並丟棄排隊的。 |
| `cev_editor_variants <asset> <高度> [crf:N]` | 產生變體。 |
| `cev_editor_recompress <asset> [crf:N] [height:PX] [variants:a,b 或 none] [refresh]` | 重新壓縮。 |
| `cev_editor_ffmpeg_install_test <zip> <sha256> [版本]` | 測試用：下載流程用本機 zip（`LocalFileFetcher`），雜湊驗證 / staging / 安裝與真實下載相同。 |

測試用環境變數：`CEV_EDITOR_SETTINGS`（設定檔位置）、`CEV_EDITOR_FFMPEG_NO_PATH=1`（偵測時不搜尋 PATH）。

## 12. 驗證

建置（Profile）：vanilla `D:/coding11/Cryengine_5.7.1` 與社群樹 `D:/coding11/CryENGINE-5-7-community`，目標 `EditorCEVPlayer` + `CEVPlayer`，皆 **0 個錯誤、0 個警告**（`D:/coding11/_buildlogs/A-m5*.log`、`B-m5*.log`）。

冒煙測試（vanilla Sandbox，`CEV_EDITOR_SCRIPT`，腳本與輸出在 `D:/coding11/_testproject/smoke/m5_*.txt|out`，log 在 `smoke_logs/` 下各次 `Game.log`；全部 `RESULT: PASS (sandbox, ..., clean quit, no crash artifacts)`）：

| 項目 | 結果與證據 |
|---|---|
| (a) 系統 ffmpeg 偵測 | `ffmpeg status: ready, path C:\Users\User\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_...\ffmpeg-8.0.1-full_build\bin\ffmpeg.exe, version 8.0.1-full_build-www.gyan.dev, source system PATH; missing recommended: none; missing for import: none`（啟動後約 2 秒，job 執行緒） |
| (b) mp4 轉 webm，`crf:34 variants:540,360 wav` | `import ok: .../m5_h264.mp4 -> Videos/m5test/m5_h264.cevvideo.cryasset`（約 12 秒）；磁碟：`m5_h264.webm`（4.3 MB）、`_source.mp4`、`_540p.webm`、`_360p.webm`、`_english.wav`；描述檔 `<Variants rule="smallestNotBelowOutput">` 含 `m5_h264_360p.webm 640x360`、`m5_h264_540p.webm 960x540`；`cev_editor_open` 後 `opened video asset 'm5_h264'`、`preview draws texture id 503 (1920x1080, alpha 0)`；之後 `cev_editor_recompress ... crf:40 height:720` 得 `FFmpeg operation for 'm5_h264' finished: 2 variant(s), video re-encoded`。 |
| (c) ProRes 4444 `.mov` alpha | `import ok`（1 秒）；預覽 `preview draws texture id 600 (640x360, alpha 1)`；ffprobe `alpha_mode=1`。alpha WebM 重編（`crf:40 variants:180`）與僅變體（`variants:180,90`）後主檔與變體皆 `ALPHA_MODE=1`。PNG 序列（`frame_1000..1011.png`，`fps:12`）：`alpha 1`、1 秒、240x320。 |
| (d) 批次 3 檔 | `batch '.../batch' finished: 3 file(s): ok 3, failed 0, cancelled 0`（b1.mp4、b2.mkv、b3.avi 依序，各約 1 秒）。 |
| 覆蓋匯入 | 不帶 `overwrite`：`import failed: ...: skipped, The video asset 'm5_h264' already exists ...`，檔案不動；帶 `overwrite`：GUID `73aa11ba-...` 不變，舊 540/360 變體與檔案移除、新 `_720p` 產生，原有 wav 保留。 |
| (e) 取消 | `cev_editor_import_cancel` 得 `import cancelled: ...m5_long.mp4`，`m5cancel/` 空（`.stage.webm.tmp`、`_source.mp4` 都被刪）；第二次匯入進行中關閉 Sandbox：乾淨結束、`m5cancel2/` 空、無殘留 ffmpeg 行程。（第一版沒有 `WaitIdle` 時，結束當下殘留 `.stage.webm.tmp` 與 77 MB 來源複本：已修。） |
| (f) 假 ffmpeg | 路徑設成 whoami.exe 改名、`CEV_EDITOR_FFMPEG_NO_PATH=1`：`ffmpeg status: absent (insufficient or not found): no usable ffmpeg: ...bogus_ffmpeg.exe (the process exited with code 1)` 加 `rejected: configured path ...`；`cev_editor_import` mp4、帶 `crf:` 的 webm、`variants`、`recompress` 皆 `refused: FFmpeg is not available (...). Locate ffmpeg.exe in the FFmpeg section ...`；批次三檔各 `import failed: ... FFmpeg is not available`，摘要 `ok 0, failed 3`；純 WebM 匯入仍成功。 |
| 下載安裝路徑（本機 zip） | 雜湊錯誤：`ffmpeg download / install FAILED: SHA-256 mismatch: the download is acd623..., expected 0000...; refusing to install`，狀態仍 absent；正確雜湊（zip 內是完整版 ffmpeg.exe）：`ffmpeg download / install ok: installed ...\ffmpeg\localtest\ffmpeg.exe`，狀態 `ready, ... source downloaded copy`，接著用它匯入 mp4 成功。測後已刪除該安裝資料夾。 |
| (g) GameLauncher | `smoke_test.sh ... --exec "cev_variant N;cev_play Videos/m5test/m5_h264.cevvideo"`：`cev_variant 1` 得 `player 1: variant 1 <Videos/m5test/m5_h264_720p.webm> 1280x720 chosen` 與 `player 1 ready: <..._720p.webm> 1280x720 5.03s audio`；`cev_variant 0` 得 `ready: <Videos/m5test/m5_h264.webm> 1920x1080`；`-1`（自動）選了變體 1（720p）。皆 PASS。 |

## 13. 未驗證與限制

- 真實下載（WinHTTP + GitHub Release）：釘住版本仍是 `unreleased`，UI 只驗到「停用 + 原因 + 只能手動選擇」的邏輯與本機 zip 的雜湊 / 安裝路徑；對話框（`EnsureUsableModal`、`Set up FFmpeg...`、進度對話框、下載按鈕）**沒有人工點過**，見 `manual-acceptance-M5.md`。
- 匯入對話框、編輯器視窗拖放（單檔 / 多檔 / PNG 資料夾）、`Generate variants...` / `Re-compress...` 對話框、資訊欄 FFmpeg 區段與 Variants 表只經編譯與 console 路徑（同一組後端）驗證，沒有 UI 自動化。
- 對話框的變體清單不事先探測來源高度（對話框時來源高度未知）：高度不低於影片者由 job 略過並記錄，不是灰掉（已存在資產的對話框則用已知高度灰掉）。
- 失敗或取消後，匯入時新建的**空資料夾**會留下（檔案都已刪除）。
- 社群樹只建置，沒有執行（其 Sandbox / GameLauncher 本身有已知問題，見 `docs/research/03-smoke-test.md` §5）。
- 最小版 FFmpeg 未實測（測試用的是完整版 8.0.1）。
- `Reimport source`（轉檔過的資產）的按鈕本身沒有單獨跑；`cev_editor_recompress ... refresh` 走同一個 job，只有 `refresh` 之外的路徑被實跑。

## 2026-11-19 修訂（E1；本節優先於上文的對應段落）

- FFmpeg 路徑設定改存在編輯器的偏好設定頁 `CEVPlayer/FFmpeg`；舊的 `%LOCALAPPDATA%\CEVPlayer\editor.json` 只在第一次啟動時讀入一次。`CEV_EDITOR_SETTINGS` 仍可作為讀取的退路（測試用）。Info 欄的 FFmpeg 區塊只顯示狀態與「Open preferences」。
