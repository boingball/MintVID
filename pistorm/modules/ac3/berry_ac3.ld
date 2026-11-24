ENTRY(berry_ac3_image)
SECTIONS {
  . = 0;
  .text : { KEEP(*(.text.header)) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.bss .bss.* COMMON) }
}
