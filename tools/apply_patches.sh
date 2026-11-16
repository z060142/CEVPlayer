#!/usr/bin/env bash
# Apply the CEVPlayer engine patch series (patches/) to a CRYENGINE source tree.
#
#   tools/apply_patches.sh <engine-root> [--force]
#
# Two engine trees are supported, each with its own series:
#   vanilla    CRYENGINE 5.7.1            patches/NN-*.patch
#   community  CECE (community fork)      patches/community/NN-*.patch where it exists,
#                                         otherwise the main patches/NN-*.patch of that number
# The tree is identified by patches/BASE_FILES.<variant> ("<blob sha1>  <path>" for every file the
# series touches, hashed with CR characters removed so CRLF/LF checkouts hash the same). A file is
# accepted when it still has its base hash, or when one of the patches touching it is already applied.
# Unknown tree: refused, unless --force (then the variant whose series applies cleanly is used).
#
# Idempotent: a patch that is already applied ("git apply --reverse --check" succeeds) is skipped.
# Nothing is committed; the patches only modify the working tree.
set -euo pipefail

usage() {
  echo "usage: apply_patches.sh <engine-root> [--force]" >&2
  echo "  Applies patches/*.patch (vanilla 5.7.1) or the community series (CECE) to <engine-root>." >&2
  exit 2
}

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
engine=""
force=0
for arg in "$@"; do
  case "$arg" in
    --force) force=1 ;;
    -h|--help) usage ;;
    -*) echo "apply_patches: unknown option $arg" >&2; usage ;;
    *) [ -z "$engine" ] && engine="$arg" || usage ;;
  esac
done
[ -n "$engine" ] || usage
[ -d "$engine/Code/CryPlugins" ] && [ -d "$engine/Tools/CMake" ] || {
  echo "apply_patches: $engine does not look like a CRYENGINE source root (Code/CryPlugins, Tools/CMake missing)" >&2
  exit 1
}
engine="$(cd "$engine" && pwd)"
shopt -s nullglob

blob_hash() { # path -> blob sha1 with CRs removed
  tr -d '\r' < "$1" | git hash-object --stdin
}

patch_files() { # patch -> touched paths (one per line)
  sed -n 's/^+++ b\/\(.*\)$/\1/p' "$1" | tr -d '\r'
}

# Series of a variant: one patch file per number NN, community overrides main.
series() { # variant
  local variant="$1" p num c
  for p in "$here"/patches/[0-9][0-9]-*.patch; do
    num="$(basename "$p")"; num="${num%%-*}"
    if [ "$variant" = "community" ]; then
      for c in "$here"/patches/community/"$num"-*.patch; do p="$c"; break; done
    fi
    echo "$p"
  done
}

is_applied() { git -C "$engine" apply --reverse --check "$1" >/dev/null 2>&1; }

# Does the tree match the variant's BASE_FILES?
matches_variant() { # variant -> 0/1
  local variant="$1" list="$here/patches/BASE_FILES.$1" hash path p f
  [ -f "$list" ] || return 1
  local applied_files=""
  while IFS= read -r p; do
    [ -n "$p" ] || continue
    if is_applied "$p"; then applied_files+="$(patch_files "$p")"$'\n'; fi
  done < <(series "$variant")
  while read -r hash path; do
    case "$hash" in ''|'#'*) continue ;; esac
    [ -f "$engine/$path" ] || { why+="  [$variant] missing: $path"$'
'; return 1; }
    if [ "$(blob_hash "$engine/$path")" = "$hash" ]; then continue; fi
    if printf '%s' "$applied_files" | grep -qxF "$path"; then continue; fi
    why+="  [$variant] differs from base: $path"$'
'
    return 1
  done < "$list"
  return 0
}

variant=""
why=""
for v in vanilla community; do
  if matches_variant "$v"; then variant="$v"; break; fi
done

if [ -z "$variant" ]; then
  if [ "$force" -ne 1 ]; then
    echo "apply_patches: $engine is not a known base (vanilla 5.7.1 / community):" >&2
    printf '%s' "$why" >&2
    echo "apply_patches: pass --force to try anyway (the variant whose patches apply cleanly is used)." >&2
    exit 1
  fi
  for v in vanilla community; do
    ok=1
    while IFS= read -r p; do
      [ -n "$p" ] || continue
      if is_applied "$p" || git -C "$engine" apply --check "$p" >/dev/null 2>&1; then :; else ok=0; break; fi
    done < <(series "$v")
    if [ "$ok" -eq 1 ]; then variant="$v"; break; fi
  done
  [ -n "$variant" ] || { echo "apply_patches: --force: no series applies cleanly to $engine" >&2; exit 1; }
  echo "apply_patches: --force: unknown base, using the $variant series"
fi

echo "apply_patches: $engine -> $variant ($(git -C "$engine" rev-parse --short HEAD 2>/dev/null || echo 'not a git repo'))"

applied=0 skipped=0
while IFS= read -r p; do
  [ -n "$p" ] || continue
  name="${p#"$here/patches/"}"
  if is_applied "$p"; then
    echo "already applied: $name"
    skipped=$((skipped + 1))
    continue
  fi
  if git -C "$engine" apply --check "$p"; then
    git -C "$engine" apply "$p"
    echo "applied: $name"
    applied=$((applied + 1))
  else
    echo "apply_patches: $name does not apply cleanly to $engine" >&2
    echo "apply_patches: (the patches use CRLF like the engine files; an LF checkout will not match)" >&2
    exit 1
  fi
done < <(series "$variant")
echo "apply_patches: done ($applied applied, $skipped already applied)"
