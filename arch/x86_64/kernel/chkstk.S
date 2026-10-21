    .text
    .globl ___chkstk_ms
___chkstk_ms:
    push %rcx
    push %rax
    cmp $0x1000, %rax
    lea 24(%rsp), %rcx
    jbe 1f
2:
    sub $0x1000, %rcx
    testl $0, (%rcx)
    sub $0x1000, %rax
    cmp $0x1000, %rax
    ja 2b
1:
    pop %rax
    pop %rcx
    ret
