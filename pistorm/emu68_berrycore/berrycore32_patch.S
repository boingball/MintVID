/*
 * BerryCore patch for Emu68 1.0.7, PiStorm32 lite build (git 305f686)
 * ----------------------------------------------------------------------
 * v96.95 (05.11.2026): the same CPU1 service as berrycore_patch.S (Pi 3,
 * PiStorm Classic), placed into the original "Emu68-pistorm32lite"
 * kernel (Emu68 1.0.7 (08.12.2025), sha256 262afc14...).  The service code
 * is byte for byte the same; only the file offsets differ:
 *
 *   version strings   0x0005B and 0x1607E3   (Pi 3: 0x0005B, 0x1540C3)
 *   secondary_boot    0x00654                (same as Pi 3)
 *   serial_writer()   0x08A60                (Pi 3: 0x08A00)
 *   service           0x21B600, in the zero padding before .trailer
 *                                            (Pi 3: 0x20F500)
 *
 * The layout of secondary_boot() and serial_writer() was compared with
 * the Pi 3 kernel instruction by instruction (same code, other addresses).
 * The mailbox layout, commands and the module interface are described in
 * berrycore_patch.S.
 */

        .section .p_0005b,"a"
        .ascii  "-BerryCore32 1.0.7 (05.11.2026)"
        .byte   0, 0

        .section .p_1607e3,"a"
        .ascii  "-BerryCore32 1.0.7 (05.11.2026)"
        .byte   0, 0

/* secondary_boot(), CPU1: skip the async_log bootargs check (-> 0x688,
   which releases the boot lock and calls 0x8A60). */
        .section .p_00654,"ax"
        b       . + (0x688 - 0x654)

/* Former serial_writer() at 0x8A60: jump to the BerryCore service.
   The rest of the old function stays in the file but is never reached. */
        .section .p_08a60,"ax"
        b       . + (0x21b600 - 0x8a60)

/* ------------------------------------------------------------------ */
/* BerryCore service (file offset 0x21B600, 932 bytes)                 */
/* Placed in an area that is filled with zeros in the original kernel. */
/*                                                                     */
/* Registers kept during the service:                                  */
/*   x20 mailbox address      w21 address mode (1 or 2)                */
/*   x22 loaded module base   w23 module size   w24 module type        */
/*   w27 sequence number of the current request                        */
/*   w12..w15 result 0, result 1, result 3, error code                 */
/* ------------------------------------------------------------------ */
        .section .p_21b600,"ax"
berrycore_service:
        mov     x22, xzr                // no module loaded
        mov     w23, wzr
        mov     w24, wzr

/* Search the mailbox: 0x8000 pages of 4 KiB (128 MiB).
   First pass: address mode 2, ARM alias 0x10_0800_0000.
   Second pass: address mode 1, plain address 0x0800_0000.
   If nothing is found, start again. */
scan_start:
        mov     x0, #0x8000000
        movk    x0, #0x10, lsl #32
        mov     w1, #0x8000
        mov     w7, #2
scan_page:
        mov     w2, #0x3942             // "B99B"
        movk    w2, #0x4239, lsl #16
        mov     w3, #0x3959             // "Y99Y"
        movk    w3, #0x5939, lsl #16
        ldr     w4, [x0]
        cmp     w4, w2
        b.ne    scan_next
        ldr     w4, [x0, #4]
        cmp     w4, w3
        b.eq    mailbox_found
scan_next:
        add     x0, x0, #1, lsl #12
        subs    w1, w1, #1
        b.ne    scan_page
        cmp     w7, #2
        b.ne    berrycore_service       // both passes done: start over
        mov     x0, #0x8000000          // second pass, mode 1
        mov     w1, #0x8000
        mov     w7, #1
        b       scan_page

mailbox_found:
        mov     x20, x0
        mov     w21, w7

/* Main loop: wait for a request ($A5 at +8). */
wait_request:
        dc      ivac, x20               // read fresh mailbox from RAM
        dsb     sy
        mov     w2, #0x3942             // "B99B"
        movk    w2, #0x4239, lsl #16
        mov     w3, #0x3959             // "Y99Y"
        movk    w3, #0x5939, lsl #16
        ldr     w4, [x20]
        cmp     w4, w2
        b.ne    berrycore_service       // mailbox gone: search again
        ldr     w4, [x20, #4]
        cmp     w4, w3
        b.ne    berrycore_service
        ldrb    w4, [x20, #8]
        cmp     w4, #0xa5
        b.ne    wait_request

        add     x17, x20, #0x40         // second cache line of the mailbox
        dc      ivac, x17
        dsb     sy
        ldr     w27, [x20, #0xc]        // sequence number
        ldr     w4, [x20, #0x10]        // command
        mov     w12, wzr
        mov     w13, wzr
        mov     w14, wzr
        mov     w15, wzr
        cmp     w4, #0
        b.eq    cmd_ping
        cmp     w4, #1
        b.eq    cmd_add
        cmp     w4, #2
        b.eq    cmd_checksum
        cmp     w4, #3
        b.eq    cmd_load_module
        cmp     w4, #4
        b.eq    cmd_call_module
        cmp     w4, #5
        b.eq    cmd_unload_module
        mov     w15, #1                 // error 1: unknown command
        b       finish_request

/* 0: PING - only answer. */
cmd_ping:
        b       finish_request

/* 1: ADD test - result 0 = operand1 + operand2. */
cmd_add:
        ldr     w5, [x20, #0x28]
        ldr     w6, [x20, #0x2c]
        add     w12, w5, w6
        b       finish_request

/* 2: CHECKSUM - result 0 = sum of all 32-bit words in buffer A. */
cmd_checksum:
        ldr     w5, [x20, #0x18]
        ldr     w6, [x20, #0x1c]
        ubfx    x8, x5, #0, #32         // zero-extend Amiga address
        cmp     w21, #2
        b.ne    1f
        mov     x9, #0x1000000000
        add     x8, x8, x9
1:      mov     w12, wzr
        cbz     w6, finish_request
2:      ldr     w13, [x8], #4
        add     w12, w12, w13
        subs    w6, w6, #4
        b.ne    2b
        mov     w13, wzr
        b       finish_request

/* 3: LOAD_MODULE - buffer A holds the module image (in place).
   Size must be 0x44 .. 1 MiB. Result 0 = module type, result 1 = size. */
cmd_load_module:
        mov     x22, xzr                // old module is forgotten
        mov     w23, wzr
        mov     w24, wzr
        ldr     w5, [x20, #0x18]
        ldr     w6, [x20, #0x1c]
        cmp     w6, #0x44
        b.lo    load_bad_size
        mov     w8, #0x100000
        cmp     w6, w8
        b.hi    load_bad_size
        ubfx    x8, x5, #0, #32         // zero-extend Amiga address
        cmp     w21, #2
        b.ne    1f
        mov     x9, #0x1000000000
        add     x8, x8, x9
1:      mov     x10, x8                 // invalidate data cache of the image
        add     w11, w6, #0x3f
        lsr     w11, w11, #6
2:      dc      ivac, x10
        add     x10, x10, #0x40
        subs    w11, w11, #1
        b.ne    2b
        dsb     sy
        mov     w9, #0x4d42             // "BMMB"
        movk    w9, #0x424d, lsl #16
        ldr     w10, [x8]
        cmp     w10, w9
        b.ne    load_bad_magic
        mov     w9, #0x4d59             // "YMMY"
        movk    w9, #0x594d, lsl #16
        ldr     w10, [x8, #4]
        cmp     w10, w9
        b.ne    load_bad_magic
        ldrb    w10, [x8, #8]           // ABI version must be 1
        cmp     w10, #1
        b.ne    load_bad_version
        ldrb    w24, [x8, #9]           // module type, must not be 0
        cbz     w24, load_bad_version
        mov     x10, x8                 // make the code visible to the
        add     w11, w6, #0x3f          // instruction fetch
        lsr     w11, w11, #6
3:      ic      ivau, x10
        add     x10, x10, #0x40
        subs    w11, w11, #1
        b.ne    3b
        dsb     sy
        isb
        mov     x22, x8                 // module is loaded
        mov     w23, w6
        mov     w12, w24
        mov     w13, w23
        b       finish_request
load_bad_size:
        mov     w15, #4
        b       finish_request
load_bad_magic:
        mov     w15, #3
        b       finish_request
load_bad_version:
        mov     w15, #5
        b       finish_request

/* 4: CALL_MODULE - calls module base + 0x40.
   Before: invalidate buffers A, B, C (read what the 68k side wrote).
   After:  clean buffers B and C (write results back to RAM). */
cmd_call_module:
        cbz     x22, call_no_module
        ldr     w5, [x20, #0x18]
        ldr     w6, [x20, #0x1c]
        bl      cache_invalidate_range
        ldr     w5, [x20, #0x20]
        ldr     w6, [x20, #0x24]
        bl      cache_invalidate_range
        ldr     w5, [x20, #0x30]
        ldr     w6, [x20, #0x34]
        bl      cache_invalidate_range
        mov     x0, x20                 // x0 = mailbox
        mov     x2, x22                 // x2 = module base
        mov     x1, xzr                 // x1 = address addend
        cmp     w21, #2
        b.ne    1f
        mov     x1, #0x1000000000
1:      add     x9, x22, #0x40
        blr     x9
        mov     w12, w0                 // result 0
        mov     w13, w1                 // result 1
        mov     w15, w2                 // error code
        mov     w14, w3                 // result 3
        ldr     w5, [x20, #0x20]
        ldr     w6, [x20, #0x24]
        bl      cache_clean_range
        ldr     w5, [x20, #0x30]
        ldr     w6, [x20, #0x34]
        bl      cache_clean_range
        b       finish_request
call_no_module:
        mov     w15, #2
        b       finish_request

/* Invalidate the data cache for w5 = Amiga address, w6 = length. */
cache_invalidate_range:
        cbz     w6, 3f
        ubfx    x8, x5, #0, #32         // zero-extend Amiga address
        cmp     w21, #2
        b.ne    1f
        mov     x9, #0x1000000000
        add     x8, x8, x9
1:      add     x11, x8, x6
        lsr     x10, x8, #6             // round start down to 64 bytes
        lsl     x10, x10, #6
        add     x11, x11, #0x3f         // round end up to 64 bytes
        lsr     x11, x11, #6
        lsl     x11, x11, #6
2:      dc      ivac, x10
        add     x10, x10, #0x40
        cmp     x10, x11
        b.lo    2b
        dsb     sy
3:      ret

/* Clean (write back) the data cache for w5 = address, w6 = length. */
cache_clean_range:
        cbz     w6, 3f
        ubfx    x8, x5, #0, #32         // zero-extend Amiga address
        cmp     w21, #2
        b.ne    1f
        mov     x9, #0x1000000000
        add     x8, x8, x9
1:      add     x11, x8, x6
        lsr     x10, x8, #6
        lsl     x10, x10, #6
        add     x11, x11, #0x3f
        lsr     x11, x11, #6
        lsl     x11, x11, #6
2:      dc      cvac, x10
        add     x10, x10, #0x40
        cmp     x10, x11
        b.lo    2b
        dsb     sy
3:      ret

/* 5: UNLOAD_MODULE */
cmd_unload_module:
        mov     x22, xzr
        mov     w23, wzr
        mov     w24, wzr
        b       finish_request

/* Write the results, set "done" and wait for the next request. */
finish_request:
        str     w12, [x20, #0x38]
        str     w13, [x20, #0x3c]
        str     w14, [x20, #0x40]
        str     w15, [x20, #0x44]
        str     w21, [x20, #0x48]
        str     w27, [x20, #0x4c]
        mov     w4, #1
        strb    w4, [x20, #0xa]         // service present
        mov     w4, #20
        strb    w4, [x20, #0xb]         // service version 20
        strb    wzr, [x20, #8]          // clear request flag
        mov     w4, #0x5a
        strb    w4, [x20, #9]           // done
        dc      cvac, x20               // write both cache lines back
        add     x17, x20, #0x40
        dc      cvac, x17
        dsb     sy
        b       wait_request
