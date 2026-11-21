# 01 · CEVPlayer 重建：開放問題調查（唯讀研究）

調查日期：2026-11-16。範圍：只讀原始碼，沒有實機執行；凡「未實機驗證」都有標明。

## 路徑縮寫

| 縮寫 | 路徑 |
|---|---|
| `V:` | `D:/coding11/Cryengine_5.7.1`（vanilla 5.7.1，檔案為 CRLF） |
| `C:` | `D:/coding11/CryENGINE-5-7-community`（CECE） |
| `X:` | `D:/coding11/XTFrame-CRYENGINE`（姊妹專案；`X:docs/01..06` 為其文件） |
| `O:` | `D:/coding11/CEVPlayer`（舊插件倉；與 `C:Code/CryPlugins/CEVPlayer` 內容相同，僅多 `docs/`、`tools/`） |

引擎相對路徑省略 `Code/CryEngine/`，例如 `V:RenderDll/...` = `V:Code/CryEngine/RenderDll/...`；`V:CryCommon/...` 同理。

---

## 0. 結論一覽

| # | 問題 | 短答 |
|---|---|---|
| 1a | 自有貼圖 | `IRenderer::UploadToVideoMemory(nullptr,w,h,eTF_R8G8B8A8,eTF_R8G8B8A8,0,false,FILTER_BILINEAR,0,"$名稱",FT_NOMIPS\|FT_DONT_STREAM)` 可取得**名稱完全等於你給的字串**的貼圖；`IRenderer::CreateTexture` 會在名稱後加流水號（不適合要被 `$name` 引用的場合）。不需 patch。 |
| 1b | 每幀上傳 | `IRenderer::UpdateTextureInVideoMemory` 在**主執行緒**呼叫即可（內部複製資料並排進渲染命令）；不可從解碼執行緒呼叫；貼圖必須 `FT_NOMIPS`。不需 patch。 |
| 1c | 全螢幕覆蓋 | 非 Aux 的公開選項只有 Scaleform（`img://TEXID:<id>` 載入到 movieclip），需要一個預先做好的 `.gfx`；其餘（PostEffect、IPostRenderer 之類）在 vanilla 5.7.1 沒有可用的「畫一張貼圖」介面。Stage 1 最不糟的做法仍是 `IRenderAuxImage::Draw2dImage` / aux 四邊形（XTFrame stage 1 採同一做法）。 |
| 1d | 綁材質 / `$name` | 材質 `.mtl` 的貼圖路徑寫 `$名稱` 即可；`CTexture::ForName` 對 `$` 開頭名稱不讀檔、只做名稱登記，先後順序無關。不需 patch。 |
| 1e | GPU YUV→RGB | 公開 API 沒有 NV12/I420 格式（`eTF_YUV` 只是列舉，渲染器沒有裝置端支援）。可行路線：3 張 `eTF_R8` 貼圖 + 自訂 `.cfx` 材質著色器，**僅限材質路線**（XTFrame 已證實散檔 `.cfx` 免 patch）。2D 螢幕覆蓋走 aux 時著色器固定，必須 RGBA → Stage 1 用 CPU 轉換。 |
| 2 | 載入期更新點 | **預設的同步載入沒有任何每幀更新點**（主執行緒卡在載入迴圈）。只有三種「還在跑」的東西：`ILog::UpdateLoadingScreen`（節流 1 秒）、載入專用渲染執行緒的 `ILoadtimeCallback`（單一槽，被 FlashUI 佔用）、以及改用 `g_asynclevelload=1` / `g_levelLoadTimeSliced=1` 時主迴圈照常跑。詳 §2。 |
| 3 | 舊插件依賴 | 見 §3 表。swscale 只做 I420→RGBA；SDL2 只做音訊輸出；引擎兩棵樹只有 CEVPlayer 自己用 Libav。SDL2 為 2.0.8。 |
| 4 | CECE vs vanilla（RenderDll） | 只有 HPi 的 MeshBaker（新 render command、新 pipeline stage、RenderDoc API）。`EndFrame`、`RenderAux`、Scaleform、貼圖上傳路徑**沒有差異**。 |
| 5 | CryVS | vanilla 沒有。CECE 是 Schematyc2 改名；外部模組靠 `gEnv->pCryVS->Signals().envRefresh` 重註冊。`IAction` 沒有完成回報介面。 |
| 6 | 資產系統 | 兩棵樹的資產系統相同（僅空白差異）。可註冊自訂型別、多檔、`Details` 自訂欄位；`CAssetBrowser` 可帶型別清單內嵌。 |
| 7 | CryAudio | 公開 `IObject` 只有 setter；沒有查詢位置或播放進度的介面。尾端追加虛擬函式可行（需 patch，且要連 CryAudioSystem 一起重建）。 |

---

## 1. Stage 1（不 patch 引擎）的渲染介面

### 1a. 建立自己擁有的貼圖

介面（`V:CryCommon/CryRenderer/IRenderer.h`）：

- `UploadToVideoMemory(...)`：1482 行。
- `CreateTexture(name,w,h,numMips,pData,eTF,flags)`：1611 行。
- `RemoveTexture(id)`：1490 行。
- `EF_GetTextureByName`：1317 行；`EF_GetTextureByID`：約 1314 行。

行為差異（實作，`V:RenderDll`）：

- `CRenderer::CreateTexture` 把名稱格式化成 `"%s%d"`（`m_TexGenID++`）才建立：`Common/Renderer.cpp:3573-3578`。要讓 `$name` 精確可查，不能用它。
- `CD3D9Renderer::UploadToVideoMemory(..., szCacheName, ...)` 直接用傳入名稱（`XRenderD3D9/DriverD3D.cpp:4694-4710`，`!szCacheName` 才產生 `$AutoDownload_N`），內部強制加 `FT_DONT_STREAM`（4711 行）、`!repeat` 加 `FT_STATE_CLAMP`。回傳貼圖 ID（`pTex->GetID()`）。舊插件即用此函式（`O:Module/Render/RenderWrapper.cpp:33,51,97`）。
- 名稱鍵是小寫並轉 `/`、只有 `FT_ALPHA` 會改鍵：`Common/Textures/Texture.cpp:282-294`（`GenName`）。所以 `$CEV_Video1` 與 `$cev_video1` 是同一張。
- 以 `pData == nullptr` 建立只配置裝置貼圖；建立時 `CreateDeviceTexture` 預設 `FlushAndWait`（`XRenderD3D9/D3DTexture.cpp:253-279`），也就是**會等渲染執行緒**。所以只在 Open 時建一次，不要每幀或在熱路徑重建。
- 釋放：`RemoveTexture` 只是 `SAFE_RELEASE`（`DriverD3D.cpp:4676-4686`）。XTFrame 實測：加 `FT_DONT_RELEASE` 會讓 `Release()` 回 -1、重建洩漏，故不要加（`X:docs/01-stage1-design.md:102`）。
- 色彩：XTFrame 使用 `FT_NOMIPS | FT_DONT_STREAM | FT_USAGE_ALLOWREADSRGB`；`ALLOWREADSRGB` 的色彩空間行為需用色票驗證，XTFrame 當時也列為待驗（`X:docs/02-probe-results.md:43`）。**未實機驗證。**

自建 render target 不是 Stage 1 需要的東西（影片只需要「CPU 上傳的貼圖」）；若將來要「把影片渲進 RT」，公開面只有 `ITexture::Clear` 與 `FT_USAGE_RENDERTARGET` 旗標，沒有公開的 draw-to-RT 介面（XTFrame 為此做了 `CreateTarget`，見其 patch 02/15）。

### 1b. 每個新影格從 CPU 上傳

`IRenderer::UpdateTextureInVideoMemory(tnum, newdata, posx, posy, w, h, eTFSrc, posz, sizez)`：`IRenderer.h:1485`；實作 `DriverD3D.cpp:4688-4692` → `CTexture::UpdateTextureRegion`（`XRenderD3D9/D3DTexture.cpp:522-547`）。

執行緒規則（讀碼結論）：

1. 在渲染執行緒上：直接 `RT_UpdateTextureRegion`。否則：先把資料**整份複製**進 `std::vector`（大小用 `TextureDataSize(USize,VSize,ZSize,GetNumMips(),...)`，`D3DTexture.cpp:530`），再 `ExecuteRenderThreadCommand` 排進命令佇列。呼叫端緩衝區在函式返回後即可重用。
2. 非渲染執行緒的路徑走 `m_Commands[m_nCurThreadFill]`，且 `CRY_ASSERT(IsMainThread())`、且命令緩衝**沒有鎖**（`Common/RenderThread.h:445-495`，488 行的鎖是被註解掉的）。→ **只能在主執行緒呼叫**，解碼執行緒要先把 RGBA 放進自己的環形緩衝，由主執行緒上傳。
3. 因為複製大小依 `GetNumMips()`，貼圖必須是 `FT_NOMIPS`（mips=1），否則會讀超過來源緩衝。
4. `RT_UpdateTextureRegion` 要求來源格式與貼圖目的格式的 typeless 形式相同，否則 `CRY_ASSERT(0)` 後直接返回（`D3DTexture.cpp:565-574`）。也就是上傳 `eTF_R8G8B8A8` 到 `eTF_R8G8B8A8` 貼圖；`eTF_R8` 同理。
5. 局部更新沒有 stride 參數，來源必須緊排（`X:docs/01-stage1-design.md:100`）。
6. 載入中的行為（重要）：`UpdateTextureRegion` 在 `m_eVideoThreadMode != eVTM_Disabled`（載入專用渲染執行緒運轉中）時，對主執行緒的呼叫加 `MainThread_defer`（`D3DTexture.cpp:534-536`），命令被存進 `m_CommandsLoading`，**要等載入結束才會執行**（`ExecuteRenderThreadCommand`：`RenderThread.h:453-473`；`StopLoadtimeFlashPlayback` 之後才併入主佇列，`XRenderD3D9/D3DRenderThread.cpp:381-391`）。所以同步載入期間用這個 API 更新不出新畫面。
7. 每次呼叫一份複製：XTFrame 的結論是「一次大矩形優於多次小矩形」（`X:docs/01-stage1-design.md:94`）；影片每幀整張更新，只呼叫一次。

渲染執行緒側的備援掛點：`IRenderer::RegisterSyncWithMainListener(ISyncMainWithRenderListener*)`（`IRenderer.h:1547`，介面 761 行）；在這個 callback 裡呼叫 `UpdateTextureInVideoMemory` 會走「已在渲染執行緒」的直接路徑，省掉一次複製。**未實機驗證**其呼叫時機與執行緒（介面註解只說同步主執行緒與渲染執行緒）。

### 1c. 不用 Aux 的全螢幕覆蓋

先盤點 vanilla 5.7.1 公開面：

- `IRenderer` 內沒有 `Draw2dImage`/`DrawImage`；2D 影像的公開入口就是 `IRenderAuxImage::Draw2dImage`（`V:CryCommon/CryRenderer/IRenderAuxGeom.h:968-1008`，內部是 `PushImage`）。
- `EF_SetPostEffectParam*`（`IRenderer.h:1378-1380`）可用的 post effect 中，`ScreenFader` 只有顏色（`V:RenderDll/Common/PostProcess/PostEffects.h:1642-1663`，且 `Execute()` 是 `ASSERT_LEGACY_PIPELINE`，`XRenderD3D9/PostProcessGame.cpp:1235`）；`FilterMaskedBlurring_MaskTexName`（`PostEffects.h:195`）是遮罩用途，不是覆蓋。**沒有「貼圖覆蓋」的 post effect。**
- `IPostRenderer` / 任何「後繪製」註冊介面：在 vanilla `CryCommon` 內沒有（grep 無結果）。XTFrame 也把「IPostRenderer」列為未做（`X:docs/01-stage1-design.md:230`）。
- 3D 路線：在鏡頭前放一個帶材質的實體當四邊形（`DrawNear`/深度關閉技巧）。可行但等於世界內物件，會受後處理、景深、Sandbox 視口影響，不建議當 2D 覆蓋。
- Scaleform（公開、非 Aux）：`CFlashUIElement::LoadTexIntoMc` 實際呼叫 `loadMovie("img://TEXID:<貼圖ID>")`（`V:CryAction/FlashUI/FlashUIElement.cpp:1569-1595`）；`CryImageCreator::LoadProtocolImage` 解析 `://TEXID:` 並用 `EF_GetTextureByID` 取貼圖（`V:CryScaleform/Impl/SharedStates.cpp:527-556`）。公開介面：`IUIElement::LoadTexIntoMc`（`V:CryCommon/CrySystem/Scaleform/IFlashUI.h:1537-1541`）、`IScaleformHelper::CreateFlashPlayerInstance`（`IScaleformHelper.h:69`）、`IFlashPlayer::SetViewScaleMode`（`IFlashPlayer.h:91`）。代價：需要一個事先用 Scaleform 工具做好的最小 `.gfx`（引擎 `Engine/EngineAssets` 內沒有可借用的；`find` 無 `.gfx`/`.swf`）；Flash 層的疊放順序與使用者自己的 Flash UI 共用。
- 載入期的覆蓋唯一可行路徑也是 Scaleform（見 §2），因為載入專用渲染執行緒的迴圈只會呼叫 `FlashRenderPlayer`，且 aux 的繪製命令帶 `SkipDuringLoading`（`V:RenderDll/XRenderD3D9/DriverD3D.cpp:3086-3120`，約 3112 行）。

結論：**沒有免 patch、非 Aux、且不需要額外資產的 2D 全螢幕覆蓋**。Stage 1 的最不糟做法：

- 遊戲進行中：aux 2D 影像／四邊形。XTFrame stage 1 用 `SAuxGeomRenderFlags(e_Def2DImageRenderflags)` + `SetTexture(id)` + `BeginDrawBuffer(6,true)`（`X:docs/01-stage1-design.md:119-127`）；舊插件用 `IRenderAuxImage::Draw2dImage`（`O:Module/Render/RenderWrapper.cpp:127`）。aux 是在 `CD3D9Renderer::EndFrame` → `RenderAux()` 提交（`V:RenderDll/XRenderD3D9/DriverD3D.cpp:3207-3226`），畫在場景、Scaleform 之上。
- 已知限制：aux 對不同貼圖的批次排序穩定性 XTFrame 的 T2 探針**一直沒觀察**（`X:docs/02-probe-results.md:17,26`）；aux 沒有預乘混合；載入畫面不畫。影片只有單層，排序問題不影響。
- 要在載入畫面顯示影片或真正的「疊在 Flash UI 之上」→ 要 patch（見建議）。

### 1d. 綁到材質槽與命名 `$name` 貼圖

- 命名貼圖：`CTexture::ForName` 對 `name[0]=='$'`：找得到就回傳，找不到就**登記一個空殼**並直接返回，不載檔（`V:RenderDll/Common/Textures/Texture.cpp:819-851`）。因此 `.mtl` 裡寫 `$CEV_Video1` 時，插件之後用 `UploadToVideoMemory(..., "$CEV_Video1", ...)`（同鍵 → `FindOrRegisterTextureObject`，`Texture.cpp:334-357`）會填進同一個物件；順序無關，載入材質先於影片也行。
- 材質執行期綁定：`IMaterial::GetShaderItem()`（`V:Cry3DEngine/IMaterial.h:215,281`）→ `SShaderItem::m_pShaderResources`（`IRenderShaderResources::GetTexture(slot)`，`V:CryCommon/CryRenderer/IShader.h:1339`）→ `SEfResTexture::m_Sampler.m_pITex`（`IShader.h:1088-1095, 909-914`）。複製材質用 `IMaterialManager::CloneMaterial`（`IMaterial.h:437`）。直接改 sampler 的 `m_pITex` 之後是否需要手動觸發重編資源集（`UpdateConstants`、`IShader.h:1316`）**未驗證**；建議路線仍是 `.mtl` 寫 `$name`。
- 舊插件的 Flow 節點用 `EF_GetTextureByName` 輸出貼圖物件（`O:Module/FlowNodes/FlowNodes.cpp:593`）。
- Flash UI：`LoadTexIntoMc(movieClip, ITexture*)` 用貼圖 ID（上述）；`IFlashUI.h:1549-1552` 的 `AddTexture(IDynTextureSource*)` 是 Flash 當**來源**輸出貼圖，方向相反，不適用。
- 引擎也有 `EF_LoadDynTexture`（`IRenderer.h:1323`），但 `IDynTextureSource` 只定義了 `DTS_I_FLASHPLAYER` 一種來源（`V:CryCommon/CryRenderer/ITexture.h:514-545`），不能塞自訂畫面。

### 1e. GPU YUV→RGB（是否免 patch）

- 格式：`ETEX_Format` 只有 `eTF_YUV/eTF_YUVA` 列舉（`ITexture.h:112-113`），渲染器端唯一的引用是 Scaleform 的 assert（`V:RenderDll/Scaleform/ScaleformPlayback.cpp:579`），沒有 NV12/I420 裝置貼圖。
- 可行的免 patch 做法：Y、U、V 各一張 `eTF_R8` 貼圖（U/V 半解析度，各自 `UpdateTextureInVideoMemory(eTF_R8)`），再用一個**自訂 `.cfx` 材質著色器**取樣三張並做轉換。XTFrame 已證實：
  - 散檔 `.cfx` 可放在 `<引擎>/Engine/Shaders/HWScripts/CryFX/` 或遊戲目錄 `<game>/GameShaders/HWScripts/CryFX/`（`V:RenderDll/Common/Shaders/ShaderCore.cpp:1090-1092`），不用重打 shaders.pak（`X:docs/06-shader-pipeline.md` §6.1、§6.5）。
  - 作為材質著色器用 `EF_LoadShaderItem("名稱",...)` 載入（`IRenderer.h:1299`；XTFrame 的 `XUIPanel.cfx` 是先例，`X:docs/06-shader-pipeline.md` §6.8）。
  - Dev 組態（Profile/Debug）可即時編譯；`_RELEASE` 必須預先把 `.cfxb` 與 HLSL 快取併入 shader cache pak（§6.4）；非同步編譯會造成首幾幀沒畫面（§6.3、§6.8）。
- 限制：這只對「材質路線」有效。2D 覆蓋走 aux 時用的是引擎固定的 `AuxGeom.cfx`（`V:Engine/Shaders/HWScripts/CryFX/AuxGeom.cfx`），不能換成 YUV 著色器。
- 結論：**Stage 1 先用 CPU 轉 RGBA**（舊插件 `sws_scale` 的位置，`O:Module/Video/VideoPlayer.cpp:387`），自寫 I420→RGBA（可 SSE/AVX2，並可在解碼執行緒做，主執行緒只上傳）。GPU YUV 當材質路線的後續優化（M5 或之後）。

---

## 2. 關卡載入期間仍會被呼叫的更新點

### 2.1 引擎有三種載入模式（`g_asynclevelload` / `g_levelLoadTimeSliced`）

選擇邏輯：`V:CryAction/Network/CET_LevelLoading.cpp:265-296`（`AddLoadLevel`）；cvar 定義：`V:CryAction/CryAction.cpp:3663-3664`。

| 模式 | 條件 | 載入發生在 | 主迴圈（`CSystem::DoFrame`） | 備註 |
|---|---|---|---|---|
| 同步（預設） | `g_asynclevelload=0` 且 `g_levelLoadTimeSliced=0` | 主執行緒，在 context-establisher 任務內一次做完（`CET_LevelLoading.cpp:51-85`，`CCET_LoadLevel`） | **整段卡住，沒有 DoFrame** | 載入畫面靠 Flash 載入專用渲染執行緒 |
| 非同步 | `g_asynclevelload=1`（`VF_CONST_CVAR`，只能由設定檔/啟動參數設，`CryAction.cpp:3663`）、非編輯器、非 dedicated | 專用 `LevelLoadingThread`（`CET_LevelLoading.cpp:87-193`） | 主迴圈照跑 | FlashUI 在這模式**不**啟動載入渲染執行緒（`FlashUI.cpp:1509-1511`） |
| 分時 | `g_levelLoadTimeSliced=1`（`CryAction.cpp:3664`） | 主執行緒每個 DoFrame 做一步（`CET_LevelLoading.cpp:195-263`、`CLevelLoadTimeslicer::DoStep`，`V:CryAction/LevelSystem.cpp:1195-1230`） | 主迴圈照跑 | `LOAD_3DENGINE_SLICED` 固定為 1（`LevelSystem.cpp:1194`） |

### 2.2 同步載入期間「還在被呼叫」的東西

1. `ILog::UpdateLoadingScreen(...)` → `CSystem::UpdateLoadingScreen()`：`V:CrySystem/Log.cpp:1454-1470`、`V:CrySystem/SystemRender.cpp:557-614`。**節流為 1 秒一次**（`s_updateFrequencyInSeconds = 1.0f`，`con_showonload` 時 0.01 秒；`SystemRender.cpp:571-584`），會呼叫 `m_pProgressListener->OnLoadingProgress(0)`（603-606 行）。`ISystem::ILoadingProgressListener` 只有**一個**槽（`ISystem.h:1022-1028`、`SetLoadingProgressListener` `ISystem.h:1210`），已被 `CLevelSystem` 佔用（`LevelSystem.cpp:906/926`）。→ 1 秒一次，不能當影片時鐘；也不要去搶這個槽。
   - 它再轉成 `ILevelSystemListener::OnLoadingProgress(ILevelInfo*, int)`（`LevelSystem.cpp:1963-1996`），同樣是 1 秒一次的頻率。
2. `ILoadtimeCallback`（`V:CryCommon/CryRenderer/IRenderer.h:753-758`：`LoadtimeUpdate(dt)`、`LoadtimeRender()`）：`IRenderer::StartLoadtimeFlashPlayback`（`IRenderer.h:1620`）會讓渲染器開一條**載入專用渲染執行緒**，迴圈約每個 vsync 呼叫 `LoadtimeUpdate` 再 `LoadtimeRender`，然後 `RT_PresentFast`（`V:RenderDll/Common/RenderThread.cpp:635-710`，callback 在 681-688 行；實作 `D3DRenderThread.cpp:345-395`）。
   - **只有一個槽**：`assert(!m_pRT->m_pLoadtimeCallback)`，已有就直接 return（`D3DRenderThread.cpp:348-352`）。
   - FlashUI 在 `ESYSTEM_EVENT_LEVEL_LOAD_START_LOADINGSCREEN` 時無條件佔用（`V:CryAction/FlashUI/FlashUI.cpp:416-427`、`StartRenderThread` `1501-1523`；條件 `gfx_loadtimethread==1 && !editor && r_MultiThreaded && !bAsyncLoad`），在 `ESYSTEM_EVENT_LEVEL_LOAD_END` 時 `StopRenderThread`（`FlashUI.cpp:487-491`）。事件由 `CLevelSystem::PrepareNextLevel` 發出（`LevelSystem.cpp:1803`）。
   - callback 在**渲染執行緒**內執行；`LoadtimeRender` 能做的事只有 `FlashRenderPlayer`（`FlashUI.cpp:620-640`）。aux 在這段不會被畫（`RenderAux` 的命令帶 `SkipDuringLoading`，且該執行緒不走 `EndFrame`）。
   - 在這條執行緒內 `UpdateTextureInVideoMemory` 會走「已在渲染執行緒」直接路徑（`IsRenderThread()` 對載入渲染執行緒也成立：`RenderThread.h:376-384`），所以 callback 內可以直接上傳新影格。若插件要在此提供影片，需要取代 FlashUI 的槽（`StopLoadtimeFlashPlayback` 再 `Start...` 自己的 callback 並轉呼 FlashUI 的 `ILoadtimeCallback`，要靠 `dynamic_cast`）。這是 hack，**未驗證**，且畫面仍必須是 Flash 影片載入畫面（見 §1c）。
3. `SYNCHRONOUS_LOADING_TICK()`（如 `Texture.cpp:605`）→ `CSystem::SynchronousLoadingTick`：只在 `gEnv->bMultiplayer && !IsEditor()` 才呼叫 `UpdateLoadingScreen`（`SystemRender.cpp:541-553`），單機不會。
4. `ISystem::UpdateLoadtime()`（`V:CrySystem/System.cpp:2330-2350`）只 tick PlatformOS，輸入更新被註解掉。
5. 音訊：`CSystem::UpdateAudioSystems` 在載入中**不更新** `pAudioSystem`（`System.cpp:2357-2366`，註解寫「do not update pAudioSystem during async level load」）。M2/M3 若走 CryAudio，載入中不會有更新。

不會被呼叫的：`IGameFrameworkListener::OnPostUpdate`（它來自 `CCryAction::PostRender`，`CryAction.cpp:2708-2730`，只在 `CSystem::DoFrame` 內）、所有 `IEnginePlugin::Update*`（`System.cpp:1564-1684`）、`CFlashUI::OnPostUpdate`（FlashUI 用它畫非載入期的 Flash；載入期若 `gfx_loadtimethread=0` 則改由 `CFlashUI::OnLoadingProgress` 自己 `RenderBegin/OnPostUpdate/RenderEnd`，`FlashUI.cpp:565-585`，同樣 1 秒一次）。這就是舊插件「OnPostUpdate 載入時被擋」的原因（`O:Module/FlowNodes/FlowNodes.cpp:73` 的 TODO 與 `O:Module/Video/2DVideoQueue.cpp:85`）。

### 2.3 非同步／分時載入期間

主迴圈每幀 `DoFrame`（`System.cpp:1538-1700`）：`UpdateBeforeSystem`(1564) → `Update` → `PostSystemUpdate` → `UpdateAfterSystem`（= `MainUpdate`，1618；`CryPluginManager.cpp:694-701`）→ `UpdateBeforeFinalizeCamera`(1635) → `UpdateBeforeRender`(1651) → `Render` → `PostRender`（= `OnPostUpdate`）→ `UpdateAfterRender`(1663) → `RenderEnd` → `UpdateAfterRenderSubmit`(1684)。這些會持續被呼叫，影片可持續播放。**未實機驗證**：舊插件在這兩個模式下為何崩潰沒有調查；`CSystem::Update` 內有 `IsLoading()` 相依邏輯，且載入執行緒（非同步）與主執行緒同時存在，渲染 API 呼叫的執行緒歸屬（`LevelLoadingThread_*` 旗標，`RenderThread.h:56-67`）需要驗證。

### 2.4 XTFrame 用什麼

- 插件更新：`EnableUpdate(EUpdateStep::BeforeRender, true)`，`UpdateBeforeRender()` 內做全部 tick（`X:docs/01-stage1-design.md:35-36`）。
- Sandbox 面板驅動：編輯模式 `eNotify_OnIdleUpdate`；game mode 用 `IGameFrameworkListener::OnPostUpdate`（`IGameEngineListener::OnPostEditorUpdate` 在 game mode 不會被呼叫，`X:docs/05-stage2-design.md:537`、`X:docs/08-xtframe-integration-log.md:1097`）。
- 載入畫面：Stage 1/2 都**沒做**（`X:docs/01-stage1-design.md:230`、`X:docs/05-stage2-design.md:845`）；其 XUI2D 渲染命令帶 `SkipDuringLoading`，載入期不畫（`X:Code/CryEngine/RenderDll/XUI2D/XUI2DRenderer.cpp:1924`，`X:docs/02-probe-results.md:949`）。
- 系統事件掛法：`ESYSTEM_EVENT_LEVEL_LOAD_START(_LOADINGSCREEN)`、`LEVEL_LOAD_END`、`LEVEL_PRECACHE_END`、`LEVEL_GAMEPLAY_START` 都只做標記或 `RequestPrewarm`，實際動作排到下一次 `UpdateBeforeRender` 開頭（`X:docs/05-stage2-design.md:154`、`X:docs/08-xtframe-integration-log.md:1401`）。

相關系統事件列舉：`V:CryCommon/CrySystem/ISystem.h:281-331`（`LEVEL_LOAD_START_PRELOADINGSCREEN`、`LEVEL_LOAD_PREPARE`、`LEVEL_LOAD_START_LOADINGSCREEN`、`LEVEL_LOAD_START`、`LEVEL_LOAD_END`、`LEVEL_PRECACHE_START/END`、`LEVEL_GAMEPLAY_START`、`LEVEL_UNLOAD*`）。

---

## 3. 舊插件的第三方依賴與 CMake

### 3.1 依賴逐項（`O:Module/…`）

| 函式庫 | 用途 | 位置 |
|---|---|---|
| **Libav swscale**（僅此，無 avcodec/avformat） | VP8/VP9 解碼出的 I420 轉 RGBA | `Video/VideoPlayer.h:36-39`（`extern "C"` + `<libswscale/swscale.h>`）、`:184`（`SwsContext*`）；`Video/VideoPlayer.cpp:49`、`216-232`（`sws_getContext(...AV_PIX_FMT_YUV420P → AV_PIX_FMT_RGBA, SWS_BILINEAR)`）、`325-326`（`sws_freeContext`）、`387`（`sws_scale`）。執行期需 `avutil-54.dll`、`swscale-3.dll`（CMake 末兩行 `deploy_runtime_files`）。 |
| **libvpx** | VP8/VP9 解碼 | `Video/VideoPlayer.h:16-17`（`vpx/vpx_decoder.h`、`vp8dx.h`）、`:84-88`（`vpx_codec_ctx_t/iter/vpx_image_t/iface/flags`）；`VideoPlayer.cpp:182,186`（`vpx_codec_vp8_dx_algo`/`vp9_dx_algo`）、`195-203`（`vpx_codec_dec_cfg_t`、`vpx_codec_dec_init`，含 `threads`）、`318`（`vpx_codec_destroy`）、`726-741`（`vpx_codec_peek_stream_info`、`vpx_codec_decode`、`vpx_codec_get_frame`）；`Render/Frame.cpp:8`（`vpx/vpx_image.h`）。 |
| **libwebm（mkvparser）** | WebM 容器解析 | `Video/Packet.h:6`、`Video/VideoFileReader.h:6`（`mkvreader.hpp`，`CVideoFileReader : mkvparser::IMkvReader`，`:10`）、`Video/VideoPlayer.h:19,93,153-161`；`VideoPlayer.cpp:71`（`mkvparser::GetVersion`）、`123-163,245-253,646-717`（Segment/Tracks/Cluster/Block 走訪）。 |
| **libvorbis（+ libogg 型別）** | 音訊解碼（packet 級，不是 `ov_*` 檔案 API） | `Audio/AudioDecoder.h:6`（`vorbis/codec.h`）、`:57-60`（`vorbis_info/comment/dsp_state/block`）；`Audio/AudioDecoder.cpp:31-35`（`ogg_packet`）、`62-65,70-71,81,122-128,148-149,163-213`（`vorbis_synthesis_*`、`vorbis_block_*`）。程式碼沒有任何 `ov_*` 呼叫，但 CMake 仍連結 `libvorbisfile_static.lib`。 |
| **SDL2**（僅音訊輸出） | 開音訊裝置、callback 混音 | `Audio/AudioWrapper.h:11-12,32`（`SDL.h`、`SDL_audio.h`、`SDL_AudioDeviceID`）；`Audio/AudioWrapper.cpp:13-14`（含 `SDL_syswm.h`，未見使用）、`37-49`（解構：`SDL_WasInit`→`SDL_CloseAudioDevice`→**`SDL_Quit()`**）、`63-100`（`SDL_PauseAudioDevice`）、`110-114`（`SDL_memset`、`SDL_MixAudioFormat`、`SDL_MIX_MAXVOLUME`）、`131-167`（`SDL_Init(SDL_INIT_AUDIO)`、`SDL_OpenAudioDevice`、`SDL_AudioSpec` 填 `AUDIO_F32`）。 |

**風險（SDL）**：舊插件解構時若 SDL 音訊已初始化就 `SDL_Quit()`（`AudioWrapper.cpp:37,47-49`），而引擎的 SDL_mixer 實作自己 `SDL_Init(SDL_INIT_AUDIO)`（`V:CryAudioSystem/implementations/CryAudioImplSDLMixer/Impl.cpp:460`）並在關閉時 `SDL_Quit()`（522 行）。兩邊共用同一個 SDL 全域狀態；插件的 `SDL_Quit()` 會把引擎的 SDL 一起關掉。重建時改用 `SDL_InitSubSystem/SDL_QuitSubSystem(SDL_INIT_AUDIO)`，且不要在引擎已初始化時關閉。

其餘：舊插件的時鐘、解碼執行緒、佇列沒有第三方依賴。更新入口 `C2DVideoQueue : IGameFrameworkListener`，`RegisterListener(... FRAMEWORKLISTENERPRIORITY_DEFAULT)`、`OnPostUpdate`（`O:Module/Video/2DVideoQueue.cpp:20-31,85-100`）；插件 `Initialize` 把 `sys_rendersplashscreen` 設 0（`O:Module/Plugin.cpp:151-158`）。

### 3.2 `O:Module/CMakeLists.txt`

- 啟用條件：`WINDOWS AND OPTION_ENGINE AND NOT OPTION_DEDICATED_SERVER`（1-7 行）；`CryEngineModule(CEVPlayer FORCE_SHARED_WIN PCH "StdAfx.cpp" SOLUTION_FOLDER "Project/Plugins")`（87 行）。
- 定位 `_CustomSDKs`：全部以 `${SDK_DIR}/_CustomSDKs/<lib>/…` 硬寫（93-111 行）。`SDK_DIR` 預設 `${CRYENGINE_DIR}/Code/SDKs`（`C:Tools/CMake/InitialSetup.cmake:31-33`，可由命令列 `-DSDK_DIR` 覆寫）。目錄約定：
  - `libvpx/includes` + `lib/libvpx.lib`
  - `libwebm/includes` + `lib/release/libwebm.lib`
  - `libogg/includes` + `lib/release/libogg_static.lib`
  - `libvorbis/includes` + `lib/release/libvorbis_static.lib`、`libvorbisfile_static.lib`
  - `libav/include` + `libav/bin/swscale.lib`、`avutil-54.dll`、`swscale-3.dll`
- 用 `include_directories`/`target_link_libraries(PRIVATE <絕對 .lib 路徑>)`；所有組態（含 Debug）都連 `release/` 的 lib（Debug 組態會有 CRT 不一致風險，舊倉用 `/ignore:4099 /ignore:4075 /ignore:4098` 與 `/wd4996 /wd4800` 壓警告，`CMakeLists.txt:82-83`）。
- `target_link_libraries(${THIS_PROJECT} PRIVATE SDL2)`（146 行）依賴引擎 `Tools/CMake/modules/SDL2.cmake` 的 imported target；`Configure.cmake:182-184`（`if(OPTION_ENGINE) if(NOT TARGET SDL2) include(.../SDL2.cmake)`）。
- 兩棵樹本機實際上都**沒有** `Code/SDKs/_CustomSDKs`（`C:Code/SDKs` 內無；`V:Code/SDKs` 尚不存在）；`CRYENGINE_v5.7.0_SDKs.zip` 內也沒有 `_CustomSDKs`/libav/libvpx（只有 `SDL2`、`SDL_mixer` 等）。

### 3.3 引擎其他部分有沒有用 Libav / swscale

- `V:Code/CryEngine`、`V:Code/CryPlugins`、`V:Tools/CMake` 對 `swscale|libav|avcodec|ffmpeg` 全無命中。
- `C:` 的 `Code/CryEngine`、`Code/Launcher`、`Code/CryPlugins`（除 CEVPlayer）也無；`Code/Sandbox` 僅 `.rc`/`Resource.h` 之類的字串雜訊（未逐一確認，不是函式庫使用）。
- 結論：Libav 只有 CEVPlayer 自己用。

### 3.4 SDL2 版本

- `V:Tools/CMake/modules/SDL2.cmake` 與 `C:` 版逐行相同：Windows 為 imported SHARED，`IMPORTED_LOCATION ${SDK_DIR}/SDL2/win_x64/bin/SDL2.dll`、`IMPORTED_IMPLIB .../lib/SDL2.lib`、`INTERFACE_INCLUDE_DIRECTORIES .../win_x64/include/SDL2`（所以程式用 `#include <SDL.h>`），並 `deploy_runtime_files` 複製 `SDL2.dll`。
- 版本：`C:Code/SDKs/SDL2/win_x64/include/SDL2/SDL_version.h:60-62` = **2.0.8**；下載的 `D:/coding11/_downloads/CRYENGINE_v5.7.0_SDKs.zip` 內同一路徑的 `SDL_version.h` 我另抽出來核對，也是 **2.0.8**。`V:Code/SDKs` 目前不存在（SDK 尚未解壓），因此 vanilla 預期的就是這份 2.0.8。
- 2.0.8 具備舊插件用到的所有 API（`SDL_OpenAudioDevice`、`SDL_MixAudioFormat`、`SDL_PauseAudioDevice`）。SDL2 的 `SDL_audio` 若 M2 改成走 CryAudio，這個依賴可整個移除（見 §7）。

---

## 4. CECE 與 vanilla 的 RenderDll 差異

做法：`diff -rq` 整個 `Code/CryEngine/RenderDll`，再逐檔 `diff -u`。**共 24 行差異，全部屬於 HPi 的 MeshBaker 與 RenderDoc 更新**：

| 檔案 | 差異 |
|---|---|
| `Common/RenderThread.h/.cpp` | 新增 `eRC_BakeMesh`（**加在 `ERenderCommand` 列舉最後**，在 `eRC_LambdaCall` 之後）、`RC_BakeMesh`、`ProcessCommands` 內 `case eRC_BakeMesh`。 |
| `Common/Renderer.h/.cpp` | 前置宣告 `CMeshBakerGraphicsPipeline`、空行。 |
| `XRenderD3D9/DriverD3D.h/.cpp` | `RT_BakeMesh`；`BeginRenderDocCapture/EndRenderDocCapture` 改用 RenderDoc in-app API 1.6（含新檔 `Common/renderdoc_app.h`）。 |
| `XRenderD3D9/GraphicsPipeline/Common/GraphicsPipeline.h/.cpp` | `EGraphicsPipelineStage` 在 `eStage_Count` 前新增 `eStage_MeshBaker`；虛擬函式 `BakeMesh`（預設回 false）。 |
| `XRenderD3D9/GraphicsPipeline/StandardGraphicsPipeline.h/.cpp` | `RegisterStage<CMeshBakerStage>()`、`BakeMesh` override。 |
| `XRenderD3D9/CMakeLists.txt` | 加入 `D3DMeshBaker.h`、`MeshBakerStage.*`、`MeshBakerPipeline/*`。 |
| 僅 CECE 有的檔案 | `D3DMeshBaker.h/_forCOP.cpp`、`GraphicsPipeline/MeshBakerStage.*`、`MeshBakerPipeline/`、`Common/MeshBakerPass*.{h,cpp}`（含 `_ORG`、`_new` 備份）、`Common/renderdoc_app.h`。 |
| `CryCommon/CryRenderer/IMeshBaking.h` | 有差異（111 行 diff，未細讀；MeshBaker 介面）。 |

重點確認：

- `CD3D9Renderer::EndFrame`（`DriverD3D.cpp:3207-3226`）、`RenderAux`（3086）、`RenderAux_RT`（3138）**兩棵樹相同**。
- Scaleform 繪製點（`RenderDll/Scaleform/*`、`D3DRenderThread.cpp` 的 `RT_FlashRenderInternal`、`StartLoadtimeFlashPlayback`）**兩棵樹相同**；`CryScaleform/` 目錄也相同。
- 貼圖上傳（`D3DTexture.cpp`、`Texture.cpp`）、渲染執行緒資源建立（`CreateDeviceTexture`、`ExecuteRenderThreadCommand`）**兩棵樹相同**。`IRenderer.h` 本身與 vanilla 相同（`CryRenderer` 只有 `IMeshBaking.h` 不同）。
- 影響 patch 的小地方：`ERenderCommand` 與 `EGraphicsPipelineStage` 在 CECE 尾端各多一項；若 patch 要在這兩個列舉尾端追加，兩樹的 hunk 上下文不同。
- 同時對 patch 01 相關檔案：
  - `Code/CryPlugins/CMakeLists.txt`：vanilla 結尾是 `add_subdirectory(CryHTTP/Module)`（無換行）；CECE 在後面多了 `add_subdirectory(CEVPlayer/Module)`，**無條件**（沒有 `EXISTS` 保護）。
  - `Tools/CMake/BuildSandbox.cmake`：CECE 新增 `PLUGIN_HPI_PLUGIN`、`PLUGIN_LOD_GENERATOR` 選項與 `AssimpSdk.cmake`、`HPiPlugin`、`LodGeneratorPlugin`；並把 vanilla 的 `if (PLUGIN_SCHEMATYC) add_subdirectory(.../EditorSchematyc2)` 換成 `if (PLUGIN_CRYVS) add_subdirectory(.../EditorCryVS)`（vanilla 47-49 行、CECE 53 行附近）。`EditorTrackView` 那行（vanilla 50 行）兩邊都在。
  - **vanilla 這份本機樹的檔案是 CRLF**（`file` 回報 "ASCII text, with CRLF line terminators"）。XTFrame 的 `patches/01-register-plugins.patch` 是以 LF 產生（基底 commit `cd017c4f`，`X:patches/BASE_COMMIT`），套到這棵 CRLF 樹時 hunk 內容行不相符；需要 `git apply --ignore-whitespace`（或先把目標檔 `unix2dos` 風格處理後再產 patch）。
  - 同一份 XTFrame patch 用 `if (OPTION_ENGINE AND EXISTS ".../CMakeLists.txt")` 與 `if (EXISTS ".../CMakeLists.txt")` 保護，沒有同步來源的樹也能 configure；CEVPlayer 的 patch 01 照做。**要同步到 CECE 時要避免重複 `add_subdirectory`**（CECE 已有無條件那一行）。
  - vanilla 這份本機樹的 `Code/CryEngine` 沒有 `CrySchematyc2` 實作，`PLUGIN_SCHEMATYC` 預設 OFF（`V:Tools/CMake/BuildEngine.cmake:4`），不要開。

---

## 5. CryVS（僅 CECE）

### 5.1 位置與 vanilla 狀況

- CECE：引擎 `C:Code/CryEngine/CryVS/`；公開標頭 `C:Code/CryEngine/CryCommon/CryVS/`（`IFramework.h`、`Env/IEnvRegistry.h`、`IActionFactory.h`、`ActionFactory.h`、`ComponentFactory.h`…）；編輯器 `C:Code/Sandbox/Plugins/EditorCryVS/`（`Editor/CryVSAssetType.cpp` 等）。
- 開關：`PLUGIN_CRYVS` 預設 ON（`C:Tools/CMake/BuildEngine.cmake:4,470`）；`SystemInit.cpp:1112-1135` `InitSchematyc` 載入 `CryVS` 引擎模組（`sys_SchematycPlugin` 0 或 1），晚於 `LoadProjectPlugins`（`C:CrySystem/SystemInit.cpp:3102-3103`，Schematyc 初始化在 3394 行）。
- **vanilla 沒有**：`V:Code/CryEngine/CryCommon/CryVS` 不存在、`V:Code/CryEngine` 沒有 `CryVS`/`CrySchematyc2` 目錄；只有 `CryCommon/CrySchematyc2/` 的標頭。
- 全域環境：CECE 把 `gEnv->pSchematyc2`（`Schematyc2::IFramework*`）改名為 `gEnv->pCryVS`（`CryVS::IFramework*`），位置相同（`C:CryCommon/CrySystem/ISystem.h:773`、`V:` 同行為 `pSchematyc2`）。`CSystem::DoFrame`/`Update` 呼叫 `pCryVS->PrePhysicsUpdate()/Update()`（`C:CrySystem/System.cpp:1497,2290`）。

### 5.2 外部插件如何註冊環境元素

- 註冊表：`CryVS::IFramework::GetEnvRegistry()`（`C:CryCommon/CryVS/IFramework.h:62`），介面 `IEnvRegistry`：`RegisterTypeDesc`、`RegisterGlobalFunction`、`RegisterComponentFactory`、`RegisterComponentMemberFunction`、`RegisterActionFactory`、`CreateFoundation`、`RegisterAbstractInterface`…（`C:CryCommon/CryVS/Env/IEnvRegistry.h:239-280`）。
- **`envRefresh` 信號是正確的重註冊點**：`SFrameworkSignals::envRefresh`（`IFramework.h:46-52`，`Signals()` 在 `IFramework.h:92`）。`CFramework::RefreshEnv()` 先 `m_libRegistry.Clear(); m_envRegistry.Clear();` 重註冊核心型別，**最後** `m_signals.envRefresh.Send()`（`C:CryVS/Framework.cpp:275-323`）。也就是每次 refresh 會清空整個註冊表，外部元素必須在這個信號裡重新註冊，否則 refresh 後就消失（Sandbox 內重整環境會觸發）。
- 官方做法（BaseEnv）：`gEnv->pCryVS->Signals().envRefresh.Connect(CryVS::EnvRefreshSignal::Delegate::FromMemberFunction<CBaseEnv,&CBaseEnv::Refresh>(*this), m_connectionScope)`；`Refresh()` 內 `CAutoRegistrar::Process()` 再 `GetEnvRegistry().Validate()`（`C:CryVS/BaseEnv/BaseEnv_BaseEnv.cpp:53,186-191`）。`CBaseEnv` 在 `CFramework` 初始化時建立並已做第一次 `Refresh()`（`Framework.cpp:119-125`、`BaseEnv_BaseEnv.cpp:46`）。
- 外部插件的順序：插件 `Initialize` 發生在 `LoadProjectPlugins`，早於 `InitSchematyc`（上述 3102/3394 行）→ `gEnv->pCryVS` 在插件 `Initialize` 時**還是 nullptr**。XTFrame 對 Schematyc 的處理是等系統事件（`ESYSTEM_EVENT_REGISTER_SCHEMATYC_ENV`、`ESYSTEM_EVENT_REGISTER_FLOWNODES`）再註冊（`X:docs/02-probe-results.md:167`；舊插件同理，`O:Module/Plugin.cpp:192`）。CryVS 沒有對應的 `REGISTER_CRYVS` 系統事件（`ISystem.h` 無）。建議：在 `ESYSTEM_EVENT_GAME_POST_INIT`（舊插件已在此初始化，`O:Module/Plugin.cpp:171`）檢查 `gEnv->pCryVS` 非空 → 立刻註冊一次，並連接 `envRefresh` 以便之後的 refresh 重註冊（`CScopedConnection`/connection scope 的型別見 `C:CryCommon/CryVS/TemplateUtils/TemplateUtils_Signalv2.h`，**未細讀**）。
- 動作工廠範例：`CRYVS_MAKE_COMPONENT_ACTION_FACTORY_SHARED(action, props, actionGUID, componentGUID)`（`C:CryCommon/CryVS/ActionFactory.h:15-16`）；`CEntityLightSwitchAction::Register()` 呼叫 `gEnv->pCryVS->GetEnvRegistry().RegisterActionFactory(pFactory)`（`C:CryVS/BaseEnv/Components/Actions/BaseEnv_EntityLightSwitchAction.cpp:60-69`）。

### 5.3 `IAction` 如何回報完成

- `IAction` 只有四個虛擬函式：`Init(const SActionParams&)`、`Start()`、`Stop()`、`Shutdown()`（`C:CryCommon/CryVS/Env/IEnvRegistry.h:143-151`）。**沒有「完成」回呼或回傳值**。`SActionParams` 帶 `IObject&`、`IComponent*`、`const IProperties&`（`IEnvRegistry.h:123-136`）。
- 範例 action（燈光開關、音訊觸發）都是即時完成或由外部 `Stop()`，沒有自行結束的通知。要做「播放完畢後讓圖往下走」只能：
  1. 用 `IActionMemberFunction`（`IEnvRegistry.h:154-199`，輸入/輸出綁定、`Call(IAction&, inputs, outputs)`）做輪詢式查詢（例如 `IsFinished` 輸出）；或
  2. 用 `CryVS::ISignal`（`IEnvRegistry.h` 的 `CreateSignal`）由 component 發出事件。
  我**沒有**追到腳本圖如何訂閱 signal / 如何對 action 做 await（`C:CryVS/Script/GraphNodes/` 未讀）；M4 前需要補查，標為未驗證。

---

## 6. 資產系統（Sandbox，兩棵樹）

兩棵樹的 `Code/Sandbox/Plugins/EditorCommon/AssetSystem` 只有 `Browser/AssetBrowser.cpp` 有差異，且只是縮排／空白（逐段檢視）。結論對兩棵樹皆成立。

- 註冊自訂型別：繼承 `CAssetType`（`V:Code/Sandbox/Plugins/EditorCommon/AssetSystem/AssetType.h:57-250`），必須覆寫 `GetTypeName()`（序列化名稱，不可改）、`GetUiTypeName()`、`GetFileExtension()`（小寫、無點、不可以 `.cryasset` 結尾、需全域唯一）；用 `DECLARE_ASSET_TYPE_DESC(type)`（`AssetType.h:253-255`）與 `REGISTER_ASSET_TYPE(type)`（`AssetType.h:256`，展開為 `REGISTER_CLASS_DESC`）。範例：`PrefabAssetType`（`V:Code/Sandbox/Plugins/PrefabAssetType/PrefabAssetType.h:8-31`、`.cpp:21`，CMake 用 `CryEditorPlugin(...)`，`CMakeLists.txt:16`）；CECE 的 `EditorCryVS/Editor/CryVSAssetType.cpp` 也是同樣模式。→ 需要一個 Sandbox 插件 DLL（`CryEditorPlugin`），免引擎 patch。
- 多檔案：`IEditableAsset::AddFile/SetFiles/AddWorkFile/SetWorkFiles/SetSourceFile`（`AssetType.h:23-41`）；`.cryasset` 是 XML：`AssetMetadata`（屬性 `version/type/guid/source/timestamp`）下有 `Files/File@path`、`WorkFiles/WorkFile@path`、`Details/Detail@name`、`Dependencies/Path`（`V:.../AssetSystem/Loader/Metadata.cpp:19-130`）。`IsImported()` 為 true 時走匯入（`Import(source, outDir)`），否則走 `CanBeCreated()/OnCreate()`（`AssetType.h:166-176,206-216`）。
- 自訂 metadata 欄位：`SetDetails(vector<pair<string,string>>)`（`AssetType.h:36`）寫入 `Details`；以 `GetDetails()` 回傳 `CItemModelAttribute*` 清單、`GetDetailValue()` 回傳值，Asset Browser 會顯示成可排序／過濾的欄位（`AssetType.h:133-134`）。`Details` 是純字串名值對，沒有結構化欄位。若要放影片資訊（解析度、長度、codec），放這裡。
- 縮圖與預覽：`HasThumbnail()/GenerateThumbnail()`、`CreatePreviewWidget()`（`AssetType.h:127-143`）。
- 內嵌 Asset Browser 並以型別過濾：`CAssetBrowser(const std::vector<CAssetType*>& assetTypes, bool bHideEngineFolder=false, QWidget* parent=nullptr)`（`V:.../AssetSystem/Browser/AssetBrowser.h:49-50`）；`GetSelectedAssets()`、`SelectAsset(path)`（58-62 行）。對話框版本 `CAssetBrowserDialog(const std::vector<string>& assetTypeNames, Mode, QWidget*)`（`Browser/AssetBrowserDialog.h:41`）。屬性編輯器的資源選擇器：`ar(Serialization::MakeResourceSelector(value, "AssetTypeName"), ...)`，`IsUsingGenericPropertyTreePicker()` 預設 true（`AssetType.h:197-201`）。

---

## 7. CryAudio（M3 背景，簡略）

- 公開 `CryAudio::IObject`（`V:CryCommon/CryAudio/IObject.h:20-150`）只有 `ExecuteTrigger`、`ExecuteTriggerWithCallbacks`、`StopTrigger`、`SetTransformation`、`SetParameter`、`SetSwitchState`、`SetEnvironment`… **全部是 setter，沒有任何 getter**。位置只存在系統側 `CObject::m_transformation`（`V:CryAudioSystem/Object.h:155`，`GetTransformation()` 在 89 行，非公開介面）與 impl 的 `Impl::IObject::GetTransformation()`（`V:CryAudioSystem/Common/IObject.h:53`，只給 impl 用）。
- 「播放中觸發的位置」：沒有現成查詢方式。能拿到的回報只有 `STriggerCallbackData` + `ESystemEvents`（`V:CryCommon/CryAudio/IAudioSystem.h:62-80,215-229`）：`TriggerExecuted`、`TriggerFinished`、`OnBar/OnBeat/...`（後者僅限支援的中介軟體）。這些都沒有位置或播放進度。呼叫端自己設定 `SetTransformation` 時當然知道位置（插件可自己記）。
- SDL_mixer 實作（`V:CryAudioSystem/implementations/CryAudioImplSDLMixer/`）：`Impl.cpp`、`Event*.cpp`、`Object.cpp`、`GlobalData.h` 等。
  - 資產資料夾：`<game>/audio/sdlmixer/assets/`（`CRY_AUDIO_DATA_ROOT="audio"`，`IAudioInterfacesCommonData.h:13`；`g_szImplFolderName="sdlmixer"`，`GlobalData.h:14`；`g_szAssetsFolderName="assets"`，`IAudioInterfacesCommonData.h:98`；組合在 `Impl.cpp:48`）；有本地化版本。
  - 掃描方式：`LoadMetadata` 以 `ICryPak::FindFirst(rootDir + path + "*.*")` 遞迴子目錄，副檔名比對 `g_supportedExtensions`（`Impl.cpp:80-125`）。副檔名表（`GlobalData.h:59-71…`）：`ogg, wav, mp3, opus, flac, mod, aiff, xm, mid, voc, 669, …`（表尾未全讀）。`Mix_Init` 旗標 `FLAC|MP3|OGG|MID|OPUS|MOD`（`Impl.cpp:49`）；`Mix_Init` 在 `Impl.cpp:462`，取樣率 48000、緩衝 4096。
  - 事件 XML：`Event`/`Sample` tag、`path`、`volume`、`loop_count`、`fade_in_time`、`attenuation_*`（`GlobalData.h:17-33`）。
  - 引擎初始化 SDL：`SDL_Init(SDL_INIT_AUDIO)`（`Impl.cpp:460`），`SDL_Quit()`（522）。
- 尾端追加虛擬函式：`IObject`（公開）的實作者只有 `CryAudioSystem` 內的 `CObject`（`Object.h:136` 為 `override`）；在 `IObject.h` 最後一個虛擬函式之後追加一個如 `GetTransformation()`/`GetTriggerPlaybackInfo()` 的虛擬並於 `CObject` 實作，既有 slot 不位移，已編好的使用者模組（用舊標頭）不受影響，除非它自己實作了 `IObject`（沒有）。需要重建並部署 `CryAudioSystem.dll`（與 XTFrame 追加 `IRenderer` vtable 尾端的守則相同，`X:docs/02-probe-results.md:320`）。「播放進度」本身要由 impl（SDL_mixer/FMOD/Wwise/…）提供，SDL_mixer 的 `CEventInstance`（`EventInstance.h`）目前不追蹤播放位置，需另外實作 `Mix_` 層的計時，屬較大改動。M3 之前不要承諾。

---

## 建議

### M1 渲染做法

1. **貼圖**：每個播放器在 Open 時以 `UploadToVideoMemory(nullptr, w, h, eTF_R8G8B8A8, eTF_R8G8B8A8, 0, false, FILTER_BILINEAR, 0, "$CEV_<id>", FT_NOMIPS)` 建一次（命名可被 `.mtl` 與 Flash 引用；內部自動 `FT_DONT_STREAM`；不加 `FT_DONT_RELEASE`）。關閉時 `RemoveTexture`。解析度變更才重建（會 `FlushAndWait`）。
2. **解碼與轉換**：解碼執行緒用 libvpx 出 I420，**自己轉 RGBA**（取代 swscale，連 libav 一併移除），寫入 N 張 RGBA 環形緩衝；主執行緒依播放時鐘挑最新可顯示的一張，只在「有新影格」時呼叫一次 `UpdateTextureInVideoMemory(id, rgba, 0, 0, w, h, eTF_R8G8B8A8)`。**所有渲染 API 只在主執行緒呼叫。**
3. **畫到哪**：
   - 世界內／材質：`.mtl` 貼圖路徑寫 `$CEV_<id>`；Flash：`LoadTexIntoMc`。
   - 2D 螢幕覆蓋（stage 1）：aux 2D 影像／四邊形（`IRenderAuxImage::Draw2dImage` 或 XTFrame 同款 `BeginDrawBuffer`），在更新點內提交。**接受兩項限制並寫入文件：載入畫面不畫、疊放順序低於 Scaleform。**
   - 之後如要「載入畫面影片」或「疊在 Flash 之上」，走 patch（追加在 `IRenderer` vtable 尾端的 `EndFrame` hook，仿 XTFrame patch 02/07：`DriverD3D.cpp` `EndFrame` 在 `RenderAux()` 前一個掛鉤，`X:docs/02-probe-results.md:322`），那是 M3 以後的決定，不是 M1。
4. **GPU YUV**：M1 不做；材質路線的 3×`eTF_R8` + 散檔 `.cfx` 是已有先例的備案（§1e）。
5. **patch 01**：只做 CMake 註冊，以 `EXISTS` 保護；對 vanilla 的 CRLF 檔案要用忽略空白的套法；對 CECE 要避免重複（CECE 已含無條件 `add_subdirectory(CEVPlayer/Module)`）。

### M1 更新掛點

- **主更新點：`IEnginePlugin::EnableUpdate(EUpdateStep::BeforeRender, true)` → `UpdateBeforeRender()`**（`V:CryCommon/CrySystem/ICryPlugin.h:25-35,62-64`；呼叫點 `V:CrySystem/System.cpp:1651`，在 `Render()` 之前）。理由：與 XTFrame 同一點（已有 Sandbox/遊戲模式的設計考量與驗證脈絡，`X:docs/01-stage1-design.md:35-36`）；aux 命令在同一幀的 `EndFrame` 提交；不依賴 `CryAction` 的 `IGameFrameworkListener`（舊插件用 `OnPostUpdate`，它在 `Render()` 之後，畫的是下一幀，且與 CryAction 耦合）。`CSystem::DoFrame` 內的呼叫有 `!(updateFlags & ESYSUPDATE_EDITOR)` 保護（`System.cpp:1649-1652`），但 Sandbox 編輯模式由 `CGameEngine::Update` 自己依序呼叫 `UpdateBeforeSystem/AfterSystem/BeforeFinalizeCamera/BeforeRender/AfterRender/AfterRenderSubmit`（`V:Code/Sandbox/EditorQt/GameEngine.cpp:1529-1563`，XTFrame 同樣引用此處，`X:docs/00-environment.md` 末段）。這些在 Sandbox 實機是否每幀被呼叫（編輯、模擬、game mode）XTFrame 的 T1 探針也標「待測」，**未實機驗證**；game mode 的面板驅動見 `X:docs/08-xtframe-integration-log.md:1097-1100`。
- **載入期**：同步載入（預設）**沒有每幀更新點**。M1 的決策建議：
  1. 在 `ISystemEventListener` 收 `ESYSTEM_EVENT_LEVEL_LOAD_START_LOADINGSCREEN`／`LEVEL_LOAD_START` 時：暫停影片時鐘（或讓它繼續讀檔解碼，但不上傳、不繪製），`LEVEL_LOAD_END` 或 `LEVEL_GAMEPLAY_START` 時恢復並以實際時間重新對時（避免跳播）。這是舊插件「載入時崩潰」的直接修正：載入期不做任何渲染 API 呼叫。
  2. 要在載入期間繼續播放：記錄為「需要 `g_asynclevelload=1` 或 `g_levelLoadTimeSliced=1`」的選項（此時 `UpdateBeforeRender` 照常被呼叫），並先在實機驗證（§2.3 所列未驗證點）。
  3. 載入畫面影片（同步載入）→ 需要 `ILoadtimeCallback`（單一槽）+ Flash 影片載入畫面，或 patch；留給後續里程碑。
- 不要依賴：`ILoadingProgressListener`（唯一槽、1 秒一次）、`OnPostUpdate`、`ILevelSystemListener::OnLoadingProgress`（1 秒一次）。

### 其他風險備忘

- 舊插件 `SDL_Quit()` 會連引擎 SDL 一起關（§3.1）；M2 改走 CryAudio 時一併消除。
- CMake 的 `_CustomSDKs` 目錄在兩棵樹都不存在，M0 要自己備；SDL2 用引擎的 2.0.8 即可。
- 移除 libav 後 `avutil-54.dll`、`swscale-3.dll` 不再需要部署。
- 本文件所有「未實機驗證」項目（色彩空間、aux 排序、非同步／分時載入下的行為、`RegisterSyncWithMainListener` 時機、CryVS 的 action 完成機制）都需要在 M1/M4 的實機測試中補上。
