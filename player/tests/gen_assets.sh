#!/bin/sh
# Regenerate the test fixtures used by `make check`.
# Requires ffmpeg. The committed assets are produced by this exact command so
# the Cinepak conformance check is reproducible.
set -e
cd "$(dirname "$0")/assets"

# 128x96, 12 fps, 2 s => 24 frames, two keyframes (GOP 12): exercises intra,
# inter-with-skip and selective codebook updates across a keyframe boundary.
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=2 \
    -c:v cinepak test_cinepak.avi -y
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=2 \
    -c:v mjpeg -q:v 5 test_mjpeg.avi -y
# Raw MJPEG is a concatenated sequence of JPEG images with no container.
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=25:duration=1 \
    -c:v mjpeg -q:v 5 -f mjpeg test_raw_mjpeg.mjpeg -y
# MPEG-1 program stream (25 fps - MPEG-1 only allows standard rates).
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=25:duration=2 \
    -c:v mpeg1video -b:v 800k -f mpeg test_mpeg1.mpg -y
# Same Cinepak content in a QuickTime MOV, with PCM audio, to exercise the
# MOV demuxer (sample-table frame reconstruction).
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=2 \
    -f lavfi -i sine=frequency=440:sample_rate=22050:duration=2 \
    -c:v cinepak -c:a pcm_s16le test_cinepak.mov -y
# MPEG-4 Part 2 (DivX/Xvid, fourcc FMP4): intra-only (I-VOP path) and Simple
# Profile (I+P with 4MV). ASP tools (B-frames/qpel/GMC) are a later stage.
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=1 \
    -c:v mpeg4 -g 1 -qscale:v 4 test_mp4v_intra.avi -y
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=2 \
    -c:v mpeg4 -bf 0 -flags +mv4 -qscale:v 4 test_mp4v_sp.avi -y
# Microsoft MPEG-4 v2 in AVI: separate H.263-derived MP42 bitstream.
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=2 \
    -c:v msmpeg4v2 -g 12 -qscale:v 4 test_mp42.avi -y
# DIV2 is an alternate AVI FourCC for the same Microsoft v2 bitstream. Remux
# the identical packets so both codec tags share one ffmpeg reference set.
ffmpeg -v error -i test_mp42.avi -c copy -tag:v DIV2 test_div2.avi -y
# WMV1 (Windows Media Video 7 / MSMPEG4 v3): same H.263-derived macroblock
# skeleton as MP42 but with selectable RL/DC/MV VLC tables, coded-block-
# pattern prediction on intra blocks, and flipflop MC rounding. Two qscales
# exercise both the plain-table and higher-quantizer/escape-3 coding paths.
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=2 \
    -c:v wmv1 -g 12 -qscale:v 4 -b:v 800k test_wmv1.avi -y
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=2 \
    -c:v wmv1 -g 12 -qscale:v 20 -b:v 800k test_wmv1_q20.avi -y
# WMV2 (Windows Media Video 8): builds on WMV1's grammar with bitplane-coded
# skip, three qscale-selected CBP tables, adaptive MV prediction, MSPEL
# motion compensation, the Adaptive Block Transform, and an H.263-style
# in-loop deblocking filter. -flags +loop turns the last of those on (off
# by default); mspel/abt/per_mb_rl are already on by default at these
# settings. Two qscales exercise both the plain-table and the qscale>10
# CBP-table-selection threshold.
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=2 \
    -c:v wmv2 -g 12 -qscale:v 4 -b:v 800k -flags +loop test_wmv2.avi -y
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=2 \
    -c:v wmv2 -g 12 -qscale:v 20 -b:v 800k -flags +loop test_wmv2_q20.avi -y
# H.264 High Profile in MP4: CABAC, 8x8 transform-capable profile, B-frame
# reordering and avcC/length-prefixed NAL handling. AAC-LC exercises the same
# container's interleaved compressed-audio samples.
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=2 \
    -f lavfi -i sine=frequency=660:sample_rate=22050:duration=2 \
    -c:v libx264 -profile:v high -level:v 2.0 -pix_fmt yuv420p \
    -g 12 -bf 2 -refs 1 -crf 22 \
    -c:a aac -profile:a aac_low -b:a 64k -shortest test_h264_high.mp4 -y
# Remux the exact H.264/AAC packets into 188-byte broadcast TS and 192-byte
# Blu-ray-style M2TS. These exercise Annex-B/PES assembly and ADTS AAC without
# introducing another encoder reference.
ffmpeg -v error -i test_h264_high.mp4 -c copy \
    -f mpegts test_h264_aac.ts -y
ffmpeg -v error -i test_h264_high.mp4 -c copy -mpegts_m2ts_mode 1 \
    -f mpegts test_h264_aac.m2ts -y
# Re-encode only the small audio track to exercise the common broadcast AAC
# LATM/LOAS path and the fixed-point AC-3 decoder.  Matroska also validates the
# new seekable-file demuxer without making the fixtures appreciably larger.
ffmpeg -v error -i test_h264_high.mp4 -c:v copy -c:a aac -b:a 64k \
    -mpegts_flags +latm -f mpegts test_h264_latm.ts -y
ffmpeg -v error -i test_h264_high.mp4 -c:v copy -c:a ac3 -b:a 96k \
    test_h264_ac3.mkv -y
ffmpeg -v error -i test_h264_high.mp4 -c:v copy -c:a ac3 -b:a 96k \
    -f mpegts test_h264_ac3.ts -y
ffmpeg -v error -i test_h264_high.mp4 -c copy test_h264_aac.mkv -y
# MPEG-2 Main Profile with B-frame reordering in a transport stream. This also
# verifies that the decoder drains both delayed reference pictures at EOF.
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=25:duration=2 \
    -c:v mpeg2video -profile:v main -pix_fmt yuv420p \
    -g 12 -bf 2 -qscale:v 4 -an -f mpegts test_mpeg2.ts -y
# Early OpenDivX AVI variant: numeric biCompression=4, 'divx' handler, and no
# VOL header in the bitstream. This reproduces Xmen-OpenDivX-200-slow.avi.
python3 ../make_legacy_opendivx.py test_mp4v_sp.avi test_opendivx_legacy.avi
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=1 \
    -c:v mpeg4 -bf 0 -flags +qpel -qscale:v 4 test_mp4v_qpel.avi -y
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=2 \
    -c:v mpeg4 -bf 2 -qscale:v 4 test_mp4v_b.avi -y
# Raw MPEG-4 Visual elementary stream: VOL + one VOP sequence, no container.
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=25:duration=1 \
    -c:v mpeg4 -bf 2 -qscale:v 4 -f m4v test_raw_mpeg4.m4v -y
# ITU-T H.263 version 1, sub-QCIF: the plain picture header, one slice per
# picture, and the single-escape TCOEF syntax the H.263+ files share.
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=2 \
    -c:v h263 -qscale:v 4 test_h263.avi -y
# The identical packets in a 3GP/QuickTime track, where the same bitstream is
# tagged 's263'. One ffmpeg reference set covers both containers.
ffmpeg -v error -i test_h263.avi -c copy -f 3gp test_h263.3gp -y
# Same content, cut into RTP-sized pieces so each picture carries GOB headers
# with their own GQUANT (mid-picture quantiser changes and prediction resets).
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=1 \
    -c:v h263 -qscale:v 4 -ps 200 test_h263_gob.avi -y
# H.263+ (H.263-1998): extended PTYPE with a custom 68x52 picture format, the
# custom picture clock frequency for 25 fps, slice-structured mode, and the
# rounding type that flip-flops between P pictures.
ffmpeg -v error -f lavfi -i testsrc2=size=68x52:rate=25:duration=1 \
    -c:v h263p -qscale:v 4 test_h263p.avi -y
# H.263+ with Annex D unrestricted motion vectors (the reversible MV VLC and
# vectors that point outside the picture) and several slices per picture.
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=1 \
    -c:v h263p -umv 1 -ps 200 -qscale:v 6 test_h263p_umv.avi -y

# Cinepak large enough that the encoder splits the frame into several strips
# (the 128x96 clip above is a single strip). Multi-strip frames are where the
# strip header's 24-bit size and the per-strip codebooks actually matter, and
# an encoder emitting an odd-sized strip desynchronises everything below it.
ffmpeg -v error -f lavfi -i testsrc2=size=320x240:rate=12:duration=2 \
    -c:v cinepak test_cinepak_strips.avi -y
# Microsoft Video 1 (MSVC/CRAM), RGB555. ffmpeg's encoder only emits the
# 16-bit variant, so the 8-bit paletted block layouts come from the generator
# below instead - but both are checked against ffmpeg's one decoder.
ffmpeg -v error -f lavfi -i testsrc2=size=128x96:rate=12:duration=2 \
    -c:v msvideo1 test_msvideo1.avi -y
python3 ../make_msvideo1_pal8.py test_msvideo1_pal8.avi
# Microsoft RLE (BI_RLE8). A width that is not a multiple of four exercises
# the row padding, and AVI stamps this one's biCompression as the numeric
# BI_RLE8 rather than a fourcc, so the tag reaching the registry is the
# lower-case 'mrle' from fccHandler.
ffmpeg -v error -f lavfi -i testsrc2=size=66x50:rate=10:duration=2 \
    -c:v msrle -pix_fmt pal8 test_msrle.avi -y

# Ground-truth frames, decoded by ffmpeg's own Cinepak decoder (per container,
# since ffmpeg re-encodes the Cinepak stream separately for each).
rm -rf ref_cinepak && mkdir -p ref_cinepak
ffmpeg -v error -i test_cinepak.avi ref_cinepak/f%03d.ppm -y
rm -rf ref_cinepak_strips && mkdir -p ref_cinepak_strips
ffmpeg -v error -i test_cinepak_strips.avi ref_cinepak_strips/f%03d.ppm -y
rm -rf ref_msvideo1 && mkdir -p ref_msvideo1
ffmpeg -v error -i test_msvideo1.avi ref_msvideo1/f%03d.ppm -y
rm -rf ref_msvideo1_pal8 && mkdir -p ref_msvideo1_pal8
ffmpeg -v error -i test_msvideo1_pal8.avi ref_msvideo1_pal8/f%03d.ppm -y
rm -rf ref_msrle && mkdir -p ref_msrle
ffmpeg -v error -i test_msrle.avi ref_msrle/f%03d.ppm -y
rm -rf ref_mov && mkdir -p ref_mov
ffmpeg -v error -i test_cinepak.mov ref_mov/f%03d.ppm -y
rm -rf ref_mjpeg && mkdir -p ref_mjpeg
ffmpeg -v error -i test_mjpeg.avi ref_mjpeg/f%03d.ppm -y
rm -rf ref_raw_mjpeg && mkdir -p ref_raw_mjpeg
ffmpeg -v error -f mjpeg -framerate 25 -i test_raw_mjpeg.mjpeg \
    ref_raw_mjpeg/f%03d.ppm -y
rm -rf ref_mpeg1 && mkdir -p ref_mpeg1
ffmpeg -v error -i test_mpeg1.mpg ref_mpeg1/f%03d.ppm -y
rm -rf ref_mp4v_intra && mkdir -p ref_mp4v_intra
ffmpeg -v error -i test_mp4v_intra.avi ref_mp4v_intra/f%03d.ppm -y
rm -rf ref_mp4v_sp && mkdir -p ref_mp4v_sp
ffmpeg -v error -i test_mp4v_sp.avi ref_mp4v_sp/f%03d.ppm -y
rm -rf ref_mp42 && mkdir -p ref_mp42
ffmpeg -v error -i test_mp42.avi ref_mp42/f%03d.ppm -y
rm -rf ref_wmv1 && mkdir -p ref_wmv1
ffmpeg -v error -i test_wmv1.avi ref_wmv1/f%03d.ppm -y
rm -rf ref_wmv1_q20 && mkdir -p ref_wmv1_q20
ffmpeg -v error -i test_wmv1_q20.avi ref_wmv1_q20/f%03d.ppm -y
rm -rf ref_wmv2 && mkdir -p ref_wmv2
ffmpeg -v error -i test_wmv2.avi ref_wmv2/f%03d.ppm -y
rm -rf ref_wmv2_q20 && mkdir -p ref_wmv2_q20
ffmpeg -v error -i test_wmv2_q20.avi ref_wmv2_q20/f%03d.ppm -y
rm -rf ref_h264_high && mkdir -p ref_h264_high
ffmpeg -v error -i test_h264_high.mp4 ref_h264_high/f%03d.ppm -y
rm -rf ref_mpeg2_ts && mkdir -p ref_mpeg2_ts
ffmpeg -v error -i test_mpeg2.ts ref_mpeg2_ts/f%03d.ppm -y
rm -rf ref_opendivx_legacy && mkdir -p ref_opendivx_legacy
ffmpeg -v error -i test_opendivx_legacy.avi ref_opendivx_legacy/f%03d.ppm -y
rm -rf ref_mp4v_qpel && mkdir -p ref_mp4v_qpel
ffmpeg -v error -i test_mp4v_qpel.avi ref_mp4v_qpel/f%03d.ppm -y
rm -rf ref_mp4v_b && mkdir -p ref_mp4v_b
ffmpeg -v error -i test_mp4v_b.avi ref_mp4v_b/f%03d.ppm -y
rm -rf ref_raw_mpeg4 && mkdir -p ref_raw_mpeg4
ffmpeg -v error -f m4v -framerate 25 -i test_raw_mpeg4.m4v \
    ref_raw_mpeg4/f%03d.ppm -y
rm -rf ref_h263 && mkdir -p ref_h263
ffmpeg -v error -i test_h263.avi ref_h263/f%03d.ppm -y
rm -rf ref_h263_gob && mkdir -p ref_h263_gob
ffmpeg -v error -i test_h263_gob.avi ref_h263_gob/f%03d.ppm -y
rm -rf ref_h263p && mkdir -p ref_h263p
ffmpeg -v error -i test_h263p.avi ref_h263p/f%03d.ppm -y
rm -rf ref_h263p_umv && mkdir -p ref_h263p_umv
ffmpeg -v error -i test_h263p_umv.avi ref_h263p_umv/f%03d.ppm -y

echo "fixtures regenerated in $(pwd)"
