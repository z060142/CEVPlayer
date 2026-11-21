#!/usr/bin/env bash
# Generate WebM test clips for CEVPlayer with ffmpeg (needs libvpx, libvpx-vp9, libopus, libvorbis).
#
#   tools/make_test_media.sh [<output dir>]        (default: ./test_media, which is gitignored)
#
# Environment overrides (seconds): MAIN_SECONDS (default 10, the 1080p clip), SHORT_SECONDS (default 5).
# Everything is synthetic (ffmpeg test sources), so the clips are deterministic and tiny.
#
# Clips:
#   vp9_1080p30_opus.webm       VP9 1920x1080 30 fps + Opus stereo 48 kHz (440 Hz tone)
#   vp9_alpha.webm              VP9 yuva420p (alpha as WebM BlockAdditional), 640x360, circular alpha mask
#   vp8_vorbis.webm             VP8 640x360 25 fps + Vorbis stereo 44.1 kHz
#   vp9_odd_321x181.webm        VP9 with odd width/height (checks the YUV->RGBA converter), no audio
#   audio_start_offset.webm     video from t=0, audio track starting at t=1.5 s (non-zero start time)
#   opus_preskip.webm           Opus with its natural encoder pre-skip (CodecDelay), VP9 320x180
#   audio_en.webm, audio_ja.webm  audio-only WebM (Opus) with a Matroska language tag, for "external audio"
#   audio_music.webm, audio_commentary.webm  audio-only WebM (Opus, 330 / 660 Hz), external audio tracks of
#                               tools/test_descriptors/tracks_test.cevvideo (A1)
#   av_sync_flash_beep.webm     720p30: white flash frame + 1 kHz beep at the start of every second
#   vp9_444.webm                VP9 profile 1 yuv444p 320x180 (not packable: the source keeps RGBA8, M8)
#   vp9_320x181.webm            VP9 4:2:0 with even width and odd height (packed layout with ch = 91, M8)
#   vp9_siting_left.webm        VP9 320x180 whose Matroska Colour carries ChromaSitingHorz/Vert (made from an H.264
#                               intermediate tagged -chroma_sample_location left; needs libx264, M8)
set -euo pipefail

usage() {
  echo "usage: make_test_media.sh [<output dir>]   (env: MAIN_SECONDS=10 SHORT_SECONDS=5 FFMPEG=ffmpeg FFPROBE=ffprobe)" >&2
  exit 2
}
case "${1:-}" in -h|--help) usage ;; esac
[ "$#" -le 1 ] || usage

out="${1:-./test_media}"
main="${MAIN_SECONDS:-10}"
short="${SHORT_SECONDS:-5}"
FFMPEG="${FFMPEG:-ffmpeg}"   # explicit ffmpeg binary, e.g. FFMPEG=C:/.../ffmpeg-shared/current/bin/ffmpeg.exe
FFPROBE="${FFPROBE:-$(dirname "$FFMPEG")/ffprobe}"
[ -x "$FFPROBE" ] || [ -x "$FFPROBE.exe" ] || FFPROBE=ffprobe
command -v "$FFMPEG" >/dev/null || { echo "make_test_media: $FFMPEG not found" >&2; exit 1; }
for enc in libvpx libvpx-vp9 libopus libvorbis; do
  "$FFMPEG" -hide_banner -encoders 2>/dev/null | grep -qw "$enc" || { echo "make_test_media: ffmpeg lacks encoder $enc" >&2; exit 1; }
done
mkdir -p "$out"
out="$(cd "$out" && pwd)"

ff() { "$FFMPEG" -hide_banner -loglevel error -y "$@"; }
# Fast VP9 settings (realtime, row-mt): quality is irrelevant for test clips.
VP9=(-c:v libvpx-vp9 -deadline realtime -cpu-used 8 -row-mt 1 -b:v 0 -crf 34)
OPUS=(-c:a libopus -b:a 96k)

echo "== vp9_1080p30_opus.webm (${main}s)"
ff -f lavfi -i "testsrc2=size=1920x1080:rate=30:duration=$main" \
   -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=$main" \
   -pix_fmt yuv420p "${VP9[@]}" -g 60 "${OPUS[@]}" -ac 2 -shortest "$out/vp9_1080p30_opus.webm"

echo "== vp9_alpha.webm (${short}s)"
ff -f lavfi -i "testsrc2=size=640x360:rate=30:duration=$short" \
   -vf "format=yuva420p,geq=lum='lum(X,Y)':cb='cb(X,Y)':cr='cr(X,Y)':a='if(gt(hypot(X-W/2,Y-H/2),H/3),0,255)'" \
   -pix_fmt yuva420p -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 1M -auto-alt-ref 0 -an \
   "$out/vp9_alpha.webm"

echo "== vp8_vorbis.webm (${short}s)"
ff -f lavfi -i "testsrc2=size=640x360:rate=25:duration=$short" \
   -f lavfi -i "sine=frequency=523:sample_rate=44100:duration=$short" \
   -pix_fmt yuv420p -c:v libvpx -deadline realtime -cpu-used 8 -b:v 800k -g 50 \
   -c:a libvorbis -b:a 96k -ac 2 -shortest "$out/vp8_vorbis.webm"

echo "== vp9_odd_321x181.webm (${short}s)"
# testsrc2 only produces even sizes, so scale to the odd size (the encoder keeps it).
ff -f lavfi -i "testsrc2=size=322x182:rate=30:duration=$short" -vf "scale=321:181,format=yuv420p" \
   "${VP9[@]}" -an "$out/vp9_odd_321x181.webm"

echo "== audio_start_offset.webm (${short}s, audio starts at 1.5 s)"
ff -f lavfi -i "testsrc2=size=640x360:rate=30:duration=$short" \
   -itsoffset 1.5 -f lavfi -i "sine=frequency=660:sample_rate=48000:duration=$short" \
   -pix_fmt yuv420p "${VP9[@]}" "${OPUS[@]}" -ac 2 -map 0:v -map 1:a "$out/audio_start_offset.webm"

echo "== opus_preskip.webm (${short}s)"
ff -f lavfi -i "testsrc2=size=320x180:rate=30:duration=$short" \
   -f lavfi -i "sine=frequency=330:sample_rate=48000:duration=$short" \
   -pix_fmt yuv420p "${VP9[@]}" "${OPUS[@]}" -ac 2 -shortest "$out/opus_preskip.webm"

echo "== audio_en.webm / audio_ja.webm (${short}s)"
ff -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=$short" \
   "${OPUS[@]}" -ac 2 -metadata:s:a:0 language=eng -metadata:s:a:0 title="English" "$out/audio_en.webm"
ff -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=$short" \
   "${OPUS[@]}" -ac 2 -metadata:s:a:0 language=jpn -metadata:s:a:0 title="Japanese" "$out/audio_ja.webm"

echo "== audio_music.webm / audio_commentary.webm (${short}s)"
ff -f lavfi -i "sine=frequency=330:sample_rate=48000:duration=$short" "${OPUS[@]}" -ac 2 "$out/audio_music.webm"
ff -f lavfi -i "sine=frequency=660:sample_rate=48000:duration=$short" "${OPUS[@]}" -ac 2 "$out/audio_commentary.webm"

echo "== av_sync_flash_beep.webm (${main}s)"
ff -f lavfi -i "color=c=black:size=1280x720:rate=30:duration=$main" \
   -f lavfi -i "aevalsrc='0.8*sin(2*PI*1000*t)*lt(mod(t,1),0.05)':sample_rate=48000:channel_layout=stereo:duration=$main" \
   -vf "drawbox=x=0:y=0:w=iw:h=ih:color=white:t=fill:enable='eq(mod(n,30),0)',format=yuv420p" \
   "${VP9[@]}" -g 30 "${OPUS[@]}" -shortest "$out/av_sync_flash_beep.webm"

echo "== vp9_444.webm (${short}s)"
ff -f lavfi -i "testsrc2=size=320x180:rate=30:duration=$short" -pix_fmt yuv444p "${VP9[@]}" -an "$out/vp9_444.webm"

echo "== vp9_320x181.webm (${short}s)"
ff -f lavfi -i "testsrc2=size=320x182:rate=30:duration=$short" -vf "scale=320:181,format=yuv420p" "${VP9[@]}" -an "$out/vp9_320x181.webm"

echo "== vp9_siting_left.webm (2s, via a tagged H.264 intermediate)"
ff -f lavfi -i "testsrc2=size=320x180:rate=30:duration=2" -pix_fmt yuv420p -c:v libx264 -chroma_sample_location left "$out/siting_left_tmp.mp4"
ff -i "$out/siting_left_tmp.mp4" "${VP9[@]}" -an "$out/vp9_siting_left.webm"
rm -f "$out/siting_left_tmp.mp4"

echo
echo "Generated in $out:"
for f in "$out"/*.webm; do
  printf '%10s bytes  %s  ' "$(wc -c < "$f" | tr -d ' ')" "$(basename "$f")"
  if command -v "$FFPROBE" >/dev/null; then
    "$FFPROBE" -v error -show_entries stream=codec_type,codec_name,width,height,start_time:stream_tags=alpha_mode,language \
      -show_entries format=duration -of compact=p=0:nk=0 "$f" 2>/dev/null | tr '\n' ' ' | sed 's/stream|//g'
  fi
  echo
done
