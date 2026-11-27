# B1 · 一個指令完成整合與建置（2026-11-27）

模型：Opus。倉 `E:/coding crycry/CEVPlayer-rebuild-integration`，分支 `xui2dl`（與 master、v2.0.0 同一提交起），提交但**不推送**。

使用者裁決：release 不附預建的插件 DLL（含 CRYENGINE 標頭編出的碼，不散佈）；第三方函式庫的預建靜態庫可以散佈（已在倉內 `Code/SDKs/CEVPlayer/`）。目標是「方便他們建置」。

現況：README「Build and integration」要使用者手動跑四步（`sync_sources.sh`、`apply_patches.sh`、CMake、建置），第四步的 `tools/build_baseline.sh` 會注入只適用於開發機的 `tools/msbuild/baseline_compat.props`。

## 要做的

新增 `tools/setup_engine.sh`（Git Bash），把既有步驟串成一個指令。**只呼叫既有腳本與工具，不重寫它們的邏輯**：

```
bash tools/setup_engine.sh <engine-root> [--xui2dl] [--with-optional] [--config Profile|Release|Debug]
                                         [--no-editor] [--no-build] [--dry-run]
```

1. 檢查：`<engine-root>` 是 CRYENGINE 5.7.1 或社群版樹（沿用 `apply_patches.sh` 的偵測；認不出就停，印原因）；找得到引擎自帶的 CMake；找得到 MSBuild（用 `vswhere`，找不到就印要裝什麼）；`--xui2dl` 時找得到 python 3。缺什麼就列出來並以非零碼結束，**不安裝任何東西**。
2. `Code/SDKs` 或其下資料夾是 junction／符號連結時：停下並印 README 那段警告指向的手動做法（`sync_sources.sh` 會 `rm -rf`），除非加 `--force-sync`。
3. 依序執行：`sync_sources.sh` → `apply_patches.sh`（`--with-optional` 轉傳）→ `--xui2dl` 時 `EnginePatch/apply_xui2dl.py`（樹上已有 XTFrame 的 XUI2D 時印一行說明仍可套用、插件會用 XUI2D）→ CMake 設定（README 的參數；`--no-editor` 時 `-DOPTION_SANDBOX=OFF`；已有 `win64/` 就沿用其產生器設定只重跑）→ 以**一般 MSBuild** 建置 `CEVPlayer`、（非 `--no-editor`）`EditorCEVPlayer`、（`--xui2dl`）`CryRenderD3D11`。不注入 `baseline_compat.props`。
4. 每一步印一行開始／結果；任一步失敗就停，印失敗的步驟與 log 路徑（log 寫在 `<engine-root>/win64/cevplayer_setup_*.log`）。結束時印產出的 DLL 路徑與下一步（在 `.cryproject` 加插件那段）。
5. `--dry-run`：只印會做什麼（轉傳 `sync_sources.sh --dry-run`），不寫任何檔。
6. 可重複執行（既有腳本本來就是冪等的）。腳本內不得有 emoji；LF 行尾。

README：「Build and integration」開頭加「Quick start」小節（一個指令＋選項表＋它不做的事：不安裝 Visual Studio／不下載引擎／不改專案檔），原本的手動步驟保留在後面標為「Manual steps」。開頭一句明寫：沒有預建的插件 DLL，第三方函式庫的預建靜態庫已在倉內、不必自己編。CHANGELOG 新開 `## Unreleased` 小節加一行。

## 驗證

- `--dry-run` 對社群版樹 `E:/coding crycry/CryENGINE-5-7-community`：不寫檔（前後 `git status`／檔案時間核對）。
- 實跑對社群版樹（它已是整合過的狀態，驗冪等與建置）：`bash tools/setup_engine.sh <社群版樹> --xui2dl`。先備份該樹的 `CEVPlayer.dll`、`CryRenderD3D11.dll`、`EditorPlugins/EditorCEVPlayer.dll` 到 `E:/coding3/xt-bench/cev-b1/backup/`（附 SHA-256）。注意該樹 `bin/win_x64/CryScaleformHelper.dll` 是使用者的、被忽略的檔，不得刪。一般 MSBuild 在這台機器若因缺 ATL/MFC 相容設定而建不了 Sandbox 插件：照實記錄錯誤訊息，改以 `--no-editor` 驗其餘部分，並在 README 的需求裡寫清楚 Sandbox 插件需要 ATL/MFC。**不要**為了通過而把開發機專屬的 props 加回腳本。
- 建置後跑一次自測（啟動方式照 `E:/coding3/xt-bench/cev-p1/run_p1.py`）：72／69／248／108，`cev_info` 後端 `xui2dl`。
- 錯誤路徑各驗一次：不存在的路徑、不是引擎樹的資料夾 → 非零碼與清楚訊息。
- **共用樹 `CRYENGINE_Source_CEVPlayer_XTFrame` 不得執行這支腳本**（連 dry-run 以外的都不行；dry-run 可以，用來驗 XTFrame 樹的提示訊息）。
- 沒驗到的情境照實列（乾淨的原版 5.7.1 樹從零開始＝未測，除非你有現成的乾淨樹可用且不是別人的）。

規則同 P1（啟動規則、不 `git add -A`、不 `git stash`、提交訊息英文 `feat:`，結尾 `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`）。證據放 `E:/coding3/xt-bench/cev-b1/`。

報告（繁體中文、精簡，最後訊息）：腳本做了什麼、各驗證項結果與數字、一般 MSBuild 在這台機器的實際情況、未測情境、提交雜湊。
