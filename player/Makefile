# MintVID portable core - host build (for development & codec validation).
# The Amiga build uses a separate toolchain (vbcc / bebbo-gcc); this Makefile
# only proves the portable core on the dev host.

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99 -g
include vendor/libavc_port/libavc.mk
LIBMPEG2_ROOT = vendor/libmpeg2
LIBMPEG2_FLAGS = -I$(LIBMPEG2_ROOT) -I$(LIBMPEG2_ROOT)/include \
                 -I$(LIBMPEG2_ROOT)/libmpeg2
LIBMPEG2_SRC = $(LIBMPEG2_ROOT)/libmpeg2/alloc.c \
               $(LIBMPEG2_ROOT)/libmpeg2/cpu_accel.c \
               $(LIBMPEG2_ROOT)/libmpeg2/cpu_state.c \
               $(LIBMPEG2_ROOT)/libmpeg2/decode.c \
               $(LIBMPEG2_ROOT)/libmpeg2/header.c \
               $(LIBMPEG2_ROOT)/libmpeg2/idct.c \
               $(LIBMPEG2_ROOT)/libmpeg2/motion_comp.c \
               $(LIBMPEG2_ROOT)/libmpeg2/slice.c
CFLAGS  += -DMR_HAVE_H264 $(LIBAVC_FLAGS) $(LIBAVC_GCC_FLAGS) \
           $(LIBMPEG2_FLAGS) \
           -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
           -Wno-unused-but-set-variable -Wno-sign-compare \
           -Wno-implicit-fallthrough -Wno-maybe-uninitialized -Wno-type-limits
CORE     = core/mr_codec.c core/mr_source.c core/mr_http.c core/mr_hls.c \
           core/mr_youtube.c \
           core/mr_demux.c core/mr_latm.c core/mr_mkv.c core/mr_avi.c core/mr_mov.c \
           core/mr_ts.c core/mr_ps.c \
           core/mr_raw_mjpeg.c core/mr_raw_mpeg4.c \
           core/mr_cinepak.c core/mr_dither.c core/mr_ham.c core/mr_scale.c \
           core/mr_c2p.c core/mr_mjpeg.c core/picojpeg.c core/mr_mpeg1.c \
           core/mr_mpeg2.c $(LIBMPEG2_SRC) \
           core/mr_mpeg4.c core/mr_msmpeg4v2.c core/mr_wmv.c core/mr_wmv2.c core/mr_h263.c core/mr_h264.c \
           core/mr_msvideo1.c core/mr_rle.c \
           core/mr_rawvideo.c core/mr_yuv.c core/mr_yuv_m68k.S \
           $(LIBAVC_SRC)
TESTSRC  = tests/mr_decode.c
MINTAMP_ROOT ?= vendor/MintAMP
MINTAMP_MP3 = $(MINTAMP_ROOT)/mp3dec.c $(MINTAMP_ROOT)/mp3tabs.c \
              $(wildcard $(MINTAMP_ROOT)/real/*.c)
MINTAMP_AAC_ALL = $(wildcard $(MINTAMP_ROOT)/decoders/aac/*.c)
MINTAMP_AAC = $(filter-out $(wildcard $(MINTAMP_ROOT)/decoders/aac/sbr*.c), \
                           $(MINTAMP_AAC_ALL))
MINTAMP_FLAGS = -DAMIGA_M68K -DMR_HOST_BUILD -DARDUINO -DESP8266 \
                -I$(MINTAMP_ROOT)/pub -I$(MINTAMP_ROOT)/real \
                -I$(MINTAMP_ROOT)/decoders/aac \
                -I$(MINTAMP_ROOT)/decoders/aac-arduino-shim
LIBA52_ROOT = vendor/liba52
LIBA52_FLAGS = -I$(LIBA52_ROOT)
LIBA52_SRC = $(LIBA52_ROOT)/bit_allocate.c $(LIBA52_ROOT)/bitstream.c \
             $(LIBA52_ROOT)/downmix.c $(LIBA52_ROOT)/imdct.c \
             $(LIBA52_ROOT)/parse.c \
             $(MINTAMP_ROOT)/decoders/wma/fft-ffmpeg.c \
             $(MINTAMP_ROOT)/decoders/wma/mdct_lookup.c
HEADERS  = core/mr_codec.h core/mr_source.h core/mr_http.h core/mr_hls.h \
           core/mr_youtube.h \
           core/mr_demux.h core/mr_latm.h core/mr_mkv.h core/mr_avi.h core/mr_mov.h \
           core/mr_ts.h core/mr_ps.h \
           core/mr_raw_mjpeg.h core/mr_raw_mpeg4.h \
           core/mr_dither.h core/mr_ham.h core/mr_scale.h core/mr_c2p.h \
           core/mr_mpeg1.h core/mr_mpeg2.h \
           core/mr_mpeg4.h core/mr_mpeg4_tables.inc \
           core/mr_msmpeg4v2.h core/mr_wmv.h core/mr_wmv_tables.inc \
           core/mr_wmv2.h core/mr_wmv2_tables.inc \
           core/mr_h263.h core/mr_h264.h core/mr_types.h
HEADERS  += core/mr_msvideo1.h core/mr_rle.h
HEADERS  += core/mr_rawvideo.h core/mr_yuv.h
HEADERS  += amiga/mintvid_version.h

all: mr_decode

# pl_mpeg is integer-only; MR_HAVE_MPEG1 enables the harness path.
mr_decode: $(TESTSRC) $(CORE) $(HEADERS) | check-libavc
	$(CC) $(CFLAGS) -DMR_HAVE_MPEG1 -o $@ $(TESTSRC) $(CORE)

# Optional host HTTPS build used by the local TLS integration test. The Amiga
# build uses AmiSSL instead; this only validates the shared HTTP/TLS parser and
# response lifecycle on the development host.
mr_decode_ssl: $(TESTSRC) $(CORE) $(HEADERS) | check-libavc
	$(CC) $(CFLAGS) -DMR_HAVE_MPEG1 -DMR_HTTP_HAVE_OPENSSL \
	    -o $@ $(TESTSRC) $(CORE) -lm -lssl -lcrypto

mr_scale_check: tests/mr_scale_check.c core/mr_scale.c core/mr_scale.h \
                core/mr_types.h
	$(CC) $(CFLAGS) -o $@ tests/mr_scale_check.c core/mr_scale.c

mr_yuv_check: tests/mr_yuv_check.c core/mr_yuv.c core/mr_yuv.h
	$(CC) $(CFLAGS) -o $@ tests/mr_yuv_check.c core/mr_yuv.c

mr_c2p_check: tests/mr_c2p_check.c core/mr_c2p.c core/mr_c2p.h \
              core/mr_types.h
	$(CC) $(CFLAGS) -o $@ tests/mr_c2p_check.c core/mr_c2p.c

mr_ham_check: tests/mr_ham_check.c core/mr_ham.c core/mr_ham.h core/mr_types.h
	$(CC) $(CFLAGS) -o $@ tests/mr_ham_check.c core/mr_ham.c

mr_dither_check: tests/mr_dither_check.c core/mr_dither.c core/mr_dither.h \
                 core/mr_types.h
	$(CC) $(CFLAGS) -o $@ tests/mr_dither_check.c core/mr_dither.c

mr_yuv_dither_check: tests/mr_yuv_dither_check.c core/mr_yuv_dither.c \
                     core/mr_yuv_dither.h core/mr_yuv.c core/mr_yuv.h \
                     core/mr_scale.c core/mr_scale.h core/mr_dither.c \
                     core/mr_dither.h core/mr_types.h
	$(CC) $(CFLAGS) -o $@ tests/mr_yuv_dither_check.c core/mr_yuv_dither.c \
	    core/mr_yuv.c core/mr_scale.c core/mr_dither.c

mr_yuv_ham_check: tests/mr_yuv_ham_check.c core/mr_yuv_ham.c \
                  core/mr_yuv_ham.h core/mr_yuv.c core/mr_yuv.h \
                  core/mr_scale.c core/mr_scale.h core/mr_ham.c \
                  core/mr_ham.h core/mr_types.h
	$(CC) $(CFLAGS) -o $@ tests/mr_yuv_ham_check.c core/mr_yuv_ham.c \
	    core/mr_yuv.c core/mr_scale.c core/mr_ham.c

mr_mpeg1_probe_check: tests/mr_mpeg1_probe_check.c core/mr_mpeg1.c \
                      core/mr_mpeg1.h core/pl_mpeg.h core/mr_types.h
	$(CC) $(CFLAGS) -o $@ tests/mr_mpeg1_probe_check.c core/mr_mpeg1.c

mr_ps_check: tests/mr_ps_check.c core/mr_ps.c core/mr_ps.h \
             core/mr_demux.h core/mr_types.h
	$(CC) $(CFLAGS) -o $@ tests/mr_ps_check.c core/mr_ps.c

mr_ts_mp2_check: tests/mr_ts_mp2_check.c core/mr_ts.c core/mr_ts.h \
                 core/mr_latm.c core/mr_latm.h
	$(CC) $(CFLAGS) -o $@ tests/mr_ts_mp2_check.c core/mr_ts.c core/mr_latm.c

mr_mkv_check: tests/mr_mkv_check.c core/mr_mkv.c core/mr_mkv.h
	$(CC) $(CFLAGS) -o $@ tests/mr_mkv_check.c core/mr_mkv.c

mr_mov_seek_check: tests/mr_mov_seek_check.c core/mr_mov.c core/mr_mov.h \
                   core/mr_rawvideo.c core/mr_demux.h core/mr_types.h
	$(CC) $(CFLAGS) -o $@ tests/mr_mov_seek_check.c core/mr_mov.c core/mr_rawvideo.c

mr_audio_check: tests/mr_audio_check.c audio/mr_audio_decode.c audio/mr_pcm.c \
                audio/mr_audio_decode.h $(CORE)
	@test -f $(MINTAMP_ROOT)/pub/mp3dec.h -a \
	      -f $(MINTAMP_ROOT)/decoders/aac/aacdec.h || { \
	  echo "ERROR: initialise the MintAMP submodule first:"; \
	  echo "  git submodule update --init --recursive"; exit 1; }
	$(CC) $(CFLAGS) $(MINTAMP_FLAGS) $(LIBA52_FLAGS) -o $@ \
	    tests/mr_audio_check.c audio/mr_audio_decode.c audio/mr_pcm.c $(CORE) \
	    $(MINTAMP_MP3) $(MINTAMP_AAC) $(LIBA52_SRC) -lm
mr_audio_check: | check-libavc

check-libavc:
	@test -f $(LIBAVC_ROOT)/decoder/ih264d.h || { \
	  echo "ERROR: initialise the libavc submodule first:"; \
	  echo "  git submodule update --init --recursive"; exit 1; }

# OpenAmiSSLTags(AmiSSL_InitAmiSSL=TRUE) makes CloseAmiSSL responsible for
# its implicit cleanup. A direct CleanupAmiSSL call before it is a double
# cleanup and has caused an illegal-instruction crash in the HLS worker.
check-amissl-lifecycle:
	@! grep -Eq '^[[:space:]]*CleanupAmiSSL[[:space:]]*[(]' core/mr_http.c || { \
	  echo "ERROR: mr_http must not call CleanupAmiSSL directly;"; \
	  echo "       CloseAmiSSL performs it for AmiSSL_InitAmiSSL=TRUE"; exit 1; }

# The HLS worker executes inside mrplay's own code segment. A bounded
# "leak and continue" path lets main() unload that segment beneath the live
# task and turns a slow stop into an #80000004 illegal-instruction alert.
check-hls-worker-lifecycle:
	@! grep -Eq 'HLS_FETCH_STOP_BOUND_MS|hls_fetch_wait_busy_bounded|g_poisoned|hls_fetch_leaked' \
	    amiga/hls_fetch.c amiga/hls_fetch.h amiga/mrplay.c || { \
	  echo "ERROR: HLS worker teardown must not retain an abandon path"; exit 1; }
	@grep -q 'static void hls_fetch_wait_busy_forever' amiga/hls_fetch.c || { \
	  echo "ERROR: HLS worker teardown must join the in-process worker"; exit 1; }
	@grep -q 'if (g_stopping || g_busy) return;' amiga/hls_fetch.c || { \
	  echo "ERROR: HLS shutdown must suppress queued lookahead promotion"; exit 1; }

# Run the Cinepak conformance check against ffmpeg's own decoder output, for
# both the AVI and MOV containers. Regenerates reference PPMs if not present.
mr_legacy_video_check: tests/mr_legacy_video_check.c \
                       core/mr_msvideo1.c core/mr_rle.c
	$(CC) $(CFLAGS) -o $@ $^

mr_codec_registry_check: tests/mr_codec_registry_check.c core/mr_codec.c \
                         core/mr_mpeg4.c core/mr_msmpeg4v2.c \
                         core/mr_h263.c core/mr_yuv.c
	$(CC) $(CFLAGS) -o $@ $^

mr_rawvideo_check: tests/mr_rawvideo_check.c core/mr_rawvideo.c
	$(CC) $(CFLAGS) -o $@ $^

mr_pcm_u8_check: tests/mr_pcm_u8_check.c audio/mr_pcm.c audio/mr_pcm.h \
                 core/mr_demux.h
	$(CC) $(CFLAGS) -o $@ tests/mr_pcm_u8_check.c audio/mr_pcm.c

mr_audio_rate_check: tests/mr_audio_rate_check.c amiga/mr_audio_rate.h
	$(CC) $(CFLAGS) -o $@ tests/mr_audio_rate_check.c

mr_aspect_check: tests/mr_aspect_check.c amiga/mr_aspect.h
	$(CC) $(CFLAGS) -o $@ tests/mr_aspect_check.c

mr_media_clock_check: tests/mr_media_clock_check.c core/mr_media_clock.c \
                      core/mr_media_clock.h
	$(CC) $(CFLAGS) -o $@ tests/mr_media_clock_check.c core/mr_media_clock.c

mr_h264_m68k_check: tests/mr_h264_m68k_check.c \
                     vendor/libavc_port/ih264_m68k_optim.c \
                     vendor/libavc_port/ih264_m68k_optim.h \
                     vendor/libavc_port/ih264_m68k_interp.S \
                     vendor/libavc_port/ih264_m68k_deblk.S \
                     vendor/libavc_port/ih264_m68k_cabac.S \
                     vendor/libavc_port/ih264_m68k_chroma_mc.S \
                     vendor/libavc_port/ih264_m68k_weighted_pred.S \
                     vendor/libavc_port/ih264_m68k_mvpred.S \
                     vendor/libavc_port/ih264_m68k_iquant_itrans_recon.S \
                     vendor/libavc_port/ih264_m68k_intra_pred.S
	$(CC) $(CFLAGS) -o $@ tests/mr_h264_m68k_check.c \
	    vendor/libavc_port/ih264_m68k_optim.c \
	    vendor/libavc_port/ih264_m68k_interp.S \
	    vendor/libavc_port/ih264_m68k_deblk.S \
	    vendor/libavc_port/ih264_m68k_cabac.S \
	    vendor/libavc_port/ih264_m68k_chroma_mc.S \
	    vendor/libavc_port/ih264_m68k_weighted_pred.S \
	    vendor/libavc_port/ih264_m68k_mvpred.S \
	    vendor/libavc_port/ih264_m68k_iquant_itrans_recon.S \
	    vendor/libavc_port/ih264_m68k_intra_pred.S

mr_h264_cabac_coeff_check: tests/mr_h264_cabac_coeff_check.c $(LIBAVC_SRC)
	$(CC) $(CFLAGS) -o $@ tests/mr_h264_cabac_coeff_check.c $(LIBAVC_SRC)

mr_h264_mvpred_dispatch_check: tests/mr_h264_mvpred_dispatch_check.c $(LIBAVC_SRC)
	$(CC) $(CFLAGS) -o $@ tests/mr_h264_mvpred_dispatch_check.c $(LIBAVC_SRC)

mr_h264_mc_degrade_check: tests/mr_h264_mc_degrade_check.c $(LIBAVC_SRC)
	$(CC) $(CFLAGS) -o $@ tests/mr_h264_mc_degrade_check.c $(LIBAVC_SRC)

mr_h264_recon8x8_check: tests/mr_h264_recon8x8_check.c $(LIBAVC_SRC)
	$(CC) $(CFLAGS) -o $@ tests/mr_h264_recon8x8_check.c $(LIBAVC_SRC)

mr_h264_intra8x8_check: tests/mr_h264_intra8x8_check.c $(LIBAVC_SRC)
	$(CC) $(CFLAGS) -o $@ tests/mr_h264_intra8x8_check.c $(LIBAVC_SRC)

mr_h264_intra_chroma_check: tests/mr_h264_intra_chroma_check.c $(LIBAVC_SRC)
	$(CC) $(CFLAGS) -o $@ tests/mr_h264_intra_chroma_check.c $(LIBAVC_SRC)

mr_iptv_check: tests/mr_iptv_check.c iptv/mr_iptv.c iptv/mr_iptv_json.c \
               iptv/mr_iptv_m3u.c iptv/mr_iptv_streaming.c iptv/mr_iptv.h \
               core/mr_play_options.c core/mr_play_options.h
	$(CC) $(CFLAGS) -o $@ tests/mr_iptv_check.c iptv/mr_iptv.c \
	    iptv/mr_iptv_json.c iptv/mr_iptv_m3u.c iptv/mr_iptv_streaming.c \
	    core/mr_play_options.c

mr_youtube_check: tests/mr_youtube_check.c core/mr_youtube.c core/mr_http.c \
                  core/mr_source.c core/mr_hls.c core/mr_youtube.h
	$(CC) $(CFLAGS) -o $@ tests/mr_youtube_check.c core/mr_youtube.c \
	    core/mr_http.c core/mr_source.c core/mr_hls.c

mr_hls_override_check: tests/mr_hls_override_check.c core/mr_hls.c \
                       core/mr_http.c core/mr_source.c core/mr_youtube.c
	$(CC) $(CFLAGS) -o $@ tests/mr_hls_override_check.c core/mr_hls.c \
	    core/mr_http.c core/mr_source.c core/mr_youtube.c

mr_youtube_search_check: tests/mr_youtube_search_check.c \
                         youtube/mr_youtube_search.c \
                         youtube/mr_youtube_search.h
	$(CC) $(CFLAGS) -o $@ tests/mr_youtube_search_check.c \
	    youtube/mr_youtube_search.c

check: check-amissl-lifecycle check-hls-worker-lifecycle mr_decode mr_codec_registry_check mr_scale_check mr_yuv_check mr_c2p_check mr_ham_check mr_dither_check mr_yuv_dither_check mr_yuv_ham_check mr_mpeg1_probe_check mr_ps_check mr_ts_mp2_check mr_mkv_check mr_mov_seek_check mr_legacy_video_check mr_rawvideo_check mr_pcm_u8_check mr_audio_rate_check mr_aspect_check mr_media_clock_check mr_h264_m68k_check mr_h264_mc_degrade_check mr_iptv_check mr_youtube_check mr_youtube_search_check mr_hls_override_check
	./mr_youtube_search_check
	./mr_youtube_check
	./mr_hls_override_check
	./mr_iptv_check
	./mr_codec_registry_check
	./mr_pcm_u8_check
	./mr_audio_rate_check
	./mr_aspect_check
	./mr_media_clock_check
	./mr_h264_m68k_check
	./mr_h264_mc_degrade_check
	./mr_decode ../testfiles/mov+cinepak+pcm_s8++grosspnt.mov 2>&1 \
	    | grep -F "audio: PCM S8 mono 11025 Hz (twos)"
	./mr_rawvideo_check
	./mr_legacy_video_check
	./mr_scale_check
	./mr_c2p_check
	./mr_ham_check
	./mr_dither_check
	./mr_yuv_dither_check
	./mr_yuv_ham_check
	./mr_mpeg1_probe_check
	./mr_ps_check
	./mr_ts_mp2_check
	@test -d tests/assets/ref_cinepak && test -d tests/assets/ref_mov \
	    && test -d tests/assets/ref_mjpeg && test -d tests/assets/ref_raw_mjpeg \
	    && test -d tests/assets/ref_mpeg1 \
	    && test -d tests/assets/ref_mp4v_intra && test -d tests/assets/ref_mp4v_sp \
	    && test -d tests/assets/ref_mp42 && test -f tests/assets/test_div2.avi \
	    && test -d tests/assets/ref_wmv1 && test -d tests/assets/ref_wmv1_q20 \
	    && test -d tests/assets/ref_wmv2 && test -d tests/assets/ref_wmv2_q20 \
	    && test -d tests/assets/ref_h264_high \
	    && test -f tests/assets/test_h264_aac.ts \
	    && test -f tests/assets/test_h264_aac.m2ts \
	    && test -f tests/assets/test_h264_latm.ts \
	    && test -f tests/assets/test_h264_ac3.ts \
	    && test -f tests/assets/test_h264_aac.mkv \
	    && test -f tests/assets/test_mpeg2.ts \
	    && test -d tests/assets/ref_mpeg2_ts \
	    && test -d tests/assets/ref_opendivx_legacy \
	    && test -d tests/assets/ref_mp4v_qpel && test -d tests/assets/ref_mp4v_b \
	    && test -d tests/assets/ref_raw_mpeg4 \
	    || sh tests/gen_assets.sh
	./mr_mkv_check tests/assets/test_h264_aac.mkv
	@echo "[Cinepak AVI]"
	./mr_decode tests/assets/test_cinepak.avi --check tests/assets/ref_cinepak
	@echo "[Cinepak MOV]"
	./mr_decode tests/assets/test_cinepak.mov --check tests/assets/ref_mov
	@echo "[MOV seek]"
	./mr_mov_seek_check tests/assets/test_cinepak.mov
	@echo "[MJPEG AVI]"
	./mr_decode tests/assets/test_mjpeg.avi --check tests/assets/ref_mjpeg
	@echo "[raw MJPEG]"
	./mr_decode tests/assets/test_raw_mjpeg.mjpeg --check tests/assets/ref_raw_mjpeg
	@echo "[MPEG-1 PS]"
	./mr_decode tests/assets/test_mpeg1.mpg --check tests/assets/ref_mpeg1
	@echo "[MPEG-4 Part 2 intra]"
	./mr_decode tests/assets/test_mp4v_intra.avi --check tests/assets/ref_mp4v_intra
	@echo "[MPEG-4 Part 2 Simple Profile]"
	./mr_decode tests/assets/test_mp4v_sp.avi --check tests/assets/ref_mp4v_sp
	@echo "[Microsoft MPEG-4 v2 / MP42]"
	./mr_decode tests/assets/test_mp42.avi --check tests/assets/ref_mp42
	@echo "[Microsoft MPEG-4 v2 / DIV2]"
	./mr_decode tests/assets/test_div2.avi --check tests/assets/ref_mp42
	@echo "[WMV1 / WMV7 qscale 4]"
	./mr_decode tests/assets/test_wmv1.avi --check tests/assets/ref_wmv1
	@echo "[WMV1 / WMV7 qscale 20]"
	./mr_decode tests/assets/test_wmv1_q20.avi --check tests/assets/ref_wmv1_q20
	@echo "[WMV2 / WMV8 qscale 4, loop filter on]"
	./mr_decode tests/assets/test_wmv2.avi --check tests/assets/ref_wmv2
	@echo "[WMV2 / WMV8 qscale 20, loop filter on]"
	./mr_decode tests/assets/test_wmv2_q20.avi --check tests/assets/ref_wmv2_q20
	@echo "[H.264 High Profile avc1 + B-frames]"
	./mr_decode tests/assets/test_h264_high.mp4 --check tests/assets/ref_h264_high
	@echo "[H.264 borrowed YUV display-buffer lifecycle]"
	./mr_decode tests/assets/test_h264_high.mp4 --h264-yuv \
	    | grep -F "decoded 24 frames"
	@echo "[H.264 speed-mode separation]"
	./mr_decode tests/assets/test_h264_high.mp4 --h264-speed=turbo \
	    | grep -F "decoded 17 frames"
	./mr_decode tests/assets/test_h264_high.mp4 --h264-speed=turbo+ \
	    | grep -F "decoded 2 frames"
	./mr_decode tests/assets/test_h264_high.mp4 --h264-speed=turbogt \
	    | grep -F "decoded 17 frames"
	@echo "[MPEG-TS: H.264 High Profile + ADTS AAC]"
	./mr_decode tests/assets/test_h264_aac.ts --check tests/assets/ref_h264_high
	@echo "[M2TS: H.264 High Profile + ADTS AAC]"
	./mr_decode tests/assets/test_h264_aac.m2ts --check tests/assets/ref_h264_high
	@echo "[MPEG-TS: H.264 High Profile + LATM AAC]"
	./mr_decode tests/assets/test_h264_latm.ts --check tests/assets/ref_h264_high
	@echo "[MPEG-TS: H.264 High Profile + AC-3]"
	./mr_decode tests/assets/test_h264_ac3.ts --check tests/assets/ref_h264_high
	@echo "[Matroska: H.264 High Profile + AAC]"
	./mr_decode tests/assets/test_h264_aac.mkv --check tests/assets/ref_h264_high
	@echo "[MPEG-TS: MPEG-2 Main Profile + B-frames]"
	./mr_decode tests/assets/test_mpeg2.ts --check tests/assets/ref_mpeg2_ts
	@echo "[MPEG-4 Part 2 legacy OpenDivX AVI]"
	./mr_decode tests/assets/test_opendivx_legacy.avi --check tests/assets/ref_opendivx_legacy
	@echo "[MPEG-4 Part 2 quarter-pel]"
	./mr_decode tests/assets/test_mp4v_qpel.avi --check tests/assets/ref_mp4v_qpel
	@echo "[MPEG-4 Part 2 B-VOPs]"
	./mr_decode tests/assets/test_mp4v_b.avi --check tests/assets/ref_mp4v_b
	@echo "[raw MPEG-4 Part 2]"
	./mr_decode tests/assets/test_raw_mpeg4.m4v --check tests/assets/ref_raw_mpeg4

check-audio: mr_audio_check
	@test -f tests/assets/test_mp3.avi -a -f tests/assets/test_aac.mp4 \
	    || sh tests/gen_audio_assets.sh
	@test -f tests/assets/test_h264_aac.ts \
	    -a -f tests/assets/test_h264_latm.ts \
	    -a -f tests/assets/test_h264_ac3.mkv \
	    -a -f tests/assets/test_h264_ac3.ts || sh tests/gen_assets.sh
	./mr_audio_check tests/assets/test_mp3.avi mp3
	./mr_audio_check tests/assets/test_aac.mp4 aac
	./mr_audio_check tests/assets/test_h264_aac.ts aac
	./mr_audio_check tests/assets/test_h264_latm.ts latm
	./mr_audio_check tests/assets/test_h264_ac3.mkv ac3
	./mr_audio_check tests/assets/test_h264_ac3.ts ac3

check-http: mr_decode mr_youtube_check
	@test -d tests/assets/ref_mpeg2_ts \
	    && test -d tests/assets/ref_h264_high \
	    && test -d tests/assets/ref_mp42 \
	    || sh tests/gen_assets.sh
	sh tests/run_http_check.sh ./mr_decode http

check-https: mr_decode_ssl
	@test -d tests/assets/ref_mpeg2_ts \
	    && test -d tests/assets/ref_h264_high \
	    && test -d tests/assets/ref_mp42 \
	    || sh tests/gen_assets.sh
	sh tests/run_http_check.sh ./mr_decode_ssl https

# Re-runs the H.264/Cinepak/MPEG-4/MP42 conformance checks and the m68k leaf-
# primitive/YUV checks built for a real m68k target (big-endian, 68030 strict-
# alignment rules) and executed under qemu-m68k, instead of this host's own
# x86-64 CPU. Not the AmigaOS toolchain this project doesn't have on the dev
# host (see CLAUDE.md) - m68k-linux-gnu targets Linux/m68k, so it can't build
# mrplay.c - but it exercises the same CORE + libavc C on real m68k
# instructions, which the plain host build cannot: an endianness or alignment
# bug in a demuxer or in vendor/libavc_port/ih264_m68k_optim.c can pass every
# host check and still be wrong on the hardware this all ships to.
# Requires: apt-get install gcc-m68k-linux-gnu binutils-m68k-linux-gnu qemu-user
check-m68k:
	sh tests/run_m68k_check.sh

clean:
	rm -f mr_decode mr_decode_ssl mr_audio_check mr_scale_check mr_yuv_check mr_c2p_check mr_ham_check \
	      mr_dither_check mr_yuv_dither_check mr_yuv_ham_check mr_mpeg1_probe_check mr_ps_check mr_ts_mp2_check mr_mkv_check \
	      mr_mov_seek_check
	rm -f mr_legacy_video_check
	rm -f mr_rawvideo_check mr_codec_registry_check
	rm -f mr_pcm_u8_check
	rm -f mr_audio_rate_check
	rm -f mr_aspect_check
	rm -f mr_media_clock_check
	rm -f mr_h264_m68k_check
	rm -f mr_h264_mc_degrade_check
	rm -f mr_iptv_check mr_youtube_check mr_youtube_search_check

.PHONY: all check check-audio check-http check-https check-libavc check-m68k clean
