/*
 * Novus OS - trap entry and the syscall gate (M4).
 *
 * Two entry paths, one frame, one dispatcher:
 *
 *   - CPU exceptions and the LAPIC timer arrive through an IDT gate. The
 *     CPU has already switched to the ring-0 stack named by TSS.RSP0 (when
 *     the interrupted code was ring 3) and pushed the iret frame.
 *   - syscall arrives with the user stack still in RSP and no frame at
 *     all. The stub builds the same iret frame by hand, from RCX (user
 *     RIP) and R11 (user RFLAGS), and switches to the kernel stack.
 *
 * isr_common saves the 15 general registers in a fixed order, calls
 * trap_dispatch(frame), and restores whatever frame the dispatcher hands
 * back. That return value is a stack pointer, not a status: a dispatcher
 * that scheduled another task returns that task's frame, so the same
 * iretq below resumes a different process. This is the whole context
 * switch - there is no separate switch_to().
 *
 * No swapgs: the kernel is single-core here (the A core is parked), and
 * the syscall stub finds the kernel stack through a global rather than
 * through GS. That keeps the GS base untouched for user code and removes
 * a class of swapgs mismatches. When the AP takes over scheduling this
 * has to become a per-CPU structure.
 */

    .section .text
    .code64

/* ---- exception stubs ---------------------------------------------------- */
/* Vectors 8, 10, 11, 12, 13, 14, 17, 21, 29, 30 push an error code; the
 * rest do not, and the frame has a slot for it either way. */

.macro ISR_NOERR num
    .globl  isr\()\num
isr\()\num:
    pushq   $0
    pushq   $\num
    jmp     isr_common
.endm

.macro ISR_ERR num
    .globl  isr\()\num
isr\()\num:
    pushq   $\num
    jmp     isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

/* LAPIC timer: one line, then the common path. */
    .globl  isr32
isr32:
    pushq   $0
    pushq   $32
    jmp     isr_common

/* The table idt.c walks to fill vectors 0..32. */
    .globl  isr_stub_table
    .align  8
isr_stub_table:
    .quad   isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    .quad   isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
    .quad   isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad   isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
    .quad   isr32

.altmacro
.macro DEVICE_ISR num
    .globl isr\()\num
isr\()\num:
    pushq $0
    pushq $\num
    jmp irq_common
.endm
.set irq_vector, 33
.rept 223
    DEVICE_ISR %irq_vector
    .set irq_vector, irq_vector + 1
.endr

.macro IRQ_TABLE_ENTRY num
    .quad isr\()\num
.endm

    .globl irq_stub_table
    .align 8
irq_stub_table:
.set irq_vector, 33
.rept 223
    IRQ_TABLE_ENTRY %irq_vector
    .set irq_vector, irq_vector + 1
.endr
.noaltmacro

/* External interrupts do not push an error code, so each generated device
 * stub adds the same zero-error/vector pair used by the common frame.
 * This keeps trap_dispatch's vector and error-code offsets uniform. */
irq_common:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld
    /* Keep the original frame pointer while aligning a separate C-call
     * stack; the reserved area also gives FXSAVE an aligned 512-byte slot.
     * Restore FP/SIMD state before returning to the interrupted context. */
    movq %rsp, %rbx
    movq %rsp, %rcx
    andq $-16, %rsp
    subq $544, %rsp
    fxsave64 32(%rsp)
    call trap_dispatch
    fxrstor64 32(%rsp)
    movq %rbx, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    iretq

/* ---- common entry ------------------------------------------------------ */
    .globl  isr_common
isr_common:
    /* 15 pushes, in the reverse of the order trap_frame_t reads them. */
    pushq   %rax
    pushq   %rbx
    pushq   %rcx
    pushq   %rdx
    pushq   %rsi
    pushq   %rdi
    pushq   %rbp
    pushq   %r8
    pushq   %r9
    pushq   %r10
    pushq   %r11
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    movq    %rsp, %rcx          /* frame * (MS x64 ABI: first arg in RCX) */
    subq    $32, %rsp           /* shadow space */
    call    trap_dispatch
    addq    $32, %rsp
    movq    %rax, %rsp          /* the frame to resume, possibly another task */

    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbx
    popq    %rax

    addq    $16, %rsp           /* vector + error code */
    iretq

/* ---- first entry into ring 3 ------------------------------------------- */
/* Same restore sequence as isr_common's tail; used once, by sched_start(),
 * to leave the kernel for the first task instead of returning to a caller
 * that does not exist. */
    .globl  sched_enter
sched_enter:
    movq    %rcx, %rsp
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %r11
    popq    %r10
    popq    %r9
    popq    %r8
    popq    %rbp
    popq    %rdi
    popq    %rsi
    popq    %rdx
    popq    %rcx
    popq    %rbx
    popq    %rax
    addq    $16, %rsp
    iretq

/* ---- syscall gate ------------------------------------------------------ */
/* Written by sched.c on every switch; see the note at the top. */
    .globl  sys_kernel_rsp
    .globl  sys_user_rsp
    .comm   sys_kernel_rsp, 8, 8
    .comm   sys_user_rsp, 8, 8

    .globl  syscall_entry
syscall_entry:
    movq    %rsp, sys_user_rsp(%rip)
    movq    sys_kernel_rsp(%rip), %rsp

    /* Build the iret frame the CPU did not build: SS, RSP, RFLAGS, CS,
     * RIP, then the errcode/vector pair the stubs also push. */
    pushq   $0x23               /* user SS (ring 3 selector) */
    pushq   sys_user_rsp(%rip)  /* user RSP */
    pushq   %r11                /* user RFLAGS */
    pushq   $0x1B               /* user CS (ring 3 selector) */
    pushq   %rcx                /* user RIP, syscall put it here */
    pushq   $0                   /* no error code */
    pushq   $0x100              /* frame marker: syscall, not an IDT vector */
    jmp     isr_common
