#!/usr/bin/env bash
# build_sdks.sh - regenerate Code/SDKs/CEVPlayer/{libvpx,libwebm,libogg,libvorbis,libopus}
# prebuilt Win64 static libraries for the CEVPlayer CRYENGINE 5.7.1 plugin.
#
# Run from Git Bash on Windows 11 with VS 2022 (C++ workload), system CMake >= 3.20,
# git, curl and tar on PATH. Everything is fetched at pinned versions and checked
# against pinned SHA-256 hashes.
#
# CRT: CRYENGINE-MSVC.cmake uses /MDd for Debug and /MD for Profile and Release
# (Tools/CMake/CRYENGINE-MSVC.cmake). Libraries are therefore built as static libs
# with the DLL CRT: Release -> lib/win64/release (also used for Profile),
# Debug -> lib/win64/debug.  (Modules that call force_static_crt() use /MT[d];
# these libs are NOT compatible with such modules.)
#
# Environment overrides:
#   TOOLS_DIR   scratch/tool root          (default D:/coding11/_tools)
#   VCPKG_ROOT  vcpkg checkout             (default $TOOLS_DIR/vcpkg)
#   WORK        build work dir             (default $TOOLS_DIR/sdkbuild)
#   DL          download cache             (default $TOOLS_DIR/dl)
set -euo pipefail
export MSYS2_ARG_CONV_EXCL="*"   # stop Git Bash from rewriting /Z7 etc. into paths

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDKS="$ROOT/Code/SDKs/CEVPlayer"
TOOLS_DIR="${TOOLS_DIR:-D:/coding11/_tools}"
VCPKG_ROOT="${VCPKG_ROOT:-$TOOLS_DIR/vcpkg}"
WORK="${WORK:-$TOOLS_DIR/sdkbuild}"
DL="${DL:-$TOOLS_DIR/dl}"
JOBS="${JOBS:-8}"
# STEPS: subset of "ogg vorbis opus webm vpx" to (re)build; default all.
STEPS="${STEPS:-ogg vorbis opus webm vpx}"
want() { [[ " $STEPS " == *" $1 "* ]]; }

# ---- pinned versions --------------------------------------------------------
OGG_VER=1.3.6
OGG_URL="https://downloads.xiph.org/releases/ogg/libogg-$OGG_VER.tar.xz"
OGG_SHA256=5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061

VORBIS_VER=1.3.7
VORBIS_URL="https://downloads.xiph.org/releases/vorbis/libvorbis-$VORBIS_VER.tar.xz"
VORBIS_SHA256=b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b

OPUS_VER=1.6.1
OPUS_URL="https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz"
OPUS_SHA256=6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1

WEBM_VER=1.0.0.32
WEBM_TAG="libwebm-$WEBM_VER"
WEBM_URL="https://github.com/webmproject/libwebm/archive/refs/tags/$WEBM_TAG.tar.gz"
WEBM_SHA256=7fd5e085bda9f8031cf2ad2a1e52d9b7b29cba9c0b96ad2ce794ce89e4249eb8

VPX_VER=1.17.0
VPX_URL="https://github.com/webmproject/libvpx/archive/refs/tags/v$VPX_VER.tar.gz"
VPX_SHA256=1020f184046187baa2985dbde38e0691f49c44088bca7a1842b0236c6081dc0a
# vcpkg's own libvpx port is 1.16.0; we overlay it with 1.17.0 (hash = vcpkg SHA512 of the tarball).
VPX_SHA512=be98d1b913575e538fefefe657c9944df85118fe2d0558361495a72c75ac6a2251048cb645fca49e60d6941c823a923f31d4ebc4de9b778008c267c42805b339
VCPKG_COMMIT=8f363bdc11359ede4bc51e5eb32a2617538049c8
VCPKG_TRIPLET=x64-windows-static-md   # static libs, /MD (+/MDd for debug)

CRT_NOTE="/MD (release, also Profile) and /MDd (debug), static library, x64, VS2022 v143"
REL_FLAGS="/Ox /GS- /DNDEBUG /D_RELEASE"
DBG_FLAGS="/Z7 /Od /Ob0 /Oy- /RTC1 /GS /DDEBUG /D_DEBUG"

# ---- helpers ----------------------------------------------------------------
log() { printf '\n=== %s ===\n' "$*"; }

fetch() { # url sha256 -> prints path
  local url="$1" sha="$2" f="$DL/$(basename "$1")"
  mkdir -p "$DL"
  case "$url" in *github.com*/archive/*) f="$DL/$(echo "$url" | sed 's#.*github.com/[^/]*/\([^/]*\)/archive/refs/tags/\(.*\)#\1-\2#')";; esac
  if [ ! -f "$f" ]; then curl -fsSL -o "$f" "$url"; fi
  echo "$sha *$f" | sha256sum -c - >&2
  echo "$f"
}

extract() { # archive destdir -> prints top-level dir
  local a="$1" d="$2"
  if [ ! -d "$d" ]; then mkdir -p "$d.tmp" && tar --force-local -xf "$a" -C "$d.tmp" && mv "$d.tmp"/* "$d" && rmdir "$d.tmp"; fi
  echo "$d"
}

# cmake_lib <name> <srcdir> <extra cmake args...>
# builds Debug and Release in separate trees, installs to $WORK/stage/<name>-<cfg>
cmake_lib() {
  local name="$1" src="$2"; shift 2
  local cfg
  for cfg in Debug Release; do
    local bd="$WORK/build/$name-$cfg" st="$WORK/stage/$name-$cfg" flags
    if [ "$cfg" = Debug ]; then flags="$DBG_FLAGS"; else flags="$REL_FLAGS"; fi
    rm -rf "$bd" "$st"
    cmake -S "$src" -B "$bd" -G "Visual Studio 17 2022" -A x64 \
      -DCMAKE_POLICY_DEFAULT_CMP0091=NEW \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>DLL' \
      -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_C_FLAGS_DEBUG="$DBG_FLAGS" -DCMAKE_CXX_FLAGS_DEBUG="$DBG_FLAGS" \
      -DCMAKE_C_FLAGS_RELEASE="$REL_FLAGS" -DCMAKE_CXX_FLAGS_RELEASE="$REL_FLAGS" \
      -DCMAKE_INSTALL_PREFIX="$st" \
      -DCMAKE_PREFIX_PATH="$WORK/stage/ogg-$cfg" \
      "$@" >"$WORK/$name-$cfg.configure.log" 2>&1 || { tail -30 "$WORK/$name-$cfg.configure.log"; exit 1; }
    cmake --build "$bd" --config "$cfg" --target install -- -m:"$JOBS" -v:m \
      >"$WORK/$name-$cfg.build.log" 2>&1 || { tail -40 "$WORK/$name-$cfg.build.log"; exit 1; }
  done
}

# publish_libs <sdkname> <stage-name> <lib files...>  (copy .lib from stage libdirs)
publish_libs() {
  local sdk="$1" st="$2"; shift 2
  local dest="$SDKS/$sdk/lib/win64" f
  rm -rf "$dest"; mkdir -p "$dest/release" "$dest/debug"
  for f in "$@"; do
    cp "$WORK/stage/$st-Release/lib/$f" "$dest/release/$f"
    cp "$WORK/stage/$st-Debug/lib/$f"   "$dest/debug/$f"
  done
}

write_version() { # sdk text...
  local sdk="$1"; shift
  printf '%s\n' "$@" > "$SDKS/$sdk/VERSION"
}

common_ver_tail() {
  cat <<EOF
Target:   Windows x64, MSVC (Visual Studio 2022 v143), static library
CRT:      $CRT_NOTE
Layout:   lib/win64/release = Release + Profile builds, lib/win64/debug = Debug builds
Built by: tools/build_sdks.sh
EOF
}

# ---- 0. sanity --------------------------------------------------------------
command -v cmake >/dev/null || { echo "cmake not found"; exit 1; }
command -v curl  >/dev/null || { echo "curl not found";  exit 1; }
mkdir -p "$WORK" "$DL" "$SDKS"
WORK="$(cd "$WORK" && pwd -W 2>/dev/null || pwd)"; WORK="${WORK//\\//}"
SRC="$WORK/src"; mkdir -p "$SRC"

if want ogg; then
# ---- 1. libogg --------------------------------------------------------------
log "libogg $OGG_VER"
OGG_SRC="$(extract "$(fetch "$OGG_URL" "$OGG_SHA256")" "$SRC/libogg-$OGG_VER")"
cmake_lib ogg "$OGG_SRC" -DINSTALL_DOCS=OFF -DINSTALL_PKG_CONFIG_MODULE=OFF -DBUILD_TESTING=OFF
rm -rf "$SDKS/libogg/include"; mkdir -p "$SDKS/libogg/include"
cp -r "$WORK/stage/ogg-Release/include/ogg" "$SDKS/libogg/include/ogg"
publish_libs libogg ogg ogg.lib
cp "$OGG_SRC/COPYING" "$SDKS/libogg/LICENSE"
write_version libogg "libogg $OGG_VER" "Source:   $OGG_URL" "SHA-256:  $OGG_SHA256 (release tarball, upstream tag v$OGG_VER)" \
  "Build:    CMake (VS2022 generator, -DBUILD_SHARED_LIBS=OFF), Release flags: $REL_FLAGS, Debug flags: $DBG_FLAGS" "$(common_ver_tail)"

fi

if want vorbis; then
# ---- 2. libvorbis -----------------------------------------------------------
log "libvorbis $VORBIS_VER"
VORBIS_SRC="$(extract "$(fetch "$VORBIS_URL" "$VORBIS_SHA256")" "$SRC/libvorbis-$VORBIS_VER")"
cmake_lib vorbis "$VORBIS_SRC" -DINSTALL_CMAKE_PACKAGE_MODULE=OFF
rm -rf "$SDKS/libvorbis/include"; mkdir -p "$SDKS/libvorbis/include"
cp -r "$WORK/stage/vorbis-Release/include/vorbis" "$SDKS/libvorbis/include/vorbis"
publish_libs libvorbis vorbis vorbis.lib vorbisenc.lib vorbisfile.lib
cp "$VORBIS_SRC/COPYING" "$SDKS/libvorbis/LICENSE"
write_version libvorbis "libvorbis $VORBIS_VER" "Source:   $VORBIS_URL" "SHA-256:  $VORBIS_SHA256 (release tarball, upstream tag v$VORBIS_VER)" \
  "Build:    CMake (VS2022 generator, -DBUILD_SHARED_LIBS=OFF), Release flags: $REL_FLAGS, Debug flags: $DBG_FLAGS" \
  "Depends:  libogg (include/ogg from Code/SDKs/CEVPlayer/libogg)" "$(common_ver_tail)"

fi

if want opus; then
# ---- 3. libopus -------------------------------------------------------------
log "libopus $OPUS_VER"
OPUS_SRC="$(extract "$(fetch "$OPUS_URL" "$OPUS_SHA256")" "$SRC/opus-$OPUS_VER")"
cmake_lib opus "$OPUS_SRC" -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF \
  -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF -DBUILD_TESTING=OFF
rm -rf "$SDKS/libopus/include"; mkdir -p "$SDKS/libopus/include"
cp -r "$WORK/stage/opus-Release/include/opus" "$SDKS/libopus/include/opus"
publish_libs libopus opus opus.lib
cp "$OPUS_SRC/COPYING" "$SDKS/libopus/LICENSE"
write_version libopus "libopus $OPUS_VER" "Source:   $OPUS_URL" "SHA-256:  $OPUS_SHA256 (release tarball, upstream tag v$OPUS_VER)" \
  "Build:    CMake (VS2022 generator, -DOPUS_BUILD_SHARED_LIBRARY=OFF, default x86 intrinsics with runtime CPU detection), Release flags: $REL_FLAGS, Debug flags: $DBG_FLAGS" \
  "$(common_ver_tail)"

fi

if want webm; then
# ---- 4. libwebm -------------------------------------------------------------
log "libwebm $WEBM_VER"
WEBM_SRC="$(extract "$(fetch "$WEBM_URL" "$WEBM_SHA256")" "$SRC/libwebm-$WEBM_VER")"
cmake_lib webm "$WEBM_SRC" -DMSVC_RUNTIME=dll -DENABLE_WEBMTS=OFF -DENABLE_WEBMINFO=OFF \
  -DENABLE_SAMPLE_PROGRAMS=OFF -DENABLE_TESTS=OFF -DENABLE_WEBM_PARSER=OFF
# Headers keep the upstream include paths (#include "mkvparser/mkvparser.h").
rm -rf "$SDKS/libwebm/include"; mkdir -p "$SDKS/libwebm/include"
for d in mkvparser mkvmuxer common; do
  mkdir -p "$SDKS/libwebm/include/$d"
  cp "$WEBM_SRC/$d"/*.h "$SDKS/libwebm/include/$d/"
done
publish_libs libwebm webm webm.lib
cp "$WEBM_SRC/LICENSE.TXT" "$SDKS/libwebm/LICENSE"
cp "$WEBM_SRC/PATENTS.TXT" "$SDKS/libwebm/PATENTS"
write_version libwebm "libwebm $WEBM_VER (git tag $WEBM_TAG)" "Source:   $WEBM_URL" "SHA-256:  $WEBM_SHA256" \
  "Build:    CMake (VS2022 generator, -DMSVC_RUNTIME=dll, webm.lib = mkvparser + mkvmuxer + common), Release flags: $REL_FLAGS, Debug flags: $DBG_FLAGS" \
  "Headers:  include/mkvparser, include/mkvmuxer, include/common (upstream source-relative include paths)" "$(common_ver_tail)"

fi

if want vpx; then
# ---- 5. libvpx (via vcpkg, which supplies msys2 make/perl + nasm) -----------
log "libvpx $VPX_VER (vcpkg @ ${VCPKG_COMMIT:0:12})"
VPX_SRC="$(extract "$(fetch "$VPX_URL" "$VPX_SHA256")" "$SRC/libvpx-$VPX_VER")"
if [ ! -d "$VCPKG_ROOT/.git" ]; then
  mkdir -p "$VCPKG_ROOT"; git -C "$VCPKG_ROOT" init -q
  git -C "$VCPKG_ROOT" remote add origin https://github.com/microsoft/vcpkg.git
fi
if [ "$(git -C "$VCPKG_ROOT" rev-parse HEAD 2>/dev/null || true)" != "$VCPKG_COMMIT" ]; then
  git -C "$VCPKG_ROOT" fetch -q --depth 1 origin "$VCPKG_COMMIT"
  git -C "$VCPKG_ROOT" checkout -q FETCH_HEAD
fi
if [ ! -f "$VCPKG_ROOT/vcpkg.exe" ]; then
  (cd "$VCPKG_ROOT" && powershell -NoProfile -Command ".\\bootstrap-vcpkg.bat -disableMetrics")
fi
# Overlay port: vcpkg's libvpx port bumped to $VPX_VER, decoders only.
OVERLAY="$WORK/vcpkg-overlay"; rm -rf "$OVERLAY"; mkdir -p "$OVERLAY"
cp -r "$VCPKG_ROOT/ports/libvpx" "$OVERLAY/libvpx"
sed -i -E "s/SHA512 [0-9a-f]+/SHA512 $VPX_SHA512/; s/--disable-dependency-tracking\)/--disable-dependency-tracking --disable-unit-tests --disable-vp8-encoder --disable-vp9-encoder)/" "$OVERLAY/libvpx/portfile.cmake"
grep -q -- '--disable-vp9-encoder' "$OVERLAY/libvpx/portfile.cmake" || { echo "overlay edit failed"; exit 1; }
sed -i -E 's/"version": "[0-9.]+"/"version": "'"$VPX_VER"'"/; /"port-version"/d' "$OVERLAY/libvpx/vcpkg.json"
# Make the version-ful REF robust: port uses v${VERSION}
"$VCPKG_ROOT/vcpkg.exe" remove "libvpx:$VCPKG_TRIPLET" --disable-metrics >/dev/null 2>&1 || true
"$VCPKG_ROOT/vcpkg.exe" install "libvpx:$VCPKG_TRIPLET" --overlay-ports="$OVERLAY" --disable-metrics \
  --x-buildtrees-root="$WORK/vcpkg-bt" --clean-after-build
VI="$VCPKG_ROOT/installed/$VCPKG_TRIPLET"
rm -rf "$SDKS/libvpx/include" "$SDKS/libvpx/lib"
mkdir -p "$SDKS/libvpx/include" "$SDKS/libvpx/lib/win64/release" "$SDKS/libvpx/lib/win64/debug"
cp -r "$VI/include/vpx" "$SDKS/libvpx/include/vpx"
cp "$VI/lib/vpx.lib"       "$SDKS/libvpx/lib/win64/release/vpx.lib"
cp "$VI/debug/lib/vpx.lib" "$SDKS/libvpx/lib/win64/debug/vpx.lib"
cp "$VPX_SRC/LICENSE" "$SDKS/libvpx/LICENSE"
cp "$VPX_SRC/PATENTS" "$SDKS/libvpx/PATENTS"
cp "$VPX_SRC/third_party/x86inc/LICENSE" "$SDKS/libvpx/LICENSE-x86inc"
write_version libvpx "libvpx $VPX_VER (git tag v$VPX_VER)" "Source:   $VPX_URL" "SHA-256:  $VPX_SHA256" \
  "Build:    vcpkg (commit $VCPKG_COMMIT) overlay port, triplet $VCPKG_TRIPLET, MSBuild project generated by libvpx configure" \
  "Options:  --disable-examples --disable-tools --disable-docs --enable-pic --disable-unit-tests --disable-vp8-encoder --disable-vp9-encoder" \
  "          (VP8 + VP9 decoders only; assembly via NASM; vp9 highbitdepth OFF)" \
  "Headers:  include/vpx (vpx_decoder.h, vp8dx.h, ...)" "$(common_ver_tail)"
fi

log "done"
du -sh "$SDKS"/*
