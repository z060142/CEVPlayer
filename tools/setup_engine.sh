#!/usr/bin/env bash
# One command from a CRYENGINE source tree to built CEVPlayer plug-ins. It only chains the existing steps:
#   tools/sync_sources.sh -> tools/apply_patches.sh -> (--xui2dl) EnginePatch/apply_xui2dl.py
#   -> CMake configure (engine-shipped CMake) -> tools/build_engine.sh (plain MSBuild of the plug-in targets)
#
#   bash tools/setup_engine.sh <engine-root> [--xui2dl] [--with-optional] [--config Profile|Release|Debug]
#                                            [--no-editor] [--no-build] [--force-sync] [--dry-run]
#
#   --xui2dl         also apply the XUI2DL renderer patch and build CryRenderD3D11 (needs python 3)
#   --with-optional  forwarded to apply_patches.sh (optional CryAudio patch)
#   --config         build configuration, default Profile
#   --no-editor      configure with -DOPTION_SANDBOX=OFF and do not build EditorCEVPlayer
#   --no-build       stop after the CMake configure step
#   --force-sync     run sync_sources.sh even when a path it replaces is a junction / symbolic link
#   --dry-run        only print what would be done (sync_sources.sh --dry-run, read-only checks); writes nothing
#
# It does not install anything (Visual Studio, ATL/MFC, python), does not download an engine and does not edit any
# project file. Logs: <engine-root>/win64/cevplayer_setup_<step>.log (overwritten on every run).
# Exit codes: 0 = done, 1 = a check or a step failed, 2 = usage.
set -uo pipefail

usage() {
  sed -n '6,7p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  echo "  see the header of tools/setup_engine.sh for the options" >&2
  exit 2
}

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
engine=""
xui2dl=0 with_optional=0 config="Profile" editor=1 build=1 force_sync=0 dry=0
while [ "$#" -gt 0 ]; do
  case "$1" in
    --xui2dl) xui2dl=1 ;;
    --with-optional) with_optional=1 ;;
    --config)
      shift
      case "${1:-}" in Profile|Release|Debug) config="$1" ;; *) echo "setup_engine: --config needs Profile, Release or Debug" >&2; usage ;; esac ;;
    --no-editor) editor=0 ;;
    --no-build) build=0 ;;
    --force-sync) force_sync=1 ;;
    --dry-run) dry=1 ;;
    -h|--help) usage ;;
    -*) echo "setup_engine: unknown option $1" >&2; usage ;;
    *) [ -z "$engine" ] && engine="$1" || usage ;;
  esac
  shift
done
[ -n "$engine" ] || usage

say() { echo "setup_engine: $*"; }

# --- 1. checks (nothing is written) ---------------------------------------------------------------------------
if [ ! -d "$engine" ]; then
  say "$engine does not exist or is not a directory" >&2
  exit 1
fi
engine="$(cd "$engine" && pwd)"
if [ ! -d "$engine/Code/CryPlugins" ] || [ ! -d "$engine/Tools/CMake" ]; then
  say "$engine is not a CRYENGINE source root (Code/CryPlugins and Tools/CMake expected)" >&2
  exit 1
fi
[ "$dry" -eq 1 ] && say "dry run: nothing is written"
say "engine $engine, config $config, editor $([ "$editor" -eq 1 ] && echo on || echo off), xui2dl $([ "$xui2dl" -eq 1 ] && echo on || echo off)"

problems=()

if ! command -v git >/dev/null 2>&1; then
  problems+=("git not found (Git for Windows provides Git Bash and git; apply_patches.sh needs it)")
else
  detect_out="$(bash "$here/tools/apply_patches.sh" "$engine" --detect 2>&1)"
  if [ $? -eq 0 ]; then
    say "tree: ${detect_out##* -> }"
  else
    problems+=("engine tree not recognised as CRYENGINE 5.7.1 or the community fork:"$'\n'"$detect_out")
  fi
fi

cmake="$engine/Tools/CMake/Win32/bin/cmake.exe"
[ -f "$cmake" ] || problems+=("engine-shipped CMake not found: $cmake (it is part of the CRYENGINE source tree)")

vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
msbuild="" vsroot=""
if [ -x "$vswhere" ]; then
  msbuild="$("$vswhere" -latest -requires Microsoft.Component.MSBuild -find 'MSBuild/**/Bin/MSBuild.exe' 2>/dev/null | head -n1 | tr -d '\r')"
  vsroot="$("$vswhere" -latest -requires Microsoft.Component.MSBuild -property installationPath 2>/dev/null | head -n1 | tr -d '\r')"
fi
if [ -z "$msbuild" ]; then
  problems+=("MSBuild not found through vswhere: install Visual Studio 2022 with the 'Desktop development with C++' workload (MSVC 14.4x, Windows SDK; for the Sandbox plug-in also 'C++ ATL' and 'C++ MFC')")
else
  say "MSBuild: $msbuild"
  if [ "$editor" -eq 1 ] && ! ls "$(cygpath -u "$vsroot")"/VC/Tools/MSVC/*/atlmfc/include/afx.h >/dev/null 2>&1; then
    say "warning: no ATL/MFC found in $vsroot; EditorCEVPlayer (Sandbox) needs the 'C++ ATL' and 'C++ MFC' components (or use --no-editor)"
  fi
fi

python=()
if [ "$xui2dl" -eq 1 ]; then
  for cand in "python3" "python" "py -3"; do
    # shellcheck disable=SC2086
    if $cand -c 'import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)' >/dev/null 2>&1; then
      read -r -a python <<< "$cand"
      break
    fi
  done
  if [ "${#python[@]}" -eq 0 ]; then
    problems+=("python 3 not found (tried python3, python, py -3); --xui2dl runs EnginePatch/apply_xui2dl.py")
  else
    say "python: ${python[*]} ($("${python[@]}" -c 'import sys; print(sys.version.split()[0])'))"
  fi
fi

# A running Sandbox / GameLauncher of this tree keeps its DLLs locked: the build would fail at link time.
if [ "$dry" -eq 0 ] && [ "$build" -eq 1 ]; then
  binw="$(cygpath -w "$engine/bin")"
  running="$(powershell -NoProfile -Command "Get-Process -ErrorAction SilentlyContinue | Where-Object { \$_.Path -and \$_.Path.StartsWith('$binw\\', 'OrdinalIgnoreCase') } | ForEach-Object { \$_.Path }" 2>/dev/null | tr -d '\r')"
  [ -z "$running" ] || problems+=("programs of this tree are running (close them first, they lock the DLLs):"$'\n'"$running")
fi

# --- 2. junctions / symbolic links on the paths sync_sources.sh replaces or writes into ------------------------
linked=()
for rel in Code Code/SDKs Code/SDKs/CEVPlayer Code/SDKs/libvpx Code/SDKs/libwebm Code/SDKs/libogg Code/SDKs/libvorbis \
           Code/SDKs/libopus Code/CryPlugins Code/CryPlugins/CEVPlayer Code/Sandbox Code/Sandbox/Plugins \
           Code/Sandbox/Plugins/EditorCEVPlayer Tools Tools/CMake Tools/CMake/modules; do
  [ -L "$engine/$rel" ] && linked+=("$rel")
done
other_links="$(find "$engine/Code/SDKs" -mindepth 1 -maxdepth 1 -type l 2>/dev/null | wc -l)"
[ "$other_links" -gt 0 ] && [ "${#linked[@]}" -eq 0 ] && \
  say "note: $other_links folder(s) in Code/SDKs are junctions / links; sync_sources.sh does not touch them"
if [ "${#linked[@]}" -gt 0 ]; then
  msg="these paths are junctions / symbolic links: ${linked[*]}"$'\n'
  msg+="  sync_sources.sh replaces its destination folders as a whole (rm -rf, then copy), which can change the tree they"$'\n'
  msg+="  point into. Copy by hand instead (MANUAL.md, 'Build and integration' caveat): Code/CryPlugins/CEVPlayer,"$'\n'
  msg+="  Code/Sandbox/Plugins/EditorCEVPlayer, Code/SDKs/CEVPlayer and cmake/modules/*.cmake (-> Tools/CMake/modules/),"$'\n'
  msg+="  then run this script with --force-sync only if you accept the rm -rf on those paths."
  if [ "$force_sync" -eq 1 ]; then say "warning: $msg"; say "--force-sync given: continuing"; else problems+=("$msg"); fi
fi

if [ "${#problems[@]}" -gt 0 ]; then
  say "cannot continue:" >&2
  for p in "${problems[@]}"; do echo "  - $p" >&2; done
  if [ "$dry" -eq 0 ]; then exit 1; fi
  say "dry run: listing the plan anyway (a real run stops here)"
fi

# --- 3. steps -------------------------------------------------------------------------------------------------
win64="$engine/win64"
total=3
[ "$xui2dl" -eq 1 ] && total=$((total + 1))
[ "$build" -eq 1 ] && total=$((total + 1))
n=0

run_step() { # <log name> <title> <command...>
  local name="$1" title="$2" log="$win64/cevplayer_setup_$1.log" rc
  shift 2
  n=$((n + 1))
  say "[$n/$total] $title: start"
  "$@" > "$log" 2>&1
  rc=$?
  if [ "$rc" -ne 0 ]; then
    say "[$n/$total] $title: FAILED (exit $rc), log: $log" >&2
    tail -n 25 "$log" | sed 's/^/    /' >&2
    exit 1
  fi
  say "[$n/$total] $title: ok ($(grep -c '' "$log") log lines, $log)"
}

sln_target() { # <project name> -> MSBuild solution target (solution folder path with backslashes)
  awk -F'"' -v want="$1" '
    /^Project\(/ { g = $8; gsub(/[{}]/, "", g); name[g] = $4 }
    /GlobalSection\(NestedProjects\)/ { nested = 1; next }
    nested && /EndGlobalSection/ { nested = 0 }
    nested { split($0, a, " = "); c = a[1]; p = a[2]; gsub(/[{}\t \r]/, "", c); gsub(/[{}\t \r]/, "", p); parent[c] = p }
    END { for (g in name) if (name[g] == want) { s = name[g]; x = g; while (x in parent) { x = parent[x]; s = name[x] "\\" s }; print s; exit } }
  ' "$win64/CRYENGINE_Win64.sln"
}

projects=(CEVPlayer)
[ "$editor" -eq 1 ] && projects+=(EditorCEVPlayer)
[ "$xui2dl" -eq 1 ] && projects+=(CryRenderD3D11)

sandbox_opt="-DOPTION_SANDBOX=$([ "$editor" -eq 1 ] && echo ON || echo OFF)"
engine_m="$(cygpath -m "$engine")"
if [ -f "$win64/CMakeCache.txt" ]; then
  cmake_args=(-S "$engine_m" -B "$engine_m/win64" "$sandbox_opt")
  cmake_note="existing win64/ found: re-run with its generator settings"
else
  cmake_args=(-S "$engine_m" -B "$engine_m/win64" -G "Visual Studio 17 2022" -A x64
              "-DCMAKE_TOOLCHAIN_FILE=$engine_m/Tools/CMake/toolchain/windows/WindowsPC-MSVC.cmake"
              -DOPTION_ENGINE=ON "$sandbox_opt" -DOPTION_CRYMONO=OFF)
  cmake_note="no win64/ yet: first configure"
fi

xui2d_note=""
if [ "$xui2dl" -eq 1 ] && [ -d "$engine/Code/CryEngine/RenderDll/XUI2D" ]; then
  xui2d_note="this tree already has the XTFrame XUI2D stage: XUI2DL can still be applied, but the plug-in will use XUI2D"
fi

if [ "$dry" -eq 1 ]; then
  n=1; say "[$n/$total] sync sources: would run tools/sync_sources.sh $engine --dry-run"
  bash "$here/tools/sync_sources.sh" "$engine" --dry-run | sed 's/^/    /'
  n=$((n + 1)); say "[$n/$total] engine patches: would run tools/apply_patches.sh $engine$([ "$with_optional" -eq 1 ] && echo ' --with-optional')"
  if [ "$xui2dl" -eq 1 ]; then
    n=$((n + 1)); say "[$n/$total] XUI2DL: would run EnginePatch/apply_xui2dl.py $engine"
    [ -n "$xui2d_note" ] && say "    note: $xui2d_note"
    if [ "${#python[@]}" -gt 0 ]; then
      say "    current state (apply_xui2dl.py --check):"
      "${python[@]}" "$here/EnginePatch/apply_xui2dl.py" "$engine" --check 2>&1 | sed 's/^/      /'
    fi
  fi
  n=$((n + 1)); say "[$n/$total] CMake configure ($cmake_note): would run $cmake ${cmake_args[*]}"
  if [ "$build" -eq 1 ]; then
    n=$((n + 1)); say "[$n/$total] build: would run tools/build_engine.sh $engine $config <targets of: ${projects[*]}>"
    if [ -f "$win64/CRYENGINE_Win64.sln" ]; then
      for p in "${projects[@]}"; do t="$(sln_target "$p")"; say "    $p -> ${t:-(not in the current solution; resolved after the configure step)}"; done
    fi
  fi
  say "dry run done; logs would go to $win64/cevplayer_setup_<step>.log"
  [ "${#problems[@]}" -gt 0 ] && exit 1
  exit 0
fi

mkdir -p "$win64"
start=$(date +%s)
run_step 1-sync "sync sources" bash "$here/tools/sync_sources.sh" "$engine"
patch_args=("$engine"); [ "$with_optional" -eq 1 ] && patch_args+=(--with-optional)
run_step 2-patches "engine patches" bash "$here/tools/apply_patches.sh" "${patch_args[@]}"
if [ "$xui2dl" -eq 1 ]; then
  [ -n "$xui2d_note" ] && say "note: $xui2d_note"
  run_step 3-xui2dl "XUI2DL renderer patch" "${python[@]}" "$here/EnginePatch/apply_xui2dl.py" "$engine"
fi
say "CMake: $cmake_note"
run_step 4-cmake "CMake configure" "$cmake" "${cmake_args[@]}"

if [ "$build" -eq 0 ]; then
  say "--no-build: stopping after the configure step ($(( $(date +%s) - start )) s)"
  exit 0
fi

targets=()
for p in "${projects[@]}"; do
  t="$(sln_target "$p")"
  if [ -z "$t" ]; then
    say "project $p is not in $win64/CRYENGINE_Win64.sln (check the configure log $win64/cevplayer_setup_4-cmake.log)" >&2
    exit 1
  fi
  targets+=("$t")
done
say "building ${projects[*]} ($config); this can take a long time on the first build"
run_step 5-build "build" bash "$here/tools/build_engine.sh" "$engine" "$config" "${targets[@]}"
say "MSBuild log (minimal verbosity): $here/build/$(basename "$engine")-$config.log"

bin="$engine/bin/win_x64"; [ "$config" = "Release" ] && bin="$engine/bin/win_x64_release"
say "done in $(( $(date +%s) - start )) s. Built:"
dlls=("$bin/CEVPlayer.dll")
[ "$editor" -eq 1 ] && dlls+=("$bin/EditorPlugins/EditorCEVPlayer.dll")
[ "$xui2dl" -eq 1 ] && dlls+=("$bin/CryRenderD3D11.dll")
for d in "${dlls[@]}"; do
  if [ -f "$d" ]; then echo "  $d ($(date -r "$d" '+%Y-%m-%d %H:%M:%S'))"; else echo "  $d (NOT FOUND)"; fi
done
cat <<EOF
Next step: add the runtime plug-in to your project's .cryproject ("plugins" array):
  { "type": "EPluginType::Native", "path": "CEVPlayer" }
The Sandbox plug-in is loaded from bin/win_x64/EditorPlugins/ automatically.
EOF
