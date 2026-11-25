# L1b · XUI2DL 核心：延後建立資源，並在社群版樹建置與驗證（2026-11-26）

模型：Opus。倉 `E:/coding crycry/CEVPlayer-rebuild-integration`，分支 `xui2dl`（L1 第一段已提交：`ae1153a`、`3ffb53a`），**不推送**。這張單取代 `docs/tasks/L1.md` 的「驗證」一節：驗證的樹從共用樹改為**社群版樹** `E:/coding crycry/CryENGINE-5-7-community`（沒有 XTFrame，正是 XUI2DL 的使用情境）。先讀 `docs/tasks/L1.md`（範圍、實作要求、規則仍然有效）與 `docs/design/M11-xui2dl.md` §3～§5。

**完全不碰共用樹** `E:/coding crycry/CRYENGINE_Source_CEVPlayer_XTFrame`（另一個 session 正在那裡建置；唯讀參考可以）。

## 1. 程式修改（一個提交）

`EnginePatch/Code/CryEngine/RenderDll/XUI2DL/XUI2DL.cpp`：資源改為延後建立（M11 §5「資源」）。

- `XUI2DL::RT_CreateResources()` 不再建立任何資源，只記下「裝置可用」。
- 渲染執行緒第一次要畫非空快照、且資源尚未建立時才建立（著色器、頂點格式、常數緩衝、預熱）；建立當幀著色器未就緒就略過並計數，之後照常。
- `RT_ReleaseResources()` 照舊釋放並丟掉目前快照，並回到「未建立」狀態。
- `GetStats().resourcesReady` 反映實際是否已建立。
- 從未收到非空 `SetFrame` 的執行，全程不得建立任何著色器或緩衝（以 log 與 `GetStats` 驗證）。

第一段回報的八點處理方式，除下列外全部照代理的做法定案：`--remove` 維持只移除掛點；探測指令的可選 `[surface]` 參數寫進指令說明字串。`NOTICE.md` 不動（授權文字由使用者決定）。

## 2. 建置（社群版樹，Profile）

- 開始前確認沒有從該樹啟動的 GameLauncher／Sandbox，也沒有對該樹的 MSBuild／cl 在跑（機器上會有另一個 session 對共用樹的 MSBuild，以命令列路徑分辨；分不出來就停下回報）。
- 備份要被改動的三個引擎檔與 `bin/win_x64/CryRenderD3D11.dll`、`CEVPlayer.dll` 到 `E:/coding3/xt-bench/cev-l1/backup-community/`（附 SHA-256 清單）。
- 以 `EnginePatch/apply_xui2dl.py` 套用（不得手改引擎檔），`--check` 輸出存證。
- 把倉內 `Code/CryPlugins/CEVPlayer` 改過的檔同步到該樹的插件目錄（逐檔，做法同先前各單）。
- 增量建 `CryRenderD3D11` 與 `CEVPlayer`（最多 8 工作）。編譯錯誤屬於 XUI2DL 自己的程式就修（修正進同一個提交或另一個 `L1b` 提交）；需要改引擎其他檔案才能過的，停下回報。
- 這棵樹的渲染器有第三方的網格烘焙改動（`eRC_BakeMesh`）：與 XUI2DL 衝突時停下回報。

## 3. 驗證

驅動沿用 `E:/coding3/xt-bench/cev-m10/m10b/run_comm.py` 與其 `scripts/` 的做法，複製到 `E:/coding3/xt-bench/cev-l1/`。**這棵樹的腳本不得以 `quit` 結尾**（會在 `C3DEngine::UnloadLevel` 崩潰並留下殺不掉的行程）：讓驅動的逾時結束行程。

- `docs/tasks/L1.md` 的 case 1～8 與 off、貼圖生命、解析度變更，判定方式照該單。「與 XTFrame 並存」一項不做（這棵樹沒有 XTFrame）。
- 延後建立：一次完全不呼叫 `cev_probe_xui2dl` 的執行 → log 沒有 XUI2DL 建立資源的訊息；另一次在 case 1 之前與之後各印一次 `GetStats`（探測指令加一個 `stats` 子指令印出全部欄位）→ `resourcesReady` 由 0 變 1。
- `cev_apitest`／`cev_flowtest`／`cev_contract_selftest`：70／65／248 不變（這棵樹先前為 70／65／248，`loadtime available=0`）。
- 斷言逐一列出，與套用補丁前的同一腳本對照（先以備份的 DLL 無法對照時，引用 `E:/coding3/xt-bench/cev-m10/m10b/logs/` 的既有 log）。

## 規則

同 `docs/tasks/L1.md` 的「規則」，另加：不碰共用樹；社群版樹腳本不用 `quit`；不繞過「樹忙碌」檢查；若留下殺不掉的行程，停下回報、不重試。證據放 `E:/coding3/xt-bench/cev-l1/`。提交訊息帶 `L1b`，結尾 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。未做的標「未編譯：共用樹」「未測：讀取迴圈、YUV、與 XTFrame 並存」。

報告（繁體中文、精簡）：提交雜湊；套用腳本在社群版樹插入的每一處；建置結果（警告與錯誤數）；每個 case 的判定與證據路徑；延後建立的證據；自測數字；斷言清單；偏離與沒做到的。
