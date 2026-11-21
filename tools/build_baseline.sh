#!/usr/bin/env bash
# M0 baseline build wrapper (adapted from XTFrame-CRYENGINE/tools/build_engine.sh).
#   CRYENGINE_SOURCE=<tree> LOGTAG=<A|B> tools/build_baseline.sh [Profile] [msbuild target ...]
# Differences: uses tools/msbuild/baseline_compat.props (ATL/MFC path, STL compat) via the
# ForceImportBeforeCppTargets property AND environment variable (nested RC ExternalProject build);
# log goes to D:/coding11/_buildlogs/$LOGTAG-engine-<config>.log.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
config="${1:-Profile}"; shift || true
engine="${CRYENGINE_SOURCE:?set CRYENGINE_SOURCE}"
tag="${LOGTAG:?set LOGTAG}"
sln="$engine/win64/CRYENGINE_Win64.sln"
vswhere="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
msbuild="$("$vswhere" -latest -requires Microsoft.Component.MSBuild -find 'MSBuild/**/Bin/MSBuild.exe' | head -n1 | tr -d '\r')"
props="$(cygpath -w "$here/msbuild/baseline_compat.props")"
export ForceImportBeforeCppTargets="$props"
log="D:/coding11/_buildlogs/$tag-engine-$config.log"
targets=(); if [ "$#" -gt 0 ]; then joined="$(IFS=';'; echo "$*")"; targets=("-t:$joined"); fi
start=$(date +%s)
"$msbuild" "$(cygpath -w "$sln")" "${targets[@]}" -p:Configuration="$config" -p:Platform=x64 -m ${MSB_EXTRA:-} -nologo -v:m \
  -p:ForceImportBeforeCppTargets="$props" "-clp:Summary;ErrorsOnly" "-flp:LogFile=$(cygpath -w "$log");Verbosity=minimal" \
  || { echo "FAILED after $(( $(date +%s) - start )) s"; exit 1; }
echo "done in $(( $(date +%s) - start )) s"
