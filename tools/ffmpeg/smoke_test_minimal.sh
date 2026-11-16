#!/usr/bin/env bash
# Smoke test of the minimal FFmpeg: runs it the way CEVPlayerFFmpeg does (same arguments as
# FfmpegCommands.cpp BuildTranscodeToWebm / BuildMakeVariant) on generated sources and checks the results.
#
#   tools/ffmpeg/smoke_test_minimal.sh <path to the minimal ffmpeg.exe>
#
# The sources (H.264 + AAC MP4, RGBA PNG sequence, ProRes 4444) are made by a REFERENCE full ffmpeg and checked with
# its ffprobe, because the minimal build has neither those encoders nor ffprobe:
#   REF_FFMPEG / REF_FFPROBE   default: ffmpeg / ffprobe from PATH (CI: pacman -S mingw-w64-x86_64-ffmpeg)
#
# Status: written without being able to run it in the development session that added it; the checks mirror
# Core/offline/tests/cevffmpeg_selftest.cpp (which was run against a full ffmpeg 8.0).
set -euo pipefail

MIN="${1:?usage: smoke_test_minimal.sh <minimal ffmpeg.exe>}"
REF_FFMPEG="${REF_FFMPEG:-ffmpeg}"
REF_FFPROBE="${REF_FFPROBE:-ffprobe}"
WORK="${CEV_SMOKE_DIR:-$(mktemp -d)}"
mkdir -p "${WORK}"

fail() { printf 'SMOKE FAIL: %s\n' "$*" >&2; exit 1; }
ok() { printf 'ok: %s\n' "$*"; }
probe() { # file entries -> ffprobe text of the first video stream
	"${REF_FFPROBE}" -v error -select_streams "$2" -show_entries "$3" -of default=nw=1 "$1"
}

command -v "${REF_FFMPEG}" >/dev/null || fail "no reference ffmpeg (${REF_FFMPEG})"
command -v "${REF_FFPROBE}" >/dev/null || fail "no reference ffprobe (${REF_FFPROBE})"

# ---- 1. capabilities, the same check as FfmpegLocator (MinimumRequirement) ---------------------------------------
"${MIN}" -hide_banner -encoders | grep -Eq ' libvpx-vp9 ' || fail "encoder libvpx-vp9"
"${MIN}" -hide_banner -encoders | grep -Eq ' libopus ' || fail "encoder libopus"
"${MIN}" -hide_banner -muxers | grep -Eq ' webm ' || fail "muxer webm"
if "${MIN}" -hide_banner -version | grep -Eq -- '--enable-(gpl|nonfree)'; then fail "GPL / nonfree configuration"; fi
ok "capabilities"

# ---- 2. sources from the reference ffmpeg -------------------------------------------------------------------------
BOX="color=c=black@0.0:size=160x90:rate=10,format=rgba,drawbox=x=40:y=20:w=80:h=50:color=red@1.0:t=fill:replace=1"
"${REF_FFMPEG}" -hide_banner -y -loglevel error -f lavfi -i testsrc2=size=320x240:rate=30:duration=2 \
	-f lavfi -i sine=frequency=440:sample_rate=44100:duration=2 -ac 1 \
	-c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "${WORK}/h264_aac.mp4"
"${REF_FFMPEG}" -hide_banner -y -loglevel error -f lavfi -i "${BOX}" -frames:v 10 -start_number 1 -pix_fmt rgba "${WORK}/seq_%03d.png"
"${REF_FFMPEG}" -hide_banner -y -loglevel error -f lavfi -i "${BOX}" -frames:v 10 -c:v prores_ks -profile:v 4444 \
	-pix_fmt yuva444p10le "${WORK}/prores4444.mov"

# ---- 3. the arguments of BuildTranscodeToWebm (single pass) -----------------------------------------------------
COMMON=(-hide_banner -y -loglevel warning -progress pipe:1 -nostats)
VIDEO_OPTS=(-c:v libvpx-vp9 -b:v 0 -crf 32 -deadline good -cpu-used 5 -row-mt 1
	-colorspace bt709 -color_primaries bt709 -color_trc bt709)

# 3a. MP4 (H.264 + AAC) -> WebM (VP9 + Opus), 2 pass, resized to 160 high
"${MIN}" "${COMMON[@]}" -i "${WORK}/h264_aac.mp4" -map 0:v:0 \
	-vf "scale=-2:160:flags=lanczos:out_color_matrix=bt709,format=yuv420p" "${VIDEO_OPTS[@]}" \
	-pass 1 -passlogfile "${WORK}/mp4.2pass" -an -f webm "${WORK}/null.webm" >"${WORK}/progress1.txt"
"${MIN}" "${COMMON[@]}" -i "${WORK}/h264_aac.mp4" -map 0:v:0 \
	-vf "scale=-2:160:flags=lanczos:out_color_matrix=bt709,format=yuv420p" "${VIDEO_OPTS[@]}" \
	-pass 2 -passlogfile "${WORK}/mp4.2pass" -map '0:a:0?' -c:a libopus -b:a 96k -f webm "${WORK}/mp4.webm.tmp" >"${WORK}/progress2.txt"
grep -q '^progress=end' "${WORK}/progress2.txt" || fail "no progress=end on stdout"
grep -q '^out_time_us=' "${WORK}/progress2.txt" || fail "no out_time_us on stdout"
probe "${WORK}/mp4.webm.tmp" v:0 stream=codec_name,width,height,pix_fmt | tr '\n' ' ' | grep -Eq 'codec_name=vp9 width=2(12|14) height=160 pix_fmt=yuv420p' \
	|| fail "mp4 -> webm video: $(probe "${WORK}/mp4.webm.tmp" v:0 stream=codec_name,width,height,pix_fmt | tr '\n' ' ')"
probe "${WORK}/mp4.webm.tmp" a:0 stream=codec_name | grep -q 'codec_name=opus' || fail "mp4 -> webm audio is not opus"
ok "mp4 (h264 + aac) -> webm (vp9 + opus), two pass, scaled"

# 3b. PNG sequence with alpha -> WebM with alpha (yuva420p, -auto-alt-ref 0)
"${MIN}" "${COMMON[@]}" -framerate 10 -i "${WORK}/seq_%03d.png" -map 0:v:0 -vf "scale=out_color_matrix=bt709,format=yuva420p" \
	"${VIDEO_OPTS[@]}" -auto-alt-ref 0 -an -f webm "${WORK}/png_alpha.webm.tmp" >/dev/null
probe "${WORK}/png_alpha.webm.tmp" v:0 stream=codec_name,width,height:stream_tags=alpha_mode | tr '\n' ' ' | grep -q 'alpha_mode=1' \
	|| fail "png sequence -> webm lost its alpha flag"
ok "png sequence (rgba) -> webm with alpha"

# 3c. ProRes 4444 with alpha -> WebM with alpha
"${MIN}" "${COMMON[@]}" -i "${WORK}/prores4444.mov" -map 0:v:0 -vf "scale=out_color_matrix=bt709,format=yuva420p" \
	"${VIDEO_OPTS[@]}" -auto-alt-ref 0 -an -f webm "${WORK}/prores_alpha.webm.tmp" >/dev/null
probe "${WORK}/prores_alpha.webm.tmp" v:0 stream_tags=alpha_mode | grep -q 'alpha_mode=1' || fail "prores 4444 -> webm lost its alpha flag"
ok "prores 4444 -> webm with alpha"

# 3d. variant: the alpha WebM again, decoded with libvpx (the native vp9 decoder drops the alpha channel)
"${MIN}" "${COMMON[@]}" -c:v libvpx-vp9 -i "${WORK}/png_alpha.webm.tmp" -map 0:v:0 \
	-vf "scale=-2:45:flags=lanczos:out_color_matrix=bt709,format=yuva420p" "${VIDEO_OPTS[@]}" -auto-alt-ref 0 -an -f webm "${WORK}/variant.webm.tmp" >/dev/null
probe "${WORK}/variant.webm.tmp" v:0 stream=height:stream_tags=alpha_mode | tr '\n' ' ' | grep -q 'height=45 alpha_mode=1' \
	|| fail "variant lost its alpha / size: $(probe "${WORK}/variant.webm.tmp" v:0 stream=height:stream_tags=alpha_mode | tr '\n' ' ')"
ok "variant of an alpha webm (libvpx decoder)"

# ---- 4. things outside the scope must be refused rather than silently work ---------------------------------------
if "${MIN}" -hide_banner -y -i "${WORK}/h264_aac.mp4" -c:v libx264 -f mp4 "${WORK}/should_not_exist.mp4" >/dev/null 2>&1; then
	fail "the minimal build can encode H.264 (it must not)"
fi
ok "out of scope encoders are absent"

printf '\nSMOKE PASS (%s)\n' "${WORK}"
