    .code16
    .section .text
    .globl bootsect_start

#include "boot_layout.h"

.equ BOOTSECT_BASE, 0x7C00

bootsect_start:
    jmp bootsect_start_real
    nop

bootsect_start_real:
    cli
    cld
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    $BOOTSECT_BASE, %sp

    /* Normalize CS as well; all later addresses are physical offsets. */
    ljmp    $0x0000, $(BOOTSECT_BASE + bootsect_real - bootsect_start)

bootsect_real:
    movb    %dl, BIOS_BOOT_DRIVE_ADDR

    /* 16-byte EDD DAP: read the fixed stage2 extent into 0000:8000. */
    movb    $0x10, BIOS_DAP_ADDR
    movb    $0, BIOS_DAP_ADDR + 1
    movw    $STAGE2_SECTORS, BIOS_DAP_ADDR + 2
    movw    $STAGE2_LOAD, BIOS_DAP_ADDR + 4
    movw    $0, BIOS_DAP_ADDR + 6
    movl    $STAGE2_LBA, BIOS_DAP_ADDR + 8
    movl    $0, BIOS_DAP_ADDR + 12

    movw    $BIOS_DAP_ADDR, %si
    movb    BIOS_BOOT_DRIVE_ADDR, %dl
    movb    $0x42, %ah
    int     $0x13
    jc      bootsect_read_error

    movb    BIOS_BOOT_DRIVE_ADDR, %dl
    ljmp    $0x0000, $STAGE2_LOAD

bootsect_read_error:
    movw    $(BOOTSECT_BASE + bootsect_error - bootsect_start), %si
    movw    $0x0007, %bx
1:
    lodsb
    testb   %al, %al
    jz      2f
    movb    $0x0E, %ah
    int     $0x10
    jmp     1b
2:
    cli
3:
    hlt
    jmp     3b

bootsect_error:
    .asciz "NO STAGE2"

    /* MBR-compatible partition table area; superfloppy leaves it zero. */
    .org 446
    .fill 64, 1, 0
    .word 0xAA55
    .org 512