#!/usr/bin/env bash
# Packs the CEV video files of a project into a pak with the engine's Resource Compiler, WITHOUT the editor-only files.
#
#   tools/pak_cev_assets.sh <engine-root> <Assets-folder> <out.pak> [extra rc.exe arguments]
#
# Why it exists (CRYENGINE 5.7.1, Tools/CryVersionSelector/rc_jobs/game_assets.xml):
#   * the stock game packaging job only copies / converts known extensions (AllExtensions, XmlExtensions, ...). .cevvideo,
#     .webm and .wav are not among them, so a stock pak build contains none of the videos. Pack them with this script (or
#     put the same Job into your own copy of game_assets.xml) and ship the result next to the other pak files.
#   * there is no built-in switch for "do not ship the source file of an asset". rc.exe has /exclude= (wildcards) and
#     /exclude_listfile= (exact paths, relative to the source root); this script feeds both:
#       - tools/pak_exclude_sources.txt          wildcard patterns (*.cryasset, .cevpreview/, ...)
#       - tools/list_unshipped_cev_files.py      the exact source copies / WAV / Ogg exports of every video asset
#   * packed: *.cevvideo, *.webm (the video files the descriptors play), and for the cue route the SDL_mixer copies and the
#     ACE libraries (audio/sdlmixer/assets/cev, audio/sdlmixer/ace/cev_*.xml).
set -euo pipefail

usage() { sed -n 2,/^set\ -euo/p "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2; exit 2; }
[ $# -ge 3 ] || usage
engine="$1"; assets="$2"; out="$3"; shift 3
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
rc="$engine/Tools/rc/rc.exe"
[ -x "$rc" ] || { echo "pak_cev_assets: $rc not found" >&2; exit 1; }
[ -d "$assets" ] || { echo "pak_cev_assets: $assets is not a folder" >&2; exit 1; }
py=""
for cand in python py python3; do   # python3 on Windows can be the Microsoft Store stub: only take one that runs
  if command -v "$cand" > /dev/null 2>&1 && "$cand" -c "import sys; sys.exit(0 if sys.version_info[0] >= 3 else 1)" > /dev/null 2>&1; then py="$cand"; break; fi
done
[ -n "$py" ] || { echo "pak_cev_assets: python 3 not found (needed for list_unshipped_cev_files.py)" >&2; exit 1; }

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
"$py" "$here/list_unshipped_cev_files.py" "$(cygpath -w "$assets")" --listfile "$(cygpath -w "$tmp/unshipped.txt")"
patterns="$(grep -v '^[[:space:]]*\(#\|$\)' "$here/pak_exclude_sources.txt" | tr -d '\r' | paste -sd';' -)"

mkdir -p "$(dirname "$out")"
( cd "$engine/Tools/rc" && ./rc.exe "*.cevvideo;*.webm;audio/sdlmixer/assets/cev/*.*;audio/sdlmixer/ace/cev_*.xml" \
    /sourceroot="$(cygpath -w "$assets")" /zip="$(cygpath -w "$out")" /zip_compression=0 \
    /exclude="$patterns" /exclude_listfile="$(cygpath -w "$tmp/unshipped.txt")" "$@" )
echo "pak_cev_assets: wrote $out (excluded: $(wc -l < "$tmp/unshipped.txt") per-asset file(s) + patterns $patterns)"
