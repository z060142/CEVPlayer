# M6b · 刪除舊節點與相容層；FlowGraph 整併為三個抽屜 10 個節點；Schematyc 名詞對齊

對應：`docs/design/M6-ui-source.md` §4.2（表格是規格）、§4.1、§1（使用者的決定：舊節點全部刪除、不留別名）。前置：M6a 已併入（Surface ID 登錄表、`CreatePlayerForSurface`、`GetSurfaceId`／`GetGeneration`／`GetPixelLayout`／`GetDecodeBackend`、`cev_play_surface`）。模型：Opus。工作樹 `E:/coding crycry/cev-wt-m6b`，分支 `m6b`。引擎樹 `E:/coding crycry/CRYENGINE_Source_CEVPlayer_XTFrame`。**不合併、不推送。**

動手前以程式核對本單每一條前提；不符就停下回報。先寫會失敗的測試。**CryVS（`Module/Bindings/CryVSEnv.*`）不在本單**——只做到「描述表的改動不讓它編不過」（社群版樹由另一張單處理）；Schematyc 元件要做。

## 要做的事

1. **刪除，不留別名**：`Module/FlowNodes/FlowNodes.cpp` 的舊節點、`Module/Components/TextureVideoPlayerComponent.*`、`Module/Compat/`（`LegacyPlayback`）、所有 `vp_*` 主控台指令與變數（含 `vp_audio_volume` 參與的音量相乘、`vp_debug_draw`）、`PlayVideoToTexture` 等舊進入點、`OutTextureName`／`OutputTextureName`。連帶清掉：`Plugin.cpp` 的舊事件處理、`mEnv->pLegacy`、CMake 清單、文件與 README 的對應段落（README 的指令表）。`IVideoPlugin::CreatePlayer(textureName)` 保留（C++ 使用者），但 FlowGraph／Schematyc 不再有任何「貼圖名」輸入。刪除後 `git grep -n "vp_\\|OutTextureName\\|OutputTextureName\\|LegacyPlayback"` 在 `Code/` 應為零（註解與文件裡的歷史說明除外，逐一確認）。
2. **描述表**（`Module/Bindings/ApiTable.*`）維持單一事實來源、一項一個函式：
   - 新增項目：`CreateForSurface(SurfaceId)`（回 Handle）、`SurfaceId`、`Generation`、`PixelLayout`、`DecodeBackend`、`FrameSerial`（字串）、`IsLoaded`、`IsLooping`、`HasAudio`、`Rate`、`Volume`、`Path` 查詢；`SetDecode(Mode)`（0 Auto、1 GPU、2 CPU——本單只記錄設定值，實際後端恆為 CPU；選 GPU 時記一次警告「hardware decode is not available in this build」）；`UnbindMaterial`。
   - 刪除項目：`TextureName` 查詢、`InterfaceVersion`、`Caps`（改由新主控台指令 `cev_info` 印出版本、能力位元、契約版本、已註冊的 surface 數）。`Create` 的 `TextureName` 參數移除（`Create` 產生沒有 ID 的播放器）。`SetOutput` 的 Texture 模式改名為 Surface 模式並移除「Name 僅供參考」的行為：Surface 模式只對以 `CreateForSurface` 建立的播放器有效。
   - 既有項目的 GUID 不得改；新增項目用新的固定 GUID；刪除項目的 GUID 不再使用（在表頭註解留下「已退役」清單，避免重用）。
   - 事件 `NewFrame` 的 `Serial` 改為字串埠（十進位、不回繞）。
3. **FlowGraph**（`Module/Bindings/FlowApiNodes.cpp` 或新檔）：不再「一項一個節點」。照 §4.2 的表做 10 個手寫描述的複合節點，內部只呼叫描述表的函式（不直接碰 `IVideoPlayer`）：
   - `Video:PlayOnScreen`、`Video:PlayOnMaterial`、`VideoUI:PlayToSurface`：節點自己擁有播放器（Play 時建立並載入、Stop 或節點／圖銷毀時 Destroy；重複 Play 視為從頭播）；事件輸出只轉送自己播放器的事件。`PlayToSurface` 的 Ready 帶 TextureId、Width、Height、Duration、HasAlpha。`SurfaceID` 已被占用時觸發 Error（代碼與文字）。
   - `VideoUI:SurfaceInfo`（輸入 SurfaceID 與 Get；`Exists`＝false 時其他輸出為 0／空）、`VideoUI:SurfaceEvents`（以 SurfaceID 訂閱：Ready、NewFrame、Resized、Closed——surface 還不存在時先掛著，出現後開始轉送）。
   - `VideoAdvanced:Open`（Open／Close → Handle、Ready、Error；Output 模式 Surface／Screen／Material 與其參數、Decode）、`Transport`（Play、Pause、Stop、Seek＋Seconds；Play 兼作繼續）、`Settings`（Loop、Rate、Volume、Language、Variant、Skippable——哪個埠被觸發就套用哪一項）、`Info`（Get → 表列輸出）、`Events`（Handle → 八個事件）。
   - 類別（抽屜）名稱正好是 `Video`、`VideoUI`、`VideoAdvanced`；舊的 `Video:Control:*`／`Video:Query:*`／`Video:Events` 不再註冊。節點的埠名、說明文字英文；每個埠有一句說明。
   - 節點設定頁上的列舉（Fit、Output、Decode）用 FlowGraph 的 enum UI 設定（`_ucs`／`enum_int` 之類，照引擎現有節點的寫法），不要讓使用者填數字。
4. **Schematyc 元件 `Video Player`**（`Module/Components/VideoPlayerComponent.*`）：屬性改為 Video、Output（Surface／Screen／Material）、SurfaceID（Output＝Surface 時使用；取代原本兼作貼圖名的 `Name`）、Material 與 Slot（Output＝Material）、Fit、Language、Loop、Autoplay、Skippable、Decode；函式與訊號照描述表的新內容。元件的 GUID 不變；屬性改名造成既有 Schematyc 腳本的舊屬性值遺失是可接受的（使用者已同意不留別名），在報告列出。
5. **自測**：`cev_apitest` 更新為涵蓋新表（項目數會變——報告新數字）；新增 `cev_flowtest`（或併入 apitest）：以程式建立每一種節點並觸發其輸入埠（照引擎的 `IFlowGraph` API 在記憶體建一張圖），驗證 `PlayToSurface` → Ready 的輸出值、`SurfaceInfo` 的輸出、`SurfaceEvents` 的先掛後到、`Settings` 的逐埠套用、`Transport` 的四個動作、圖銷毀時播放器被銷毀（以 `cev_surfaces` 的計數為準）。做不到以程式建圖時，改以最小的 `.xml` 流程圖檔在測試關卡載入，並說明。
6. `cev_contract_selftest`（46 項）、`tools/smoke_test.sh` 仍過（smoke 腳本若用到被刪的指令，改用 `cev_play`）。`docs/research/05b-engine.md` 提到的開發用指令保留。

## 規則

- 引擎樹：只複製改過與新增的檔進去（刪除的檔也要在引擎樹刪掉對應複本——只限 `Code/CryPlugins/CEVPlayer` 內）；不要讓 SDK 目錄被刪掉重抄；新增／刪除原始檔後以 `Tools/Integration/Build.ps1 -ConfigureOnly` 重產方案，只增量建 `CEVPlayer`（`EditorCEVPlayer` 若因介面改動編不過，一併增量建並回報）；不重建渲染器、不改引擎與 CryXTUI。
- 啟動 GameLauncher 一律帶 `sys_assert_dialogues 0`（不要 `sys_log_asserts 1`）；log 裡出現的任何斷言都要列在報告並判斷是否與本單有關。**這棵樹有既存的啟動不穩定**（約半數啟動在 CryXTUI 輸入註冊前後出現 `CryString.h:734` 斷言、卡住或存取違規，未改動的建置也會——另有調查在進行）：測試專案的插件清單**不要載入 CryXTUI**（本單不需要它），以排除干擾；若仍遇到，照實記錄並重跑，不得把失敗當作通過。關機前不留開著的 surface。
- Popen＋`SetProcessAffinityMask(0xFFF)`、每個指令有逾時、背景指令 10 分鐘沒輸出就檢查並改前景；不用 `cmd /c start … > log`。只關自己啟動的行程（以完整路徑分辨；另一棵引擎樹可能有別的代理在跑）。
- 獨立建置的中間檔放 `T:/cev/m6b/`，做完刪；證據放 `E:/coding3/xt-bench/cev-m6b/`。最多 8 個平行建置工作。不 `git add -A`、不 `git stash`、不推送、不合併；不動 `E:\\coding2`、`E:\\coding4`、主樹。
- 程式、註解、提交訊息英文；提交帶 `M6b`、結尾 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。
- 報告（繁體中文、精簡）：刪了什麼（檔案與指令清單）、10 個節點各自的埠表、描述表新增／退役的項目、每項測試的結果與證據、既有腳本／流程圖會失效的地方、與本單的偏離、沒做到與沒測到的、引擎樹最後的狀態。
