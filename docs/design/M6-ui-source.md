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
2. **`open` 不阻塞**：函式內只依副檔名與既有登錄決定接不接（規則 10：契約函式不呼叫引擎，所以不開檔；2026-11-19 M7 實作時的釐清，已通知消費端）；開檔、容器辨識與解碼器建立是非同步的，期間 `state`＝loading，失敗（含容器不是它認得的格式）以 `state`＝error＋`errorCode` 呈現。消費端只在回 0 時問下一個來源，之後的錯誤不再轉交。CEVPlayer 接 `.webm` 與 `.cevvideo`。
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
- **現況**（`Module/Bindings/ApiTable.cpp`）：描述表一項產生一個節點——控制 15（Create、Destroy、Load、Play、Pause、Resume、Stop、Seek、SetLoop、SetRate、SetLanguage、SetVolume、SetOutput、SetVariant、SetSkippable）、查詢 13（State、Time、Duration、Width、Height、TextureId、TextureName、HasAlpha、IsPremultiplied、Language、Variant、InterfaceVersion、Caps）、事件 1 個節點（Ready、Started、NewFrame、Marker、Looped、Ended、Skipped、Error），共 29 個。
- **整理後 10 個節點、三個抽屜**（使用者 2026-11-19：「連通 UI 的節點開一個獨立抽屜」「其他節點整理一下，看能不能簡化或合併」）：

  | 抽屜 | 節點 | 合併了什麼 |
  |---|---|---|
  | 主抽屜 `CEVPlayer`（開箱即用） | `CEVPlayer:PlayOnScreen`——Video、Fit、Loop、Skippable、Decode；Play／Stop → Started、Ended、Skipped、Error | Create＋Load＋SetOutput(Screen)＋Play＋SetLoop＋SetSkippable＋Destroy |
  | | `CEVPlayer:PlayOnMaterial`——Video、Material（空＝實體自己的）、Slot、SubMaterial、Loop、Autoplay、Decode；Play／Stop → Started、Ended、Error | 同上，SetOutput(Material) |
  | 子抽屜 `CEVPlayer:UI`（連通 UI） | `CEVPlayer:UI:PlayToSurface`——Video、SurfaceID、Loop、Autoplay、Decode；Play／Stop → Ready（帶 TextureId、Width、Height、Duration、HasAlpha）、Ended、Error | Create＋Load＋Play＋Destroy，輸出是具名 surface |
  | | `CEVPlayer:UI:SurfaceInfo`——SurfaceID → Exists、TextureId、Width、Height、PixelLayout、Premultiplied、HasAlpha、FrameSerial、State | TextureId、TextureName、IsPremultiplied 三個查詢移到這裡（給不經契約的 UI 系統用） |
  | | `CEVPlayer:UI:SurfaceEvents`——SurfaceID → Ready、NewFrame、Resized、Closed | 以 ID 訂閱，不必持有 Handle |
  | 子抽屜 `CEVPlayer:Advanced`（進階） | `Open`——Video、Output（Surface／Screen／Material）與其參數、Decode；Open／Close → Handle、Ready、Error | Create、Load、SetOutput、Destroy |
  | | `Transport`——Handle；Play、Pause、Stop、Seek（＋Seconds） | Play、Pause、Resume、Stop、Seek（Play 兼作繼續） |
  | | `Settings`——Handle；Loop、Rate、Volume、AudioTrack、Variant、Skippable（哪個埠被觸發就套用哪一項；A1 起 Language 改 AudioTrack） | SetLoop、SetRate、SetVolume、SetAudioTrack、SetVariant、SetSkippable |
  | | `Info`——Handle；Get（或每幀）→ State、Time、Duration、Width、Height、HasAlpha、AudioTrack、Variant、DecodeBackend、FrameSerial、SurfaceId（A1 起 Language 改 AudioTrack） | State、Time、Duration、Width、Height、HasAlpha、AudioTrack、Variant |
  | | `Events`——Handle → Ready、Started、NewFrame、Marker、Looped、Ended、Skipped、Error | 不變 |

- （2026-11-19 註記，PR 整理時對照程式：M7 起 `UI:PlayToSurface` 與 `Advanced:Settings` 多一個 `PauseDecodeWhenHidden` 輸入，`Advanced:Info` 多一個 `Error` 輸出，CryVS 的 `CEVPlayer::Advanced` 多 `SetPauseDecodeWhenHidden`；完整埠表以 README「Node sets」為準。）
- 抽屜的位置：主抽屜 `CEVPlayer` 直接放兩個開箱即用節點，`UI` 與 `Advanced` 是它之下的子抽屜（使用者 2026-11-19：「把 basic 獨立抽屜取消放在主抽屜，Advanced 跟 UI 的抽屜保留放在主抽屜內，主抽屜改名 CEVPlayer」）。CryVS 的全域函式命名空間對應為 `CEVPlayer`、`CEVPlayer::UI`、`CEVPlayer::Advanced`（目錄路徑 `Function::CEVPlayer::…`）。
- 取消：`TextureName`（名詞消失）、`InterfaceVersion` 與 `Caps`（開發者資訊，改由主控台 `cev_info` 顯示）。事件與查詢的幀序號改以字串輸出，不再 2^31 回繞。
- 描述表仍是單一事實來源：腳本綁定（Schematyc 函式、CryVS）維持一項一個函式——程式裡細粒度是正常的；合併只發生在 FlowGraph 節點這一層（複合節點由手寫的描述組合表內的函式）。Schematyc 元件 `Video Player` 的屬性對應同一組名詞（Output、SurfaceID、Decode、Loop、Autoplay、Skippable）。
- **CryVS（社群版引擎的視覺腳本）也要有同一組節點**（使用者 2026-11-19：「CryVS 的節點要加」）。現況：`Module/Bindings/CryVSEnv.cpp` 以描述表產生一個元件與其函式／訊號（與 Schematyc 相同），只在社群版引擎樹編譯過、從未實跑（`PLAN.md` M2 備註）。M6 的要求：CryVS 端提供與 FlowGraph 相同的三個抽屜與 10 個節點的對應物（名詞一致：SurfaceID、Decode、Output…），並在社群版引擎（`E:\coding crycry\CryENGINE-5-7-community`，`z060142/CryENGINE-5-7-community`）實際執行驗證。CryVS 的節點模型（是否有不屬於元件的獨立函式節點、類別怎麼分抽屜、複合節點怎麼表達）在動工前先唯讀調查，寫進 M6 的任務單。
- 「路不鎖死」：任何 UI 系統都可以只靠節點輸出的 `TextureId`／`Width`／`Height`（RGBA8）使用，不必實作契約；完整的好處（§5、§6）只在經由契約時生效。

## 5. 節流與打包平面（M7、M8）

### 5.1 ring slot 改為打包排列（M8）
- 每格一塊連續緩衝，列寬＝影片寬 `w`（緊密，無對齊空隙——引擎的上傳函式要求緊密排列）：Y 佔 `(0,0,w,h)`；U `(0,h,cw,ch)`、V `(cw,h,cw,ch)`；alpha `(0,h+ch,w,h)`。只支援 4:2:0 且寬為偶數；其他取樣格式與奇數寬走 RGBA8。
- `WriteFrame` 沿用（換指標與列寬）。解碼器直接解進上傳緩衝不做：libvpx 的外部緩衝只有 VP9、帶邊框、且被留作參考幀。

### 5.2 RGBA 改為按需
- RGBA 緩衝與 `ConvertSlot` 只在聚合結果需要 RGBA8 時配置與執行；切換發生在幀與幀之間（配置不在穩定播放路徑上），不重新載入 core。
- 貼圖：打包平面時是 `eTF_R8`、`w × (h + ch [+ h])`、無 mip；RGBA8 時照舊。切換或尺寸改變 → 釋放再建立、`generation`＋1。
- **M8 動工前先驗（階段 0，已驗：可用，§10）**：引擎的 `UploadToVideoMemory`／`UpdateTextureInVideoMemory` 是否接受 `eTF_R8`（已讀到的程式沒有排除，格式表未查）；不接受則 M8 停下重議。

### 5.3 看不見時（M7）
1. 聚合後不可見：立刻改為「只解碼」（不取 slot、不複製、不轉色、不上傳——現成的 seek skip 模式）；時鐘與聲音照走。
2. 連續不可見超過 2 秒（`cev_hiddenStopSeconds`，0＝永不完全停止）：停止視訊解碼。
3. 重新可見：從當下時間的前一個關鍵幀以 skip 模式補解到當下，再恢復輸出。補解的量由關鍵幀間隔決定——**匯入轉碼加 `-g`（預設 2 秒的幀數）**；既有素材關鍵幀稀疏時補解可能卡頓，`cev_stats` 顯示補解耗時。
4. 結尾與循環的判定改為不依賴視訊解碼器到達 EOF（以時鐘與 duration 判定），否則停止解碼後 `Ended`／`Looped` 會延遲到寬限期。
5. 播放器選項 `PauseDecodeWhenHidden`（預設開）。全螢幕與材質輸出視為永遠可見。

### 5.4 依畫面大小選解析度（M7）
- 開始播放（從 Stopped／Ended 的 Play、Load、`open`；**循環回到開頭時不重選**——重選會重新載入而中斷聲音）時，以聚合的像素高度取「不小於所需高度的最小變體」；沒有 hint 時用主影片。播放中不自動切換（切換會重新載入、聲音重新定位）；之後若要無縫切換另案。

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
- **執行緒與介面（2026-11-19 裁決，全文在 `docs/tasks/M9.md` 末尾）**：每個使用 MF 後端的播放器一條專用執行緒，COM／MF／D3D11 物件只在它上面建立、使用、釋放；後端介面把「送入封包」與「取出完成的畫面」分開，ring 的 slot 跟著封包的票走（MF 的讀回延遲兩幀），skip 以不帶 slot 的票表示；libvpx 後端在同一個介面下同步完成。
- **第一版：讀回**。解碼輸出複製到 staging 貼圖、非同步 Map（排隊 1～2 幀，不阻塞）、把 NV12 轉排成 §5.1 的打包排列（Y 直接複製，UV 拆成兩半）進 ring slot，其後與軟體解碼相同。CEVPlayer 仍然不碰渲染器。
- **第二版（量測後再決定，需要使用者同意的渲染器 patch）**：解碼輸出直接成為引擎裝置上的貼圖（`CTS_LAYOUT_NV12`，兩張貼圖），不經記憶體。契約已保留排列代號與 `textureId2`。

## 7. 里程碑與驗收

| 里程碑 | 內容 | 驗收 |
|---|---|---|
| 階段 0（**完成** 2026-11-19，結果見 §10） | 只量不改產品行為：`eTF_R8` 上傳、UI 是否同一幀取到新畫面、MF 硬解在本機是否成立、現行 CPU 轉色採用的色度位置與素材標記（決定 `chromaSiting` 的回報值）、基準表 | 五個答案與基準表寫進 `docs/research/05-ui-source-baseline.md` |
| M6 | Surface ID 登錄表、契約 v1 `query`、舊節點刪除、兩個抽屜、介面版本 5、文件改寫 | UI 與節點各寫同一個 ID 就出畫面（RGBA8）；換檔／換語言後不需重新接線；重複 ID 被拒；沒有消費端插件時全部功能照舊；Core 自測與 `cev_apitest` 過 |
| M7 | `hint`：可見性（§5.3）、開始時選變體、匯入 `-g`、結尾判定、量測欄位；契約 v2 的 `state`／時間與 `open`／`close`／`control` | 暫停時 `contentSerial` 不動；不可見時複製／轉色／上傳為零、2 秒後解碼為零、重新可見的補解時間有上限；`<video src>` 經由 `open` 播放 |
| M8 | 打包排列、按需 RGBA、`eTF_R8` 上傳、`acceptLayouts` 協商 | 兩種排列的畫面在消費端逐像素比對差 ≤ 2 級；協商切換時聲音不中斷；基準表更新為實測 |
| M9 | 解碼後端介面、Media Foundation 後端（讀回）、`Decode` 選項 | 無元件的機器自動退回；GPU／CPU 輸出逐像素差 ≤ 2 級；基準表加入 GPU 解碼 |
| M10 | 關卡讀取期間推進 | 機制由 XTFrame 主線定（CE 倉 `docs/10-loading-time.md`，§10.2 第 14～18 點是來源端的規則）；**等主線通知引擎 patch（CE-90）併入後才實作**。要點見 §9 |

消費端與執行期的對應工作（不在本倉）：CryXTUI——自動接上未綁定的 `engine://<id>`、持有參考與每幀確認、預乘修正、`hint`、平面取樣的著色（CE-86／CE-87 與其後續）；XTFrame 主倉——外部貼圖內容版本與使用情形（T-746）、「打包平面外部貼圖」的取樣能力（需求另送主線，編號由主線取）。

## 8. 已知風險

- 引擎對 `eTF_R8` 動態貼圖的支援未查證（階段 0）。
- 主執行緒上傳與 UI 繪製是否同一幀：由命令排隊順序推論為是，未實測；啟用引擎的載入期 video thread 時（`MainThread_defer`）可能不同。
- Sandbox 編輯模式下引擎不呼叫插件的 `UpdateBeforeRender`，影片不前進（引擎既有行為）。
- 裝置重設沒有處理；其他插件握有貼圖參考時的關機順序靠 §3 規則 2。
- VP9 Video Extensions 不保證存在；VP8 的硬體解碼支援面很窄。
- Core 沒有 CI；M4、M5 的畫面驗收尚未執行。

## 9. 關卡讀取期間（M10 的前提，2026-11-19 由 XTFrame 主線提供；細節可能因其探測而修訂）

- 同步載入關卡時主執行緒卡在 `LoadLevel`，畫面由引擎的載入期迴圈在**渲染執行緒**推動：每圈呼叫 `ILoadtimeCallback::LoadtimeUpdate(dt)`、`LoadtimeRender()`、present。主線的引擎 patch 17 讓多個回呼可以註冊（`IRenderer::AddLoadtimeCallback(cb, order)`）。
- CEVPlayer 註冊自己的回呼，order 100（UI 消費端用 200，所以同一圈內先上傳、UI 後取樣）。載入期間 `LoadtimeUpdate(dt)` 就是播放器的 tick：在渲染執行緒、用它給的 dt（引擎計時器在載入期間不前進）。
- 在回呼裡呼叫 `UpdateTextureInVideoMemory` 立即執行且合法；載入期間從主執行緒呼叫會被延後到載入結束；從其他執行緒（含解碼執行緒）呼叫不合法。
- tick 會換兩次執行緒（載入開始：主 → 渲染；載入結束：渲染 → 主），兩個入口不會同時執行。tick 的程式不得假設固定執行緒，載入迴圈裡不得使用只限主執行緒的引擎服務：不碰 entity／flow／Schematyc、不開新檔（CryPak 讀取在迴圈期間被鎖——載入中途開新影片要另外測）、不依賴 `gEnv->pTimer` 或主幀編號。
- 契約函式在載入期間由渲染執行緒呼叫；§3 規則 10（契約函式不呼叫引擎）已滿足。
- 載入期間的聲音不在該文件的保證內，由 M10 自行驗證。Sandbox 開關卡時沒有載入迴圈（不支援）；非同步／分段載入時主迴圈照跑，走一般的 tick。
- 對 M6～M9 的要求：播放器的 tick 從現在起就寫成「不假設主執行緒、時間由呼叫者給 dt」，M10 才不必重寫。

## 10. 階段 0 的結果與對本設計的修訂（2026-11-19；細節在 `docs/research/05a-standalone.md`、`05b-engine.md`，原始資料在 `E:/coding3/xt-bench/cev-s0/`）

本機：13600K、RTX 4070 Ti；引擎樹 `CRYENGINE_Source_CEVPlayer_XTFrame`（Profile，CryXTUI v0.17.0）。耗時為 ms、平均／p99。

| 素材（VP9） | 解碼 | 平面複製 | CPU 轉色 | 主執行緒上傳 | 每幀上傳 | 行程 RAM 增量 |
|---|---|---|---|---|---|---|
| 1080p30 | 1.59／2.64 | 0.35／0.72 | 4.28／5.61 | 1.63／3.02 | 8.29 MB | 110 MB |
| 1080p30 alpha | 2.42／3.96 | 0.68／1.27 | 5.37／9.32 | 1.66／4.19 | 8.29 MB | 141 MB |
| 1080p60 | 1.62／2.74 | 0.38／0.93 | 4.37／8.22 | 1.83／3.63 | 8.29 MB | 139 MB |
| 2160p30 | 7.24／12.06 | 2.43／4.85 | 17.17／27.11 | 8.09／14.40 | 33.2 MB | 420 MB |
| 2160p30 alpha | 10.18／17.60 | 3.66／7.84 | 21.61／36.79 | 7.68／16.14 | 33.2 MB | 573 MB |

- **今天 2160p 帶 alpha 無法即時播放**（畫面落後時鐘約 1 秒）；2160p 的兩大成本是 CPU 轉色與主執行緒上的上傳呼叫（8 ms）。打包平面（M8）把這兩項分別變成 0 與約 1／2.7——M8 排在 M9 之前不變。
- 行程 RAM 的增量遠大於 ring 的推算值（1080p 推算 44～51 MB、實測 110～160 MB）：M8 動工時先拆開量是誰佔的。
- **`eTF_R8` 動態貼圖可用**（§5.2 的前提成立）：1920×1620、3840×3240、非 2 的冪皆成功，裝置格式 `R8_UNORM`、緊密列；每幀更新 0.4 ms（1080p 打包）與 1.5 ms（2160p 打包）。`UploadToVideoMemory` 的同步路徑忽略 `eTFDst`。**同名貼圖移除後立刻重建會拿回同一個 id**——`generation` 必須由播放器自己遞增，不能以 id 是否改變來判斷。
- **同一幀**：CryXTUI 的 layer 模式（`xtui_direct 0`）下，`UpdateBeforeRender` 送出的貼圖更新與 UI 繪製在同一個顯示幀生效（36／36 張截圖）。未測：插件更新順序相反、載入期迴圈。
- **消費端的缺陷（已通知 XTFrame 主線）**：CryXTUI 預設的 Direct 模式（`xtui_direct 1`）與軟體路徑完全不畫 `engine://` 綁定的貼圖。M6 的驗收在它修好之前只能在 `xtui_direct 0` 下做。另：XTUI surface 開著時直接 `quit` 會崩潰（先 `xtui_close` 則否）。
- **色度位置**：現行 CPU 轉換取最近點（等效置中）；Core 沒有讀容器的 `ChromaSitingHorz/Vert`。契約的 `chromaSiting` 回報**素材本身**的位置——M8 在 Core 讀這兩個欄位；素材沒標時回報 `CTS_SITING_LEFT`（FFmpeg 轉出的素材都標 left，4:2:0 影片的通例）。VP9 alpha 素材的色彩平面未乘 alpha：`flags` bit3＝0。
- **匯入轉碼確實沒有 `-g`**；測試素材 `vp9_alpha.webm` 全片只有一個關鍵幀（§5.3 第 3 點的對策必要）。
- **Media Foundation（§6 修訂）**：
  - 這個 MFT（`VP9VideoExtensionDecoder`，同步、`MF_SA_D3D11_AWARE`）沒有任何屬性表示「硬體」。判定改為：**給它 D3D manager 後 `SetInputType` 成功＝硬體解碼**；失敗就用 libvpx。不給 manager 的路徑是軟體解碼而且比 libvpx 慢（1080p30 7.8 ms／幀），不使用。
  - **VP8 沒有硬體解碼**（`MF_E_UNSUPPORTED_D3D_TYPE`）：`Decode=GPU` 遇到 VP8 一律退回 CPU。
  - 硬解輸出與 libvpx 逐像素相同（Y 與 alpha 平面最大差 0；UV 未比）。alpha 第二條流可用第二個 MFT 實例。seek 正常。
  - 讀回（第一版）的成本：解碼呼叫 0.4～0.6 ms；複製到 staging 後**必須 `Flush`**、排隊 **2 幀**再 Map——1080p 0.4 ms、2160p 2.4 ms（同步讀回是 2.7 與 7.9 ms，p99 25 ms，不用）。輸出貼圖高度會對齊（1080 → 1088），UV 起點是 `RowPitch × 貼圖高`，依 aperture 裁切；第一次輸出前固定有一次 `STREAM_CHANGE`。
  - 對照：2160p 以 libvpx 解碼約 7 ms（4 執行緒）＋複製 2.4 ms；硬解＋讀回約 3 ms 且幾乎不佔 CPU。`Auto` 的規則（高度 ≥ 1080 用 GPU）與數字相符。
  - 第二版（零複製）比原先設想的多兩步：輸出貼圖只有 `DECODER` 綁定旗標、不能直接取樣（要再做一次 GPU 複製或接 video processor），而且在 MFT 自己的裝置上（要共享貼圖才能給引擎）。量測後再議的結論不變。
- 階段 0 留下的量測工具：`cev_stats` 的新欄位（複製、轉色、上傳耗時與位元組）、`cev_stats_file`、開發用指令 `cev_probe_r8`／`cev_probe_sync`／`cev_bench_play`／`cev_bench_stop`、`tools/probes/`（`core_bench`、`mf_decode_probe`、`make_bench_media.sh`）。

## 11. M7 的實測（2026-11-19；`E:/coding3/xt-bench/cev-m7/`）

1080p30、關鍵幀 2 秒，ms 平均：

| 狀態 | 解碼 | 複製 | 轉色 | 上傳 | 上傳 MB／s |
|---|---|---|---|---|---|
| 可見 | 1.76 | 0.49 | 4.40 | 2.43 | 262 |
| 只解碼（剛看不見） | 1.82 | 0 | 0 | 0 | 0 |
| 停止（看不見超過 2 秒） | 0 | 0 | 0 | 0 | 0 |

- 重新可見的補解（隱藏 4 秒、10 次）：13.8～94.0 ms，中位 50 ms，最多 57 幀；恢復後第一張畫面落後時鐘 14～39 ms。全片只有一個關鍵幀的 1080p 素材：303～443 ms（177～261 幀）——匯入時的 `-g`（2 秒）就是為了把它壓在第一種情況。
- 補解起點：時鐘前的關鍵幀若晚於停下的位置就從關鍵幀解，否則從停下處接著解。隱藏時不清空 ring（隱藏中暫停再顯示時第一幀不會消失）；同一個關鍵幀間隔內恢復時先清掉隱藏前的舊幀。
- 不可見時的結尾以時鐘判定：Ended 在 5.019 s（片長 5.0）；循環與標記點照送。`open` 的函式耗時 0.004 ms。
- 階段 0 的更正：`vp9_alpha.webm` 有兩個關鍵幀（0 與 4.267 s），不是一個。Core 自測的 `av_sync_flash_beep` 失敗來自 PATH 上 ImageMagick 附帶的 ffmpeg 4.2.3 產生的測試素材（webm muxer 把時間戳偏移 7 ms）；以 scoop 的 ffmpeg 7.1.1 產生的素材全過——產生測試素材時要用明確的 ffmpeg 路徑。

## 12. 平面取樣的參考實作（M8，消費端照做）

參考程式：`Code/CryPlugins/CEVPlayer/Core/tests/ReferenceSampler.h`（只供測試與文件，不在執行期路徑）。輸入是來源上傳的那一張 R8 貼圖（`w × (h + ch [+ h])` texel，列緊密）與快照的 `planeY/U/V/A`、`colorMatrix`、`colorRange`、`chromaSiting`；輸出是畫面大小 `width × height` 的預乘 RGBA8。下文 `ch = ⌈h/2⌉`、`cw = w/2`（打包排列只用在寬為偶數時）。

**取樣位置**（texel 中心在整數座標 i；輸出像素 (x, y)）：
- Y 與 A：讀各自矩形內的 texel (x, y)（1:1，最近點與雙線性相同）。
- 色度：在 U／V 矩形內的連續座標 `cx = (x + ox) / 2`、`cy = (y + oy) / 2`，`ox = −0.5`（CENTER）或 `0`（LEFT、TOPLEFT），`oy = 0`（TOPLEFT）或 `−0.5`（LEFT、CENTER）。雙線性內插，座標**先夾在 `[0, cw−1] × [0, ch−1]`**——換成貼圖 UV 就是夾在矩形內縮半個 texel：`u ∈ [(rx + 0.5)/W, (rx + rw − 0.5)/W]`、`v ∈ [(ry + 0.5)/H, (ry + rh − 0.5)/H]`（W、H 是整張貼圖的尺寸），所以永遠不會取到相鄰平面。
- 來源自己的 RGBA8（現行 CPU 轉換）是最近點：色度 texel `(x >> 1, y >> 1)`，不看 `chromaSiting`。

**色彩**（浮點，K 值：BT.601 `Kr = 0.299, Kb = 0.114`；BT.709 `Kr = 0.2126, Kb = 0.0722`；`Kg = 1 − Kr − Kb`）：
- limited（`colorRange` 0）：`Y' = (Y − 16) / 219`，`Cb = (U − 128) / 224`，`Cr = (V − 128) / 224`；full（1）：`Y' = Y / 255`，`Cb = (U − 128) / 255`，`Cr = (V − 128) / 255`。
- `R = Y' + 2(1 − Kr)·Cr`；`G = Y' − (2Kb(1 − Kb)/Kg)·Cb − (2Kr(1 − Kr)/Kg)·Cr`；`B = Y' + 2(1 − Kb)·Cb`。
- 每個分量先夾到 [0, 1]，**再**乘 alpha（`a = A / 255`；沒有 A 平面時 1；`flags` bit3 = 0 表示色彩平面是 straight），最後 `round(c · a · 255)`；輸出 alpha `round(a · 255)`。結果是預乘。

**與來源 RGBA8 逐位元組相同的最近點變體**（16.16 定點，`ColorConvert.cpp` 的表）：`luma = yk·(Y − yOff) + 32768`，`R = clamp8((luma + rv·Cr8) >> 16)`、`G = clamp8((luma − gu·Cb8 − gv·Cr8) >> 16)`、`B = clamp8((luma + bu·Cb8) >> 16)`，`Cb8 = U − 128`、`Cr8 = V − 128`，`>>` 為算術右移，`clamp8` 夾到 0..255。係數 `{yOff, yk, rv, gu, gv, bu}`：601 limited `{16, 76284, 104595, 25675, 53279, 132201}`；709 limited `{16, 76284, 117489, 13975, 34925, 138438}`；601 full `{0, 65536, 91881, 22554, 46802, 116130}`；709 full `{0, 65536, 103206, 12276, 30679, 121609}`（即 1.164、1.596／0.392／0.813／2.017 與 709 的 1.793／0.213／0.533／2.112 乘 65536 取整）。alpha 為 255 時直接輸出；否則每個分量 `((c·a + 128)·257) >> 16`。

**驗證**（M8，`cevcore_selftest --only packed --bench <六支量測素材>`）：最近點變體套在 Core 的打包緩衝上，與 `ConvertSlot` 的 RGBA8 **逐位元組相同**——三支測試素材各 5 幀、六支量測素材各 4 幀全數相同。雙線性變體與來源 RGBA8 的差：平均 0.25～2.3 級，最大值在色度銳利邊緣可到 255（測試圖樣的色塊邊界；這是最近點與內插本身的差，不是錯誤）。消費端的著色器應與雙線性變體比對（浮點誤差 ≤ 1 級）。

## 13. M8 的實測（2026-11-19；`E:/coding3/xt-bench/cev-m8/`）

**RAM 拆解**（§10 的疑點；方法：`tools/probes/ram_probe` 在獨立行程裡依序建出各元件、量 private bytes；引擎內以 `cev_probe_r8 <w> <h> [rgba]` 量貼圖每幀更新造成的行程增量）：

| 素材 | 整檔讀入 | libvpx（參考幀、執行緒緩衝，90 幀後） | ring 4 格（舊：平面＋RGBA） | 引擎上傳複本（舊：RGBA8 每幀更新） | 合計（舊） | ring 4 格（新：打包、無 RGBA） | 引擎上傳複本（新：R8） | 合計（新） |
|---|---|---|---|---|---|---|---|---|
| 1080p30 | 9.7 | 32.0 | 43.6 | ≈32（≈4 份 8.3 MB） | ≈117（S0 實測 110） | 11.9 | ≈6（2 份 3.1 MB） | ≈60 |
| 1080p30 alpha | 9.8 | 63.3 | 51.5 | ≈32 | ≈157（S0 141） | 19.8 | ≈10 | ≈103 |
| 2160p30 alpha | 37.9 | 243.5 | 206.1 | ≈127（≈4 份 33 MB） | ≈615（S0 573） | 79.3 | 16～40 | ≈380 |

- 「推算只有 ring」少算的部分：CryPak 來源把整個檔案讀進記憶體、libvpx 自己的參考幀池（VP9 8 個參考＋執行緒／row-MT 緩衝，alpha 是第二個解碼器）、以及引擎 `UpdateTextureInVideoMemory` 每次把整張圖複製進 render command（排隊中的 2～4 幀同時存在）。音訊（PCM ring 500 ms）不到 1 MB。PlayerCore 端到端（3 秒播放）：舊 83.7／123.9／487.1 MB，新 53.0／92.2／358.9 MB。

**兩種排列**（GameLauncher、無 CryXTUI、`cev_bench_hint` 扮演接受打包的消費端，各 20 秒，ms 平均／p99）：

| 素材 | 排列 | 解碼 | 複製 | 轉色 | 主執行緒上傳 | 每幀上傳 | 上傳／秒 | 掉幀 |
|---|---|---|---|---|---|---|---|---|
| 1080p30 | RGBA8 | 1.85／2.91 | 0.50／0.93 | 4.61／6.53 | 2.41／3.21 | 8.29 MB | 30.5 | 0 |
| | 打包 | 1.90／2.99 | 0.54／1.14 | 0 | 1.06／1.54 | 3.11 MB | 30.2 | 0 |
| 1080p30 alpha | RGBA8 | 2.93／4.83 | 1.03／1.50 | 5.93／9.06 | 2.27／3.24 | 8.29 MB | 30.4 | 0 |
| | 打包 | 3.13／5.01 | 0.92／1.45 | 0 | 1.64／2.78 | 5.18 MB | 30.1 | 1 |
| 1080p60 | RGBA8 | 1.95／3.25 | 0.51／0.91 | 4.70／9.47 | 2.63／4.54 | 8.29 MB | 60.5 | 0 |
| | 打包 | 1.87／2.73 | 0.47／0.76 | 0 | 1.02／1.63 | 3.11 MB | 60.5 | 0 |
| 1080p60 alpha | RGBA8 | 3.15／4.42 | 1.03／1.62 | 5.87／8.68 | 2.67／4.16 | 8.29 MB | 60.6 | 0 |
| | 打包 | 3.19／4.87 | 0.89／1.48 | 0 | 1.68／2.84 | 5.18 MB | 60.6 | 0 |
| 2160p30 | RGBA8 | 7.99／12.01 | 2.90／4.23 | 17.72／23.31 | 10.59／17.09 | 33.2 MB | 29.9 | 0 |
| | 打包 | 7.61／11.91 | 2.55／3.93 | 0 | 4.09／5.83 | 12.4 MB | 30.1 | 0 |
| 2160p30 alpha | RGBA8 | 14.11／27.63 | 5.18／7.39 | 26.01／45.91 | 12.81／24.60 | 33.2 MB | **22.8** | 0 |
| | 打包 | 13.62／22.50 | 4.31／6.32 | 0 | 8.67／11.68 | 20.7 MB | **30.2** | 0 |

- **2160p30 alpha 在打包排列下即時播放**（每秒 30 次上傳＝每幀都到；RGBA8 只有 22.8）。
- 引擎內的 GPU／行程 RAM 欄位在連續換素材的單次執行裡基準漂移，不採用；RAM 以上表的拆解為準。
- 像素：打包貼圖內容＝Core 的平面（`cev_contract_selftest` 以上傳緩衝的雜湊對照快照矩形重組的平面，含 A），截圖 `shots/packed_yuva420_1080p30_alpha.jpg`（R8 以紅色顯示：Y、U｜V、A 三段）。`ITexture::GetData32` 對 R8 不可用（非 RGBA8 時以 `GetDeviceDataSize()` 整塊 memcpy、忽略 staging 列距，且在呼叫端執行緒直接下載），未採用。

## 14. M9 的實測與場合建議（2026-11-19；`E:/coding3/xt-bench/cev-m9/`，RTX 4070 Ti；各 20 秒；ms 平均）

| 後端 | 排列 | 素材 | 解碼 | 讀回 | 轉色 | 上傳 | 行程 RAM 增量 |
|---|---|---|---|---|---|---|---|
| CPU | RGBA8 | 1080p30 | 1.76 | – | 4.46 | 2.14 | 64 MB |
| CPU | 打包 | 1080p30 | 2.07 | – | 0 | 1.06 | 14 MB |
| GPU | 打包 | 1080p30 | 0.38 | 0.09 | 0 | 0.67 | 112 MB |
| CPU | RGBA8 | 2160p30 alpha | 13.24 | – | 25.59 | 10.04 | 517 MB |
| CPU | 打包 | 2160p30 alpha | 11.38 | – | 0 | 5.15 | 418 MB |
| GPU | 打包 | 2160p30 alpha | 0.58 | 6.43 | 0 | 3.65 | 753 MB |

- 硬解的打包輸出與 libvpx **逐位元組相同**（六支素材全片、Y／U／V／A；seek 後第一張 60／60）。全部組合掉幀 0。讀回在 MF 的專用執行緒上，不佔池與主執行緒。
- **硬解省 CPU、不省記憶體**：行程 CPU 少 25～60 個百分點（以單核 100% 計；2160p 最明顯），但行程 RAM 比 CPU 後端多 100～540 MB（libvpx 的參考幀緩衝消失，換來 MF 裝置與解碼表面池、staging 貼圖、ring 多的兩格都算在行程的 private bytes）。這與 §6 原先的預期相反；要不要縮減（例如縮小 MFT 的表面池、共用裝置）待量測後另議。
- 同時六個 1080p 播放器全部取得硬解。VP8 一律 libvpx。裝置遺失、沒有 MFT、`SetInputType` 失敗都退回 libvpx（以測試開關模擬；真實情境未測）。
- 未測：其他廠牌顯示卡、沒有 VP9 Video Extensions 的實機、多顯示卡時「DXGI 第一張卡＝引擎用的卡」、GPU video decode 引擎使用率。

**場合建議**（`Decode` 屬性／`cev_decodeMode`；排列由消費端協商，能打包就打包）

| 場合 | 建議 | 理由 |
|---|---|---|
| 遊戲選單（1080p、常駐） | GPU | 解碼 0.4 ms、CPU 留給 UI 與邏輯；RAM 多約 100 MB |
| 開場／過場（2160p 或帶 alpha） | GPU | libvpx 要 11～13 ms／幀；硬解 0.6 ms＋背景讀回 |
| 關卡內單一大螢幕 | GPU；記憶體吃緊時 CPU | 硬解多 300 MB 以上 RAM |
| 關卡內多個小螢幕（≤720p 或數量多） | CPU（即 Auto 的結果） | 小解析度軟解很便宜；每個硬解播放器多一條執行緒與一份私有裝置記憶體 |
| 記憶體預算緊 | CPU | RAM 最少 |
| VP8 素材 | CPU（自動） | 沒有硬解 |

`Auto`＝硬體可用且影片高度 ≥ `cev_decodeAutoMinHeight`（1080）時用 GPU。

