#!/bin/sh
# Regenerates the format fixtures with a stock ffmpeg. They are committed so the
# suite can test what *opening* each format does without needing encoders this
# project's own FFmpeg build does not have (there is no mp3 encoder in it). Each
# is about a second long and a few kilobytes: big enough to have a waveform, a
# frame and a duration, small enough not to matter in the repository.
set -e
cd "$(dirname "$0")"
q="-hide_banner -loglevel error -y"
tone="-f lavfi -i sine=frequency=440:duration=1"
short="-f lavfi -i sine=frequency=440:duration=0.25"
bars="-f lavfi -i testsrc2=s=64x36:r=24:d=1"

ffmpeg $q -f lavfi -i color=red:s=32x32:d=1 -frames:v 1 cover.png
ffmpeg $q $tone -c:a libmp3lame -b:a 32k tone.mp3
ffmpeg $q -i tone.mp3 -i cover.png -map 0 -map 1 -c copy -id3v2_version 3 -disposition:v attached_pic tone-art.mp3
ffmpeg $q $tone -i cover.png -map 0 -map 1 -c:a aac -b:a 32k -c:v copy -disposition:v attached_pic tone-art.m4a
ffmpeg $q $tone -i cover.png -map 0 -map 1 -c:a flac -c:v copy -disposition:v attached_pic tone-art.flac
ffmpeg $q $tone -c:a libvorbis -q:a 0 tone.ogg
ffmpeg $q $tone -c:a libopus -b:a 16k tone.opus
ffmpeg $q $short -c:a pcm_s24le tone-s24.wav
ffmpeg $q $short -c:a pcm_f32le tone-f32.wav
ffmpeg $q $short -c:a pcm_s16be tone.aif
ffmpeg $q -f lavfi -i testsrc2=s=64x36:r=24:d=0.25 $short -c:v prores_ks -profile:v 0 -c:a pcm_s16le -shortest prores.mov
ffmpeg $q $bars -c:v libvpx-vp9 -b:v 50k vp9.webm
ffmpeg $q $bars -c:v libx265 -pix_fmt yuv420p10le -x265-params log-level=none hevc10.mkv
ffmpeg $q $bars -frames:v 6 bars.gif
ffmpeg $q $bars -frames:v 1 still.jpg
ffmpeg $q $bars -frames:v 1 still.webp
