#!/usr/bin/env bash
# Apply the CEVPlayer engine patch series (patches/) to a CRYENGINE source tree.
#
#   tools/apply_patches.sh <engine-root> [--force] [--with-optional] [--detect]
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
# Optional patches (patches/optional/NN-*.patch, community override patches/community/optional/NN-*.patch) are NOT part
# of the series: they are only applied with --with-optional (the plug-in works without them and falls back at run
# time). Their files are listed in BASE_FILES too, and an already applied optional patch never makes the tree unknown.
#
# Idempotent: a patch that is already applied ("git apply --reverse --check" succeeds) is skipped.
# Nothing is committed; the patches only modify the working tree.
# --detect: only identify the tree (prints the variant, exit 0; unknown tree: the reasons, exit 1), write nothing.
set -euo pipefail

usage() {
  echo "usage: apply_patches.sh <engine-root> [--force] [--with-optional] [--detect]" >&2
  echo "  Applies patches/*.patch (vanilla 5.7.1) or the community series (CECE) to <engine-root>." >&2
  echo "  --with-optional  also applies patches/optional/*.patch (community: patches/community/optional/)." >&2
  echo "  --detect         only identifies the tree (vanilla / community), writes nothing." >&2
  exit 2
}

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
engine=""
force=0
with_optional=0
detect=0
for arg in "$@"; do
  case "$arg" in
    --force) force=1 ;;
    --with-optional) with_optional=1 ;;
    --detect) detect=1 ;;
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

# Optional series of a variant: patches/optional/NN-*.patch, community overrides main per number.
optional_series() { # variant
  local variant="$1" p num c
  for p in "$here"/patches/optional/[0-9][0-9]-*.patch; do
    num="$(basename "$p")"; num="${num%%-*}"
    if [ "$variant" = "community" ]; then
      for c in "$here"/patches/community/optional/"$num"-*.patch; do p="$c"; break; done
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
  done < <(series "$variant"; optional_series "$variant")
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
[ "$detect" -eq 1 ] && exit 0

applied=0 skipped=0
apply_list() { # patch paths on stdin
  local p name
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
  done
}
apply_list < <(series "$variant")

if [ "$with_optional" -eq 1 ]; then
  apply_list < <(optional_series "$variant")
else
  while IFS= read -r p; do
    [ -n "$p" ] || continue
    if is_applied "$p"; then echo "optional, applied earlier (left as is): ${p#"$here/patches/"}"; else echo "optional, not applied (use --with-optional): ${p#"$here/patches/"}"; fi
  done < <(optional_series "$variant")
fi
echo "apply_patches: done ($applied applied, $skipped already applied)"
