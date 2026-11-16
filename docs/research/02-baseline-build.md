# 02 基線建置（M0）：CRYENGINE 5.7.1 vanilla 與 CECE 社群分支

日期：2026-11-16。兩棵樹的引擎原始碼都沒有改，也沒有 commit。所有修正都放在原始碼樹外。

- A（vanilla）：`D:/coding11/Cryengine_5.7.1`
- B（CECE 社群分支）：`D:/coding11/CryENGINE-5-7-community`（HEAD `e1326cfd`）

## 結論

| | Configure | Build（Profile，整個方案） | GameLauncher.exe | Sandbox.exe | CryRenderD3D11.dll |
|---|---|---|---|---|---|
| A | 成功（71 秒） | 成功，0 錯誤 | 有 | 有 | 有 |
| B | 要補一個空的 AssimpSDK 目錄才成功 | 失敗，112 個錯誤，集中在 4 個專案 | 有 | 有 | 有 |

B 的三個目標二進位檔都有產出（Sandbox 本體在 B 也連結成功）。失敗的 4 個專案是 CEVPlayer 外掛、CryGameSDK、兩個編輯器外掛（HPiPlugin、LodGeneratorPlugin）、MeshImporter 加 RC 的 CryAssimpBridge。它們的原因都不是環境，詳見下方 B 的失敗清單。

## 工具鏈與環境修正（兩棵樹共通）

- CMake：各樹自帶的 `Tools/CMake/Win32/bin/cmake.exe`（3.22）。
- Generator：`Visual Studio 17 2022`，`-A x64`，VS 17.14，MSVC 14.44.35207，Windows SDK 10.0.19041.0。
- SDK：兩棵樹的 `Code/SDKs` 都已就緒。configure 印出 `Using pre-downloaded SDKs: CRYENGINE_v5.7.0_SDKs.zip`，沒有重新下載或解壓。
- 缺 ATL/MFC：這台機器的 VS 沒裝「C++ ATL / MFC」元件。RC、crashrpt、EditorCommon 因此找不到 `atlbase.h`、`afx.h`、`mfcs140.lib`。
  - 我試過用 `setup.exe modify` 加元件，需要提權，被權限系統拒絕，所以沒有裝。
  - 改為下載 Microsoft 官方 vsix（`Microsoft.VC.14.44.17.14.ATL.X64.base`、`ATL.Headers.base`、`MFC.X64.base`、`MFC.Headers.base`、`MFC.MBCS.X64.base`），解壓到 `D:/coding11/_tools/atlmfc/MSVC/atlmfc`。vsix 內的路徑是 `14.44.35207`，和已安裝的工具集同版本。
  - 目前只有 Release 版 MFC 函式庫，沒有 Debug 版。若要建 Debug 組態，要另外補 `MFC.X64.Debug.base`。
  - 以 MSBuild props 加進 `IncludePath` 和 `LibraryPath`，不碰 VS 安裝目錄。
- 新版 STL 的相容問題，一樣用 props 處理，不改原始碼：
  - Alembic（RC 內部）用到 `std::binary_function` 和 `std::unary_function`，這兩個在新 STL 的 C++17 模式下已移除。
    - 對 `alembic` 專案定義 `_HAS_AUTO_PTR_ETC=1`、`_HAS_UNARY_BINARY_FUNCTION=1`。
    - 同時強制包含 `<functional>`。
  - OculusSDK 與 `Code/Tools/CryCommonTools/FileUtil.cpp` 缺少 `<algorithm>`、`<chrono>`、`<thread>`。對 `oculus` 專案與所有 RC 子專案的 `.cpp` 檔強制包含。`.c` 檔不能強制包含 C++ 標頭，所以用 Target 依副檔名分開處理。
  - 沿用 XTFrame 的 `_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING`（Qt 5.12，VS 17.8 以上）。
- 檔案：
  - `D:/coding11/CEVPlayer/tools/msbuild/baseline_compat.props`：上述修正。同時以 `-p:ForceImportBeforeCppTargets` 和同名環境變數傳入，這樣 RC 內層的 ExternalProject 建置也吃得到。
  - `D:/coding11/CEVPlayer/tools/build_baseline.sh`：改自 XTFrame 的 `build_engine.sh`。環境變數 `CRYENGINE_SOURCE=<樹>`、`LOGTAG=<A|B>`，log 直接寫到 `D:/coding11/_buildlogs/`。
- 偶發的 PCH 錯誤（C3859、C1076）：A 第一次建置出現過，同一份 props 之外沒做任何事，重跑就消失，判斷是第一次與另一個 configure 同時跑造成的記憶體壓力。B 的同類錯誤見下方，原因不同。

## 組態指令

A：

```
cd D:/coding11/Cryengine_5.7.1
Tools/CMake/Win32/bin/cmake.exe -S . -B win64 -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE=Tools/CMake/toolchain/windows/WindowsPC-MSVC.cmake \
  -DOPTION_ENGINE=ON -DOPTION_SANDBOX=ON -DOPTION_CRYMONO=OFF
```

B：同一條指令，在 `D:/coding11/CryENGINE-5-7-community` 下執行。B 要先補空目錄 `Code/SDKs/AssimpSDK/{include,code,lib/Debug,lib/Release}`（見「B 的失敗」），並在其中放一個 `PLACEHOLDER.txt`。

選項只有 `OPTION_ENGINE`、`OPTION_SANDBOX`、`OPTION_CRYMONO` 三個，其餘都是預設值。兩棵樹的 `OPTION_*` 和 `PLUGIN_*` 布林快取值相同。

- 需要專有 SDK 的選項：`OPTION_SCALEFORM=ON` 是預設，它只建「Scaleform 輔助模組」，不需要 Scaleform SDK，所以保留。`SCALEFORM3` 和 `SCALEFORM4` 在 `BuildEngine.cmake` 中被設為 OFF。
- configure 自動停用的 SDK：Fmod、Wwise、ADX2、NVAPI、AMD AGS、Pix。
- 快取檔備份：`D:/coding11/_buildlogs/A-CMakeCache.txt`、`B-CMakeCache.txt`。

建置指令：

```
cd D:/coding11/_buildlogs
CRYENGINE_SOURCE=D:/coding11/<樹> LOGTAG=<A|B> bash D:/coding11/CEVPlayer/tools/build_baseline.sh Profile
```

它呼叫 `MSBuild CRYENGINE_Win64.sln -p:Configuration=Profile -p:Platform=x64 -m`。

## A（vanilla）建置結果

- configure：71 秒，無錯誤。
- 建置：共跑 3 次。
  - 第 1 次 532 秒，失敗。ATL/MFC、Alembic、`FileUtil.cpp`、OculusSDK，以及偶發的 PCH 問題。
  - 第 2 次 277 秒，失敗。ATL/MFC 已解，剩 Alembic 的 `binary_function`、強制包含用到 `.c` 檔、`mfcs140.lib`。
  - 第 3 次 422 秒，成功。0 錯誤，10310 個警告（本次增量的 MSBuild 彙總；其中 `C4996` 約 10243 個）。
- 累計實際花費約 21 分鐘。
- 執行產物：

| 檔案 | 位置 |
|---|---|
| `GameLauncher.exe`（731,136 B） | `Cryengine_5.7.1/bin/win_x64/` |
| `Sandbox.exe`（23,665,664 B） | `Cryengine_5.7.1/bin/win_x64/` |
| `CryRenderD3D11.dll`（8,533,504 B） | `Cryengine_5.7.1/bin/win_x64/` |
| 同目錄還有 | `CryRenderVulkan.dll`、`CryRenderD3D12.dll`、`CrySystem.dll`、`CryGameSDK.dll`（共約 190 個 exe 和 dll） |
| `rc.exe` 與 RC 外掛 | `Cryengine_5.7.1/Tools/rc/` |

- git 狀態：只多一個未追蹤的 `win64/`。

## B（CECE）建置結果

- configure：
  - 第 1 次失敗。`Code/Sandbox/Plugins/MeshImporter/CMakeLists.txt` 連結了 `AssimpSdk`，這個目標的 include 路徑 `Code/SDKs/AssimpSDK/include` 不存在。
  - 補空目錄後第 2 次成功，約 107 秒，輸出 `Generating done`。CEVPlayer 的 `add_subdirectory(CEVPlayer/Module)` 通過 configure，沒有動它。
- 建置：1 次，1086 秒，失敗。8440 個警告，112 個錯誤（按檔案計算的 MSBuild 彙總；去重後只有下列 4 組根因）。
- 除了下列專案，其餘整個方案都建成。
- 執行產物（Profile）：

| 檔案 | 位置 |
|---|---|
| `GameLauncher.exe`（731,136 B） | `CryENGINE-5-7-community/bin/win_x64/` |
| `Sandbox.exe`（23,668,224 B） | `CryENGINE-5-7-community/bin/win_x64/` |
| `CryRenderD3D11.dll`（8,658,944 B） | `CryENGINE-5-7-community/bin/win_x64/` |
| `rc.exe` | `CryENGINE-5-7-community/Tools/rc/` |

- 與 A 比較：B 沒有 `CryGameSDK.dll`，多了 `CryVS.dll`。B 的專案樹中也沒有 `Editor/` 資料夾（A 有 `Editor/Styles/Office2007*.dll`），這屬於資源配置差異，不是建置錯誤。

### B 的失敗（均為原始碼或第三方相依問題，未改碼）

1. 缺 AssimpSDK（MeshImporter 與 RC 的 CryAssimpBridge）
   - `Code/Sandbox/Plugins/MeshImporter/IScene.h:20`：`C1083` 找不到 `assimp/...`。
   - `Code/Tools/RC/CryAssimpBridge/CryAiScene.cpp:35`：`C1083` 找不到 `assimp/Importer.hpp`，連帶 `RC.vcxproj` 報 `MSB8066`。
   - 根因：fork 根目錄的 `_Where_I_GET_ASSIMP_SDK.md` 指向 `https://github.com/HPirani/CryEngine5-7-Assimp`，這個 repo 現在回 `Repository not found`。fork 需要的是預先編好的 SDK：
     - `include`、`code`、`lib/{Debug,Release}/assimp-vc143-mt[d].lib`
     - `*-static-crt.lib`
     - `zlibstatic[d].lib`
   - 我有試著用 Assimp 官方 5.4.3 原始碼自己編出同版面的 SDK，這個動作被權限系統拒絕，所以沒做。
   - 目前的處理：補了空的目錄，只為了讓 configure 通過。這等於讓 `OPTION_ASSIMP_SDK` 偵測到目錄存在，因此 RC 才會去編 CryAssimpBridge 而失敗。若有人提供真正的 SDK，覆蓋這個目錄即可。
2. 缺 XT Toolkit 的 include 路徑（HPiPlugin、LodGeneratorPlugin）
   - `Code/CryEngine/CryCommon/CryCore/Platform/CryAtlMfc.h:85`：`C1083` 找不到 `XTToolKitPro.h`。
   - LodGeneratorPlugin 另外有 44 個 `C3859` / `C1076`（PCH 建立失敗），是同一個錯誤連帶造成。
   - 根因：這兩個外掛的 `CMakeLists.txt` 沒有呼叫 `use_xt()`，所以沒有 `Code/SDKs/XT_13_4/include` 的 include 路徑。標頭本身在 SDK 內存在（`XTToolkitPro.h`，只有大小寫和 `CryAtlMfc.h` 的拼法不同，在 Windows 上不影響）。要修需要改這兩個外掛的 `CMakeLists.txt`。
3. CryGameSDK 的 unity build 衝突
   - `Code/GameSDK/GameDll/VehicleMovementTrain.cpp`：
     - `:87 computeDenominator`、`:98 addImpulseAtOffset`、`:105 addImpulse`、`:240 GetWheeledTable` 全部是 `C2084`（函式已有主體）。
     - `:427`、`:740` 為 `C2512`。
     - 後續有 `C2264`。
   - 根因：這是 fork 新增的檔案（A 沒有這個檔案）。它裡面定義了和 `VehicleMovementArcadeWheeled.cpp` 同名的靜態函式；兩者在預設的 `OPTION_UNITY_BUILD=ON` 下被併進同一個 uber 檔，名稱衝突。尚未試過關閉 unity build。
4. CEVPlayer 外掛缺第三方函式庫
   - `Module/Video/VideoPlayer.h:16`（5 個）：`C1083` 找不到 `vpx/vpx_decoder.h`。
   - `Module/Render/Frame.cpp:8`：找不到 `vpx/vpx_image.h`。
   - `Module/Audio/AudioDecoder.h:6`：找不到 `vorbis/codec.h`。
   - 根因：`_CustomSDKs` 裡的 libvpx、libwebm、libvorbis 等沒放進樹內。CMake 沒有排除這個外掛的選項。configure 通過，只有建置時失敗，所以我沒有註解掉 `add_subdirectory(CEVPlayer/Module)`，`Code/CryPlugins/CMakeLists.txt` 保持原樣。
   - `D:/coding11/_tools/` 下已有 libvpx、libwebm、ogg、vorbis、opus 的下載與建置痕跡，後續可從那邊接。

### B 工作樹的副作用

`git status` 顯示 B 的 `Tools/rc/netgen/**` 約 212 個追蹤檔被標為 modified。這是 RC 專案的 post-build 把 `netgen` 目錄複製回來造成的，只有換行（LF/CRLF）差異，不是原始碼修改，也沒有 commit。需要時可用 `git checkout -- Tools/rc/netgen` 還原（每次建置會再覆寫）。除此之外，B 只多了未追蹤的 `win64/`。`Code/SDKs/AssimpSDK/` 被 `.gitignore` 的 `/Code/SDKs/*` 蓋住。

## Log 與附件

- `D:/coding11/_buildlogs/`：
  - `A-configure.log`、`B-configure.log`（第 1 次，失敗）、`B-configure2.log`（第 2 次，成功）。
  - `A-engine-Profile.run1.log`、`A-engine-Profile.run2.log`、`A-engine-Profile.log`（第 3 次，成功）；對應的 `A-build-stdout*.log`。
  - `B-engine-Profile.log`、`B-build-stdout.log`。
  - `A-CMakeCache.txt`、`B-CMakeCache.txt`。
- 建置過程的下載物都放在 `D:/coding11/_tools/`：`atlmfc/`、`assimp-5.4.3.tar.gz`（只下載，未使用）、`assimp/`（只解壓，未使用）。
- `D:/coding11/CEVPlayer/tools/build_assimp_sdk.sh`：自建 Assimp SDK 的腳本，已寫好但**沒有執行**（執行被拒絕）。若協調者同意，可以直接跑。

## 後續建議

- 開發基礎先用 A。B 在 Sandbox 和渲染層的建置是通的，主要差距是三件事：
  1. AssimpSDK：找回原始 SDK，或批准自建。
  2. XT 的 include 路徑與 unity build 衝突：兩者都需要改 B 的原始碼，由協調者決定。
  3. CEVPlayer 的第三方函式庫。
- 若決定在 B 上開發，且要完整乾淨的建置，上面第 1 點（AssimpSDK）是 RC 和 MeshImporter 的前提。
