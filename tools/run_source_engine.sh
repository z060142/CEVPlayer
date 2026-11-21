#!/usr/bin/env bash
# Start GameLauncher or Sandbox from an engine source tree built with tools/build_engine.sh, on a
# project that uses the CEVPlayer plugin. For the user to run (agents do not launch the engine).
#
#   tools/run_source_engine.sh <engine-root> [launcher|sandbox] [<project .cryproject>]
#   CRYENGINE_SOURCE=<engine-root> tools/run_source_engine.sh [launcher|sandbox] [<project .cryproject>]
# The project defaults to $CEV_PROJECT.
set -euo pipefail

usage() {
  echo "usage: run_source_engine.sh [<engine-root>] [launcher|sandbox] [<project .cryproject>]" >&2
  echo "       (<engine-root> may also come from \$CRYENGINE_SOURCE; the project defaults to \$CEV_PROJECT)" >&2
  exit 2
}

case "${1:-}" in -h|--help) usage ;; esac
engine="${CRYENGINE_SOURCE:-}"
if [ "$#" -gt 0 ] && [ -d "$1" ]; then engine="$1"; shift; fi
[ -n "$engine" ] || usage
what="${1:-launcher}"
project="${2:-${CEV_PROJECT:-}}"
[ -n "$project" ] || { echo "run_source_engine: give a .cryproject (argument or \$CEV_PROJECT)" >&2; usage; }

engine="$(cd "$engine" && pwd)"
bin="$engine/bin/win_x64"
case "$what" in
  launcher) exe="$bin/GameLauncher.exe" ;;
  sandbox)  exe="$bin/Sandbox.exe" ;;
  *) usage ;;
esac
[ -f "$exe" ] || { echo "run_source_engine: $exe not built (tools/build_engine.sh)" >&2; exit 1; }
[ -f "$project" ] || { echo "run_source_engine: project $project not found" >&2; exit 1; }
cd "$bin"
exec "$exe" -project "$(cygpath -w "$project")"
