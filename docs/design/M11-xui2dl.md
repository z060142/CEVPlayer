# M11 · XUI2DL：以引擎內的精簡 2D 階段取代 Aux（2026-11-26）

## 1. 使用者的決定

- 徹底拿掉 Aux。全螢幕輸出改走引擎渲染器內的 **XUI2DL**：從 XTFrame 的 XUI2D 衍生的最小版本。
- XUI2DL 只做最小維護：不加新功能、不追蹤 XUI2D 的功能更新，只修 bug、相容性與資源管理問題。
- 保留的能力只有六項：2D 貼圖繪製（含純色矩形）、YUV GPU 色彩轉換、alpha 混合、viewport／scissor、讀取迴圈渲染、基本渲染資源管理。
- **選路（2026-11-26 晚間定案）：沒有 XTFrame 的引擎走 XUI2DL；有 XTFrame（完整 XUI2D）的引擎走 XTFrame 帶的 XUI2D，XUI2DL 不建立任何渲染資源。** XTFrame 主線先完成 CE 端的更新（XUI2D 的打包平面著色等，它的 CE-97），通知後我方取得最新原始碼、依本決策重新規劃 XUI2D 那一條路，兩邊一起完成。
- YUV 的規格是 XTFrame 倉 `Docs/design/23-host-integration.md` §23.aj（下稱「§23.aj」；XTFrame 自 v0.19.0 已有 GPU 打包 YUV 轉色，參考實作在其 `Code/Runtime/gpu/src/planar_params.{h,cpp}` 與 GL／wgpu 的平面著色器）。先前引用的「61 章」已被對方刪除，本文其餘各處的「61 章」一律讀作 §23.aj。
- YUV 取樣：不要鋸齒優先 → 61 章的雙線性變體。
- 只掛幀結束一個點：影片在 Scaleform 之上、XUI2D（XTFrame UI）之前。
- 要用全螢幕就一定得打補丁；沒打補丁的引擎沒有全螢幕輸出，不留 Aux 退路。
- 來源與授權說明：寫成一個檔案放在 XUI2DL 標頭旁邊（`NOTICE.md`），不寫進 README。

## 2. 不做的事

模板、漸層、字形、特效、離屏／暫存目標、MSAA、粒子、壓縮貼圖、場景視圖、投影、貼圖建立與上傳（插件沿用現有的引擎上傳路徑）、多個表面、任意頂點。畫面上的 `cev_stats` 文字隨 Aux 一起移除與否待使用者回覆（見 §9）。

## 3. 補丁的形狀

倉內目錄 `EnginePatch/`：

| 路徑 | 內容 |
|---|---|
| `EnginePatch/Code/CryEngine/RenderDll/XUI2DL/` | 新檔，原樣複製進引擎樹：`XUI2DLApi.h`（凍結的 C 介面）、`XUI2DL.h`、`XUI2DL.cpp`、`NOTICE.md` |
| `EnginePatch/Engine/Shaders/HWScripts/CryFX/XUI2DL.cfx` | 著色器，原樣複製 |
| `EnginePatch/apply_xui2dl.py` | 可重放的套用腳本：複製上列檔案，並在引擎既有檔案以錨點插入掛點；每個掛點包在 `// XUI2DL begin` … `// XUI2DL end` 之間，重跑不重複插入；`--check` 只回報狀態，`--remove` 移除掛點 |

插件自己另存一份 `XUI2DLApi.h`（`Module/Render/XUI2DLApi.h`），兩份必須逐位元組相同（自測比對）。

引擎既有檔案的掛點（不改 `IRenderer.h`、不碰 `RenderDll/XUI2D/` 與 `XUI2D_Patched`）：

| 檔案 | 位置 | 內容 |
|---|---|---|
| `RenderDll/XRenderD3D9/DriverD3D.cpp` | `CD3D9Renderer::EndFrame`，Scaleform 之後、`XUI2D::EndFrame`（若有）與 `RenderAux()` 之前 | `XUI2DL::EndFrame(m_activeContextKey);` 與檔頭的 include |
| `RenderDll/XRenderD3D9/D3DRenderThread.cpp` | `RT_CreateRenderResources` 尾端、`RT_ReleaseRenderResources` 的系統資源分支 | `XUI2DL::RT_CreateResources();`／`XUI2DL::RT_ReleaseResources();` 與 include |
| `RenderDll/XRenderD3D9/CMakeLists.txt` | 原始檔清單 | `../XUI2DL/*` |
| `RenderDll/Common/RenderThread.cpp` 等 | 讀取迴圈（L3 才加） | 見 §7 |

## 4. 介面（`XUI2DLApi.h`，C，凍結）

由渲染器 DLL 匯出 `extern "C" const XUI2DLApi* XUI2DL_GetApi(uint32_t version)`；插件以 `GetProcAddress` 取得（與現有的 `XUI2D_Patched` 探測同一做法）。版本不符回 `nullptr`。

```c
#define XUI2DL_API_VERSION 1
enum { XUI2DL_CAP_CORE = 1, XUI2DL_CAP_YUV = 2, XUI2DL_CAP_LOADTIME = 4 };
enum { XUI2DL_KIND_SOLID = 0, XUI2DL_KIND_RGBA_PREMULT = 1, XUI2DL_KIND_RGBA_STRAIGHT = 2,
       XUI2DL_KIND_YUV420 = 3, XUI2DL_KIND_YUVA420 = 4 };
enum { XUI2DL_BLEND_REPLACE = 0, XUI2DL_BLEND_SRCOVER = 1 };   /* sources are premultiplied */
enum { XUI2DL_MAX_LAYERS = 16 };

typedef struct XUI2DLRect { float x, y, w, h; } XUI2DLRect;
typedef struct XUI2DLPlane { uint32_t x, y, w, h; } XUI2DLPlane;   /* texels in the packed texture */

typedef struct XUI2DLLayer
{
    uint32_t    size;          /* sizeof(XUI2DLLayer) */
    uint32_t    kind;          /* XUI2DL_KIND_* */
    uint32_t    blend;         /* XUI2DL_BLEND_* */
    int32_t     textureId;     /* engine texture id; ignored for SOLID */
    XUI2DLRect  dst;           /* viewport pixels, origin top-left, y down */
    XUI2DLRect  src;           /* image pixels, row 0 = top row; w <= 0 means the whole image */
    XUI2DLRect  scissor;       /* viewport pixels; w <= 0 means none */
    float       color[4];      /* premultiplied r,g,b,a multiplier; SOLID: the colour itself */
    uint32_t    imageWidth;    /* YUV kinds: W, H of the picture (chapter 61) */
    uint32_t    imageHeight;
    XUI2DLPlane planeY, planeU, planeV, planeA;
    uint32_t    colorMatrix;   /* 0 = BT.601, 1 = BT.709 */
    uint32_t    colorRange;    /* 0 = limited, 1 = full */
    uint32_t    chromaSiting;  /* 0 = left, 1 = center, 2 = top-left */
    uint32_t    reserved;      /* 0 */
} XUI2DLLayer;

typedef struct XUI2DLFrame
{
    uint32_t           size;        /* sizeof(XUI2DLFrame) */
    uint32_t           layerCount;  /* 0..XUI2DL_MAX_LAYERS; 0 clears the retained list */
    const XUI2DLLayer* pLayers;     /* drawn in order, first = bottom */
    XUI2DLRect         viewport;    /* output pixels; w <= 0 means the whole output */
} XUI2DLFrame;

typedef struct XUI2DLStats
{
    uint32_t size;
    uint32_t resourcesReady;   /* device resources exist */
    uint32_t shaderReady;      /* every technique compiled */
    uint32_t layersHeld;       /* layers in the retained list */
    uint32_t outputWidth, outputHeight;   /* of the last executed frame */
    uint64_t framesDrawn;      /* render-thread executions that drew at least one layer */
    uint64_t drawsExecuted;
    uint64_t drawsSkipped;     /* pipeline state not ready, texture gone, unsupported kind */
} XUI2DLStats;

typedef struct XUI2DLApi
{
    uint32_t size;      /* sizeof(XUI2DLApi) */
    uint32_t version;   /* XUI2DL_API_VERSION */
    uint32_t caps;      /* XUI2DL_CAP_* */
    uint32_t reserved;
    int  (*SetFrame)(const XUI2DLFrame* pFrame);   /* main thread; 1 = accepted, 0 = refused (bad size / count / kind) */
    void (*GetStats)(XUI2DLStats* pStats);         /* any thread */
} XUI2DLApi;
```

語意：

- **保留式**：`SetFrame` 取代整份清單；渲染器每幀重畫最後一份，直到下一次 `SetFrame`。`layerCount = 0` 之後不畫任何東西，也不持有任何貼圖。
- **座標**：像素，原點左上、y 向下；`dst`／`scissor` 相對於 `viewport` 的原點。輸出大小改變時由插件重算並重送（渲染器不替插件做 fit）。
- **貼圖方向**：貼圖的第 0 列是影像的頂列（CEVPlayer 的上傳就是這樣）。
- **顏色**：全部預乘；`RGBA_STRAIGHT` 在著色器裡先乘 alpha。輸出寫進後緩衝的是編碼值，不做色彩空間轉換。
- **YUV**：照 61 章（佈局、雙線性取樣與 siting、轉色、預乘輸出、平面交界不滲色）。L1 不實作，`caps` 沒有 `XUI2DL_CAP_YUV` 時這兩種圖層被略過並計入 `drawsSkipped`。
- 之後擴充只能在結構尾端追加欄位（靠 `size` 判斷）或升 `XUI2DL_API_VERSION`；既有欄位的意義不變。

## 5. 渲染端的做法（取自 XUI2D 的那條路徑，見 `E:/coding3/xt-bench/cev-xui2dl/L0-survey.md` A、B、E）

- 主執行緒 `SetFrame`：驗證 → 把每個 `textureId` 解析成 `_smart_ptr<CTexture>` → 連同圖層做成一份**不可變的快照**（引用計數）→ 換成目前的快照。
- 主執行緒 `XUI2DL::EndFrame(key)`：目前快照非空時，排一個渲染執行緒命令帶著快照的一個引用去畫。L1 這個命令帶「讀取中略過」旗標（讀取迴圈在 L3 處理）。
- 渲染執行緒：取輸出與 viewport（做法同 XUI2D 的 `ResolveOutput`）→ 每個圖層一個四邊形，頂點寫進 BU_DYNAMIC 緩衝，**上傳後重設命令列表的串流快取**（XUI2D 的 CE-94 教訓）→ 以 `CPrimitiveRenderPass`／`CRenderPrimitive` 繪製；scissor 改變就另開一個 pass instance。
- 貼圖生命：快照持有引用，所以插件銷毀播放器後，渲染器仍安全地畫完已排隊的幀。**快照的最後一個引用不得在 render loading thread 上釋放**（`~CTexture` 會斷言），做法照 XUI2D 的 `RT_FreeArena`（延後到迴圈結束）。
- 資源：**延後建立**——裝置建立時不建任何東西，第一次收到非空的 `SetFrame` 之後才在渲染執行緒建著色器、頂點格式、常數緩衝並預熱著色器（所以走 XUI2D 的引擎上，即使兩個補丁都套了，XUI2DL 也不佔資源）；`RT_ReleaseResources`（每次卸關卡的 `FreeSystemResources(FRR_SYSTEM_RESOURCES)` 也會走到）只釋放渲染資源，**保留快照**——快照是呼叫端的狀態，跨卸關卡保留；只在渲染器關閉時（`RT_ReleaseRenderResources` 的 `FRR_TEXTURES` 分支、`CTexture::ShutDown` 之前，`XUI2DL::RT_ReleaseRetained()`，L3c）丟掉。資源不存在或著色器未就緒時不畫、計入 `drawsSkipped`。解析度改變不重建資源，每幀重讀輸出大小。
- 著色器是引擎目錄下的 `.cfx` 散檔。允許執行期編譯的建置（PC 三個組態的預設）可直接用；關閉編譯的出貨組態需要把預編快取併入引擎的著色器 pak——與 XUI2D 相同的未解問題，列為已知限制。

## 6. YUV 著色器（L2）

照 61 章 §61.2～§61.4。常數：四個平面矩形、整張貼圖尺寸、轉色的列向量（由矩陣與範圍兩個列舉在 C++ 端算好傳入）、siting 偏移。合規：1:1 輸出時與 `Core/tests/ReferenceSampler.h` 的雙線性變體每通道差 ≤ 1 級；縮放時只驗平面交界不滲色。

## 7. 讀取迴圈（L3，動工前與 XTFrame 主線對一次）

- 迴圈內、`m_rdldLock` 之內、`RT_PresentFast()` 之前加一個繪製掛點：畫渲染執行緒持有的快照。
- 沒有補丁 17 的引擎：XUI2DL 自帶「有內容時自動進出迴圈＋一個單一客戶的回呼」（等同補丁 17 的核心），介面追加 `SetLoadtimeUpdate`，`caps` 加 `XUI2DL_CAP_LOADTIME`。
- 有補丁 17 的引擎：沿用它的回呼與自動進出，XUI2DL 只加繪製掛點。
- 待與 XTFrame 主線確認：兩者並存時後緩衝由誰清、XTFrame 的讀取畫面清除輸出時影片是否被擦掉。

## 8. 階段

| 單 | 內容 | 驗證的樹 |
|---|---|---|
| L1 | 引擎核心：介面、純色與 RGBA、混合、viewport／scissor、資源管理、套用腳本、探測指令（L1b：延後建立資源，並在社群版樹建置與驗證） | 社群版樹 |
| L2 | YUV 著色器與合規測試 | 社群版樹 |
| L3 | 讀取迴圈 | 共用樹（有補丁 17）＋社群版樹（沒有） |
| L5 | XUI2D 後端：有 XTFrame 時經 `IXUIRenderer` 送幀（`Module/Render/FullscreenXUI2D.*`，`CEV_HAVE_XUI2D`）。實際做法：①提交時機＝插件 `UpdateBeforeRender` 的最後（播放器 tick 與所有貼圖上傳之後），與 CryXTUI 的幀同一批；有圖層的幀每幀送、沒有圖層的幀不送（該表面那一幀不畫）。②一個主視窗螢幕表面，`SetSurfaceOrder` = `-(1 << 20)`，排在 CryXTUI 的表面之下。③XUI2D 不持有包裝的引擎貼圖：插件對每張用到的貼圖持有 `_smart_ptr<ITexture>`，最後一次被送出的幀用到之後再保留 4 個主執行緒幀才釋放（並銷毀其句柄） | 共用樹 |
| L4 | 插件切換：`FullscreenOutput` 改送圖層、全螢幕不再強制 RGBA、刪除全部 Aux、更新探測指令與文件 | 兩棵樹 |

## 9. 已知限制與待定

- XUI2D 的 `xui2d_afterScene` 開啟時，XTFrame UI 提前畫在 Scaleform 之下，此時 XUI2DL 的影片會蓋在 XTFrame UI 之上（只有一個掛點的後果）。
- 縮小超過 2 倍時沒有 mip，會閃（61 章同樣列為已知限制）。
- 出貨組態的著色器快取（§5）。
- `cev_stats` 的畫面文字：XUI2DL 沒有字形，待使用者決定是移除（只留 log 與 CSV）還是另想辦法。
- 社群版樹的渲染器有第三方改動，套用腳本的錨點要在 L3／L4 另外確認。
- 讀取迴圈（L3c）：讀取期間才開始的全螢幕播放器，要到讀取結束後才出現（迴圈內不建立貼圖、不送清單）；讀取中全螢幕播放器的貼圖身分凍結，串流換解析度或排列需求改變時迴圈內保留舊貼圖與畫面，讀取結束後才重建。
- `NOTICE.md` 目前只有來源與維護說明，沒有附 MIT 的版權聲明與許可全文；要不要補由使用者決定。
