#!/usr/bin/env bash
# S0-A bench clips: VP9, 10 s, keyframe interval 2 s, with/without alpha.
set -euo pipefail
out="${1:-E:/coding3/xt-bench/cev-s0/media}"; mkdir -p "$out"
ff() { ffmpeg -hide_banner -loglevel error -y "$@"; }
for spec in 1920x1080:30 1920x1080:60 3840x2160:30; do
  s=${spec%:*}; r=${spec#*:}; h=${s#*x}; g=$((r*2)); tag="${h}p${r}"
  [ "$h" = 2160 ] && tag="2160p${r}"
  ff -f lavfi -i "testsrc2=size=$s:rate=$r:duration=10" -pix_fmt yuv420p \
     -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -row-mt 1 -b:v 0 -crf 34 -g $g -keyint_min $g -an "$out/vp9_${tag}.webm"
  ff -f lavfi -i "testsrc2=size=$s:rate=$r:duration=10" -f lavfi -i "gradients=size=$s:rate=$r:duration=10:speed=0.02,format=gray" \
     -filter_complex "[0:v]format=yuva420p[c];[c][1:v]alphamerge,format=yuva420p" -pix_fmt yuva420p \
     -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -row-mt 1 -b:v 0 -crf 34 -g $g -keyint_min $g -auto-alt-ref 0 -an "$out/vp9_${tag}_alpha.webm"
  echo "done $tag"
done
ls -l "$out"
