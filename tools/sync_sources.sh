#!/usr/bin/env bash
# Copy this repository's own source into a CRYENGINE source tree (one direction only: edit here,
# sync there). Run it before configuring the engine; tools/apply_patches.sh registers the plugin.
#
#   tools/sync_sources.sh <engine-root> [--dry-run]
#
# Synced (each destination directory is replaced as a whole, so stale files are removed; nothing
# outside these paths is touched):
#   Code/CryPlugins/CEVPlayer              (an older copy that CECE ships is replaced)
#   Code/Sandbox/Plugins/EditorCEVPlayer   (only if it exists in this repository)
#   Code/SDKs/CEVPlayer/{libvpx,libwebm,libogg,libvorbis,libopus}  (one parent folder; stale
#   pre-2026-11 copies directly under Code/SDKs/<lib> are removed)
# Single files (overwritten, other files in the destination directory are left alone):
#   cmake/modules/{CEVPlayerSDK,libvpx,libwebm,libogg,libvorbis,libopus}.cmake
#       -> <engine>/Tools/CMake/modules/
# (The modules live in cmake/ here, not Tools/CMake/: on Windows "Tools" and the existing "tools"
# script directory would be the same folder.)
# SDL2 is not synced: the engine's own Code/SDKs/SDL2 and Tools/CMake/modules/SDL2.cmake are used.
set -euo pipefail

usage() {
  echo "usage: sync_sources.sh <engine-root> [--dry-run]" >&2
  exit 2
}

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
engine=""
dry=0
for arg in "$@"; do
  case "$arg" in
    --dry-run) dry=1 ;;
    -h|--help) usage ;;
    -*) echo "sync_sources: unknown option $arg" >&2; usage ;;
    *) [ -z "$engine" ] && engine="$arg" || usage ;;
  esac
done
[ -n "$engine" ] || usage
[ -d "$engine/Code" ] && [ -d "$engine/Tools/CMake" ] || {
  echo "sync_sources: $engine does not look like a CRYENGINE source root (Code, Tools/CMake missing)" >&2
  exit 1
}
engine="$(cd "$engine" && pwd)"

sync_dir() { # <relative dir (same path here and in the engine)>
  local rel="$1" src="$here/$1" dst="$engine/$1"
  if [ ! -d "$src" ]; then echo "skip (missing here): $rel"; return; fi
  if [ "$dry" -eq 1 ]; then echo "would sync: $rel"; return; fi
  rm -rf "${dst:?}"
  mkdir -p "$(dirname "$dst")"
  cp -r "$src" "$dst"
  echo "synced: $rel ($(find "$dst" -type f | wc -l) files)"
}

sync_file() { # <path here relative to the repo> <path in the engine>
  local src="$here/$1" dst="$engine/$2"
  if [ ! -f "$src" ]; then echo "skip (missing here): $1"; return; fi
  if [ "$dry" -eq 1 ]; then echo "would sync: $1 -> $2"; return; fi
  mkdir -p "$(dirname "$dst")"
  cp -f "$src" "$dst"
  echo "synced: $2"
}

sync_dir "Code/CryPlugins/CEVPlayer"
sync_dir "Code/Sandbox/Plugins/EditorCEVPlayer"
sync_dir "Code/SDKs/CEVPlayer"
for sdk in libvpx libwebm libogg libvorbis libopus; do # old flat layout
  old="$engine/Code/SDKs/$sdk"
  if [ -f "$old/VERSION" ] && grep -q "CEVPlayer\|build_sdks" "$old/VERSION" 2>/dev/null; then
    if [ "$dry" -eq 1 ]; then echo "would remove old: Code/SDKs/$sdk"; else rm -rf "${old:?}"; echo "removed old: Code/SDKs/$sdk"; fi
  fi
done
for mod in CEVPlayerSDK libvpx libwebm libogg libvorbis libopus; do
  sync_file "cmake/modules/$mod.cmake" "Tools/CMake/modules/$mod.cmake"
done
echo "sync_sources: done -> $engine"
