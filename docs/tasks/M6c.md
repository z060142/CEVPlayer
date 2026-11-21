# M6c · CryVS 節點：三個抽屜、全域函式、多輸出、元件訊號；在社群版引擎實跑

對應：`docs/design/M6-ui-source.md` §4.2（CryVS 一段與 10 個節點的表）、`docs/research/06-community-baseline.md`（社群版樹怎麼建、兩個要靠設定繞開的 fork 問題）。前置：M6a、M6b 已併入（描述表 47 項、`CreateForSurface`、`AttachSurface`、`SetDecode`…）。模型：Opus。工作樹 `E:/coding crycry/cev-wt-m6c`，分支 `m6c`。引擎樹 `E:/coding crycry/CryENGINE-5-7-community`（已建置 Profile；其 CEVPlayer 複本是 M6a 之前的版本——先同步成你的工作樹內容再建）。**不合併、不推送；不改 fork 的引擎原始碼。**

## 已知的 CryVS 事實（2026-11-19 調查；動手前以程式核對，不符就停下回報）

- CryVS＝Schematyc2 改名。`IEnvRegistry`（`CryCommon/CryVS/Env/IEnvRegistry.h`）可登錄：全域函式（`IGlobalFunction`，可 `SetNamespace`、`BindOutput` 多個輸出——非 const 參考參數；範例 `BaseEnv/GlobalFunctions/BaseEnv_TypeFunctions.cpp`）、元件與其成員函式、訊號、action、型別。
- 節點目錄的路徑：第一層固定（`Function::`、`Condition::`…），全域函式是 `Function::<Namespace>::<Name>`；元件成員函式的第二層是腳本裡的元件實例名（不能固定成抽屜名）。函式節點同步、進一出一；多個執行出口不存在；事件只能是訊號，訊號要有元件實例（`SenderGUID`）才收得到。
- 現行 `Module/Bindings/CryVSEnv.cpp`：一個元件 `CEVPlayer::VideoPlayer`（GUID 固定）、表內每項一個成員函式（單一回傳）、每個事件一個訊號；在 `ESYSTEM_EVENT_GAME_POST_INIT` 登錄並連上 `envRefresh`。風險：(a) 腳本可能比登錄早載入而把元素當成壞參照；(b) 關閉時不反登錄；(c) 以「元件已存在就整段跳過」防重複。M6b 已照新描述表改過這個檔但**沒有編譯過**。

## 要做的事

1. **先讓現況在社群版樹編過並載入**（M6b 改過的 `CryVSEnv.cpp`），再往下做。
2. **三個抽屜＝三個 namespace 的全域函式**（handle 式，內部只呼叫描述表的函式）：
   - `Video`：`PlayOnScreen(Video, Fit, Loop, Skippable, Decode) → Handle`、`PlayOnMaterial(Entity, Video, Material, Slot, SubMaterial, Loop, Autoplay, Decode) → Handle`、`Stop(Handle)`。
   - `VideoUI`：`PlayToSurface(Video, SurfaceID, Loop, Autoplay, Decode) → Handle`、`StopSurface(SurfaceID)`、`SurfaceInfo(SurfaceID) → Exists, TextureId, Width, Height, PixelLayout, Premultiplied, HasAlpha, FrameSerial, State`（一個節點多個輸出）。
   - `VideoAdvanced`：`Open(Video, Output, SurfaceID, Fit, Material, Slot, SubMaterial, Decode) → Handle`、`Close(Handle)`、`Play`／`Pause`／`Stop`／`Seek(Handle, Seconds)`、`SetLoop`／`SetRate`／`SetVolume`／`SetLanguage`／`SetVariant`／`SetSkippable`（各自一個函式——CryVS 表達不了「哪個埠被觸發就套用哪一項」）、`Info(Handle) → State, Time, Duration, Width, Height, HasAlpha, Language, Variant, DecodeBackend, FrameSerial, SurfaceId`。
   - 名詞與 FlowGraph 一致（SurfaceID、Decode、Output…）；列舉（Fit、Output、Decode）以 CryVS 的型別登錄，不讓作者填數字。每個函式固定 GUID（新的；寫進描述表旁的固定 GUID 清單）。以這些函式建立的播放器屬於「腳本擁有」：關卡卸載與離開遊戲模式時由既有的 `DestroyScriptOwned` 收掉。
3. **元件 `VideoPlayer` 保留**，負責事件：屬性照 M6b 的名詞（Video、Output、SurfaceID、Material、Slot、Fit、Language、Loop、Autoplay、Skippable、Decode）；新增屬性 `WatchSurfaceID`——非空時元件不擁有播放器，只訂閱那個 surface（用 `AttachSurface`），訊號為 Ready、NewFrame、Resized、Closed（對應 FlowGraph 的 `VideoUI:SurfaceEvents`）；為空時照舊擁有自己的播放器並送出八個事件訊號。元件既有的成員函式維持（GUID 不變）。
4. **登錄的三個風險**：(a) 查清楚腳本載入／`RefreshFiles(EditorFixUp)` 與 `GAME_POST_INIT` 的先後（讀碼並以 log 佐證）；若腳本會先載入，把登錄提前到 `gEnv->pCryVS` 可用的最早時點（`CSystem::InitSchematyc` 之後插件能得到的第一個事件），並保留 `envRefresh` 的重新登錄。(b) 關閉時反登錄（若登錄表沒有移除的 API，說明並確認關機順序下不會留下指向已卸載 DLL 的物件——以實跑關機無崩潰為準）。(c) 防重複改為逐元素判斷。
5. **自測 `cev_cryvstest`**（只在 `CEV_HAVE_CRYVS` 下存在）：從 `IEnvRegistry` 以 GUID 取出每一個登錄的全域函式與元件成員函式，檢查名稱、namespace、輸入輸出的數量與型別；實際呼叫 `VideoUI::PlayToSurface` → `SurfaceInfo`（多輸出的值與描述表查詢一致）→ `StopSurface`，`VideoAdvanced::Open`／`Info`／`Close`；列出登錄的訊號。把所有登錄元素（完整路徑）寫到 log，作為「三個抽屜存在」的證據。
6. **實跑一個 CryVS 腳本**（盡力而為，照實回報）：在社群版 Sandbox 的測試專案建立一個使用上述節點的最小腳本類別（元件＋`VideoUI::PlayToSurface`＋Ready 訊號裡寫一行 log），放到關卡裡的實體上，進遊戲模式執行並以 log 佐證。CryVS 腳本檔若能手寫／由既有範例改寫就手寫；必須用編輯器介面才能建立時，說明卡在哪一步並提供給人照做的步驟，不要假裝驗過。

## 規則

- 同步與建置：照 `docs/research/06-community-baseline.md`；引擎樹的 `Code/SDKs` 是真目錄＋對 `E:/coding2/.../Code/SDKs` 各子目錄的連結——**連結目標唯讀**，`sync_sources.sh` 若會刪 SDK 目錄，改成只複製改過的檔。只增量建 `CEVPlayer`（必要時 `EditorCEVPlayer`）；不重建其他引擎目標。測試專案沿用 `E:/coding3/xt-bench/cev-m6c/project/`（`sys_SchematycPlugin=0`、Sandbox 需 `sys_asserts=0`——fork 的兩個既有問題）。
- 驅動測試用 `cev_run_script <file>`（M6b 新增），不用四行以上的 exec cfg；`sys_assert_dialogues 0`；不用 `sys_log_asserts 1`。log 裡的斷言逐一列出並判斷。失敗或卡住的執行不得當作通過。
- Popen＋`SetProcessAffinityMask(0xFFF)`、每個指令有逾時、背景指令 10 分鐘沒輸出就檢查並改前景；不用 `cmd /c start … > log`。只關自己啟動的行程（以完整路徑分辨）。最多 8 個平行建置工作。暫存放 `T:/cev/m6c/`（做完刪），證據放 `E:/coding3/xt-bench/cev-m6c/`。
- 不 `git add -A`、不 `git stash`、不推送、不合併；不動 `E:\\coding2`、`E:\\coding4`、CEVPlayer 主樹；社群版樹只改同步進去的 CEVPlayer 檔與建置產物，不提交。
- 程式、註解、提交訊息英文；提交帶 `M6c`、結尾 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。
- 報告（繁體中文、精簡）：登錄的元素清單（完整路徑）、每項測試的結果與證據、三個風險各自的結論、腳本實跑做到哪裡、與本單的偏離、沒做到與沒測到的、社群版樹最後的狀態。
