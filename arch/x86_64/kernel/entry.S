/*
 * Novus OS - M1 BSP kernel entry (x86_64 assembly).
 *
 * First kernel code executed after the UEFI boot stage exits boot
 * services (arch/x86_64/boot/main.c calls kernel_entry). Duties, in
 * order:
 *
 *   1. Install the 64 KiB BSP kernel stack. kstack lives in .bss and is
 *      64-byte aligned; its top is therefore 16-byte aligned. Reserve
 *      the 32-byte home area required by the MS x64 ABI before calling C.
 *   2. Zero the .bss section. The mingw ld default PE linker script
 *      defines __bss_start__ / __bss_end__ around the .bss output
 *      section (verified with x86_64-w64-mingw32-nm on the linked
 *      image); the UEFI PE loader additionally zero-fills section tails
 *      that have no raw file data, so this pass is belt-and-braces.
 *   3. Call the C kernel entry kmain() without disturbing the incoming
 *      arguments: RCX = mmap addr, RDX = map size, R8 = desc size
 *      (MS x64 ABI, passed through from the boot stage unchanged).
 *
 * kmain never returns; if it ever does, interrupts are masked and the
 * BSP parks with hlt forever.
 */

    /* 64 KiB BSP kernel stack, 64-byte aligned, in .bss. */
    .section .bss
    .balign 64
kstack:
    .skip 65536
    .section .text
    .code64
    .globl  kernel_entry
kernel_entry:
    /* Switch to the kernel stack (top of kstack). */
    leaq    kstack+65536(%rip), %rsp

    /* Preserve the kernel arguments across .bss clearing. RBX and
     * R12-R15 are callee-saved, so kmain also finds them intact. */
    movq    %rcx, %rbx
    movq    %rdx, %r12
    movq    %r8,  %r13

    /* Zero .bss: RDI = start, RCX = byte count, AL = fill value. */
    xorl    %eax, %eax
    leaq    __bss_start__(%rip), %rdi
    leaq    __bss_end__(%rip), %rcx
    subq    %rdi, %rcx
    cld
    rep stosb

    /* Restore the arguments and enter the C kernel. */
    movq    %rbx, %rcx
    movq    %r12, %rdx
    movq    %r13, %r8
    subq    $32, %rsp             /* MS x64 shadow space for kmain() */
    call    kmain

    /* kmain must never return; park the BSP if it does. */
    cli
1:  hlt
    jmp     1b
