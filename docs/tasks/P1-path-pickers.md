# P1 · 所有填檔案路徑的地方改用選擇器（2026-11-26）

模型：Opus。倉 `E:/coding crycry/CEVPlayer-rebuild-integration`，分支 `xui2dl`，提交但**不推送**。

使用者要求：「所有填檔案路徑的地方，請使用檔案選擇器，不要弄一般欄位」。

## 現況（主模型已查）

已有選擇器，不動：Schematyc 元件的 `Video`（`VideoFileName`）、CryVS 元件的 `video`（`Serialization::ResourceFilePath`）、偏好設定頁的 FFmpeg 路徑（`yasli::FileOpen`）、匯入對話框的音訊觸發器。

仍是一般文字欄位，這張單要改：

| # | 位置 | 欄位 | 要的選擇器 |
|---|---|---|---|
| 1 | `Module/Bindings/FlowApiNodes.cpp` 四個節點（`PlayOnScreen`、`PlayOnMaterial`、`PlayToSurface`、`Advanced:Open`） | `Video` | 檔案選擇器（`.webm`、`.cevvideo`） |
| 2 | 同檔 `PlayOnMaterial`、`Advanced:Open` | `Material` | 材質選擇器 |
| 3 | `Module/Components/VideoPlayerComponent.cpp` | `m_material` | 材質選擇器（Schematyc 既有的材質資源型別） |
| 4 | `Module/Bindings/CryVSEnv.cpp` | `material` | 材質選擇器（引擎 Serialization 既有的材質裝飾器） |
| 5 | Sandbox 插件 `Code/Sandbox/Plugins/EditorCEVPlayer` 全部對話框與屬性樹 | 任何讓使用者手打檔案或資料夾路徑的欄位 | 先清查（`TranscodeDialog` 的 `m_pOther` 等逐一判斷是不是路徑）；是路徑就加選擇器 |

不是檔案路徑，不改：`SurfaceID`、`UIElement`、`MovieClip`、`AudioTrack`、資產名稱、主控台指令的參數。

## 做法的規定

- **FlowGraph**：先讀引擎的 FlowGraph 編輯器原始碼（社群版樹 `E:/coding crycry/CryENGINE-5-7-community` 的 Sandbox 裡處理 port 資料型別的那段：port 名稱前綴與 `_UICONFIG("dt=...")` 的對照），確認哪種寫法會給出檔案／材質選擇器。
  - 優先用 `_UICONFIG("dt=...")`，**port 名稱不變**（既有的圖不壞）。
  - 若引擎只認名稱前綴：改用前綴名並以 `humanName` 顯示原名；這會讓已存的圖斷線，報告裡要明寫，README／CHANGELOG 加一行。
  - 檔案選擇器若能限定副檔名就限定 `.webm;.cevvideo`；不能就用通用檔案選擇器，報告說明。
  - 選出的路徑格式必須是播放器現在吃的格式（相對於遊戲資料夾）；若選擇器回傳的形式不同，在節點讀值處正規化，不改播放器。
- **Schematyc／CryVS 的材質**：用引擎既有的型別／裝飾器，不自寫選擇器。序列化後的字串必須與現有存檔相容（舊值照樣讀得出來）；`m_material` 的成員 id `'mtl_'` 與名稱不變。型別換了若使 Schematyc 的存檔不相容，停下回報，不要硬改。
- **Sandbox 插件**：用引擎既有的對話框（`CSystemFileDialog`／`CEngineFileDialog`，本插件 `FfmpegService.cpp` 已有用法）。欄位旁加瀏覽鈕即可，仍可手打。
- 不加新的 cvar、不改介面版號、不改 `ApiTable` 的條目數（自測數字要維持 72／69／248，cryvs 108；若因合理原因變動，照實寫）。
- 文件：README 提到這些 port／屬性的地方若描述為「輸入路徑」則同步改一句；CHANGELOG 的 Unreleased 加一行。

## 驗證

1. 社群版樹建置 `CEVPlayer`（Profile），0 錯誤、警告照實列；換入後跑 `cev_apitest`、`cev_flowtest`、`cev_contract_selftest`、`cev_cryvstest`。啟動規則同前幾張單（`+sys_assert_dialogues 0 +sys_no_crash_dialog 1`、`+cev_run_script`、Popen＋親和性 0xFFF＋逾時、腳本不以 `quit` 結尾、樹忙碌檢查不得繞過）。
2. 舊圖相容：用一張改動前的 FlowGraph（倉內或 `E:/coding3/xt-bench/cev-*/` 的測試專案裡現成的）確認 `Video`／`Material` 的值仍讀得到。
3. Sandbox 插件有改動時：建置 `EditorCEVPlayer`（哪棵樹能建就在哪建；建不了就標「未編譯」）。
4. **選擇器的實際畫面**：能在 Sandbox 裡開起來看就看並截圖；做不到（無人值守、螢幕關閉）就標「未以畫面驗證，只有讀碼」，不要寫成已驗證。
5. 共用樹 `CRYENGINE_Source_CEVPlayer_XTFrame` 這張單**不碰**。

## 規則

不 `git add -A`、不 `git stash`；提交訊息英文，前綴 `feat:`，結尾 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。證據放 `E:/coding3/xt-bench/cev-p1/`。GameSDK 範例專案唯讀。

報告（繁體中文、精簡，寫在最後訊息裡）：每一列改成什麼寫法與依據（引擎原始碼的檔名行號）；清查第 5 列找到的欄位清單與各自的處理；舊圖／舊存檔是否相容；建置與自測結果；哪些只有讀碼沒有畫面驗證；提交雜湊。

## P1b · 節點函式的路徑參數（2026-11-26）

模型：Opus。P1 已完成（`1003bd1`）。使用者說明：沒有已存的圖要保護，「弄好就對了」——相容性不是限制，每個填路徑的地方都要有選擇器。P1 留下兩處：

| # | 位置 | 要求 |
|---|---|---|
| 1 | CryVS 全域函式（`CryVSEnv.cpp`：`PlayOnScreen`、`PlayOnMaterial`、`UI::PlayToSurface`、`Advanced::Open`）的 `Video` 與 `Material` 輸入 | `Material` 用引擎的 `CryVS::SMaterialFileName`；`Video` 註冊一個本插件的 CryVS 檔案型別（選擇器限 `.webm;.cevvideo`，起始資料夾 `Videos`，做法比照元件屬性已用的 `Serialization::ResourceFilePath`），節點上未接線時的預設值欄位要出現選擇器 |
| 2 | Schematyc：由 `ApiTable` 產生的函式中接影片路徑或材質名稱的參數（如 Load／Open 類） | 參數型別改用既有的 `VideoFileName` 與 `Schematyc::MaterialFileName` |
| 3 | CryVS 元件的成員函式若有同類參數 | 同 1 |

- 綁定表（`ApiTable`）若需要新的參數種類（例如「影片路徑」「材質名稱」）就加，FlowGraph 那邊的行為不變。介面版號不動。
- 自測數字若因型別註冊而改變，更新期望值並在報告寫明前後數字與原因。
- 建置、啟動、提交規則與 P1 相同（社群版樹；共用樹不碰；不推送）。畫面沒看過就寫「只有讀碼」。
- README／CHANGELOG 同步一句。

報告：每一處的改法與引擎依據（檔名行號）、自測前後數字、只有讀碼的部分、提交雜湊、仍然沒有選擇器的路徑欄位（若有，逐一列出原因）。
