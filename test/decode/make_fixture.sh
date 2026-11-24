#!/bin/sh
# Regenerate the committed matrix fixtures (all 440 Hz stereo sines, 2 s,
# 48 kHz; sample.flac under test/ is NOT regenerated here — its pins carry
# the recorded 10 s 441000-frame truth). testdecode.c pins are recorded
# per committed fixture: exact totals for lossless/opus, ranges for the
# lossy paths — re-check the pins before committing any change here.
set -e
cd "$(dirname "$0")"
GEN='sine=frequency=440:sample_rate=48000:duration=2'
ffmpeg -v error -y -f lavfi -i "$GEN" -c:a libvorbis -ac 2 sample.ogg
ffmpeg -v error -y -f lavfi -i "$GEN" -c:a flac    -ac 2 sample.oga
ffmpeg -v error -y -f lavfi -i "$GEN" -c:a pcm_s16le -ac 2 sample.wav
ffmpeg -v error -y -f lavfi -i "$GEN" -c:a libmp3lame -b:a 32k -ac 2 sample.mp3
ffmpeg -v error -y -f lavfi -i "$GEN" -c:a aac -b:a 48k -ac 2 sample.m4a
for f in sample.ogg sample.oga sample.wav sample.mp3 sample.m4a; do
    printf '%s ' "$f"
    ffprobe -v error -show_entries stream=codec_name,duration -of csv=p=0 "$f"
done
