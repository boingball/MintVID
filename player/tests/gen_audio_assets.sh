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

# ffmpeg's own decode of the AC-3 fixtures, as the reference mr_ac3_check
# measures against: raw signed 16-bit little-endian at the track's own rate and
# channel count (the stereo one included - a stereo fold is the case most
# likely to go wrong). Kept as files rather than regenerated per run so a
# machine without ffmpeg can still run the check.
ffmpeg -v error -i test_h264_ac3.mkv -vn -f s16le -acodec pcm_s16le \
    ref_ac3_mkv.raw -y
ffmpeg -v error -i test_h264_ac3.ts -vn -f s16le -acodec pcm_s16le \
    ref_ac3_ts.raw -y
# The same AC-3 as a raw elementary stream, for the m68k conformance run: that
# build gets mr_ac3_check without the demuxer (MR_AC3_CHECK_NO_DEMUX), so it
# does not have to cross-build the whole container/H.264 tier to check the
# decoder on a real big-endian target.
ffmpeg -v error -i test_h264_ac3.mkv -c:a copy -f ac3 test_ac3.ac3 -y
ffmpeg -v error -i test_ac3_stereo.mkv -vn -f s16le -acodec pcm_s16le \
    ref_ac3_stereo.raw -y

# MPEG-1 program stream with MPEG-2 Layer II audio: 22.05 kHz is below MPEG-1
# audio's lowest rate, so this is an ISO 13818-3 "low sampling frequency"
# stream - the shape any file encoded for Paula ends up with, and the one the
# .mpg source used to report as having no audio at all. Pink noise rather than
# a tone on purpose: it spreads energy over every subband, so a wrong bit
# allocation shows up immediately instead of hiding under an unused table.
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=25:duration=1 \
    -f lavfi -i "anoisesrc=color=pink:sample_rate=22050:duration=1:amplitude=0.7" \
    -c:v mpeg1video -b:v 200k -c:a mp2 -b:a 64k -ar 22050 -ac 1 \
    -shortest -f mpeg test_mpeg1_mp2.mpg -y
ffmpeg -v error -i test_mpeg1_mp2.mpg -vn -f s16le -acodec pcm_s16le \
    ref_mpeg1_mp2.raw -y

echo "audio fixtures regenerated in $(pwd)"
