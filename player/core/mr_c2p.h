/*
 * MintVID - chunky-to-planar (C2P) for the AGA path.
 *
 * WritePixelArray8 works but carries general-case overhead (temp RastPort,
 * clipping). This is a specialised C2P that writes 8-bit chunky straight into a
 * screen's bitplanes via an 8x8 bit transpose (host-verified correct). It is
 * the portable-C stand-in for RiVA's hand-tuned RendererAGAC2P.i; a later asm
 * pass can replace the transpose hot loop.
 *
 * Constraints: x0 and the processed width must be multiples of 8 (byte
 * aligned). The caller pads the chunky buffer and aligns x0.
 */
#ifndef MR_C2P_H
#define MR_C2P_H

#include "mr_types.h"

/* Convert a chunky frame (pw wide, multiple of 8) into `nplanes` bitplanes.
 * planes[k] is plane k's memory; bpr = bytes per plane row; x0byte = x offset
 * in bytes (x0/8); y0 = first destination row. Non-interleaved planes. */
void mr_c2p8(const uint8_t *chunky, int pw, int h, int chunky_stride,
             int nplanes, uint8_t *const planes[], int bpr,
             int x0byte, int y0);

/* 32-pixel direct-to-plane variant using only 32-bit integer operations. */
void mr_c2p8_riva32(const uint8_t *chunky, int pw, int h, int chunky_stride,
                    int nplanes, uint8_t *const planes[], int bpr,
                    int x0byte, int y0);

/* Copy changed words from Fast RAM, keeping a Fast-RAM shadow. First use
 * must pass valid=0. Returns bytes written to the destination. */
unsigned long mr_planar_shadow_copy(const uint8_t *src, uint8_t *dst,
                                    uint8_t *shadow, unsigned long bytes, int valid);

#endif /* MR_C2P_H */
