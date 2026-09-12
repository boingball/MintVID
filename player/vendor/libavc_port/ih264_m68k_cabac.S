/*
 * Hand-written m68k assembly for the single most-executed CABAC primitive
 * in H.264 decode: decoding one "regular" (context-modelled) arithmetic-
 * coding decision, spec 9.3.3.2.2 ("Arithmetic decoding process"). Chosen
 * over hand-porting the much larger, already-hand-inlined
 * ih264d_read_coeff4x4_cabac() (residual coefficient decode, ~500 lines,
 * with several implementation-specific deferred-renormalisation windows
 * at different thresholds that are easy to get subtly wrong) because this
 * is the one CABAC building block that (a) is a real, standalone,
 * non-inlined C function already (ih264d_decode_bin() in ih264d_cabac.c),
 * called from ~40+ sites across every CABAC-coded syntax element in the
 * decoder - mb_type, cbp, ref_idx, mvd, intra pred modes, mb_qp_delta,
 * coded_block_flag, etc, not just residual coefficients - and (b) has a
 * small, crisp, independently specifiable contract with no data-dependent
 * structural branching beyond its own two conditionals, verifiable to the
 * same bar as every other piece of hand-asm in this port. A real-hardware
 * timing breakdown (ih264d_stage_profile.c's mc/deblock/recon/intra
 * buckets) and a host callgrind profile both point at CABAC/CAVLC parsing
 * as the largest remaining unmeasured slice of libavc-core time, with
 * ih264d_read_coeff4x4_cabac() the single hottest function in it - this
 * primitive is that function's (and everything else's) inner loop.
 *
 * UWORD32 mr_ih264d_decode_bin_m68k(UWORD32 u4_ctx_inc,
 *     bin_ctxt_model_t *ps_src_bin_ctxt, dec_bit_stream_t *ps_bitstrm,
 *     decoding_envirnoment_t *ps_cab_env);
 *
 * Ported instruction-for-instruction from ih264d_decode_bin()'s C (which
 * is exactly DECODE_ONE_BIN_MACRO in ih264d_cabac.h - renormalisation
 * always at the ONE_RIGHT_SHIFTED_BY_8 = 256 threshold, no deferred-renorm
 * trick), not the differently-tuned inlined copies inside
 * ih264d_read_coeff4x4_cabac() that use other thresholds (or none at all)
 * as a manual optimisation for that function's specific call pattern -
 * those stay exactly as shipped, untouched by this change.
 *
 * bin_ctxt_model_t is a single UWORD8 (u1_mps_state), so
 * ps_src_bin_ctxt + u4_ctx_inc is byte pointer arithmetic. That one byte
 * packs both the 6-bit CABAC state index (bits 0-5) and the 1-bit valMPS
 * (bit 6); bit 7 is always 0 in valid state (matches the C: no masking is
 * applied when using the raw byte as a table index, so none is added
 * here either - this is a faithful port, not a hardening pass).
 *
 * decoding_envirnoment_t layout: 0=u4_code_int_range (UWORD32),
 * 4=u4_code_int_val_ofst (UWORD32), 8=cabac_table (pointer).
 * dec_bit_stream_t layout: 0=u4_ofst (UWORD32), 4=pu4_buffer (pointer).
 * cabac_table is a 512-entry (128 mps_states x 4 quantised-range buckets)
 * UWORD32 table; each packed entry is rangeLPS (bits 0-7), transIdxMPS
 * (bits 8-14), transIdxLPS (bits 15-21) - read as one 32-bit indexed load.
 *
 * CLZ (spec's renormalisation shift count) is m68k's native BFFFO
 * (68020+, part of the bit-field instruction set) - a single instruction,
 * already what GCC lowers __builtin_clz() to on this target (confirmed via
 * `m68k-linux-gnu-gcc -O2 -m68030 -S`), so this isn't a CLZ-algorithm win;
 * the win is keeping every live value pinned in registers across the
 * whole function body (nothing round-trips through ps_cab_env/ps_bitstrm
 * except the initial loads and final stores) instead of however tightly
 * the compiler's register allocator manages it under real register
 * pressure (11+ live values through a function this size, against m68k's
 * 8 data + ~6 usable address registers). BFFFO on an all-zero source
 * returns 32 (one past the field), not the 31 CLZ() defines for a zero
 * input (CLZ(0)=31 is a defensive C-UB-avoidance default, never a real
 * CABAC state - codIRange is a decoder invariant that never reaches zero)
 * - both CLZ computations below still replicate the exact zero-guarded
 * value for bit-exact equivalence in all cases, not just realistic ones.
 *
 * NEXTBITS/FLUSHBITS (ih264d_bitstrm.h): the bitstream is a UWORD32 array;
 * NEXTBITS(word, bitpos, buf, n) extracts n bits starting at bitpos
 * without consuming them (buf[bitpos>>5]<<bit_off, merged with
 * buf[(bitpos>>5)+1]>>(32-bit_off) when bit_off=bitpos&31 is nonzero, then
 * shifted down to n bits) - already-natural big-endian bit order on this
 * target, no byte-swap needed. FLUSHBITS just advances the bit cursor.
 * RENORM_RANGE_OFFSET's "+23" is this implementation's lookahead-window
 * convention: u4_code_int_val_ofst holds 9 "real" CABAC offset bits plus
 * up to 23 bits already prefetched from 23 bits ahead of the true stream
 * cursor, refilled exactly clz bits at a time to stay full - replicated
 * exactly here, not re-derived from the spec's naive bit-at-a-time model.
 *
 * Verification: differential fuzz test against the original C
 * (ih264d_decode_bin_generic, kept byte-for-byte as shipped, renamed only)
 * across randomised in-domain (range, offset, mps_state, bitstream)
 * states, asserting bit-exact match of the returned symbol and every
 * mutated output (range, offset, mps_state, bitstream cursor) - see
 * check_decode_bin_cabac() in tests/mr_h264_m68k_check.c. Plus the full
 * make check / make check-m68k H.264-vs-ffmpeg conformance suite (worst
 * per-frame MAE must stay unchanged) and a qemu -d exec trace hit-count
 * check that this function actually fires during real decode.
 *
 * __wrap_ih264d_decode_bin alias (below, !MR_H264_CABAC_PROFILE only):
 * ih264d_cabac_wrap.c used to be the *only* place providing this symbol -
 * a plain C function whose entire body was `return
 * mr_ih264d_decode_bin_m68k(u4_ctx_inc, ps_src_bin_ctxt, ps_bitstrm,
 * ps_cab_env);`. That is a second full call/return layer - its own
 * prologue/epilogue, its own reload of all four arguments from its
 * caller's stack frame to pass down again - wrapped around a function
 * whose own header comment above already justifies hand-asm on the
 * strength of "keeping every live value pinned in registers across the
 * whole function body". Paid on every single decoded bin (mb_type, cbp,
 * ref_idx, mvd, intra pred modes, mb_qp_delta, residual significance/
 * coded_block_flag - tens of thousands of calls per frame), this was
 * exactly the kind of per-call tax that primitive was hand-written to
 * avoid one layer further out.
 *
 * The fix: --wrap only needs a symbol named __wrap_ih264d_decode_bin to
 * exist somewhere in the link with the right calling convention - it does
 * not have to be a C function. Exporting it as a second name for this
 * exact same asm entry point (identical argument layout: this function's
 * four arguments are exactly __wrap_ih264d_decode_bin's four arguments,
 * ph264d_decode_bin's own C signature) removes the trampoline layer
 * entirely - GNU ld's --wrap=ih264d_decode_bin now redirects every call
 * directly into this function, with zero C code in between.
 * mr_ih264d_decode_bin_m68k stays exported too (tests/mr_h264_m68k_check.c
 * calls it directly by that name for the differential fuzz test above),
 * both names simply labelling the same address.
 *
 * Gated !defined(MR_H264_CABAC_PROFILE): a diagnostic build wants the C
 * trampoline back (see ih264d_cabac_wrap.c), so it can count and time
 * every call through it instead - real overhead, opt-in only, same
 * philosophy as every other wrapper in ih264d_stage_profile.c. The two
 * files' guards are complementary (never both true at once), so exactly
 * one of them provides __wrap_ih264d_decode_bin in any given build -
 * never zero (--wrap hard-errors on a missing wrapper) and never both
 * (duplicate symbol).
 */
#if defined(MR_M68K_ASM)
	.text
	.even
	.globl	mr_ih264d_decode_bin_m68k
#if !defined(MR_H264_CABAC_PROFILE)
	.globl	__wrap_ih264d_decode_bin
#endif

mr_ih264d_decode_bin_m68k:
#if !defined(MR_H264_CABAC_PROFILE)
__wrap_ih264d_decode_bin:
#endif
	movem.l	%d2-%d7/%a2-%a4,-(%sp)
	move.l	40(%sp),%d0		/* u4_ctx_inc      (4+36)  */
	movea.l	44(%sp),%a0		/* ps_src_bin_ctxt (8+36)  */
	adda.l	%d0,%a0			/* a0 = ps_bin_ctxt = ps_src_bin_ctxt + ctx_inc (1-byte struct) */
	movea.l	48(%sp),%a2		/* a2 = ps_bitstrm  (12+36) */
	movea.l	52(%sp),%a3		/* a3 = ps_cab_env  (16+36) */

	move.l	(%a3),%d0		/* d0 = range = ps_cab_env->u4_code_int_range */
	move.l	4(%a3),%d1		/* d1 = val_ofst = ps_cab_env->u4_code_int_val_ofst */
	movea.l	8(%a3),%a1		/* a1 = pu4_table = ps_cab_env->cabac_table */

	clr.l	%d2
	move.b	(%a0),%d2		/* d2 = mps_state = ps_bin_ctxt->u1_mps_state */

	/* clz = CLZ(range); CLZ()'s own zero-guard (CLZ(0)=31) replicated */
	tst.l	%d0
	bne.b	.Lbin_clz1_nz
	moveq	#31,%d3
	bra.b	.Lbin_clz1_done
.Lbin_clz1_nz:
	bfffo	%d0{#0:#0},%d3
.Lbin_clz1_done:
	/* d3 = clz */

	move.l	%d0,%d4
	lsl.l	%d3,%d4			/* d4 = range << clz */
	moveq	#29,%d7
	lsr.l	%d7,%d4
	and.l	#3,%d4			/* d4 = qnt_int_range (0..3) */

	move.l	%d2,%d5
	lsl.l	#2,%d5
	add.l	%d4,%d5			/* d5 = (mps_state<<2)+qnt : table index */
	move.l	(%a1,%d5.l*4),%d4	/* d4 = table_lookup */

	move.l	%d4,%d5
	and.l	#255,%d5		/* d5 = range_lps = table_lookup & 0xff */
	moveq	#23,%d7
	sub.l	%d3,%d7			/* d7 = 23-clz */
	lsl.l	%d7,%d5			/* d5 = range_lps << (23-clz) */

	sub.l	%d5,%d0			/* range -= range_lps */

	move.l	%d2,%d6
	moveq	#6,%d7
	lsr.l	%d7,%d6
	and.l	#1,%d6			/* d6 = symbol = (mps_state_orig>>6)&1 */

	move.l	%d4,%d2
	moveq	#8,%d7
	lsr.l	%d7,%d2
	and.l	#0x7f,%d2		/* d2 = mps_state (tentative MPS-path value) */

	/* CHECK_IF_LPS: if (val_ofst >= range) take the LPS path */
	cmp.l	%d0,%d1
	bcs.b	.Lbin_mps		/* val_ofst(d1) < range(d0) unsigned -> MPS, skip */
	eor.l	#1,%d6			/* symbol = 1-symbol */
	sub.l	%d0,%d1			/* val_ofst -= range */
	move.l	%d5,%d0			/* range = range_lps */
	move.l	%d4,%d2
	moveq	#15,%d7
	lsr.l	%d7,%d2
	and.l	#0x7f,%d2		/* mps_state = (table_lookup>>15)&0x7f */
.Lbin_mps:

	/* renormalise if range < 256 (ONE_RIGHT_SHIFTED_BY_8) */
	cmp.l	#256,%d0
	bcc.b	.Lbin_no_renorm

	movem.l	%d2/%d6,-(%sp)		/* park mps_state/symbol - free 2 scratch regs */
	movea.l	4(%a2),%a4		/* a4 = ps_bitstrm->pu4_buffer */
	move.l	(%a2),%d7		/* d7 = old bitstream offset */

	tst.l	%d0
	bne.b	.Lbin_clz2_nz
	moveq	#31,%d3
	bra.b	.Lbin_clz2_done
.Lbin_clz2_nz:
	bfffo	%d0{#0:#0},%d3
.Lbin_clz2_done:
	/* d3 = clz2 */

	move.l	%d7,%d4
	add.l	#23,%d4			/* d4 = bitpos = old_offset+23 (NEXTBITS position) */
	add.l	%d3,%d7			/* d7 = new_offset = old_offset+clz2 (FLUSHBITS) */

	move.l	%d4,%d2
	lsr.l	#5,%d2
	lsl.l	#2,%d2			/* d2 = word_off*4 (byte offset into buffer) */
	and.l	#31,%d4			/* d4 = bit_off */

	adda.l	%d2,%a4			/* a4 = &buffer[word_off] */
	move.l	(%a4),%d2		/* d2 = buffer[word_off] */
	lsl.l	%d4,%d2			/* d2 <<= bit_off (no-op if bit_off==0) */

	tst.l	%d4
	beq.b	.Lbin_no_merge
	move.l	4(%a4),%d6		/* d6 = buffer[word_off+1] */
	moveq	#32,%d5
	sub.l	%d4,%d5			/* d5 = 32-bit_off */
	lsr.l	%d5,%d6
	or.l	%d6,%d2			/* merge in the next word's high bits */
.Lbin_no_merge:
	moveq	#32,%d5
	sub.l	%d3,%d5			/* d5 = 32-clz2 */
	lsr.l	%d5,%d2			/* d2 = read_bits */

	lsl.l	%d3,%d0			/* range <<= clz2 */
	lsl.l	%d3,%d1			/* val_ofst <<= clz2 */
	or.l	%d2,%d1			/* val_ofst |= read_bits */

	move.l	%d7,(%a2)		/* ps_bitstrm->u4_ofst = new_offset */
	movem.l	(%sp)+,%d2/%d6		/* restore mps_state, symbol */
.Lbin_no_renorm:

	move.l	%d1,4(%a3)		/* ps_cab_env->u4_code_int_val_ofst = val_ofst */
	move.l	%d0,(%a3)		/* ps_cab_env->u4_code_int_range = range */
	move.b	%d2,(%a0)		/* ps_bin_ctxt->u1_mps_state = mps_state */
	move.l	%d6,%d0			/* return symbol */

	movem.l	(%sp)+,%d2-%d7/%a2-%a4
	rts

#endif /* MR_M68K_ASM */

#if defined(__ELF__)
	.section .note.GNU-stack,"",@progbits
#endif
