OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(legacy_entry)

KERNEL_BASE = 0x00100000;

SECTIONS
{
  . = KERNEL_BASE;
  __kernel_start__ = .;

  .text : ALIGN(16)
  {
    KEEP(*(.text.boot))
    *(.text .text.*)
  }

  .rodata : ALIGN(16)
  {
    *(.rodata .rodata.*)
  }

  .data : ALIGN(16)
  {
    *(.data .data.*)
    *(.got .got.*)
  }

  __bss_start__ = .;
  .bss (NOLOAD) : ALIGN(16)
  {
    *(COMMON)
    *(.bss .bss.*)
  }
  __bss_end__ = .;
  __kernel_end__ = .;

  /DISCARD/ :
  {
    *(.comment)
    *(.note .note.*)
    *(.eh_frame .eh_frame.*)
    *(.gcc_except_table .gcc_except_table.*)
  }
}

ASSERT(legacy_entry == KERNEL_BASE,
       "legacy_entry must be the first byte loaded by stage2")
ASSERT(__bss_end__ <= 0x100000000,
       "BIOS kernel image exceeds the low 4 GiB identity map")