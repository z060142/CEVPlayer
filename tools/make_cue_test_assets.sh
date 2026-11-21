#!/usr/bin/env bash
# Build the assets of the M3 cue route test in a test project (needs ffmpeg, and av_sync_flash_beep.webm from
# tools/make_test_media.sh already in <project>/Assets/Videos).
#
#   tools/make_cue_test_assets.sh [<project dir>]      (default D:/coding11/_testproject/CEVTest)
#
# Creates / updates (the WAV and the silent video are generated, not versioned):
#   <project>/Assets/audio/sdlmixer/assets/cev_test_beep.wav     48 kHz 16 bit stereo, the audio of av_sync_flash_beep
#   <project>/Assets/Videos/av_sync_flash_beep_silent.webm       the picture only (ffmpeg -an, stream copy)
#   <project>/Assets/audio/sdlmixer/ace/cev_test.xml             tools/test_audio/cev_test.xml (trigger cev_test_beep + pause / resume)
#   <project>/Assets/Videos/cue_beep.cevvideo                    tools/test_descriptors/cue_beep.cevvideo
# The SDL_mixer implementation is the default of CRYENGINE 5.7 (cvar s_ImplName = CryAudioImplSDLMixer), no project setting is needed.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
project="${1:-D:/coding11/_testproject/CEVTest}"
command -v ffmpeg >/dev/null || { echo "make_cue_test_assets: ffmpeg not found on PATH" >&2; exit 1; }
src="$project/Assets/Videos/av_sync_flash_beep.webm"
[ -f "$src" ] || { echo "make_cue_test_assets: $src missing (run tools/make_test_media.sh and copy it there)" >&2; exit 1; }
wavdir="$project/Assets/audio/sdlmixer/assets"
acedir="$project/Assets/audio/sdlmixer/ace"
mkdir -p "$wavdir" "$acedir"
ffmpeg -v error -y -i "$src" -vn -ar 48000 -ac 2 -c:a pcm_s16le "$wavdir/cev_test_beep.wav"
ffmpeg -v error -y -i "$src" -an -c:v copy "$project/Assets/Videos/av_sync_flash_beep_silent.webm"
cp "$here/tools/test_audio/cev_test.xml" "$acedir/cev_test.xml"
cp "$here/tools/test_descriptors/cue_beep.cevvideo" "$project/Assets/Videos/cue_beep.cevvideo"
echo "make_cue_test_assets: done in $project"
ls -l "$wavdir/cev_test_beep.wav" "$project/Assets/Videos/av_sync_flash_beep_silent.webm"
