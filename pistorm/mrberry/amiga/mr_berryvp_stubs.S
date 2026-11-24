/* C-callable wrappers for berryvp.library's register ABI.
 * Bebbo GCC C ABI -> A6 library base + D/A register arguments. */
	.text
	.even
	.globl _bvp_call_getversion
	.globl bvp_call_getversion
	.globl _bvp_call_getcaps
	.globl bvp_call_getcaps
	.globl _bvp_call_openstream
	.globl bvp_call_openstream
	.globl _bvp_call_closestream
	.globl bvp_call_closestream
	.globl _bvp_call_configvideo
	.globl bvp_call_configvideo
	.globl _bvp_call_sendvideo
	.globl bvp_call_sendvideo
	.globl _bvp_call_configaudio
	.globl bvp_call_configaudio
	.globl _bvp_call_sendaudio
	.globl bvp_call_sendaudio
	.globl _bvp_exec_openlibrary
	.globl bvp_exec_openlibrary
	.globl _bvp_exec_closelibrary
	.globl bvp_exec_closelibrary
	.globl _bvp_exec_setsignal
	.globl bvp_exec_setsignal

_bvp_call_getversion:
bvp_call_getversion:
	move.l %a6,-(%sp)
	movea.l 8(%sp),%a6
	jsr -30(%a6)
	movea.l (%sp)+,%a6
	rts

_bvp_call_getcaps:
bvp_call_getcaps:
	move.l %a6,-(%sp)
	movea.l 8(%sp),%a6
	jsr -36(%a6)
	movea.l (%sp)+,%a6
	rts

_bvp_call_openstream:
bvp_call_openstream:
	move.l %a6,-(%sp)
	movea.l 8(%sp),%a6
	move.l 12(%sp),%d0
	jsr -42(%a6)
	movea.l (%sp)+,%a6
	rts

_bvp_call_closestream:
bvp_call_closestream:
	move.l %a6,-(%sp)
	movea.l 8(%sp),%a6
	jsr -48(%a6)
	movea.l (%sp)+,%a6
	rts

_bvp_call_configvideo:
bvp_call_configvideo:
	movem.l %d2-%d3/%a6,-(%sp)
	movea.l 16(%sp),%a6
	move.l 20(%sp),%d0
	move.l 24(%sp),%d1
	move.l 28(%sp),%d2
	move.l 32(%sp),%d3
	jsr -54(%a6)
	movem.l (%sp)+,%d2-%d3/%a6
	rts

_bvp_call_sendvideo:
bvp_call_sendvideo:
	movem.l %d2-%d3/%a6,-(%sp)
	movea.l 16(%sp),%a6
	movea.l 20(%sp),%a0
	move.l 24(%sp),%d0
	move.l 28(%sp),%d1
	move.l 32(%sp),%d2
	move.l 36(%sp),%d3
	jsr -66(%a6)
	movem.l (%sp)+,%d2-%d3/%a6
	rts


_bvp_call_configaudio:
bvp_call_configaudio:
	movem.l %d2-%d3/%a6,-(%sp)
	movea.l 16(%sp),%a6
	move.l 20(%sp),%d0
	move.l 24(%sp),%d1
	move.l 28(%sp),%d2
	move.l 32(%sp),%d3
	jsr -60(%a6)
	movem.l (%sp)+,%d2-%d3/%a6
	rts

_bvp_call_sendaudio:
bvp_call_sendaudio:
	movem.l %d2-%d3/%a6,-(%sp)
	movea.l 16(%sp),%a6
	movea.l 20(%sp),%a0
	move.l 24(%sp),%d0
	move.l 28(%sp),%d1
	move.l 32(%sp),%d2
	move.l 36(%sp),%d3
	jsr -72(%a6)
	movem.l (%sp)+,%d2-%d3/%a6
	rts



/* Exec wrappers deliberately bypass GCC/libnix _SysBase.
 * Classic Amiga ExecBase is the longword at absolute address 4. */
_bvp_exec_openlibrary:
bvp_exec_openlibrary:
	move.l %a6,-(%sp)
	move.l 4.w,%a6
	movea.l 8(%sp),%a1
	move.l 12(%sp),%d0
	jsr -408(%a6)
	movea.l (%sp)+,%a6
	rts

_bvp_exec_closelibrary:
bvp_exec_closelibrary:
	move.l %a6,-(%sp)
	move.l 4.w,%a6
	movea.l 8(%sp),%a1
	jsr -414(%a6)
	movea.l (%sp)+,%a6
	rts

_bvp_exec_setsignal:
bvp_exec_setsignal:
	move.l %a6,-(%sp)
	move.l 4.w,%a6
	move.l 8(%sp),%d0
	move.l 12(%sp),%d1
	jsr -306(%a6)
	movea.l (%sp)+,%a6
	rts

/* AC309I: exact VPv102/VPv149 1-MHz BCM2835 system-timer source.
 * Pi peripheral register is little-endian; m68k is big-endian. */
	.globl _ac309i_read_sys_us
	.globl ac309i_read_sys_us
_ac309i_read_sys_us:
ac309i_read_sys_us:
	movea.l #0xF2003004,%a0
	move.l (%a0),%d0
	ror.w #8,%d0
	swap %d0
	ror.w #8,%d0
	rts

/* P96W45: BVP_Control(cmd) - LVO -78, D0 = command. */
	.globl _bvp_call_control
	.globl bvp_call_control
_bvp_call_control:
bvp_call_control:
	move.l %a6,-(%sp)
	movea.l 8(%sp),%a6
	move.l 12(%sp),%d0
	jsr -78(%a6)
	movea.l (%sp)+,%a6
	rts

/* P96W47: BVP_Control(cmd, arg) - D0 = command, D1 = argument. */
	.globl _bvp_call_control2
	.globl bvp_call_control2
_bvp_call_control2:
bvp_call_control2:
	move.l %a6,-(%sp)
	movea.l 8(%sp),%a6
	move.l 12(%sp),%d0
	move.l 16(%sp),%d1
	jsr -78(%a6)
	movea.l (%sp)+,%a6
	rts
