/* FASTIMDCT03 fragment, appended to berry_eac3.module at 0x1A000.
   The cos table is placed at the same address as in the P32 build
   (0x1A3CC), so the fragment is always 0x23CC bytes and the module keeps
   the size 0x1C3CC that berryvp.library P32 expects. */
ENTRY(berry_fast_mdct_int32)
SECTIONS {
  . = 0x1A000;
  .text : { KEEP(*(.text.fastmdct)) *(.text .text.*) }
  ASSERT(. <= 0x1A3CC, "fast_mdct code larger than the P32 slot (0x3CC bytes)")
  . = 0x1A3CC;
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  .bss : { *(.bss .bss.* COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
