    .section .text.boot,"ax"
    .code64
    .globl legacy_entry
    .type legacy_entry, @function
legacy_entry:
    cli
    cld
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    movq    $0x7E000, %rsp

    /* This is the first C-capable entry, so clear static state first. */
    xorl    %eax, %eax
    leaq    __bss_start__(%rip), %rdi
    leaq    __bss_end__(%rip), %rcx
    subq    %rdi, %rcx
    rep stosb

    /* Win64 caller alignment and 32-byte shadow space for legacy_main. */
    andq    $-16, %rsp
    subq    $32, %rsp
    call    legacy_main

    cli
1:
    hlt
    jmp     1b
    .size legacy_entry, .-legacy_entry