#!/bin/sh
# Regenerate the committed fixture sample.opus: a 440 Hz sine, stereo,
# 48 kHz, 2.0065 s, encoded with ffmpeg's libopus (default bitrate).
# The committed file is deterministic-by-role, not byte-deterministic:
# testopus.c pins dec_total_frames == 96000 (final granule 96312 minus
# the 312-sample preskip), ch=2, rate=48000, and audibility of the read
# output — re-run this and re-check those pins before committing.
set -e
cd "$(dirname "$0")"
ffmpeg -v error -y -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" \
    -c:a libopus -ac 2 sample.opus
ffprobe -v error -show_entries stream=duration_ts,initial_padding \
    -of default=nw=1 sample.opus
