ENTRY(_start)
SECTIONS
{
  . = 0x400000;
  .text : { KEEP(*(.text._start)) *(.text .text.*) }
  .bss : { *(.bss .bss.*) *(COMMON) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
