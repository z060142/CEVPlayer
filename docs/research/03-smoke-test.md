# 03 · 冒煙測試（smoke test）harness

日期：2026-11-16。目的：用一支可重複執行、不需人工操作的腳本，在 CRYENGINE 5.7.1 原始碼建置上啟動 GameLauncher（可選 Sandbox），載入 CEVPlayer 外掛、執行一串主控台指令、自動結束，並回報 PASS／FAIL（是否當機、卡死）。

## 1. 檔案與用法

| 路徑 | 說明 |
|---|---|
| `D:/coding11/CEVPlayer/tools/smoke_test.sh` | 主腳本（bash，Git Bash 下執行） |
| `D:/coding11/CEVPlayer/tools/smoke_dialog_watch.ps1` | 監看程式：偵測阻塞對話框、當機處理常式、關閉 Sandbox 視窗 |
| `D:/coding11/_testproject/CEVTest/` | 最小測試專案（repo 外） |
| `D:/coding11/_testproject/CEVTestBase/` | 同上但**不載入 CEVPlayer**（拿來隔離「是引擎還是外掛」的問題） |
| `D:/coding11/_testproject/smoke_logs/<時間>_<引擎目錄名>_<launcher\|sandbox>/` | 每次執行的輸出：`Game.log`、`stdout.txt`、`dialog.txt`（若有）、複製過來的 `error.log`／`error.dmp` |

```
tools/smoke_test.sh <engine-root> [--sandbox] [--seconds N] [--delay N] [--exec "cmd1;cmd2"]
                    [--project FILE] [--timeout N] [--out DIR] [--expect RE] [--forbid RE]
                    [--no-plugin-check] [--no-logfile] [--keep-cfg] [--tail N]
```

- `--exec` 是**參數化的指令清單**，以 `;` 分隔，依序執行。要間隔請自己放 `wait_seconds N`。M1 的新指令直接丟進去即可，例如 `--exec "cev_play vp8_vorbis"`。
- `--seconds N`：最後一個指令之後再閒置 N 秒才 `quit`（預設 10）；`--delay N`：開機後先等 N 秒再開始執行指令（預設 2）。
- `--timeout N`：硬性逾時（預設 300 秒），逾時強制殺行程並判 FAIL。
- `--expect`／`--forbid`：額外要求／禁止 log 出現某個 regex（可重複）。
- 預設要求 log 有 `CEVPlayer` 外掛載入行；`--no-plugin-check` 可關閉（配 `CEVTestBase` 用）。
- 結束碼：0 = PASS，1 = FAIL，2 = 用法／環境錯誤。

範例：

```
tools/smoke_test.sh D:/coding11/Cryengine_5.7.1 --seconds 10                      # 閒置 10 秒
tools/smoke_test.sh D:/coding11/Cryengine_5.7.1 --seconds 6 \
    --exec "vp_play vp8_vorbis" --expect "player 1 ready" --forbid "Can.t play 2D"   # 播影片
tools/smoke_test.sh D:/coding11/Cryengine_5.7.1 --sandbox --seconds 10            # Sandbox 啟動後關閉
tools/smoke_test.sh D:/coding11/CryENGINE-5-7-community --seconds 10              # 社群樹（目前 FAIL，見 §5）
```

## 2. 運作原理（為什麼這樣做，皆已用原始碼＋實測確認）

### 2.1 指令怎麼送進去、怎麼自動退出（GameLauncher）

- 腳本在專案的 `Assets/` 產生 `_smoke_<pid>.cfg`：`wait_seconds <delay>` → 使用者指令 → `wait_seconds <seconds>` → `quit`；啟動參數為 `+then exec _smoke_<pid>.cfg`，結束後刪除該檔。
- 為什麼是 `+then exec`：`CSystem::ExecuteCommandLine()`（`System.cpp:2867`）在 `InitGameFramework` 內就執行 `+<cmd>`，那時外掛還沒註冊自己的指令（CEVPlayer 在 `ESYSTEM_EVENT_GAME_POST_INIT` 才註冊 `vp_*`）。`then` 會把後面的指令延後（`Command_Then` → `ExecuteString(..., bDeferExecution=true)`），而延後模式下 `exec` 檔會被逐行排進 deferred queue（`XConsole.cpp:2147` 的 unroll），由每幀 `CXConsole::Update()` → `ExecuteDeferredCommands()` 依 `wait_seconds` 逐步執行。實測 log：`Found console batch file "_smoke_NNN.cfg" at assets/_smoke_NNN.cfg`，之後 `Executing console command 'wait_seconds 2'`…`'quit'`。
- `exec` 找檔順序（log 原文）：`Searching console batch file (try game, config, root)`，所以放在專案 `Assets/` 下可直接用檔名找到。
- `quit` 是優雅退出：log 出現 `CSystem::Quit invoked`、`System Shutdown`、一路 `Unloading ...dll`（含 `Unloading CEVPlayer.dll...`）。

### 2.2 重要陷阱：GameLauncher 乾淨退出時結束碼是 **1**

`WinMain` 回傳 `CryInitializeEngine(...) ? EXIT_SUCCESS : EXIT_FAILURE`；非手動主迴圈時 `CreateSystemInterface` 跑完 `RunMainLoop()` 後回傳 `nullptr`（`CrySystem/DllMain.cpp:204-206`），所以**正常 `quit` 也回 1**。因此不能用「結束碼 == 0」判 PASS。判定依據改為：log 有 `Executing console command 'quit'` 且有 `System Shutdown`；結束碼只要不是 0/1（例如 0xC0000005 之類）或被硬性逾時殺掉就判 FAIL。Sandbox 乾淨退出時結束碼為 0。

### 2.3 Sandbox 的差異

Sandbox 的 `CSystem::Update` 帶 `ESYSUPDATE_EDITOR`，**不會**呼叫 `m_env.pConsole->Update()`（`System.cpp:1981-1986`），所以 deferred queue 不動，`+then exec` 在編輯器裡是死的（實測：log 顯示 `Executing command from command line: then exec ...` 但之後沒有任何 exec；整整 240 秒不退出）。做法：

- Sandbox 模式改用即時 `+exec`（只有使用者指令，不含 wait／quit；這些指令在編輯器初始化時執行，外掛指令多半尚未註冊，只適合 cvar）。
- 由 `smoke_dialog_watch.ps1 -CloseAfter` 在主視窗（標題含 `Sandbox`）出現後 `delay+seconds` 秒呼叫 `CloseMainWindow()`（WM_CLOSE，等同 File > Exit）。沒有開關卡時它會乾淨結束（結束碼 0，log 有 `System:Quit`）。
- 結論：Sandbox 可以自動啟動並自動關閉；但**無法在 Sandbox 內送時序化的外掛指令**（若要，需另走 `-runpython`／`-edCommand`，本次未做）。

### 2.4 Log 位置

- 預設 `-logfile` 會被接到「引擎根目錄（cwd）」之後（`PathUtil::Make(cwd, name)`），**傳絕對路徑會變成不存在的路徑、完全沒有 log**；必須傳相對於引擎根的路徑。腳本用 `realpath --relative-to=<engine> <out>/Game.log` 處理。
- 不傳 `-logfile` 時 log 在 `<engine-root>/game.log`；若已有另一個實例佔著，新實例寫 `game(1).log`——所以**同一棵引擎樹不要同時跑兩個 smoke test**；`--no-logfile` 保留此行為。
- 專案目錄下：`<project>/user/` 是 `%USER%`（shader 快取等）；引擎當機時 `error.log`／`error.dmp` 寫在**專案目錄**（`D:/coding11/_testproject/CEVTest/error.log`）。腳本會在 `<out>`、引擎根、`bin/win_x64`、專案目錄找本次啟動後新增的 `error.log`、`*.dmp`、`*.mdmp`、`crash*.xml`，複製到 `<out>` 並判 FAIL。

### 2.5 防卡死（hard timeout＋對話框監看）

- 引擎以背景程序執行，由 `timeout -k 5 <N>` 包住，另以輪詢迴圈等結束碼檔，最多等 `N+20` 秒；仍未結束就 `taskkill /F` 並放棄等待（見 §6 的「殭屍行程」）。
- `smoke_dialog_watch.ps1` 每 1.5 秒檢查：
  - 視窗標題含 Error／Fatal／Assert／Crash 等 → 用 UI Automation 抓出對話框全文寫入 `dialog.txt`，殺行程（例：`CRYENGINE FATAL ERROR`、`Assertion Failed`）；
  - log 末 20 行出現 `Saved dump file`／`has crashed` → 視為當機處理常式已跑，3 秒後殺行程；
  - `-CloseAfter`：Sandbox 自動關閉。
- 有對話框時腳本最多再等 8 秒就放棄等待，不必等到完整逾時。

### 2.6 PASS 條件（全部成立才 PASS）

1. 沒有觸發硬性逾時；結束碼是 0 或 1（其他視為當機）。
2. 有 game log；log 沒有 `Exception Code|Unhandled exception|ACCESS_VIOLATION|has crashed|CrashRpt|Stack overflow|FATAL ERROR|Assertion failed`。
3. 啟動後沒有新的 `error.log`／`*.dmp`；沒有阻塞對話框。
4. （launcher）`Executing console command 'quit'` 與 `System Shutdown`／`System:Quit` 都在 log 裡。
5. log 含 `CEVPlayer`（可關）；所有 `--expect` 都符合、沒有任何 `--forbid`。

輸出另列：CEVPlayer 相關行、`[Error]` 數（扣掉引擎自帶的 `[CVARS] ... missing from default group` 噪音，93 條，每次必有）、其餘錯誤、log 末尾（已過濾 `leak after level unload` 噪音）。

## 3. 測試專案

`D:/coding11/_testproject/CEVTest/`：

```
CEVTest.cryproject
Assets/Videos/{audio_en,audio_ja,audio_start_offset,av_sync_flash_beep,opus_preskip,
               vp8_vorbis,vp9_1080p30_opus,vp9_alpha,vp9_odd_321x181}.webm   （測試媒體複本，約 13 MB）
user/            （引擎自動產生：shader 快取等）
```

- 沒有 `Game.dll`、沒有 `Code/`：5.7 的 GameLauncher 不需要遊戲 DLL，插件清單只有 `CryDefaultEntities` 與 `CEVPlayer`（`"type": "EPluginType::Native"`，`path` 為不含副檔名的 DLL 名，從引擎 `bin/win_x64` 載入）。**也沒有用 CryGameSDK**——所以社群樹 CryGameSDK.dll 沒建成不影響本專案。
- `console_variables`：`r_Driver=DX11`、`r_Fullscreen=0`、`r_Width=960`、`r_Height=540`、`sys_no_crash_dialog=1`、`sys_target_platforms=pc`。
- **不需要關卡**：`vp_play <名稱>` 在空場景就能播（見 §4.2）。
- 影片路徑：舊外掛的 `VIDEO_FOLDER "Videos/"` + 名稱 + `.webm`，經 CryPak `FOpen` 相對於遊戲根（= `Assets/`），所以 `vp_play vp8_vorbis` 對應 `Assets/Videos/vp8_vorbis.webm`。
- 引擎第一次載入時會把 `.cryproject` 重寫成 `"version": 4` 的格式（排版變成 tab、`"code": [""]`、多一個 `libs` 項），這是引擎自己的升級，內容等價；repo 內以重寫後的版本為準，不影響使用。
- 舊外掛的主控台介面（`Module/Plugin.cpp`）：`vp_play <name>`（不含副檔名）、`vp_pause`、`vp_resume`、`vp_stop`；cvar `vp_debug_log`、`vp_debug_draw`、`vp_disableLogs`、`vp_audio_volume`。這些在 `ESYSTEM_EVENT_GAME_POST_INIT` 才註冊。API：`IVideoPlugin::PlayVideoToMainWindow(...)`；FlowGraph 節點 `CEVPlayer:2DVideoPlayer`／`TextureVideoPlayer`／`FlashVideoPlayer`。

## 4. 前置條件與首次執行

### 4.1 不需要 .pak，但第一次會轉檔與編 shader

- 原始碼建置的 `Engine/` 是散檔（`Config`、`EngineAssets`、`Fonts`、`Shaders`），**沒有 `.pak`**；log 有兩條無害警告：`Cannot open Pak file ...\config.pak`、`...\engine\shaders.pak`。
- `Engine/EngineAssets` 只帶 `.tif` 時，引擎執行中呼叫 `Tools/rc/rc.exe` 轉成 `.dds`（log：`ProcessTextureIfNeeded()`、`Tools/rc/rc_log.log`）。**這會在引擎樹的 `Engine/EngineAssets/` 內新增 `.dds`**（本次在 vanilla 樹新增了 `Icons/levelshadercachemiss.dds`、`Icons/navigationprocessing.dds` 等，git 看是 untracked；已存在 69 個 `.dds`）。
- shader 在本機以 `d3dcompiler` 編譯（`r_ShadersRemoteCompiler=0`），快取寫進 `<project>/user/shaders/cache/`；**不需要** `CrySCompileServer`。
- 第一次在乾淨環境啟動約 1 分鐘（轉檔＋編 shader）；之後 GameLauncher 啟動到 `quit` 約 14～15 秒（idle 10 秒＋啟動）。預設 `--timeout 300` 足夠；若換新專案目錄（沒有 shader 快取）想保險，給 `--timeout 600`。
- 我最初把「launcher 一直不退出」誤判為 shader 編譯卡住——實際只是還沒有 `quit` 指令（沒 exec 到 cfg），引擎正常在跑。

## 5. 結果

以下 log 皆為 `D:/coding11/_testproject/smoke_logs/` 下對應目錄的 `Game.log`。**注意：測試途中 `bin/win_x64/CEVPlayer.dll` 被另一位 agent 換成了 M1 新外掛**（vanilla 樹 11:55:09、社群樹 11:50:59 更新；兩者是同一個 2136064 bytes 的 DLL）。(a) 的第一次執行與 (b) 的舊外掛輸出是在換版前取得；換版後我用同一腳本重跑，兩個版本都 PASS。

### (a) vanilla GameLauncher，idle 10 秒 → **PASS**

```
RESULT: PASS (launcher, 15s, clean quit, no crash artifacts)   exit code 1（見 §2.2）
<11:34:15> Loading plug-in CEVPlayer
<11:34:15> [Warning] <ThreadInfo>: Unable to find config for thread:CEVPlayer_DecoderThread
<11:34:16> Successfully registered flownode 'CEVPlayer:2DVideoPlayer'  (…TextureVideoPlayer / …FlashVideoPlayer)
<11:34:27> Unloading CEVPlayer.dll...
```

引擎自帶的 `[Error]` 共 94 條，其中 93 條是 `[CVARS]: [MISSING] ... missing from default group`，另 1 條 `<Localization> No localization files found!`——皆與外掛無關、每次都有。

### (b) 播放影片：**有路徑，不需要關卡** → PASS

`--exec "vp_play vp8_vorbis" --seconds 6`，舊外掛（`vp_debug_log 1`）log：

```
<11:34:41> [CONSOLE] Executing console command 'vp_play vp8_vorbis'
<11:34:41> [CEVPlayer] <CVideoPlayer> Loading <Videos/vp8_vorbis.webm> ...
<11:34:41> [CEVPlayer] <CVideoPlayer> Video Width : 640 / Height : 360 / Rate : 25.000000
<11:34:41> [CEVPlayer] <CAudioWrapper> Successfully initialize SDL audio
<11:34:41> [CEVPlayer] <CVideoPlayer> Playing video <Videos/vp8_vorbis.webm>...
<11:34:41> [CEVPlayer] <CRenderWrapper> Create 2D texture with name <$video_player_2D> id <395> resolution <640x360>
<11:34:46> [CEVPlayer] <CVideoPlayer> Video <Videos/vp8_vorbis.webm> finished, is loop <false>
<11:34:46> [CEVPlayer] <C2DVideoQueue> Video stopping. Last frame time <4.963>, supposed frame time <5.003>
```

M1 新外掛（換版後，同一指令 `vp_play vp8_vorbis`，`--expect "player 1 ready: <Videos/vp8_vorbis.webm>"` 通過）：

```
<12:00:11> [CONSOLE] Executing console command 'vp_play vp8_vorbis'
<12:00:11> [CEVPlayer] player 1 ready: <Videos/vp8_vorbis.webm> 640x360 5.00s audio
<12:00:11> [CEVPlayer] renderer probe: CEVPlayer_Patched not found, stage 1 (CPU conversion, aux fullscreen)
<12:00:11> [Warning] [CEVPlayer] audio stream has 44100 Hz, the M1 audio device is 48000 Hz without a resampler: no audio for this video
<12:00:17> Unloading CEVPlayer.dll...      RESULT: PASS (launcher, 10s, ...)
```

兩個版本都能由 `vp_play` 在無關卡下啟動播放、解碼、播完、乾淨關閉。「畫面是否真的出現在視窗」腳本不判（只有 log 證據：建立了 2D 材質 texture id 395／aux fullscreen 路徑）；若要看畫面需人眼或截圖，這支腳本不做。

### (c) 社群樹 `D:/coding11/CryENGINE-5-7-community`：**FAIL（引擎樹本身的問題，與 CEVPlayer 無關）**

1. **預設就無法啟動**：樹裡沒有 `Engine/` 目錄，只有 `engine_/`（內含少量 `.cfx`，不是完整引擎資產）。啟動即跳 `CRYENGINE FATAL ERROR`：`Unable to locate CryEngine root folder. Ensure that the 'engine' folder exists in your CryEngine root directory`，GameLauncher 卡在這個 modal 對話框（watcher 抓到全文並殺行程，約 3 秒判 FAIL）。
2. 為了繼續測，我暫時建了 junction `CryENGINE-5-7-community\Engine` → `D:\coding11\Cryengine_5.7.1\Engine`（借 vanilla 的引擎資產；測完已移除，沒有留在樹裡）。此時引擎起得來、載入 CEVPlayer、`quit` 正常執行，**然後在關機時當機**：

```
<11:46:01> Unloading CEVPlayer.dll...
<11:46:01> Unloading CryDynamicResponseSystem.dll...<11:46:03> Saved dump file to 'error.dmp'
error.log: Exception Code: 0xC0000005 (EXCEPTION_ACCESS_VIOLATION, read 0x00007FFE0D7680F0)
           Exception Module: CrySystem.dll
           CSystemEventDispatcher::OnSystemEvent() [SystemEventDispatcher.cpp:46]
           C3DEngine::UnloadLevel() [3dEngineLoad.cpp:330]  ← C3DEngine::ShutDown ← CSystem::ShutDown ← ~CSystem
```

   `CSystemEventDispatcher` 在 `CryDynamicResponseSystem.dll` 卸載後仍呼叫已卸載模組裡的 listener（`CryDynamicResponseSystem/ResponseSystem.cpp:105` 註冊了 `CResponseSystem` 作為 listener）。**用不含 CEVPlayer 的 `CEVTestBase` 專案重跑，同樣當機**，且同一位置——確認是社群樹的 bug，不是外掛造成。vanilla 樹同一專案不會。
3. 當機後行程不會自己結束（卡在當機處理常式），watcher 偵測到 `Saved dump file` 後殺它；實測 `taskkill /F` 之後行程仍留在 `tasklist` 一段時間（見 §6）。
4. 社群樹 **Sandbox**（同樣需 junction）：啟動後跳 `Assertion Failed`：`ResourceSelectorHost.cpp:57  m_typeMap.find(entry->GetTypeName()) == m_typeMap.end()`，訊息 `Resource selector already registered with name "EntityClassName"`；`CEVTestBase`（無外掛）同樣出現 → 也是社群樹本身的問題（使用者平常可按「Ignore」略過）。

結論：社群樹的 GameLauncher／Sandbox 二進位在「沒有 CEVPlayer」時已經有上述問題，所以那棵樹不適合當 CEVPlayer 的驗證基準；以 vanilla 樹為準。

### (d) vanilla Sandbox，啟動後自動關閉 → **PASS**

```
RESULT: PASS (sandbox, 17s, clean quit, no crash artifacts)    exit code 0
Loading plug-in CEVPlayer
Successfully registered flownode 'CEVPlayer:2DVideoPlayer' (…)
CSystem::Quit invoked from thread ... (main is ...)
System:Quit
```

機制見 §2.3（主視窗標題 `CRYENGINE Sandbox - Build 0 - Project 'CEVTest'`，10 秒後 WM_CLOSE）。

### 彙總

| 案例 | 結果 | 證據 |
|---|---|---|
| (a) vanilla launcher idle 10 s | PASS | log：Loading plug-in CEVPlayer、quit、System Shutdown；無 error.log／dmp |
| (b) vanilla launcher `vp_play vp8_vorbis`（舊外掛與新 M1 外掛皆測） | PASS | `Executing console command 'vp_play ...'`、播放 5 秒、finished／removed、乾淨關閉 |
| (c) community launcher | FAIL | 缺 `Engine/` 的 FATAL ERROR；借 junction 後關機時存取違規（`error.log`），無 CEVPlayer 時亦然 |
| (d) vanilla Sandbox idle→自動關閉 | PASS | 結束碼 0，log 有 `System:Quit` |
| (d') community Sandbox | FAIL | Assertion Failed `ResourceSelectorHost.cpp:57`，無 CEVPlayer 時亦然 |

## 6. 已知問題與注意事項

- **殭屍行程**：社群樹當機後，`GameLauncher.exe` 在 `tasklist` 留下「已結束（`HasExited=True`）但仍佔著 `game.log`」的條目一段時間（無法用 `taskkill /F` 清掉）；它佔著的 log 檔導致舊 log 刪不掉。對策：腳本預設用 `-logfile` 寫進每次獨立的輸出目錄，不受影響；否則下一次會寫 `game(1).log`。測試後殘留的 `smoke_logs/*community*` 目錄可能有被佔住而無法刪除的檔案，稍後再刪即可。
- **引擎樹會被寫入**：`Engine/EngineAssets/**/*.dds`（rc 轉檔）。vanilla 樹是 git 工作區，這些會顯示成 untracked；專案端寫入 `<project>/user/`（`shaders/cache`、`testresults/testlog.log` 等，不在 repo）。這次沒有修改任何引擎原始碼。vanilla 樹原本就有大量 `M`（`Tools/rc/netgen/*` 等，非本次所為）。
- 並行：同一引擎樹不要同時跑兩個 smoke test（`game.log` 衝突、`Assets/_smoke_<pid>.cfg` 雖以 pid 區分，但 shader 快取與 `user/` 會被兩個引擎實例搶）。
- 結束碼：GameLauncher 乾淨退出為 1，Sandbox 為 0；勿拿結束碼單獨當判準。
- 舊／新外掛的 log 都有一條無害警告 `Unable to find config for thread:CEVPlayer_DecoderThread`（執行緒沒有設定檔，用預設）。M1 新外掛另有 `audio stream has 44100 Hz, the M1 audio device is 48000 Hz without a resampler: no audio for this video`（vp8_vorbis.webm 目前無聲，屬 M1 已知限制，不是 harness 的問題）。
- 腳本不驗證畫面內容，只驗證「指令有被執行、外掛有回應、沒有當機、能乾淨結束」。要驗證畫面需另外加截圖（例如在 `--exec` 裡用 `r_` 截圖指令，或外部螢幕擷取）。
- Sandbox 無法送時序化的外掛指令（§2.3）。要在 Sandbox 內測外掛指令，需另設計（`-runpython`＋Qt 計時器，或 `-edCommand`）。
- 若要把 `<engine-root>` 換成別棵樹，注意該樹需有完整 `Engine/`（含 `Config`、`EngineAssets`、`Fonts`、`Shaders`）以及 `bin/win_x64/CEVPlayer.dll`；腳本會先檢查 EXE 是否存在，但不檢查 `Engine/`（缺了就會看到 §5(c) 的 FATAL ERROR 對話框，被 watcher 抓到並判 FAIL）。
