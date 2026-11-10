ENTRY(berry_faad_image)
SECTIONS {
  . = 0;
  .text : ALIGN(16) { KEEP(*(.text.berry_entry)) *(.text .text.*) }
  .rodata : ALIGN(16) { *(.rodata .rodata.*) }
  /* Mix data + bss into one PROGBITS output section so the raw module contains
     deterministic zero initialization for library globals. */
  .data : ALIGN(16) {
    *(.data .data.*)
    __bss_start = .;
    *(.bss .bss.* COMMON)
    __bss_end = .;
    BYTE(0)
  }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
