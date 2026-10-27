/* 和 apps/link.ld 保持一致：_start 固定在 0x400000，输出一个扁平镜像。
 * 模板自带一份，复制到仓库外时不必依赖 apps/ 的相对路径。 */
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
