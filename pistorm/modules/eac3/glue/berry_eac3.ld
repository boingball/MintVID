ENTRY(berry_eac3_image)
SECTIONS {
 . = 0;
 .text : {
   KEEP(*(.text.header))
   . = ALIGN(4);
   __berry_ff_tx_mdct_naive_inv_int32_c = .;
   KEEP(*(.text.ff_tx_mdct_naive_inv_int32_c))
   *(.text .text.*)
 }
 .rodata : { *(.rodata .rodata.*) }
 .data : { *(.data .data.*) }
 .bss : { *(.bss .bss.* COMMON) }
 /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
