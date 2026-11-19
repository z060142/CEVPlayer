# M6～M10 · 經由 UI 播放：Surface ID、貼圖來源契約、節點整理、打包平面、硬體解碼

狀態：**規劃定案 2026-11-19，尚未實作**。本文件是 CEVPlayer 這一側的設計；消費端（CryXTUI）的設計在 `z060142/XTFrame-CRYENGINE` 的 `docs/09-video-source-contract.md`（§9.10：UI 的影片播放全部交給播放來源），中立的執行期能力在 XTFrame 主倉 23 章。契約的 C 宣告在本文件 §3——兩倉各自照抄一份，**互不引用對方的檔案、互不連結、不認對方的 DLL 名**；任一方不存在時另一方照常編譯與運作。

## 1. 使用者的決定（2026-11-19）

- 「我需要它的寫入貼圖能力可以用簡單的方法指定 ID，然後 XTFrame 也要有接收的機制」「取代 CEVPlayer 原本的機制也沒關係」
- 「一個更進一步傳送原始解碼資料的方式……最省資料傳輸、CPU、GPU、RAM、VRAM」「互相引用是指兩者單獨編譯、單獨存在」
- 「把原版（main branch）的舊節點全部砍掉。然後將新節點分成開箱即用跟進階兩種，抽屜分開」
- 「用 Media Foundation 方案，CPU 解作為 Fallback，且 GPU 解跟 CPU 解設為可選，主要是為了不同場合（遊戲選單、遊戲開場、關卡讀取、關卡內大屏、關卡內小屏等）可以讓遊戲開發者分配負載」
- 「GPU 轉色的部分就變成 XTFrame 的能力就好，不用特地寫了」「本來影片撥放就應該經由 UI，所以所有完整好處都應該要經由 UI。路不要鎖死，任何 UI 都可以經由那些節點獲得資料」「素材只吃 webm，照舊」
- 渲染器 patch：先做不需要 patch 的版本（硬解後讀回）並量測，再決定。看不見時的行為由設計決定（§5.3）。XTFrame 編輯器不為 `engine://` 槽做佔位畫面。
- 關卡讀取期間的播放：XTFrame 必須能在讀取時運作，由 XTFrame 主線設計；CEVPlayer 等它定案後照它的機制做（M10，未設計）。

## 2. 現況與要改掉的東西（調查 2026-11-19，e4f1721）

- 識別：播放器以引擎貼圖名 `$name` 識別，沒有自己的登錄表；同名只警告、兩個播放器寫同一張貼圖（`Module/Player/PlayerManager.cpp:233-251`）；自動命名的貼圖在尺寸改變時名字與 id 都換（`VideoPlayerImpl.cpp:973-986`）。
- 資料路徑（1080p 一幀）：libvpx → 逐列複製進 ring slot 約 3.1 MB（含 alpha 5.2 MB）→ 工作執行緒 CPU 轉 RGBA8 預乘 8.3 MB → 主執行緒 `UpdateTextureInVideoMemory`（引擎先整塊複製 8.3 MB 進 render command，再 `UpdateSubresource`）。ring 4 格，每格 YUV(A)＋RGBA，約 46～54 MB；貼圖 8.3 MB。**數字為依尺寸推算，未量。**
- `outputRgba` 寫死為 true，只在載入時讀一次（`VideoPlayerImpl.cpp:42`、`PlayerCore.cpp:287`）。
- 節點：舊 FlowGraph 節點（`Module/FlowNodes/FlowNodes.cpp`）、舊元件（`TextureVideoPlayerComponent`）、相容層（`Module/Compat/`）、`vp_*` 與新的一套並存；同一個貼圖概念有三個名字。
- 公開標頭與 M1 文件寫「CryXTUI 引用 `Interface/`」（`IVideoPlayer.h:3-6`、`M1-runtime-core.md:10`）——改為本文件的契約。
- DLL 目前沒有任何匯出。Core 可不靠引擎單獨建置與自測（`cevcore_selftest`）。

## 3. 貼圖來源契約（`CryTexSource`）

發現方式：來源 DLL 匯出 `CryTexSource_v1`；消費端掃描行程已載入的模組，逐一 `GetProcAddress`。回傳的 `CryTexSourceApi::version` 是來源支援的最高版本；結構只在尾端加欄位。v1 是 docs/09 §9.3 已定的內容；**v2 是下面尾端新增的部分**（2026-11-19 經 XTFrame 主線審過，十二點修訂已納入；消費端的副本在 CE 倉 docs/09 §9.11）。

```c
/* Threading: all calls come from one thread at a time - the consumer's UI update thread. That is
   the main thread, except during level loading as specified in <loading doc, pending>. The source
   must not assume the engine main thread.
   Strings: UTF-8, valid only for the duration of the call. Names are surface ids, compared
   case-insensitively.
   Sizes: every struct starts with `size` (set by whoever allocates it). Neither side reads or writes
   a field that lies beyond the other side's `size`; fields that were not written keep the caller's
   zero-initialised value. */

enum { CTS_LAYOUT_RGBA8 = 0,          /* one RGBA8 texture, premultiplied per flags bit0 */
       CTS_LAYOUT_PACKED_YUV420 = 1,  /* one R8 texture: Y on top, U and V side by side below */
       CTS_LAYOUT_PACKED_YUVA420 = 2, /* same, plus an alpha plane below */
       CTS_LAYOUT_NV12 = 3 };         /* reserved: two textures (R8 luma, R8G8 chroma) */

enum { CTS_SITING_LEFT = 0,           /* chroma co-sited with the left luma column, centred vertically (MPEG-2 style) */
       CTS_SITING_CENTER = 1,         /* centred both ways (JPEG / MPEG-1 style) */
       CTS_SITING_TOPLEFT = 2 };      /* co-sited with the top-left luma sample */

typedef struct CryTexSourceRect { int32_t x, y, w, h; } CryTexSourceRect;   /* texels */

typedef struct CryTexSourceInfo {       /* out */
    /* v1 */
    uint32_t size;
    int32_t  textureId;       /* engine texture id, 0 when there is none right now */
    int32_t  width, height;   /* picture size in pixels (not the packed texture's size) */
    uint32_t flags;           /* bit0 premultiplied (RGBA8), bit1 has alpha, bit2 playing,
                                 bit3 (v2) planar colour planes are already multiplied by alpha */
    uint64_t contentSerial;   /* increases whenever the pixels were replaced AND whenever
                                 `generation` changes; never decreases */
    /* v2 */
    uint32_t generation;      /* +1 whenever textureId, the texture size or layout changed */
    uint32_t layout;          /* CTS_LAYOUT_* */
    uint32_t colorMatrix;     /* 0 BT.601, 1 BT.709 (planar layouts) */
    uint32_t colorRange;      /* 0 limited (16-235), 1 full (planar layouts) */
    uint32_t chromaSiting;    /* CTS_SITING_* (planar layouts) */
    int32_t  textureId2;      /* second texture of a two-texture layout, else 0 */
    CryTexSourceRect planeY, planeU, planeV, planeA;   /* rects inside textureId (packed layouts) */
    uint32_t state;           /* 0 none, 1 loading, 2 ready, 3 playing, 4 paused, 5 ended, 6 error */
    int32_t  errorCode;       /* source-defined, 0 = none */
    double   timeSeconds, durationSeconds;   /* the source's clock (audio master) */
    uint32_t loopCount;       /* +1 on every wrap of a looping video */
} CryTexSourceInfo;

typedef struct CryTexSourceHint {       /* in */
    /* v1 */
    uint32_t size;            /* a hint with flags = 0 and pixelWidth = pixelHeight = 0 from a v2
                                 consumer that also sets `release` means "I stopped using the name" */
    uint32_t flags;           /* bit0 visible on screen this frame, bit1 (v2) release */
    int32_t  pixelWidth, pixelHeight;   /* largest on-screen size it is sampled at, 0 = unknown */
    /* v2 */
    uint64_t consumer;        /* unique per consumer (e.g. an address); 0 = anonymous v1 consumer */
    uint32_t acceptLayouts;   /* bit n = can sample CTS_LAYOUT_n; 0 means RGBA8 only */
} CryTexSourceHint;

typedef struct CryTexSourceOpen {       /* in, v2 */
    uint32_t size;
    uint32_t flags;           /* bit0 autoplay, bit1 loop, bit2 muted */
    float    volume;          /* 0..1 */
} CryTexSourceOpen;

enum { CTS_VERB_PLAY = 1, CTS_VERB_PAUSE, CTS_VERB_SEEK /* seconds */, CTS_VERB_RATE,
       CTS_VERB_LOOP /* 0/1 */, CTS_VERB_VOLUME /* 0..1 */, CTS_VERB_MUTE /* 0/1 */ };

typedef struct CryTexSourceApi {
    uint32_t version;         /* highest supported: 1 or 2 */
    /* v1 */
    int  (*query)(const char* name, CryTexSourceInfo* out);            /* 1 = name is ours, out filled */
    void (*hint)(const char* name, const CryTexSourceHint* hint);      /* advisory */
    /* v2 */
    /* 0 = not taken; 1 = opened, owned by this name's opener (who must `close`);
       2 = the name already exists and is not owned by this open - attach by `query`, do not `close`. */
    int  (*open)(const char* name, const char* src, const CryTexSourceOpen* options);
    void (*close)(const char* name);
    int  (*control)(const char* name, uint32_t verb, double value);    /* intent; 0 = refused */
} CryTexSourceApi;

__declspec(dllexport) const CryTexSourceApi* CryTexSource_v1(void);
```

規則：
1. **名字＝Surface ID**。來源以它查自己的登錄表。`open` 的 `name` 由消費端給（作者自選的 id，或消費端為 `<video src>` 產生的），`src` 是已解析的 CryPak 路徑。回 1 的播放器屬於這次 `open`，由開的人 `close`；回 2 表示關卡邏輯（節點／元件）已經用同一個 ID 建了播放器——消費端以 `query` 接上，不得 `close`。兩個來源對同一個名字都回應 `query` 時，模組掃描順序在前者為準。
2. **`open` 不阻塞**：只做便宜的容器辨識就返回；開檔與解碼器建立是非同步的，期間 `state`＝loading，失敗以 `state`＝error＋`errorCode` 呈現。消費端只在回 0 時問下一個來源，之後的錯誤不再轉交。
3. **消費端每幀 `query`**；回 0（名字不存在、來源已關閉）時顯示透明並放掉貼圖參考——兩個插件之間沒有關機順序的協調，靠這條。`generation` 改變時重新取貼圖並更新固有尺寸（`contentSerial` 同時也會變）。
4. **模組生命週期**：消費端對保留其 API 的模組持有一個模組參考；名字解析不到或 `open` 無人接手時重新掃描，至多每秒一次。來源的插件關閉之後，匯出函式必須仍可呼叫到行程結束：`query`／`open`／`control` 回 0、`hint`／`close` 不做事。
5. **`hint` 以 `(name, consumer)` 累計**：活著的消費端在持有名字的每一幀都送 hint（不可見時 visible＝0）。任一可見即可見；像素大小取最大；**所有**活著的消費端都接受某個平面排列時才用它。`consumer`＝0 是匿名的 v1 消費端，算作一個只接受 RGBA8 的消費端。消費端離開：明確送 `release`，或在來源自己被 tick 的連續 120 次更新裡都沒有送 hint（不以牆鐘計——主執行緒卡住時不會誤判）。沒有任何消費端送過 hint 的名字＝只有 RGBA8（不經契約的 UI、材質、全螢幕、編輯器預覽都屬於這一類）。
6. 排列切換（RGBA8 ↔ 打包平面）不重新載入影片、聲音不中斷：只重建貼圖，`generation`＋1（§5.2）。
7. **色彩**：v2 只有 8 位元、SDR、sRGB 傳遞函數；其他內容由來源轉換或以 RGBA8 提供。平面排列的色彩平面是否已乘 alpha 由 `flags` bit3 回報（未乘的 straight 色彩經雙線性取樣在 alpha＝0 的邊緣可能出現暈邊——消費端依 bit3 選擇正確的合成）；消費端的輸出一律是預乘。4:2:0 的色度位置由 `chromaSiting` 回報。RGBA8 維持預乘（bit0＝1）。
8. **打包排列沒有 mip**：縮小顯示時的品質靠依畫面大小選變體（§5.4）。
9. `control` 是意圖，來源可以拒絕；時鐘永遠是來源的（聲音為主）。SEEK 是盡力而為的幀精確，`timeSeconds` 回報實際位置；`state`＝loading 時的 SEEK 會被記住；RATE 超出支援範圍回 0。循環時不會出現 ended——以 `loopCount` 得知每次回繞。
10. **CEVPlayer 的實作方式**（為了規則裡的執行緒條款）：契約的函式不呼叫任何引擎 API。`query` 讀播放器在自己 tick 結尾寫好的快照；`hint` 只記錄；`open`／`close`／`control` 排進佇列，在下一次 tick 套用。所有引擎呼叫（貼圖建立與上傳、聲音、檔案）都發生在 CEVPlayer 自己的 tick。

## 4. 對外介面（M6）

### 4.1 Surface ID 與登錄表
- `CPlayerManager` 維護 `id → player`（不分大小寫；重複建立回錯誤，不再只是警告）。ID 是作者自選的字串（`[A-Za-z0-9_.:-]`，1～63 字元）；換檔、換語言、換變體、尺寸改變都不變。
- 引擎貼圖名是內部細節：`$cev:<id>`；沒有 ID 的播放器（全螢幕、材質）維持自動命名。
- `IVideoPlugin` 尾端新增 `CreatePlayerForSurface(id)`／`FindPlayerBySurface(id)`；`IVideoPlayer` 新增 `GetSurfaceId()`、`GetPixelLayout()`、`GetDecodeBackend()`；`kVideoPlayerInterfaceVersion` 4 → 5。標頭註解與 M1 文件裡「CryXTUI 引用本標頭」改為「其他插件以 §3 的契約對接；本標頭給直接連結 CEVPlayer 的遊戲程式」。

### 4.2 節點：砍掉舊的，分兩個抽屜
- **刪除，不留別名**：`Module/FlowNodes/FlowNodes.cpp` 的舊節點、`TextureVideoPlayerComponent`、`Module/Compat/`、所有 `vp_*` 指令與變數、`OutTextureName`／`OutputTextureName`。使用它們的既有關卡與流程圖會失效（使用者已同意）。
- **開箱即用**（FlowGraph 類別 `Video`；Schematyc 元件 `Video Player`）：
  - `Video:Play`——輸入 Video（檔案）、Output（Surface／Screen／Material）、SurfaceID 或材質與槽、Loop、Autoplay、Decode（Auto／GPU／CPU）、Play／Stop／Pause／Resume；輸出 Ready（帶 SurfaceID、TextureId、Width、Height、Duration、HasAlpha、DecodeBackend）、Started、Ended、Looped、Error（帶代碼與文字）。一個節點就能播。
  - `Video:Stop`、`Video:Pause`（以 SurfaceID 或 Handle 指定）、`Video:Events`（以 SurfaceID 訂閱，不必持有 Handle）。
- **進階**（類別 `VideoAdvanced`）：現有由描述表產生的 `Control:*` 與 `Query:*`，整理成一個名詞（`SurfaceId`；貼圖以 `TextureId` 查詢），補 `FrameSerial`、`IsLoaded`、`IsLooping`、`HasAudio`、`PixelLayout`、`DecodeBackend`、`Rate`、`Volume`、`Path`、解除材質綁定；幀序號改以兩個 32 位元埠或字串輸出，不再 2^31 回繞。逐一清單在 M6 的任務單定（依 `Module/Bindings/ApiTable.cpp`）。
- 「路不鎖死」：任何 UI 系統都可以只靠節點輸出的 `TextureId`／`Width`／`Height`（RGBA8）使用，不必實作契約；完整的好處（§5、§6）只在經由契約時生效。

## 5. 節流與打包平面（M7、M8）

### 5.1 ring slot 改為打包排列（M8）
- 每格一塊連續緩衝，列寬＝影片寬 `w`（緊密，無對齊空隙——引擎的上傳函式要求緊密排列）：Y 佔 `(0,0,w,h)`；U `(0,h,cw,ch)`、V `(cw,h,cw,ch)`；alpha `(0,h+ch,w,h)`。只支援 4:2:0 且寬為偶數；其他取樣格式與奇數寬走 RGBA8。
- `WriteFrame` 沿用（換指標與列寬）。解碼器直接解進上傳緩衝不做：libvpx 的外部緩衝只有 VP9、帶邊框、且被留作參考幀。

### 5.2 RGBA 改為按需
- RGBA 緩衝與 `ConvertSlot` 只在聚合結果需要 RGBA8 時配置與執行；切換發生在幀與幀之間（配置不在穩定播放路徑上），不重新載入 core。
- 貼圖：打包平面時是 `eTF_R8`、`w × (h + ch [+ h])`、無 mip；RGBA8 時照舊。切換或尺寸改變 → 釋放再建立、`generation`＋1。
- **M8 動工前先驗（階段 0）**：引擎的 `UploadToVideoMemory`／`UpdateTextureInVideoMemory` 是否接受 `eTF_R8`（已讀到的程式沒有排除，格式表未查）；不接受則 M8 停下重議。

### 5.3 看不見時（M7）
1. 聚合後不可見：立刻改為「只解碼」（不取 slot、不複製、不轉色、不上傳——現成的 seek skip 模式）；時鐘與聲音照走。
2. 連續不可見超過 2 秒（`cev_hiddenStopSeconds`，0＝永不完全停止）：停止視訊解碼。
3. 重新可見：從當下時間的前一個關鍵幀以 skip 模式補解到當下，再恢復輸出。補解的量由關鍵幀間隔決定——**匯入轉碼加 `-g`（預設 2 秒的幀數）**；既有素材關鍵幀稀疏時補解可能卡頓，`cev_stats` 顯示補解耗時。
4. 結尾與循環的判定改為不依賴視訊解碼器到達 EOF（以時鐘與 duration 判定），否則停止解碼後 `Ended`／`Looped` 會延遲到寬限期。
5. 播放器選項 `PauseDecodeWhenHidden`（預設開）。全螢幕與材質輸出視為永遠可見。

### 5.4 依畫面大小選解析度（M7）
- 開始播放（與每次循環重新開始、`open`）時，以聚合的像素高度取「不小於所需高度的最小變體」；沒有 hint 時用主影片。播放中不自動切換（切換會重新載入、聲音重新定位）；之後若要無縫切換另案。

### 5.5 量測（階段 0 與每個里程碑的驗收）
- `cev_stats` 補：上傳耗時（主執行緒呼叫時間）、每幀上傳位元組、複製／轉色分開、補解耗時、目前排列與解碼後端、不可見狀態。`cev_stats_file <path>` 寫 CSV。
- 基準表（1080p30、1080p60、2160p30；有／無 alpha；RGBA8 對打包平面；CPU 對 GPU 解碼）：解碼、複製、轉色、上傳的 ms 與位元組、RAM、VRAM。

## 6. 硬體解碼（M9）

- 後端介面：Core 的視訊解碼抽成 `IVideoDecoderBackend`（libvpx 是其一；M8 先把接縫留好）。解封裝仍是 libwebm。
- **Media Foundation 後端**：以 `MFTEnumEx`（輸入 `MFVideoFormat_VP90`／`VP80`）取得解碼 MFT，給它自己的 D3D11 裝置（與引擎同一張顯示卡）的 device manager，輸出 NV12。這個 MFT 來自 Microsoft Store 的「VP9 Video Extensions」，**不是 Windows 本體**（2026-11-19 在本機列舉到 `VP9VideoExtensionDecoder`，同一個元件處理 VP8 與 VP9）——所以：
  - 啟動時探測；沒有元件、建立失敗、或回報不是硬體加速 → 該播放器用 libvpx，並在 Ready 與 `cev_stats` 回報實際後端。
  - 各顯示卡上 VP9／VP8 是否真的硬體解碼：**未驗**（階段 0 以實際素材驗）。
- 選擇：播放器屬性 `Decode = Auto | GPU | CPU`，全域 `cev_decodeMode`。`GPU` 不可用時退回 CPU 並警告一次。`Auto` 的規則先定為「可用且影片高度 ≥ 1080 時用 GPU」，量測後修訂。
- alpha：第二條視訊流另開一個 MFT；開不了就只有 alpha 走 libvpx。
- **第一版：讀回**。解碼輸出複製到 staging 貼圖、非同步 Map（排隊 1～2 幀，不阻塞）、把 NV12 轉排成 §5.1 的打包排列（Y 直接複製，UV 拆成兩半）進 ring slot，其後與軟體解碼相同。CEVPlayer 仍然不碰渲染器。
- **第二版（量測後再決定，需要使用者同意的渲染器 patch）**：解碼輸出直接成為引擎裝置上的貼圖（`CTS_LAYOUT_NV12`，兩張貼圖），不經記憶體。契約已保留排列代號與 `textureId2`。

## 7. 里程碑與驗收

| 里程碑 | 內容 | 驗收 |
|---|---|---|
| 階段 0 | 只量不改產品行為：`eTF_R8` 上傳、UI 是否同一幀取到新畫面、MF 硬解在本機是否成立、現行 CPU 轉色採用的色度位置與素材標記（決定 `chromaSiting` 的回報值）、基準表 | 五個答案與基準表寫進 `docs/research/05-ui-source-baseline.md` |
| M6 | Surface ID 登錄表、契約 v1 `query`、舊節點刪除、兩個抽屜、介面版本 5、文件改寫 | UI 與節點各寫同一個 ID 就出畫面（RGBA8）；換檔／換語言後不需重新接線；重複 ID 被拒；沒有消費端插件時全部功能照舊；Core 自測與 `cev_apitest` 過 |
| M7 | `hint`：可見性（§5.3）、開始時選變體、匯入 `-g`、結尾判定、量測欄位；契約 v2 的 `state`／時間與 `open`／`close`／`control` | 暫停時 `contentSerial` 不動；不可見時複製／轉色／上傳為零、2 秒後解碼為零、重新可見的補解時間有上限；`<video src>` 經由 `open` 播放 |
| M8 | 打包排列、按需 RGBA、`eTF_R8` 上傳、`acceptLayouts` 協商 | 兩種排列的畫面在消費端逐像素比對差 ≤ 2 級；協商切換時聲音不中斷；基準表更新為實測 |
| M9 | 解碼後端介面、Media Foundation 後端（讀回）、`Decode` 選項 | 無元件的機器自動退回；GPU／CPU 輸出逐像素差 ≤ 2 級；基準表加入 GPU 解碼 |
| M10 | 關卡讀取期間推進 | 未設計——等 XTFrame 主線的機制 |

消費端與執行期的對應工作（不在本倉）：CryXTUI——自動接上未綁定的 `engine://<id>`、持有參考與每幀確認、預乘修正、`hint`、平面取樣的著色（CE-86／CE-87 與其後續）；XTFrame 主倉——外部貼圖內容版本與使用情形（T-746）、「打包平面外部貼圖」的取樣能力（需求另送主線，編號由主線取）。

## 8. 已知風險

- 引擎對 `eTF_R8` 動態貼圖的支援未查證（階段 0）。
- 主執行緒上傳與 UI 繪製是否同一幀：由命令排隊順序推論為是，未實測；啟用引擎的載入期 video thread 時（`MainThread_defer`）可能不同。
- Sandbox 編輯模式下引擎不呼叫插件的 `UpdateBeforeRender`，影片不前進（引擎既有行為）。
- 裝置重設沒有處理；其他插件握有貼圖參考時的關機順序靠 §3 規則 2。
- VP9 Video Extensions 不保證存在；VP8 的硬體解碼支援面很窄。
- Core 沒有 CI；M4、M5 的畫面驗收尚未執行。
