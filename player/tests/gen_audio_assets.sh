#!/bin/sh
# Generate compressed-audio integration fixtures for `make check-audio`.
set -e
cd "$(dirname "$0")/assets"

# MP3 exercises AVI WAVE tag 0x55 and packet joins; AAC exercises mp4a/esds
# AudioSpecificConfig plus one raw access unit per MP4 sample. 44.1 kHz also
# covers Paula's 2:1 output decimation.
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=25:duration=2 \
    -f lavfi -i sine=frequency=523:sample_rate=44100:duration=2 \
    -c:v mpeg4 -bf 0 -qscale:v 5 -c:a libmp3lame -b:a 96k \
    -shortest test_mp3.avi -y
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=25:duration=2 \
    -f lavfi -i sine=frequency=659:sample_rate=44100:duration=2 \
    -c:v mpeg4 -bf 0 -qscale:v 5 -c:a aac -b:a 96k \
    -shortest test_aac.mp4 -y

# Stereo fixtures with a different tone in each channel: the mono fixtures
# above cannot tell "kept one channel" apart from "kept both", so these are the
# ones that actually exercise --audio-mono's decoder-side folds (MintAMP's
# MP3SetOutputMono(), pl_mpeg's plm_audio_set_mono(), liba52's A52_MONO).
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=25:duration=2 \
    -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2[l];\
sine=frequency=1320:sample_rate=44100:duration=2[r];[l][r]amerge=inputs=2" \
    -c:v mpeg4 -bf 0 -qscale:v 5 -c:a libmp3lame -b:a 128k -ac 2 \
    -shortest test_mp3_stereo.avi -y
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=25:duration=2 \
    -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2[l];\
sine=frequency=1320:sample_rate=44100:duration=2[r];[l][r]amerge=inputs=2" \
    -c:v mpeg2video -bf 0 -qscale:v 5 -c:a mp2 -b:a 192k -ac 2 \
    -shortest -f mpegts test_mp2_stereo.ts -y
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=25:duration=2 \
    -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2[l];\
sine=frequency=1320:sample_rate=44100:duration=2[r];[l][r]amerge=inputs=2" \
    -c:v mpeg4 -bf 0 -qscale:v 5 -c:a ac3 -b:a 192k -ac 2 \
    -shortest test_ac3_stereo.mkv -y

echo "audio fixtures regenerated in $(pwd)"
