#!/usr/bin/env bash
# Copy the built CEVPlayer binaries from an engine build tree into an installed engine's bin/win_x64.
# Every overwritten file is backed up once as <name>.pre-cevplayer (never overwritten again), so the
# stock files can be restored with --restore.
#
#   tools/deploy.sh <built bin dir (<engine>/bin/win_x64)> <installed bin dir> [--restore]
#
# Deployed: CEVPlayer*.dll (CEVPlayer.dll) and, when present, EditorPlugins/EditorCEVPlayer*.dll,
# plus the matching .pdb files. SDL2.dll is not deployed (the engine ships it).
set -euo pipefail

usage() {
  echo "usage: deploy.sh <built bin dir (<engine>/bin/win_x64)> <installed bin dir> [--restore]" >&2
  exit 2
}

build="${1:-}"
dest="${2:-}"
mode="${3:-}"
case "$build" in -h|--help|'') usage ;; esac
[ -n "$dest" ] || usage
[ -z "$mode" ] || [ "$mode" = "--restore" ] || usage

if [ "$mode" = "--restore" ]; then
  restored=0
  while IFS= read -r bak; do
    orig="${bak%.pre-cevplayer}"
    mv -f "$bak" "$orig"
    echo "restored: ${orig#"$dest"/}"
    restored=$((restored + 1))
  done < <(find "$dest" -name '*.pre-cevplayer' -type f 2>/dev/null)
  # Files that did not exist in the installed engine before the first deploy are removed again.
  while IFS= read -r marker; do
    orig="${marker%.pre-cevplayer.absent}"
    rm -f "$orig" "$marker"
    echo "removed: ${orig#"$dest"/}"
  done < <(find "$dest" -name '*.pre-cevplayer.absent' -type f 2>/dev/null)
  echo "deploy: restore done ($restored backups restored)"
  exit 0
fi

[ -d "$build" ] || { echo "deploy: build dir $build not found" >&2; exit 1; }
[ -d "$dest" ] || { echo "deploy: installed bin dir $dest not found" >&2; exit 1; }

shopt -s nullglob
count=0
for src in "$build"/CEVPlayer*.dll "$build"/CEVPlayer*.pdb \
           "$build"/EditorPlugins/EditorCEVPlayer*.dll "$build"/EditorPlugins/EditorCEVPlayer*.pdb; do
  rel="${src#"$build"/}"
  target="$dest/$rel"
  mkdir -p "$(dirname "$target")"
  # Back up the stock file once; remember (marker) when there was none, so --restore can delete ours.
  if [ ! -f "$target.pre-cevplayer" ] && [ ! -f "$target.pre-cevplayer.absent" ]; then
    if [ -f "$target" ]; then cp -p "$target" "$target.pre-cevplayer"; else : > "$target.pre-cevplayer.absent"; fi
  fi
  cp -p "$src" "$target"
  echo "deployed: $rel"
  count=$((count + 1))
done
[ "$count" -gt 0 ] || { echo "deploy: no CEVPlayer*.dll found in $build" >&2; exit 1; }
echo "deploy: done ($count files) -> $dest"
