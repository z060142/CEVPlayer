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

- 授權：`--disable-gpl --disable-nonfree --disable-version3`；FFmpeg 本體 LGPL-2.1+，外部函式庫 libvpx / libopus / libogg / libvorbis / dav1d 皆為 BSD。建置腳本在打包前檢查 `ffmpeg -version` 的 configuration 不含 gpl/nonfree/version3、`-L` 為 LGPL，並檢查範圍內的編解碼器確實存在。
- 建置腳本 `tools/ffmpeg/build_minimal.sh`（可在本機 MSYS2 MINGW64 執行）+ `tools/ffmpeg/versions.env`（所有來源的版本、URL、SHA-256 釘死：FFmpeg 8.0.3、libvpx 1.15.0、opus 1.5.2、libogg 1.3.5、libvorbis 1.3.7、dav1d 1.5.3；來源下載後先驗雜湊再解開）。
- 組件名稱已對照 FFmpeg 8.0.3 原始碼確認存在（`allcodecs.c`、`allformats.c`、`allfilters.c`、`parsers.c`、`bitstream_filters.c`、`protocols.c`、`configure` 的 `ffmpeg_select`）；libvpx / dav1d / opus / libvorbis 的 configure 選項也對照過原始碼。
- 打包內容（zip 根目錄平放）：`ffmpeg.exe`、`LICENSE`（LGPL-2.1）、`licenses/`（各元件授權）、`BUILD.txt`（來源 URL + SHA-256、工具鏈版本、configure 引數）、建置腳本與 `versions.env`。**建置腳本與來源版本隨二進位一起發佈**（LGPL 要求與可重現性）。
- 發佈檔名 `ffmpeg-minimal-<FFMPEG_VERSION>-cev<CEV_BUILD_REV>-win64.zip`，另附 `.sha256`。

### 工作流程 `.github/workflows/ffmpeg-minimal.yml`

- 觸發：手動（`workflow_dispatch`，可勾選 publish）與 tag `ffmpeg-minimal-v*`（tag 必須等於 `versions.env` 算出的版本，否則失敗）。
- build 工作（windows-latest + MSYS2 MINGW64，`msys2/setup-msys2@v2`）：解析版本 → 快取下載 → 安裝工具鏈（toolchain、nasm、meson、ninja、pkgconf、zip）→ `build_minimal.sh` → **之後**才安裝參考用的完整 ffmpeg（避免其函式庫混入建置）→ `smoke_test_minimal.sh`（用與外掛相同的引數轉 MP4/PNG alpha/ProRes 4444/alpha 變體，用 ffprobe 驗證；並確認最小版不能編 H.264）→ 計算 SHA-256 寫入工作摘要（附可直接貼進標頭的常數）→ 上傳 artifact。
- publish 工作（ubuntu-latest，只在 tag 或勾選 publish 時）：驗證 `.sha256` → `gh release create/upload` 到本 fork（`z060142/CEVPlayer`）的 Releases。

### 未驗證（誠實聲明）

CI 工作流程與建置腳本**沒有在實際的 MSYS2 / GitHub Actions 上執行過**（開發環境沒有 MSYS2 與 Actions）。已做的只有：YAML 以 python `yaml` 解析、內嵌 shell 與兩支腳本以 `bash -n` 檢查語法、組件與選項名稱對照原始碼、下載來源的 SHA-256 實際下載後計算。第一次執行可能需要小修（例如 libvpx 的 `--target`、靜態連結旗標、msys 路徑轉換）。`smoke_test_minimal.sh` 的檢查與自測 `cevffmpeg_selftest` 的真實執行部分對應（後者已對完整版 ffmpeg 8.0 執行過）。

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
