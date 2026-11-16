# Patches

Engine patches for CEVPlayer, applied in numeric order by `tools/apply_patches.sh <engine-root>`.
The goal is to keep the engine patch surface as small as possible: everything else is synced in as
plain files by `tools/sync_sources.sh`.

## Supported trees

| Variant | Tree | Base | Series |
|---|---|---|---|
| `vanilla` | CRYENGINE 5.7.1 | `86736a0` | `patches/NN-*.patch` |
| `community` | CECE (CryENGINE-5-7-community) | `e1326cf` | `patches/community/NN-*.patch` where one exists, else the main `patches/NN-*.patch` of that number |

`BASE_COMMIT` records the commits (informational). The tree is identified by `BASE_FILES.vanilla` /
`BASE_FILES.community`: `<git blob sha1>  <path>` for every file the series touches, hashed with CR
characters removed (`tr -d '\r' < file | git hash-object --stdin`), so CRLF and LF checkouts hash the
same. A file is accepted when it has its base hash or when one of the patches touching it is already
applied. An unknown tree is refused unless `--force` is passed.

The patches carry **CRLF line endings** inside their hunks, because the engine files are CRLF. They must
apply with plain `git apply` (`.gitattributes` keeps them out of line-ending normalisation). An engine
checkout converted to LF will not match; check out with `core.autocrlf=true` like the reference trees.

## Series

| Patch | Files | Purpose |
|---|---|---|
| `01-register-plugins.patch` | `Code/CryPlugins/CMakeLists.txt`, `Tools/CMake/BuildSandbox.cmake` | Registers the CEVPlayer engine plugin (`add_subdirectory(CEVPlayer/Module)`) and the future Sandbox plugin `EditorCEVPlayer`. Both are guarded with `EXISTS`, so a tree without the synced sources still configures. |
| `community/01-register-plugins.patch` | `Tools/CMake/BuildSandbox.cmake` | Same for CECE. CECE already contains an unguarded `add_subdirectory(CEVPlayer/Module)` in `Code/CryPlugins/CMakeLists.txt`, so only the Sandbox hunk is needed. |

## Usage

```bash
tools/apply_patches.sh D:/coding11/Cryengine_5.7.1           # idempotent: re-running skips applied patches
tools/apply_patches.sh D:/coding11/CryENGINE-5-7-community
tools/sync_sources.sh  <engine-root>                         # plugin sources, SDKs, CMake modules
```

Nothing is committed in the engine tree. To reset: `git -C <engine> checkout -- Code/CryPlugins/CMakeLists.txt Tools/CMake/BuildSandbox.cmake`.

## Adding a patch

1. Generate it against the pristine base (`git diff` in a scratch repo that contains copies of the base files,
   committed with `core.autocrlf=false`, edited while keeping CRLF).
2. If CECE differs in the touched files, add `patches/community/NN-<same topic>.patch`.
3. Add the touched files to `BASE_FILES.*` (hash of the pristine file, see above) unless already listed.
4. Check: `git -C <tree> apply --check <patch>` for both trees, then run `tools/apply_patches.sh` twice.
