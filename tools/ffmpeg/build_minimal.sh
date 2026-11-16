#!/usr/bin/env bash
# Builds the CEVPlayer minimal FFmpeg (win64, static ffmpeg.exe, LGPL) and packs it into
#   <out>/ffmpeg-minimal-<FFMPEG_VERSION>-cev<CEV_BUILD_REV>-win64.zip  (+ .sha256)
#
# Runs in an MSYS2 MINGW64 shell (the GitHub workflow .github/workflows/ffmpeg-minimal.yml does exactly this on
# windows-latest). Locally: install MSYS2, open the "MSYS2 MINGW64" shell and
#   pacman -S --needed base-devel mingw-w64-x86_64-toolchain mingw-w64-x86_64-nasm mingw-w64-x86_64-meson \
#             mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf zip curl xz tar
#   tools/ffmpeg/build_minimal.sh [--work <dir>] [--out <dir>] [--jobs <n>]
#
# Scope (docs/design/M5-ffmpeg.md): --disable-everything, then only
#   demuxers  mov/mp4, matroska/webm, avi, image2 (PNG sequence)
#   decoders  h264, hevc, prores (incl. 4444), vp8, vp9 (native + libvpx), av1 (libdav1d), png, aac, mp3, opus,
#             vorbis, flac, pcm
#   encoders  libvpx-vp9, libopus, libvorbis, pcm_s16le
#   muxers    webm, ogg, wav
#   filters   scale, format, aresample (+ what ffmpeg.c itself selects: aformat, null, anull, trim, ...)
# No GPL and no nonfree component (--disable-gpl --disable-nonfree --disable-version3): every library is BSD licensed,
# FFmpeg itself is LGPL-2.1-or-later. The script refuses to package a binary whose configuration says otherwise.
#
# Status: written without being able to run it in this repository's development session (no MSYS2 there). The
# component names were checked against the FFmpeg 8.0.3 sources; the first CI run may still need small fixes.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tools/ffmpeg/versions.env
source <(tr -d '\r' <"${HERE}/versions.env") # tolerate CRLF checkouts (core.autocrlf)

WORK="${CEV_WORK_DIR:-${PWD}/ffmpeg-build}"
OUT="${CEV_OUT_DIR:-${PWD}/ffmpeg-out}"
JOBS="${CEV_JOBS:-$(nproc 2>/dev/null || echo 2)}"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--work) WORK="$2"; shift 2 ;;
		--out) OUT="$2"; shift 2 ;;
		--jobs) JOBS="$2"; shift 2 ;;
		-h|--help) sed -n '2,22p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

PACKAGE_VERSION="${FFMPEG_VERSION}-cev${CEV_BUILD_REV}"
PACKAGE_NAME="ffmpeg-minimal-${PACKAGE_VERSION}-win64"

log() { printf '\n==== %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------------------------------------------
# environment
# ---------------------------------------------------------------------------------------------------------------
if [[ "${MSYSTEM:-}" != "MINGW64" ]]; then
	die "run this in an MSYS2 MINGW64 shell (MSYSTEM=${MSYSTEM:-unset})"
fi
for tool in gcc g++ make nasm meson ninja pkg-config curl tar xz sha256sum zip cygpath; do
	command -v "${tool}" >/dev/null 2>&1 || die "missing tool: ${tool} (see the pacman line at the top of this script)"
done

mkdir -p "${WORK}" "${OUT}"
WORK="$(cd "${WORK}" && pwd)"
OUT="$(cd "${OUT}" && pwd)"
DL="${WORK}/downloads"
SRC="${WORK}/src"
PREFIX="${WORK}/prefix"
mkdir -p "${DL}" "${SRC}" "${PREFIX}"
# Native tools get mixed mode paths (D:/a/...), the MSYS shell keeps its own.
PREFIX_M="$(cygpath -m "${PREFIX}")"
# Only our own prefix: nothing from the MSYS2 packages (e.g. a reference ffmpeg and its libraries) may leak in.
export PKG_CONFIG_LIBDIR="${PREFIX_M}/lib/pkgconfig"
unset PKG_CONFIG_PATH
export CFLAGS="-O2"
export LDFLAGS=""

# ---------------------------------------------------------------------------------------------------------------
# sources
# ---------------------------------------------------------------------------------------------------------------
fetch() { # name url sha256 -> $DL/<file>
	local name="$1" url="$2" sha="$3"
	local file="${DL}/${url##*/}"
	if [[ "${url}" == *"/archive/refs/tags/"* ]]; then
		file="${DL}/${name}-${url##*/}"
	fi
	if [[ -f "${file}" ]] && echo "${sha}  ${file}" | sha256sum -c --status -; then
		echo "${file}"
		return
	fi
	log "download ${name}: ${url}" >&2
	curl -fL --retry 4 --retry-delay 5 --connect-timeout 30 -o "${file}.part" "${url}" >&2
	if ! echo "${sha}  ${file}.part" | sha256sum -c --status -; then
		rm -f "${file}.part"
		die "SHA-256 mismatch for ${url} (expected ${sha})"
	fi
	mv "${file}.part" "${file}"
	echo "${file}"
}

unpack() { # name archive -> $SRC/<name> (single top level folder stripped)
	local name="$1" archive="$2"
	rm -rf "${SRC:?}/${name}"
	mkdir -p "${SRC}/${name}"
	tar -xf "${archive}" -C "${SRC}/${name}" --strip-components=1
}

log "fetch sources"
for dep in FFMPEG LIBVPX OPUS LIBOGG LIBVORBIS DAV1D; do
	url_var="${dep}_URL"; sha_var="${dep}_SHA256"
	archive="$(fetch "${dep,,}" "${!url_var}" "${!sha_var}")"
	unpack "${dep,,}" "${archive}"
done

# ---------------------------------------------------------------------------------------------------------------
# libraries (static, into $PREFIX)
# ---------------------------------------------------------------------------------------------------------------
build_autotools() { # name configure-args...
	local name="$1"; shift
	log "build ${name}"
	( cd "${SRC}/${name}" && ./configure --prefix="${PREFIX_M}" --disable-shared --enable-static "$@" \
		&& make -j"${JOBS}" && make install )
}

build_autotools libogg
build_autotools libvorbis --with-ogg="${PREFIX_M}" --disable-docs --disable-examples --disable-oggtest
build_autotools opus --disable-doc --disable-extra-programs

log "build dav1d"
(
	cd "${SRC}/dav1d"
	meson setup build --prefix="${PREFIX_M}" --libdir=lib --default-library=static --buildtype=release \
		-Denable_tools=false -Denable_tests=false -Denable_examples=false -Denable_docs=false
	ninja -C build
	ninja -C build install
)

log "build libvpx"
(
	cd "${SRC}/libvpx"
	./configure --prefix="${PREFIX_M}" --target=x86_64-win64-gcc --disable-shared --enable-static \
		--enable-vp8 --enable-vp9 --enable-runtime-cpu-detect \
		--disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-install-docs
	make -j"${JOBS}"
	make install
)

# ---------------------------------------------------------------------------------------------------------------
# ffmpeg
# ---------------------------------------------------------------------------------------------------------------
log "configure ffmpeg ${FFMPEG_VERSION}"
FFMPEG_CONFIGURE_ARGS=(
	--arch=x86_64
	--target-os=mingw32
	--pkg-config-flags=--static
	"--extra-cflags=-I${PREFIX_M}/include"
	"--extra-ldflags=-L${PREFIX_M}/lib -static"
	# license: LGPL only
	--disable-gpl --disable-nonfree --disable-version3
	# nothing the plugin does not need
	--disable-everything --disable-autodetect
	--disable-doc --disable-debug --disable-network
	--disable-ffplay --disable-ffprobe --disable-avdevice --disable-postproc
	--enable-ffmpeg
	# external libraries (all BSD)
	--enable-libvpx --enable-libopus --enable-libvorbis --enable-libdav1d
	# import: containers
	--enable-demuxer=mov,matroska,avi,image2
	# import: video / audio decoders
	--enable-decoder=h264,hevc,prores,vp8,vp9,libvpx_vp8,libvpx_vp9,libdav1d,png
	--enable-decoder=aac,mp3,mp3float,opus,vorbis,flac
	--enable-decoder=pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_u8,pcm_s16be,pcm_s24be
	# output
	--enable-encoder=libvpx_vp9,libopus,libvorbis,pcm_s16le
	--enable-muxer=webm,ogg,wav
	# parsers / bitstream filters the demuxers and decoders above use (vp9 selects its own superframe split)
	--enable-parser=h264,hevc,aac,aac_latm,mpegaudio,vp8,vp9,av1,opus,vorbis,flac,png
	--enable-bsf=null,vp9_superframe,vp9_superframe_split,extract_extradata
	# file input / output and `-progress pipe:1`
	--enable-protocol=file,pipe
	# filters: scale, format, aresample (ffmpeg.c selects aformat / null / anull / trim / atrim ... itself)
	--enable-filter=scale,format,aresample
)
mkdir -p "${WORK}/ffmpeg-build"
(
	cd "${WORK}/ffmpeg-build"
	"${SRC}/ffmpeg/configure" "${FFMPEG_CONFIGURE_ARGS[@]}"
	log "build ffmpeg"
	make -j"${JOBS}" ffmpeg.exe
)
FFMPEG_EXE="${WORK}/ffmpeg-build/ffmpeg.exe"
[[ -f "${FFMPEG_EXE}" ]] || die "ffmpeg.exe was not built"
strip --strip-unneeded "${FFMPEG_EXE}"

# ---------------------------------------------------------------------------------------------------------------
# verification of the result (license + scope)
# ---------------------------------------------------------------------------------------------------------------
log "verify"
VERSION_TEXT="$("${FFMPEG_EXE}" -hide_banner -version)"
printf '%s\n' "${VERSION_TEXT}"
if grep -Eq -- '--enable-(gpl|nonfree|version3)' <<<"${VERSION_TEXT}"; then
	die "the binary reports a GPL / nonfree / version3 configuration"
fi
LICENSE_LINE="$("${FFMPEG_EXE}" -hide_banner -L | head -n 3 | tr '\n' ' ')"
case "${LICENSE_LINE}" in
	*"GNU Lesser General Public License"*) ;;
	*) die "unexpected license text: ${LICENSE_LINE}" ;;
esac

need_list() { # kind pattern...
	local kind="$1"; shift
	local listing
	listing="$("${FFMPEG_EXE}" -hide_banner "-${kind}")"
	for item in "$@"; do
		grep -Eq "^ [A-Z. ]+ +(${item})( |,|\$)" <<<"${listing}" || die "missing ${kind}: ${item}"
	done
}
need_list encoders libvpx-vp9 libopus libvorbis pcm_s16le
need_list muxers webm ogg wav
need_list decoders h264 hevc prores vp8 vp9 libvpx-vp9 libdav1d png aac mp3 opus vorbis flac pcm_s16le
need_list demuxers 'mov,mp4[^ ]*' 'matroska,webm' avi image2

# ---------------------------------------------------------------------------------------------------------------
# package: ffmpeg.exe + licenses + how it was built
# ---------------------------------------------------------------------------------------------------------------
log "package ${PACKAGE_NAME}"
STAGE="${WORK}/stage/${PACKAGE_NAME}"
rm -rf "${WORK}/stage"
mkdir -p "${STAGE}/licenses"
cp "${FFMPEG_EXE}" "${STAGE}/ffmpeg.exe"
cp "${SRC}/ffmpeg/COPYING.LGPLv2.1" "${STAGE}/LICENSE"
cp "${SRC}/ffmpeg/COPYING.LGPLv2.1" "${STAGE}/licenses/FFmpeg-COPYING.LGPLv2.1"
cp "${SRC}/ffmpeg/LICENSE.md" "${STAGE}/licenses/FFmpeg-LICENSE.md"
cp "${SRC}/libvpx/LICENSE" "${STAGE}/licenses/libvpx-LICENSE"
cp "${SRC}/libvpx/PATENTS" "${STAGE}/licenses/libvpx-PATENTS"
cp "${SRC}/opus/COPYING" "${STAGE}/licenses/opus-COPYING"
cp "${SRC}/libogg/COPYING" "${STAGE}/licenses/libogg-COPYING"
cp "${SRC}/libvorbis/COPYING" "${STAGE}/licenses/libvorbis-COPYING"
cp "${SRC}/dav1d/COPYING" "${STAGE}/licenses/dav1d-COPYING"
cp "${HERE}/build_minimal.sh" "${HERE}/versions.env" "${HERE}/smoke_test_minimal.sh" "${STAGE}/"

{
	echo "CEVPlayer minimal FFmpeg ${PACKAGE_VERSION} (win64, static, LGPL)"
	echo
	echo "This is an unmodified FFmpeg build; the source is the upstream tarball listed below (no patches)."
	echo "To rebuild: MSYS2 MINGW64 shell, run build_minimal.sh next to versions.env (needs network access)."
	echo
	echo "Sources (SHA-256 verified by the build script):"
	for dep in FFMPEG LIBVPX OPUS LIBOGG LIBVORBIS DAV1D; do
		v="${dep}_VERSION"; u="${dep}_URL"; s="${dep}_SHA256"
		printf '  %-10s %-8s %s\n             sha256 %s\n' "${dep,,}" "${!v}" "${!u}" "${!s}"
	done
	echo
	echo "Toolchain:"
	gcc --version | head -n 1 | sed 's/^/  /'
	nasm --version | head -n 1 | sed 's/^/  /'
	meson --version | sed 's/^/  meson /'
	echo
	echo "ffmpeg -version:"
	sed 's/^/  /' <<<"${VERSION_TEXT}"
	echo
	echo "configure arguments of this script:"
	printf '  %s\n' "${FFMPEG_CONFIGURE_ARGS[@]}"
} >"${STAGE}/BUILD.txt"

ZIP="${OUT}/${PACKAGE_NAME}.zip"
rm -f "${ZIP}" "${ZIP}.sha256"
# Flat layout: ffmpeg.exe is at the top of the zip (the editor's installer also accepts one top folder or bin/).
( cd "${STAGE}" && zip -q -r -9 -X "${ZIP}" . )
( cd "${OUT}" && sha256sum "${PACKAGE_NAME}.zip" >"${PACKAGE_NAME}.zip.sha256" )

log "done"
cat "${ZIP}.sha256"
echo "version for the plugin (kFfmpegMinimalVersion): ${PACKAGE_VERSION}"
