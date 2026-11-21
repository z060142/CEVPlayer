# 05a · 階段 0 獨立部分（S0-A）：Core 獨立建置、Core 基準、色度位置、Media Foundation 硬體解碼

日期：2026-11-19。分支 `stage0/standalone`（工作樹 `E:/coding crycry/cev-wt-s0a`，基底 4d0706c 之後的 S0 任務單）。只量不改：新增的只有 `tools/probes/` 下的量測程式，產品程式碼未改動。

本機：i5-13600K（量測行程以 `SetProcessAffinityMask(0xFFF)` 釘在效能核，`tools/probes/run_pinned.py`）、Windows 11 Pro 10.0.26200、VS 2022、Windows SDK 10.0.26100。
顯示卡（程式列出，`mf_decode_probe --enum`）：adapter 0 `NVIDIA GeForce RTX 4070 Ti`（預設）、adapter 1 `Intel(R) UHD Graphics 770`、adapter 2 `Microsoft Basic Render Driver`。

原始輸出（全部保留）：`E:/coding3/xt-bench/cev-s0/`——`cevcore_selftest.log`、`core_bench.log`、`mf_enum.log`、`mf_probe.log`（完整）、`mf_probe_summary.txt`（摘要）、`media/`（六支量測素材）、`make_bench_media.sh`。

## 1. Core 獨立建置與自測

```
cmake -S Code/CryPlugins/CEVPlayer/Core -B T:/cev/core -G "Visual Studio 17 2022" -A x64 \
      -DCEV_SDK_DIR="E:/coding crycry/CEVPlayer-rebuild-integration/Code/SDKs" -DCEV_TEST_MEDIA_DIR=T:/cev/media
cmake --build T:/cev/core --config Release -- -m:8
PATH=~/scoop/shims:$PATH tools/make_test_media.sh T:/cev/media      # scoop ffmpeg 7.1.1（PATH 上第一個 ffmpeg 是 ImageMagick 附的 4.2.3，須排在後面）
T:/cev/core/Release/cevcore_selftest.exe --media T:/cev/media
```

- SDK 用主樹 `Code/SDKs/CEVPlayer` 的預建庫（唯讀連結，未複製）。建置無警告（`/W4 /WX`）。
- 結果：`Summary: 6228 checks, 0 failures, 58.6 s, total allocations 42208 / frees 42207` → **`RESULT: PASS`**。

量測素材（`tools/probes/make_bench_media.sh`，ffmpeg 7.1.1，libvpx-vp9 realtime cpu-used 8 crf 34，10 秒，`-g`＝`-keyint_min`＝2 秒的幀數；alpha 版以 `gradients` 當 alpha、`-auto-alt-ref 0`）：

| 檔案 | 尺寸 | fps | alpha | 大小 |
|---|---|---|---|---|
| vp9_1080p30.webm | 1920×1080 | 30 | 無 | 10.1 MB |
| vp9_1080p30_alpha.webm | 1920×1080 | 30 | 有 | 10.3 MB |
| vp9_1080p60.webm | 1920×1080 | 60 | 無 | 19.9 MB |
| vp9_1080p60_alpha.webm | 1920×1080 | 60 | 有 | 20.5 MB |
| vp9_2160p30.webm | 3840×2160 | 30 | 無 | 39.2 MB |
| vp9_2160p30_alpha.webm | 3840×2160 | 30 | 有 | 39.7 MB |

## 2. Core 基準（不經引擎）

程式：`tools/probes/core_bench/core_bench.cpp`（連 `CEVPlayerCore`）。單執行緒迴圈、不節流、無聲音：每個封包 `VideoDecoder::Decode`（主流＋alpha 流）→ `WriteFrame` 進 FrameRing slot（＝複製）→ `ConvertYuvaToRgbaPremultiplied`（＝轉色）。`threads=0` 走 PlayerCore 的自動規則（`PlayerCore.cpp:234-238`：高 ≥1000 → 4、≥700 → 2、其他 1）。ring RAM 依 `FrameRing::Init` 的配置（列寬 16 對齊）×4 格。牆鐘時間，ms。

| 素材 | threads | 解碼 平均/p99 | 複製 平均/p99 | 轉色 平均/p99 | 平面 B/幀 | RGBA B/幀 | 打包排列 R8 | ring 4 格（含 RGBA／不含） |
|---|---|---|---|---|---|---|---|---|
| 1080p30 | 4（自動） | 1.38 / 2.51 | 0.20 / 0.38 | 4.04 / 4.31 | 3,110,400 | 8,294,400 | 1920×1620 | 43.5 / 11.9 MB |
| 1080p30 | 1 | 3.28 / 5.89 | 0.23 / 0.35 | 3.99 / 4.27 | 〃 | 〃 | 〃 | 〃 |
| 1080p30 alpha | 4（自動） | 2.04 / 3.60 | 0.36 / 0.60 | 5.24 / 5.63 | 5,184,000 | 8,294,400 | 1920×2700 | 51.4 / 19.8 MB |
| 1080p30 alpha | 1 | 4.33 / 7.67 | 0.41 / 0.68 | 5.19 / 5.35 | 〃 | 〃 | 〃 | 〃 |
| 1080p60 | 4（自動） | 1.35 / 2.17 | 0.19 / 0.31 | 4.06 / 4.30 | 3,110,400 | 8,294,400 | 1920×1620 | 43.5 / 11.9 MB |
| 1080p60 | 1 | 3.20 / 5.36 | 0.22 / 0.37 | 4.00 / 4.35 | 〃 | 〃 | 〃 | 〃 |
| 1080p60 alpha | 4（自動） | 1.98 / 3.19 | 0.34 / 0.59 | 5.25 / 5.61 | 5,184,000 | 8,294,400 | 1920×2700 | 51.4 / 19.8 MB |
| 1080p60 alpha | 1 | 4.25 / 6.22 | 0.39 / 0.58 | 5.21 / 5.61 | 〃 | 〃 | 〃 | 〃 |
| 2160p30 | 4（自動） | 5.62 / 9.33 | 0.94 / 1.29 | 15.72 / 16.54 | 12,441,600 | 33,177,600 | 3840×3240 | 174.0 / 47.5 MB |
| 2160p30 | 1 | 12.92 / 21.41 | 1.02 / 1.38 | 15.59 / 16.49 | 〃 | 〃 | 〃 | 〃 |
| 2160p30 alpha | 4（自動） | 7.87 / 12.74 | 1.69 / 2.19 | 20.53 / 21.62 | 20,736,000 | 33,177,600 | 3840×5400 | 205.7 / 79.1 MB |
| 2160p30 alpha | 1 | 16.91 / 27.81 | 1.75 / 2.18 | 20.49 / 21.50 | 〃 | 〃 | 〃 | 〃 |

觀察：
- 轉色是最貴的一段：1080p 約 4～5.2 ms，2160p 約 15.7～20.5 ms（單執行緒、純 C 迴圈），比解碼（自動執行緒）還貴 2～3 倍。2160p30 alpha 的解碼＋複製＋轉色合計約 30 ms，逼近 33 ms 幀預算。
- 複製的最大值（7～37 ms）都出在第一幀（slot 記憶體第一次被碰到的 page fault），p99 不受影響。
- 每幀位元組：打包排列與現行平面位元組相同（`w×(h+ch[+h])`），是 RGBA8 的 37.5%（無 alpha）／62.5%（有 alpha）。設計文件 §2 推算的 3.1 MB／5.2 MB／8.3 MB、ring 46～54 MB 與實測相符（1080p：43.5～51.4 MB）。
- 這些是不經引擎的純計算數字；引擎內的上傳耗時不在本單（S0-B）。

## 3. 色度位置

(a) **現行 CPU 轉換是最近點（無內插）**。`Core/src/ColorConvert.cpp:76-87`：列取 `row >> sy`、行取 `x >> sx`，即色度樣本 i 原樣複製給亮度 2i 與 2i+1（垂直同理）。最近點複製只有在色度樣本位於 2i 與 2i+1 正中間時才對兩個亮度樣本都是「最近」，所以它等效於 **center（水平、垂直都置中，JPEG／MPEG-1 式）**。對 left（MPEG-2 式，水平與 2i 共位）的素材，等於把色度往右偏移了 0.5 亮度像素（且無內插）。

(b) **`ChromaSitingHorz/Vert` 沒有被讀**。`Core/src/WebmDemuxer.cpp:730-736` 只取 `Colour::matrix_coefficients` 與 `Colour::range`；libwebm 的 `mkvparser::Colour` 本身有解析 `chroma_siting_horz/vert`（`mkvparser.h:465-466`），只是 Core 沒用。`VideoStreamInfo` 也沒有對應欄位。
素材實際標記（`tools/probes/mkv_colour.py` 逐元素解析 Video/Colour；ffprobe 交叉確認）：

| 素材 | Colour 內容 | ffprobe chroma_location |
|---|---|---|
| `make_test_media.sh` 的全部視訊（vp9_1080p30_opus、vp9_alpha、vp8_vorbis…） | 只有 `Range=1` | unspecified |
| S0 六支量測素材（ffmpeg lavfi → libvpx-vp9） | 只有 `Range=1` | unspecified |
| FFmpeg 7.1.1 把標 left 的 H.264（`-chroma_sample_location left`）轉成 VP9 WebM | `Range=1, ChromaSitingHorz=1, ChromaSitingVert=2` | left |

即：合成來源的素材**不標位置**；從一般 H.264／MPEG 來源轉出的素材會帶 `Horz=1（左共位）, Vert=2（半）`＝MPEG-2 的 left。匯入轉碼（`offline/src/ffmpeg/FfmpegCommands.cpp:255-275`）沒有指定 chroma 位置，沿用 ffmpeg 從來源帶過去的值；它也只在 `colorMatrix==bt709` 時寫 `-colorspace`，所以 MatrixCoefficients 多半缺席，Core 依高度猜（≥720 → BT.709）。
另外：試圖以 `-chroma_sample_location center` 產生 center 標記的 H.264 來源，ffprobe 仍回報 left，所以「center 標記經轉碼後寫成什麼」**未能量測**（找不到能產生 center 標記來源的現成方法；未再深究）。

(c) 契約建議（以實測為依據，數值由主模型裁決）：
- `chromaSiting`：應反映**素材**的位置，而非現行 CPU 轉換的做法——打包平面是由消費端的著色器取樣，取樣位置要跟素材一致。對應：`Horz=1,Vert=2` → `CTS_SITING_LEFT`；`Horz=2,Vert=2` → `CTS_SITING_CENTER`；`Horz=1,Vert=1` → `CTS_SITING_TOPLEFT`；缺席 → 需要一個預設值（本機能看到的帶標記素材全是 left；缺席的素材全是合成來源，看不出原意）。這需要 Core 在 `VideoStreamInfo` 加讀 `chroma_siting_horz/vert`（M8 的工作，本單未改）。
- RGBA8 排列與現行 CPU 轉換的關係：現行轉換等效 center，若契約對 left 素材回報 LEFT 而 RGBA8 路徑仍以 center 轉換，兩種排列在色度邊緣會有 0.5 像素的差（M8 驗收「兩種排列逐像素差 ≤ 2 級」在高彩度邊緣可能過不了）——**推論，未量**。
- `flags` bit3（平面色彩已乘 alpha）：**對 VP9 alpha 素材應為 0**。證據：`VideoDecoder::WriteFrame` 逐列 memcpy 平面，預乘只發生在 `ConvertYuvaToRgbaPremultiplied`（`ColorConvert.cpp:28-31, 95-99`）；實測 `vp9_alpha.webm` 中 alpha＝0 的像素（n=231,271）平均 Y 125.6／U 125.9／V 124.8，不是預乘後的黑（Y16 U128 V128），即編碼內容本身是 straight 色彩（`core_bench` 輸出最後一行）。

## 4. Media Foundation 硬體解碼探測

程式：`tools/probes/mf_decode_probe/mf_decode_probe.cpp`（連 Core 的 `WebmDemuxer` 解封裝、`VideoDecoder` 當 libvpx 參考；不進任何產品目標）。指令：
`GPUCTR=1 python tools/probes/run_pinned.py <log> 600 mf_decode_probe.exe <clip> --frames 600 --compare 10`。
GPU 引擎使用率以 `\GPU Engine(*)\Utilization Percentage` 依 pid 過濾取樣（每秒一次）。
「節流」＝依影片幀率餵封包（`timeBeginPeriod(1)`＋Sleep），這時的行程 CPU 才代表實際播放成本；不節流的數字代表吞吐上限。

### 4.1 列舉與 MFT 屬性（實際讀到的值）

| 項目 | 結果 |
|---|---|
| `MFTEnumEx(VIDEO_DECODER, VP90, 0x43)` | 1 個：`VP9VideoExtensionDecoder` |
| `MFTEnumEx(…, VP80, 0x43)` | 同一個 `VP9VideoExtensionDecoder` |
| `MFTEnumEx(…, HARDWARE\|SORTANDFILTER 0x44)` | VP90／VP80 都是 0 個 |
| `MFTEnumEx(…, ALL\|UNTRUSTED_STOREMFT 0x43F)` | 仍是同一個 |
| `MFT_TRANSFORM_CLSID_Attribute` | GUID_NULL（Store 套件的 activate 不帶 CLSID） |
| `MF_TRANSFORM_FLAGS_Attribute` | 0x1（`MFT_ENUM_FLAG_SYNCMFT`） |
| `MFT_ENUM_HARDWARE_URL_Attribute`／merit | 無／0 |
| `MF_TRANSFORM_ASYNC` | 未設定 → **同步 MFT**（不需 unlock、不走事件模型） |
| `MF_SA_D3D11_AWARE` | **1** |

### 4.2 輸出

| 模式 | 輸出類型（`GetOutputCurrentType`） | 樣本 |
|---|---|---|
| 無 D3D manager（VP9、VP8） | NV12，frame＝影片尺寸（1920×1080），`MF_MT_DEFAULT_STRIDE` 缺席，aperture＝影片尺寸，`MF_MT_VIDEO_CHROMA_SITING` 缺席；stream info flags 0x7（不提供樣本，需呼叫端配置，cbSize＝w×h×1.5） | 系統記憶體；**不是** `IMFDXGIBuffer`，也沒有 `IMF2DBuffer`；以 `Lock` 取，列寬＝影片寬（逐像素比對 0 差證明） |
| 自建 D3D11 裝置＋DXGI device manager（VP9） | NV12，frame **1920×1088**（1080p，高對齊 16）／3840×2160／640×368（360p），aperture＝實際影片尺寸，stride 缺席；flags 0x107（`MFT_OUTPUT_STREAM_PROVIDES_SAMPLES`）；第一次 `ProcessOutput` 回 `MF_E_TRANSFORM_STREAM_CHANGE` 一次 | **`IMFDXGIBuffer`＝是**（也有 `IMF2DBuffer`）：`ID3D11Texture2D` 格式 103（`DXGI_FORMAT_NV12`），ArraySize 1、Mip 1、**BindFlags 0x200（只有 `D3D11_BIND_DECODER`）**、Usage DEFAULT、subresource 0 |
| 同上，VP8 | **`SetInputType` 失敗 `0xC00D6D76`（`MF_E_UNSUPPORTED_D3D_TYPE`）** | — |

另試：在 `SetOutputType` 前對輸出流屬性設 `MF_SA_D3D11_BINDFLAGS = DECODER|SHADER_RESOURCE`（P5）→ 輸出貼圖仍是 0x200，未被採納（第二版「直接當貼圖用」需要另外的複製或處理器）。

### 4.3 是否為硬體解碼

| 素材 | 無 manager：節流時行程 CPU（ms/幀，佔一核%） | 有 manager：節流時行程 CPU | GPU VideoDecode 引擎（有 manager 期間，取樣最大值） |
|---|---|---|---|
| 1080p30 | 7.76 ms（23.4%） | 0.42 ms（1.3%） | 3.4% |
| 1080p30 alpha（兩個 MFT） | 9.90 ms（29.8%） | 0.47 ms（1.4%） | 4.1% |
| 1080p60 | 6.64 ms（39.9%） | 0.65 ms（3.9%） | 9.6% |
| 1080p60 alpha | 10.78 ms（64.8%） | 0.70 ms（4.2%） | 3.9% |
| 2160p30 | 35.37 ms（106.3%） | 0.63 ms（1.9%） | 12.6% |
| 2160p30 alpha | 62.55 ms（187.9%） | 0.99 ms（3.0%） | 9.3% |
| VP8 640×360 | 0.25 ms（0.6%） | 不可用（見上） | — |

結論：**VP9 給了 D3D manager 才是硬體解碼**（本行程出現 `engtype_videodecode` 使用率、CPU 降到 1～4%）；**不給 manager 時同一個 MFT 是軟體解碼**，而且比 Core 的 libvpx 慢（1080p30：MFT 7.8 ms CPU／幀 vs libvpx 單執行緒 3.3 ms 牆鐘）。**VP8 只能軟體**（給 manager 就拒絕輸入類型）。

### 4.4 每幀耗時與讀回（D3D11 模式，節流）

「讀回」＝`CopySubresourceRegion` 到 staging（同尺寸 NV12、STAGING、CPU_READ）＋`Map`＋把 Y 與 UV 列複製出來（模擬寫進 ring slot）＋`Unmap`。非同步＝本幀 copy 後 `Flush`，Map 的是 1 或 2 幀前的 staging。量的是呼叫端執行緒上的耗時（ms）。

| 素材 | 解碼呼叫（ProcessInput＋Output）平均/p99 | 同步讀回 平均/p99 | 非同步延 1 幀 平均/p99 | 非同步延 2 幀 平均/p99 | 讀回時行程 CPU（同步／延 1／延 2，佔一核%） |
|---|---|---|---|---|---|
| 1080p30 | 0.36 / 0.62* | 2.66 / 6.95 | 0.47 / 2.48 | 0.37 / 0.67 | 9.6 / 1.9 / 1.9 |
| 1080p60 | — | 1.76 / 4.26 | 0.38 / 0.76 | 0.40 / 0.73 | 13.9 / 3.4 / 5.8 |
| 2160p30 | — | 7.85 / 25.43 | 2.39 / 4.72 | 2.42 / 3.91 | 24.7 / 10.0 / 8.6 |
| 2160p30（alpha 素材的主流） | — | 7.55 / 14.11 | 2.27 / 3.41 | 2.05 / 2.65 | 25.8 / 9.6 / 8.8 |

\* 1080p30 alpha 探測的 P1b 數字（無讀回）；各素材 P1b 的解碼呼叫都在 0.4～0.6 ms 平均，完整值見 `mf_probe.log`。
- 延 1 幀時 `Map(DO_NOT_WAIT)` 仍偶有 `WAS_STILL_DRAWING`（1080p30：300 幀中 11 次）；延 2 幀全部 0 次。**注意：沒有 `Flush` 時，延 1～2 幀的 Map 幾乎每次都要等**（第一輪量到 299／300），立即 context 上的 copy 要先 Flush 才會送出。
- 非同步讀回在 2160p 仍需約 2.4 ms，主要是 12.4 MB 的列複製；1080p 約 0.4 ms。

### 4.5 alpha、seek、與 libvpx 的差

- **alpha 第二條流可以用第二個 MFT 解**（另一個 `IMFActivate` → 另一個實例，同一個 D3D manager）：1080p／2160p alpha 素材 300／600 幀全部有輸出；alpha MFT 每封包呼叫 0.10～0.14 ms（節流、D3D11）。
- **seek 正常**：`MFT_MESSAGE_COMMAND_FLUSH` 後從 4.0 秒的關鍵幀重餵 8 個封包 → 8 幀輸出，與 libvpx 從同一關鍵幀解出的畫面 Y 平面**最大差 0、平均差 0**（六支 VP9 素材、兩種模式都是；VP8 無 manager 模式也是）。
- **逐像素比對（Y 平面，前 10 幀）**：VP9 兩種模式、六支素材 **最大差 0、平均差 0**；alpha 流的 Y vs libvpx 的 A 平面 **最大差 0**；VP8（無 manager）最大差 0。即 MFT 的輸出與 libvpx 位元一致（UV 平面未比對）。

## 5. 沒做到的

- 「center 標記的來源經 FFmpeg 轉碼後標成什麼」：未能量測（見 §3(b)）。
- UV 平面與 libvpx 的比對：未做（只比 Y 與 alpha）。
- GPU 記憶體（VRAM）用量：未量。GPU 引擎使用率是每秒取樣，樣本數少（2～5 個），只當「有沒有用到 videodecode 引擎」的證據，不當成本數字。
- Intel UHD 770（adapter 1）上的 MFT 行為：未測（只用預設 adapter）。
- 不節流的 D3D11 模式 CPU 佔用（50～100%）是探測程式自己的忙等迴圈，不代表解碼成本，表中未採用。

## 6. 對設計文件（M6-ui-source.md）的修正建議

1. §6「啟動時探測…回報不是硬體加速 → 用 libvpx」：這個 MFT **沒有任何屬性表示硬體**（`MF_TRANSFORM_FLAGS`＝SYNCMFT、無 hardware URL、HARDWARE 列舉為 0）。實際判定方式是「給 D3D manager 後 `SetInputType` 是否成功」：VP9 成功＝硬體路徑；VP8 回 `MF_E_UNSUPPORTED_D3D_TYPE`。建議改寫為：只在有 D3D manager 時使用 MF；`SetInputType`／`SET_D3D_MANAGER` 失敗即退回 libvpx；**不使用無 manager 的 MF 路徑**（它是軟體解碼且比 libvpx 慢）。
2. §6 與 §8「VP8 的硬體解碼支援面很窄」：本機實測是**完全不行**（同一個元件、有 manager 就拒絕）。建議 `Decode=GPU` 對 VP8 一律退回 CPU。
3. §6「同一個元件處理 VP8 與 VP9」：列舉上正確，但 VP8 只有軟體路徑。
4. §6 第一版讀回：「非同步 Map（排隊 1～2 幀）」需補一句：copy 之後要 `ID3D11DeviceContext::Flush`，否則延遲的 Map 照樣等；延 2 幀在本機沒有任何一次需要等。輸出貼圖高度會對齊（1080 → 1088），讀回時以 `MF_MT_MINIMUM_DISPLAY_APERTURE`／影片尺寸裁切，UV 平面起點＝`RowPitch × 貼圖高`（不是影片高）。第一次輸出前一定有一次 `MF_E_TRANSFORM_STREAM_CHANGE`。
5. §6 第二版（直接當貼圖）：輸出貼圖 BindFlags 只有 `DECODER`，要求 `SHADER_RESOURCE` 的屬性未被採納——不能直接拿來取樣，第二版的設計要加一次 GPU 複製或 video processor，且 MFT 建在自己的裝置上，與引擎裝置共用需要共享貼圖。
6. §6 `Auto` 規則：實測 1080p30 的 libvpx 解碼（4 執行緒）1.4 ms＋轉色 4 ms；MF 硬解＋非同步讀回在呼叫端約 0.4 ms＋0.4 ms。差距在 1080p 已明顯、2160p 更大（libvpx 5.6 ms＋轉色 15.7 ms vs 硬解 0.6 ms＋讀回 2.4 ms）。「高度 ≥1080 用 GPU」與數字一致；低於 1080 的成本小，維持 CPU 合理。
7. §2「數字為依尺寸推算，未量」：ring 與每幀位元組已實測（§2 表），與推算相符；可改為引用本文件。另：轉色才是 CPU 的最大項（2160p alpha 20.5 ms），§5.2「RGBA 改為按需」的收益應以此為主要依據。
8. §3 規則 7／契約 `chromaSiting`：Core 目前不讀容器的 ChromaSiting，M8 需要在 `VideoStreamInfo` 加欄位；缺席時的預設值需要主模型裁決（本機能看到的帶標記素材全是 left）。`flags` bit3 對 VP9 alpha＝0（平面是 straight）。
9. §5.3 第 3 點「匯入轉碼加 `-g`」：確認現行 `FfmpegCommands.cpp` 沒有 `-g`，素材關鍵幀間隔由 libvpx 預設決定；測試素材 `vp9_alpha.webm`（5 秒）全片只有第 0 幀是關鍵幀。
