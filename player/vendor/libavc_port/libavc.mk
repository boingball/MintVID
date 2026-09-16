# Ittiam libavc decoder, built with its architecture-neutral integer C paths.
# Encoder, MVC/SVC and all CPU-specific assembly are intentionally excluded.
LIBAVC_ROOT ?= vendor/libavc
LIBAVC_PORT ?= vendor/libavc_port

LIBAVC_COMMON = $(filter-out $(LIBAVC_ROOT)/common/ithread.c \
                  $(LIBAVC_ROOT)/common/ih264_resi_trans_quant.c \
                  $(LIBAVC_ROOT)/common/ih264_trans_data.c, \
                  $(wildcard $(LIBAVC_ROOT)/common/*.c))
LIBAVC_DECODER = $(wildcard $(LIBAVC_ROOT)/decoder/*.c)
# The assembly body is guarded by MR_M68K_ASM, so host builds preprocess it
# to an empty translation unit while m68k builds get the real implementation.
# Keeping it in the shared source list avoids a second m68k-only variable.
LIBAVC_PORTSRC = $(LIBAVC_PORT)/ih264d_function_selector_port.c \
                 $(LIBAVC_PORT)/ih264_mc_degrade.c \
                 $(LIBAVC_PORT)/ih264d_stage_profile.c \
                 $(LIBAVC_PORT)/ih264d_cabac_profile.c \
                 $(LIBAVC_PORT)/ih264d_update_qp_wrap.c \
                 $(LIBAVC_PORT)/ih264_m68k_optim.c \
                 $(LIBAVC_PORT)/ih264_m68k_interp.S \
                 $(LIBAVC_PORT)/ih264_m68k_deblk.S \
                 $(LIBAVC_PORT)/ih264_m68k_cabac.S \
                 $(LIBAVC_PORT)/ih264d_cabac_wrap.c \
                 $(LIBAVC_PORT)/ih264_m68k_chroma_mc.S \
                 $(LIBAVC_PORT)/ih264_m68k_weighted_pred.S \
                 $(LIBAVC_PORT)/ih264_m68k_mvpred.S \
                 $(LIBAVC_PORT)/ih264d_mvpred_dispatch_port.c \
                 $(LIBAVC_PORT)/ih264_m68k_cabac_coeff.S \
                 $(LIBAVC_PORT)/ih264_m68k_cabac_coeff8x8.S \
                 $(LIBAVC_PORT)/ih264d_parse_cabac_coeff_port.c \
                 $(LIBAVC_PORT)/ih264d_mbinfo_wrap_port.c \
                 $(LIBAVC_PORT)/ih264d_intramb_wrap_port.c \
                 $(LIBAVC_PORT)/ih264_m68k_iquant_itrans_recon.S \
                 $(LIBAVC_PORT)/ih264_m68k_intra_pred.S \
                 $(LIBAVC_PORT)/ih264_m68k_bs.S \
                 $(LIBAVC_PORT)/ithread_port.c $(LIBAVC_PORT)/compat.c
LIBAVC_SRC = $(LIBAVC_COMMON) $(LIBAVC_DECODER) $(LIBAVC_PORTSRC)
LIBAVC_FLAGS = -I$(LIBAVC_PORT) -I$(LIBAVC_ROOT)/common \
               -I$(LIBAVC_ROOT)/decoder -include $(LIBAVC_PORT)/compat.h
LIBAVC_GCC_FLAGS = -fno-strict-aliasing -fwrapv
# ih264d_cabac_wrap.c's __wrap_ih264d_decode_bin,
# ih264d_mvpred_dispatch_port.c's __wrap_ih264d_mvpred_nonmbaff/
# _nonmbaffB, ih264d_parse_cabac_coeff_port.c's __wrap_ih264d_parse_
# residual4x4_cabac/__wrap_ih264d_read_coeff4x4_cabac, and
# ih264d_update_qp_wrap.c's __wrap_ih264d_update_qp only exist under
# MR_M68K_ASM (see those files), so these flags must only be added to
# m68k cross-build link commands, never the host build - GNU ld's --wrap
# hard-errors with "undefined reference to __wrap_..." if the wrapper
# symbol it redirects to doesn't exist in the link. ih264d_mvpred_mbaff
# (MBAFF slices) and ih264d_read_coeff8x8_cabac (transform8x8/High
# Profile) are deliberately left unwrapped - see ih264d_mvpred_dispatch_
# port.c's and ih264d_parse_cabac_coeff_port.c's header comments for why.
LIBAVC_M68K_LDFLAGS = -Wl,--wrap=ih264d_decode_bin \
                      -Wl,--wrap=ih264d_mvpred_nonmbaff \
                      -Wl,--wrap=ih264d_mvpred_nonmbaffB \
                      -Wl,--wrap=ih264d_parse_residual4x4_cabac \
                      -Wl,--wrap=ih264d_read_coeff4x4_cabac \
                      -Wl,--wrap=ih264d_update_qp
# ih264d_mbinfo_wrap_port.c's __wrap_ih264d_get_mb_info_cabac_nonmbaff and
# ih264d_intramb_wrap_port.c's __wrap_ih264d_parse_imb_cabac only exist
# under MR_H264_CABAC_PROFILE (unlike every wrap above, which exist
# unconditionally under MR_M68K_ASM) - both are pure timing pass-throughs
# with no asm replacement behind them, so there is nothing to gain from
# linking either into a normal (non-profiling) build, only an unwanted
# extra call/return layer on a function called on most macroblocks. So
# these --wrap flags are only added when CABAC_PROFILE=1 is what actually
# defined MR_H264_CABAC_PROFILE in the first place (see Makefile.amiga's
# CABAC_PROFILE_FLAGS) - a normal build links zero bytes of either wrapper.
ifeq ($(CABAC_PROFILE),1)
LIBAVC_M68K_LDFLAGS += -Wl,--wrap=ih264d_get_mb_info_cabac_nonmbaff \
                       -Wl,--wrap=ih264d_parse_imb_cabac
endif
