# M6c-0 community baseline (CryENGINE-5-7-community e1326cf + CEVPlayer)

Date: 2026-11-19. Tree: `E:/coding crycry/CryENGINE-5-7-community` (main e1326cfd, nothing committed, nothing pushed).
Machine: VS 2022 17.14, MSVC 14.44.35207 (ATL/MFC installed), engine-shipped CMake 3.22.

## 1. Code/SDKs arrangement (option b)

`Code/SDKs` was a junction to `E:/coding2/CryENGINE-5-7-community/Code/SDKs` (read-only target). Replaced by a real
directory holding one junction per target sub-directory (51) plus a real `CEVPlayer/` directory written by
`sync_sources.sh` (copied from the CEVPlayer repo). Option (a) (`-DSDK_DIR`) was not used: it redirects every SDK.

```powershell
$p="E:\coding crycry\CryENGINE-5-7-community\Code\SDKs"; $t="E:\coding2\CryENGINE-5-7-community\Code\SDKs"
$i=Get-Item $p; if($i.LinkType -ne 'Junction'){throw}; $i.Delete()          # removes only the link
New-Item -ItemType Directory $p
foreach($d in ls $t -Directory){ New-Item -ItemType Junction -Path (Join-Path $p $d.Name) -Target $d.FullName }
(ls $t -Directory).Count   # still 51
```
Before running `sync_sources.sh` check that the target has no `libvpx/libwebm/libogg/libvorbis/libopus`
(the script `rm -rf`s an old flat copy; through a junction that would delete the target). It had none.

## 2. Patch and sync (CEVPlayer repo used read-only)

```bash
cd "/e/coding crycry/CEVPlayer-rebuild-integration"
bash tools/apply_patches.sh "/e/coding crycry/CryENGINE-5-7-community"   # community/01-register-plugins.patch
bash tools/sync_sources.sh  "/e/coding crycry/CryENGINE-5-7-community"   # CEVPlayer 96, EditorCEVPlayer 52, SDKs 62 files, 8 cmake modules
```

## 3. Engine assets

The fork has no `Engine/` directory. Copied (read only from the source) the 150 MB `Engine/` of the integration tree:
`robocopy "E:\coding crycry\CRYENGINE_Source_CEVPlayer_XTFrame\Engine" "<tree>\Engine" /E`. The fork's `engine_/`
(modified shader .ext/.cfx files) was not applied.

## 4. Configure (29 s)

```powershell
$e="E:/coding crycry/CryENGINE-5-7-community"
& "$e/Tools/CMake/Win32/bin/cmake.exe" -S $e -B "$e/win64" -G 'Visual Studio 17 2022' -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$e/Tools/CMake/toolchain/windows/WindowsPC-MSVC.cmake" `
  -DOPTION_DOWNLOAD_SDKS=OFF -DOPTION_ENGINE=ON -DOPTION_SANDBOX=ON -DOPTION_RC=ON -DOPTION_CRYMONO=OFF
```
Output includes `CEVPlayer: CryVS headers found, building the CryVS bindings (CEV_HAVE_CRYVS=1)`.
Side effect: an empty untracked `CRYENGINE_v5.7.0_SDKs.zip` at the tree root.

## 5. Build (whole solution, Profile, 1579 s = 26 min)

`logs/build.sh` (MSBuild, `-m:4 -p:CL_MPCount=2`, props
`E:\coding crycry\CRYENGINE_Source_CEVPlayer_XTFrame\Tools\Integration\toolset_compat.props` passed as
`-p:ForceImportBeforeCppTargets` and as the environment variable). Logs: `logs/build-full.log`, `logs/stdout-full.log`.
Exit 1 because of non-required projects; all required outputs exist in `bin/win_x64`:
Sandbox.exe, GameLauncher.exe, CryRenderD3D11.dll, CryVS.dll, CEVPlayer.dll, EditorPlugins/EditorCryVS.dll,
EditorPlugins/EditorCEVPlayer.dll, `Tools/rc/rc.exe` (built 12:13).

Failed (not required, fork sources untouched):

| Project | Error | Cause |
|---|---|---|
| HPiPlugin | C1083 `XTToolKitPro.h` | CMakeLists lacks `use_xt()` |
| LodGeneratorPlugin | LNK2019 `CLODGeneratorQEM::GenerateLOD_QEM` / LNK1120 | unresolved symbol in the fork's plugin |
| CryGameSDK | C2084 etc. in `VehicleMovementTrain.cpp` | unity-build duplicate statics |
| CryAssimpBridge (RC plugin) + RC.vcxproj MSB8066 | C3668/C3861 in MSVC `xiosbase` (`_Doraise`/`_RAISE`) | Assimp SDK in Code/SDKs has headers but empty `lib/`; the header clash also stops compile. rc.exe itself built |

MeshImporter built this time.

## 6. Test project and runs

Project `E:/coding3/xt-bench/cev-m6c/project/CEVM6C.cryproject` (CryDefaultEntities + CEVPlayer, no GameSDK; two videos).
Runner `run_one.ps1` (Start-Process + WaitForExit deadline + kill own PID). Logs are written to the engine root
(`game.log`, `editor.log`, `cryvs.log`); copies in `logs/`.

- `sys_SchematycPlugin=1` (CryVS only): GameLauncher crashes in init: null `gEnv->pSchematyc` at
  `CryEntitySystem/CryEntityDLL.cpp:58` (ESYSTEM_EVENT_REGISTER_SCHEMATYC_ENV). Fork bug; `logs/launcher-sys1-crash.*`.
- **Use `sys_SchematycPlugin=0`** (CryVS + Schematyc): works.
- GameLauncher `+cev_apitest`: `cev_apitest: PASS (33 checks, 0 failed)`; `CryVS: registered component 'CEVPlayer::VideoPlayer' (26 functions, 8 signals)`.
- GameLauncher `+cev_play Videos/vp8_vorbis.webm`: ready 640x360 5.00s, audio clock, `ended` after 5 s.
- Sandbox: first run blocked on an assert dialog (`Resource selector already registered with name "EntityClassName"`,
  `EditorQt/ResourceSelectorHost.cpp:57`, EditorCryVS and EditorSchematyc both register it). With `sys_asserts=0` in the
  project the main window opens (`CRYENGINE Sandbox - Build 0 - Project 'CEVM6C'`), log shows
  `Successfully loaded plugin 'CryVS Plugin'` (EditorCryVS), `[CEVEditor] plugin loaded`, `plugin 'CEVPlayer'`.
- `cryvs.log` is created but stays empty (0 bytes).

## 7. Tree state

`git status --short`: CEVPlayer sync (32 D / 9 M / 17 ?? under Code/CryPlugins/CEVPlayer, ?? EditorCEVPlayer,
?? 8 Tools/CMake/modules), M Tools/CMake/BuildSandbox.cmake (patch), 212 M Tools/rc/netgen (RC post-build, line endings
only), ?? Engine/, ?? win64/, ?? CRYENGINE_v5.7.0_SDKs.zip. Code/SDKs is gitignored.
