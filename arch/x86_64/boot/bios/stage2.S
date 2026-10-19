    .code16
    .section .text
    .globl stage2_entry

#include "boot_layout.h"

stage2_entry:
    cli
    cld
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movl    $0x7000, %esp
    movb    %dl, STAGE2_ADDR(stage2_drive)

    call    stage2_check_long_mode
    jc      stage2_error
    call    stage2_init_boot_info
    call    stage2_collect_e820
    jc      stage2_error
    call    stage2_try_vbe
    call    stage2_check_edd
    jc      stage2_error
    call    stage2_enable_a20
    jc      stage2_error
    call    stage2_load_kernel
    jc      stage2_error
    jmp     stage2_enter_protected

/* Verify CPUID and the extended long-mode capability before changing mode. */
stage2_check_long_mode:
    pushfl
    popl    %eax
    movl    %eax, %ecx
    xorl    $0x00200000, %eax
    pushl   %eax
    popfl
    pushfl
    popl    %eax
    xorl    %ecx, %eax
    jz      1f
    pushl   %ecx
    popfl
    movl    $0x80000000, %eax
    cpuid
    cmpl    $0x80000001, %eax
    jb      1f
    movl    $0x80000001, %eax
    cpuid
    btl     $29, %edx
    jnc     1f
    clc
    ret
1:
    stc
    ret

stage2_init_boot_info:
    movw    $0x5000, %ax
    movw    %ax, %es
    xorw    %di, %di
    xorw    %ax, %ax
    movw    $64, %cx
    rep stosw
    movl    $BIOS_BOOT_INFO_MAGIC, %es:0
    movl    $BIOS_BOOT_INFO_VERSION, %es:4
    movl    $24, %es:BIOS_BI_E820_ENTRY_SIZE
    movl    $BIOS_E820_BUFFER, %es:BIOS_BI_E820_ADDR
    movl    $0, %es:BIOS_BI_E820_ADDR + 4
    ret

/* E820 calls must happen in real mode. Store up to 128 raw 24-byte entries. */
stage2_collect_e820:
    movl    $0, STAGE2_ADDR(stage2_e820_count)
    movl    $0, STAGE2_ADDR(stage2_e820_token)
    movw    $0x0100, STAGE2_ADDR(stage2_e820_offset)
1:
    cmpl    $BIOS_E820_MAX_ENTRIES, STAGE2_ADDR(stage2_e820_count)
    jae     4f
    movw    $0x5000, %ax
    movw    %ax, %es
    movw    STAGE2_ADDR(stage2_e820_offset), %di
    movl    $1, %es:20(%di)
    movl    $0x0000E820, %eax
    movl    $0x534D4150, %edx
    movl    $24, %ecx
    movl    STAGE2_ADDR(stage2_e820_token), %ebx
    int     $0x15
    jc      4f
    cmpl    $0x534D4150, %eax
    jne     4f
    cmpl    $20, %ecx
    jb      4f
    xorw    %ax, %ax
    movw    %ax, %ds
    movl    %ebx, STAGE2_ADDR(stage2_e820_token)
    incl    STAGE2_ADDR(stage2_e820_count)
    addw    $24, STAGE2_ADDR(stage2_e820_offset)
    movw    $0x5000, %ax
    movw    %ax, %es
    movl    STAGE2_ADDR(stage2_e820_count), %eax
    movl    %eax, %es:BIOS_BI_E820_COUNT
    cmpl    $0, STAGE2_ADDR(stage2_e820_token)
    jne     1b
    clc
    ret
4:
    stc
    ret

/* Find and set an exact 1280x800x32 VBE linear-framebuffer mode. */
stage2_try_vbe:
    movw    $0x5100, %ax
    movw    %ax, %es
    xorw    %di, %di
    movl    $0x32454256, %es:0
    movw    $0x4F00, %ax
    int     $0x10
    cmpw    $0x004F, %ax
    jne     8f
    xorw    %ax, %ax
    movw    %ax, %ds
    cmpl    $0x41534556, %es:0
    jne     8f
    movw    %es:4, %ax
    cmpw    $0x0200, %ax
    jb      8f
    movw    %ax, STAGE2_ADDR(stage2_vbe_version)
    movw    %es:14, %ax
    movw    %ax, STAGE2_ADDR(stage2_vbe_list_offset)
    movw    %es:16, %ax
    movw    %ax, STAGE2_ADDR(stage2_vbe_list_segment)
    movw    $0, STAGE2_ADDR(stage2_vbe_scan_count)

1:
    cmpw    $512, STAGE2_ADDR(stage2_vbe_scan_count)
    jae     8f
    movw    STAGE2_ADDR(stage2_vbe_list_segment), %ax
    movw    %ax, %es
    movw    STAGE2_ADDR(stage2_vbe_list_offset), %di
    movw    %es:(%di), %cx
    addw    $2, %di
    movw    %di, STAGE2_ADDR(stage2_vbe_list_offset)
    cmpw    $0xFFFF, %cx
    je      8f
    movw    %cx, STAGE2_ADDR(stage2_vbe_candidate)
    incw    STAGE2_ADDR(stage2_vbe_scan_count)

    movw    $0x5120, %ax
    movw    %ax, %es
    xorw    %di, %di
    movw    STAGE2_ADDR(stage2_vbe_candidate), %cx
    movw    $0x4F01, %ax
    int     $0x10
    cmpw    $0x004F, %ax
    jne     9f
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %es:0, %ax
    andw    $0x0091, %ax
    cmpw    $0x0091, %ax
    jne     1b
    cmpw    $1280, %es:18
    jne     1b
    cmpw    $800, %es:20
    jne     1b
    cmpb    $32, %es:25
    jne     1b
    cmpb    $6, %es:27
    jne     1b
    movw    STAGE2_ADDR(stage2_vbe_candidate), %ax
    movw    %ax, STAGE2_ADDR(stage2_vbe_mode)
    movw    %ax, %bx
    orw     $0x4000, %bx
    movw    $0x4F02, %ax
    int     $0x10
    cmpw    $0x004F, %ax
    jne     8f
    xorw    %ax, %ax
    movw    %ax, %ds

    /* Re-fetch mode info after setting it; use the VBE 3 linear pitch. */
    movw    STAGE2_ADDR(stage2_vbe_mode), %cx
    movw    $0x5120, %ax
    movw    %ax, %es
    xorw    %di, %di
    movw    $0x4F01, %ax
    int     $0x10
    cmpw    $0x004F, %ax
    jne     8f
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %es:16, %ax
    cmpw    $0x0300, STAGE2_ADDR(stage2_vbe_version)
    jb      2f
    movw    %es:50, %dx
    testw   %dx, %dx
    jz      2f
    movw    %dx, %ax
2:
    testw   %ax, %ax
    jz      8f
    movzwl  %ax, %eax
    movl    %eax, STAGE2_ADDR(stage2_vbe_pitch)
    movzwl  %es:18, %eax
    movl    %eax, STAGE2_ADDR(stage2_vbe_width)
    movzwl  %es:20, %eax
    movl    %eax, STAGE2_ADDR(stage2_vbe_height)
    movl    %es:40, %eax
    testl   %eax, %eax
    jz      8f
    movl    %eax, STAGE2_ADDR(stage2_vbe_fb_base)
    movb    %es:31, %al
    movb    %al, STAGE2_ADDR(stage2_vbe_red_size)
    movb    %es:32, %al
    movb    %al, STAGE2_ADDR(stage2_vbe_red_pos)
    movb    %es:33, %al
    movb    %al, STAGE2_ADDR(stage2_vbe_green_size)
    movb    %es:34, %al
    movb    %al, STAGE2_ADDR(stage2_vbe_green_pos)
    movb    %es:35, %al
    movb    %al, STAGE2_ADDR(stage2_vbe_blue_size)
    movb    %es:36, %al
    movb    %al, STAGE2_ADDR(stage2_vbe_blue_pos)

    xorw    %ax, %ax
    movw    %ax, %ds
    movw    $0x5000, %ax
    movw    %ax, %es
    movl    STAGE2_ADDR(stage2_vbe_width), %eax
    movl    %eax, %es:BIOS_BI_WIDTH
    movl    STAGE2_ADDR(stage2_vbe_height), %eax
    movl    %eax, %es:BIOS_BI_HEIGHT
    movl    STAGE2_ADDR(stage2_vbe_pitch), %eax
    movl    %eax, %es:BIOS_BI_PITCH_BYTES
    movw    $32, %es:BIOS_BI_BPP
    movb    STAGE2_ADDR(stage2_vbe_red_size), %al
    movb    %al, %es:BIOS_BI_RED_SIZE
    movb    STAGE2_ADDR(stage2_vbe_red_pos), %al
    movb    %al, %es:BIOS_BI_RED_POS
    movb    STAGE2_ADDR(stage2_vbe_green_size), %al
    movb    %al, %es:BIOS_BI_GREEN_SIZE
    movb    STAGE2_ADDR(stage2_vbe_green_pos), %al
    movb    %al, %es:BIOS_BI_GREEN_POS
    movb    STAGE2_ADDR(stage2_vbe_blue_size), %al
    movb    %al, %es:BIOS_BI_BLUE_SIZE
    movb    STAGE2_ADDR(stage2_vbe_blue_pos), %al
    movb    %al, %es:BIOS_BI_BLUE_POS
    movl    STAGE2_ADDR(stage2_vbe_fb_base), %eax
    movl    %eax, %es:BIOS_BI_FB_BASE
    movl    $0, %es:BIOS_BI_FB_BASE + 4
    movl    STAGE2_ADDR(stage2_vbe_pitch), %eax
    mull    STAGE2_ADDR(stage2_vbe_height)
    movl    %eax, %es:BIOS_BI_FB_SIZE
    movl    %edx, %es:BIOS_BI_FB_SIZE + 4
    movw    STAGE2_ADDR(stage2_vbe_mode), %ax
    movw    %ax, %es:BIOS_BI_VBE_MODE
    movl    $1, %es:BIOS_BI_VBE_AVAILABLE
    jmp     8f
9:
    xorw    %ax, %ax
    movw    %ax, %ds
    jmp     1b
8:
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    clc
    ret

stage2_check_edd:
    movb    STAGE2_ADDR(stage2_drive), %dl
    movw    $0x55AA, %bx
    movb    $0x41, %ah
    int     $0x13
    jc      1f
    cmpw    $0xAA55, %bx
    jne     1f
    testw   $1, %cx
    jz      1f
    xorw    %ax, %ax
    movw    %ax, %ds
    clc
    ret
1:
    stc
    ret

/* Enable A20 through port 92, then set it through the 8042 as a fallback. */
stage2_enable_a20:
    inb     $0x92, %al
    andb    $0xFE, %al
    orb     $0x02, %al
    outb    %al, $0x92
    inb     $0x92, %al
    testb   $0x02, %al
    jnz     9f

    movw    $0xFFFF, %cx
1:
    inb     $0x64, %al
    testb   $0x02, %al
    jz      2f
    loop    1b
    stc
    ret
2:
    movb    $0xD0, %al
    outb    %al, $0x64
    movw    $0xFFFF, %cx
3:
    inb     $0x64, %al
    testb   $0x01, %al
    jnz     4f
    loop    3b
    stc
    ret
4:
    inb     $0x60, %al
    orb     $0x03, %al
    movb    %al, STAGE2_ADDR(stage2_kbc_output)
    movw    $0xFFFF, %cx
5:
    inb     $0x64, %al
    testb   $0x02, %al
    jz      6f
    loop    5b
    stc
    ret
6:
    movb    $0xD1, %al
    outb    %al, $0x64
    movw    $0xFFFF, %cx
7:
    inb     $0x64, %al
    testb   $0x02, %al
    jz      8f
    loop    7b
    stc
    ret
8:
    movb    STAGE2_ADDR(stage2_kbc_output), %al
    outb    %al, $0x60
9:
    clc
    ret

/* Read 64-sector chunks into a low bounce buffer, then copy above 1 MiB. */
stage2_load_kernel:
    movl    STAGE2_ADDR(stage2_kernel_sectors), %eax
    testl   %eax, %eax
    jz      1f
    cmpl    $0x007FF800, %eax
    ja      1f
    movl    %eax, STAGE2_ADDR(stage2_kernel_remaining)
    movl    $KERNEL_LBA, STAGE2_ADDR(stage2_kernel_lba_low)
    movl    $0, STAGE2_ADDR(stage2_kernel_lba_high)
    movl    $KERNEL_LOAD, STAGE2_ADDR(stage2_kernel_dest)

2:
    movl    STAGE2_ADDR(stage2_kernel_remaining), %eax
    cmpl    $BIOS_KERNEL_CHUNK_SECTORS, %eax
    jbe     3f
    movl    $BIOS_KERNEL_CHUNK_SECTORS, %eax
3:
    movl    %eax, STAGE2_ADDR(stage2_kernel_chunk)
    shll    $9, %eax
    movl    %eax, STAGE2_ADDR(stage2_kernel_chunk_bytes)

    movb    $0x10, BIOS_DAP_ADDR
    movb    $0, BIOS_DAP_ADDR + 1
    movw    STAGE2_ADDR(stage2_kernel_chunk), %ax
    movw    %ax, BIOS_DAP_ADDR + 2
    movw    $0, BIOS_DAP_ADDR + 4
    movw    $0x3000, BIOS_DAP_ADDR + 6
    movl    STAGE2_ADDR(stage2_kernel_lba_low), %eax
    movl    %eax, BIOS_DAP_ADDR + 8
    movl    STAGE2_ADDR(stage2_kernel_lba_high), %eax
    movl    %eax, BIOS_DAP_ADDR + 12
    movw    $BIOS_DAP_ADDR, %si
    movb    STAGE2_ADDR(stage2_drive), %dl
    movb    $0x42, %ah
    int     $0x13
    jc      1f
    xorw    %ax, %ax
    movw    %ax, %ds
    call    stage2_unreal_copy

    movl    STAGE2_ADDR(stage2_kernel_chunk), %eax
    subl    %eax, STAGE2_ADDR(stage2_kernel_remaining)
    addl    %eax, STAGE2_ADDR(stage2_kernel_lba_low)
    adcl    $0, STAGE2_ADDR(stage2_kernel_lba_high)
    shll    $9, %eax
    addl    %eax, STAGE2_ADDR(stage2_kernel_dest)
    cmpl    $0, STAGE2_ADDR(stage2_kernel_remaining)
    jne     2b
    clc
    ret
1:
    stc
    ret

/* Briefly enter flat protected mode to copy a bounce chunk with A20 on. */
stage2_unreal_copy:
    cli
    lgdtl   STAGE2_ADDR(stage2_gdt_descriptor)
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0
    ljmpl   $0x08, $STAGE2_ADDR(stage2_copy_pm32)

    .code32
stage2_copy_pm32:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movw    %ax, %fs
    movl    $BIOS_DISK_BOUNCE, %esi
    movl    STAGE2_ADDR(stage2_kernel_dest), %edi
    movl    STAGE2_ADDR(stage2_kernel_chunk_bytes), %ecx
1:
    testl   %ecx, %ecx
    jz      2f
    movb    (%esi), %al
    movb    %al, %fs:(%edi)
    incl    %esi
    incl    %edi
    decl    %ecx
    jmp     1b
2:
    /* Switch back to real mode: first far jump to the 16-bit code
     * selector (0x20) while still in protected mode, then clear PE.
     * This avoids the grey state of PE=0 with a 32-bit CS cache. */
    ljmpw   $0x20, $STAGE2_ADDR(stage2_copy_real16)

    .code16
stage2_copy_real16:
    movl    %cr0, %eax
    andl    $0xFFFFFFFE, %eax
    movl    %eax, %cr0
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    ljmpw   $0x0000, $STAGE2_ADDR(stage2_copy_real16_done)
stage2_copy_real16_done:
    ret

stage2_enter_protected:
    cli
    lgdtl   STAGE2_ADDR(stage2_gdt_descriptor)
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0
    ljmpl   $0x08, $STAGE2_ADDR(stage2_setup_pm32)

    .code32
stage2_setup_pm32:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movl    $0x7000, %esp

    /* Six 4 KiB pages: PML4, PDPT, and four page directories. */
    movl    $BIOS_PAGE_TABLES, %edi
    xorl    %eax, %eax
    movl    $6144, %ecx
    rep stosl
    movl    $0x00021003, BIOS_PAGE_TABLES
    movl    $0x00022003, BIOS_PAGE_TABLES + 0x1000
    movl    $0x00023003, BIOS_PAGE_TABLES + 0x1008
    movl    $0x00024003, BIOS_PAGE_TABLES + 0x1010
    movl    $0x00025003, BIOS_PAGE_TABLES + 0x1018

    movl    $BIOS_PAGE_TABLES + 0x2000, %edi
    movl    $0x00000083, %eax
    movl    $2048, %ecx
1:
    movl    %eax, (%edi)
    movl    $0, 4(%edi)
    addl    $0x00200000, %eax
    addl    $8, %edi
    loop    1b

    movl    %cr4, %eax
    orl     $0x620, %eax   /* PAE | OSFXSR | OSXMMEXCPT (SSE for the kernel) */
    movl    %eax, %cr4
    movl    $BIOS_PAGE_TABLES, %eax
    movl    %eax, %cr3
    movl    $0xC0000080, %ecx
    rdmsr
    orl     $0x100, %eax
    wrmsr
    movl    %cr0, %eax
    orl     $0x80000000, %eax
    movl    %eax, %cr0
    ljmpl   $0x18, $STAGE2_ADDR(stage2_long_mode)

    .code64
stage2_long_mode:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movq    $BIOS_LONG_STACK_TOP, %rsp
    xorq    %rbp, %rbp
    movq    $KERNEL_LOAD, %rax
    jmp     *%rax

    .code16
stage2_error:
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    $STAGE2_ADDR(stage2_error_text), %si
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

stage2_error_text:
    .asciz "STAGE2 ERROR"

    .balign 8
stage2_gdt:
    .quad   0x0000000000000000
    .quad   0x00CF9A000000FFFF   /* 0x08: 32-bit code */
    .quad   0x00CF92000000FFFF   /* 0x10: 32-bit data */
    .quad   0x00209A0000000000   /* 0x18: 64-bit code */
    .quad   0x00009A000000FFFF   /* 0x20: 16-bit code (real-mode exit) */
stage2_gdt_end:
stage2_gdt_descriptor:
    .word   stage2_gdt_end - stage2_gdt - 1
    .long   STAGE2_ADDR(stage2_gdt)

    .balign 4
stage2_patch_header:
    .ascii  "NVB2"
    .word   1
    .word   STAGE2_SECTORS
    .long   KERNEL_LBA
stage2_kernel_sectors:
    .long   0
    .long   KERNEL_LOAD
    .quad   0

stage2_drive:                 .byte 0
stage2_e820_count:            .long 0
stage2_e820_token:            .long 0
stage2_e820_offset:           .word 0
stage2_vbe_version:           .word 0
stage2_vbe_list_offset:       .word 0
stage2_vbe_list_segment:      .word 0
stage2_vbe_scan_count:        .word 0
stage2_vbe_candidate:         .word 0
stage2_vbe_mode:              .word 0
stage2_vbe_width:             .long 0
stage2_vbe_height:            .long 0
stage2_vbe_pitch:             .long 0
stage2_vbe_fb_base:           .long 0
stage2_vbe_red_size:          .byte 0
stage2_vbe_red_pos:           .byte 0
stage2_vbe_green_size:        .byte 0
stage2_vbe_green_pos:         .byte 0
stage2_vbe_blue_size:         .byte 0
stage2_vbe_blue_pos:          .byte 0
stage2_kbc_output:            .byte 0
stage2_kernel_remaining:      .long 0
stage2_kernel_lba_low:        .long 0
stage2_kernel_lba_high:       .long 0
stage2_kernel_dest:           .long 0
stage2_kernel_chunk:          .long 0
stage2_kernel_chunk_bytes:    .long 0

    .org STAGE2_SECTORS * 512