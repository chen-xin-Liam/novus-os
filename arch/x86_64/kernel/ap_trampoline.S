/*
 * Novus OS - AP (B-core) trampoline, M1 Task 6.
 *
 * The BSP (kernel/smp.c) copies this blob to physical 0x8000 and starts
 * the AP with a SIPI of vector 8: the AP wakes in 16-bit real mode with
 * CS = 0x0800 / IP = 0, i.e. it fetches the first instruction from
 * linear 0x8000 - exactly where the blob was placed (blob org is 0).
 *
 * Job: real mode -> 32-bit protected mode -> load the kernel CR3
 * (PAE + EFER.LME + CR0.PG) -> 64-bit long mode, then call the C entry
 * ap_main() on a BSP-provided stack. ap_main never returns; if it ever
 * does, the AP parks with cli/hlt (the M4 scheduler will take over AP
 * parking here).
 *
 * BSP-patched slots: the LAST 24 bytes of the flat binary are three
 * 64-bit words filled by smp.c after the copy (blob offsets, all <4K so
 * 16-bit fields below are irrelevant to them):
 *   tramp_cr3       - kernel page-table base (CR3 value)
 *   tramp_stack_top - top of the AP's 16 KiB stack (identity-mapped)
 *   tramp_ap_main   - linear address of the C function ap_main()
 * smp.c locates the slots as blob_size - 24; 'nm build/ap_trampoline.o'
 * proves the layout: tramp_cr3 = size-24, tramp_stack_top = size-16,
 * tramp_ap_main = size-8.
 *
 * Addressing rules (the flat binary must contain no relocations):
 *  - Real/32-bit code references blob data with the assembly-time
 *    constant TRAMP_BASE + (symbol - ap_trampoline_entry). Same-section
 *    symbol differences fold to plain constants, so DS = 0 makes the
 *    displacement the linear address directly.
 *  - Segment bases are asymmetric by design: the 32-bit code segment
 *    (selector 0x08) carries base 0x8000 so the first far jump can keep
 *    a plain 16-bit in-blob offset as EIP, while the 64-bit code
 *    segment's base is architecturally forced to 0 - hence the second
 *    far jump encodes TRAMP_BASE + offset in its 32-bit offset field.
 *  - 64-bit code uses RIP-relative addressing: RIP already holds the
 *    linear address (the kernel CR3 identity-maps the low 4 GiB).
 *
 * The whole blob must stay under 4 KiB (one page at 0x8000).
 */

    .code16gcc

    .equ TRAMP_BASE, 0x8000        /* SIPI vector 8 << 12 */

    .section .text

    .globl ap_trampoline_entry
ap_trampoline_entry:
    /* --- 16-bit real mode (entered at linear 0x8000, CS base 0x8000) --- */
    cli
    cld
    xorw  %ax, %ax
    movw  %ax, %ds                 /* DS = ES = SS = 0: flat absolute
                                      addressing, linear == displacement */
    movw  %ax, %es
    movw  %ax, %ss
    movw  $0x7C00, %sp             /* temporary stack at linear 0x7C00,
                                      below the blob; nothing is pushed
                                      (interrupts stay masked) */

    /* Load the trampoline-local GDT (base baked in as 0x8000 + offset). */
    lgdtl (tramp_gdt_desc - ap_trampoline_entry + TRAMP_BASE)

    movl  %cr0, %eax
    orl   $1, %eax                 /* CR0.PE: protected mode */
    movl  %eax, %cr0
    ljmp  $0x08, $prot_entry       /* 32-bit code segment, base 0x8000 */

    .code32
prot_entry:
    movw  $0x10, %ax               /* flat data segment (base 0, 4 GiB) */
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %ss

    movl  %cr4, %eax
    orl   $0x20, %eax              /* CR4.PAE: prerequisite for long mode */
    movl  %eax, %cr4

    /* CR3 from the BSP-patched slot (kernel page tables; the identity
     * map in PML4[0] keeps blob, slots and AP stack reachable). */
    movl  (tramp_cr3 - ap_trampoline_entry + TRAMP_BASE), %eax
    movl  %eax, %cr3

    /* EFER.LME (bit 0x100): long mode enable. */
    movl  $0xC0000080, %ecx
    rdmsr
    orl   $0x100, %eax
    wrmsr

    movl  %cr0, %eax
    orl   $0x80000000, %eax        /* CR0.PG: paging on, long mode arms */
    movl  %eax, %cr0

    /* Enter the 64-bit segment. Its base is forced to 0 in long mode,
     * so the 32-bit offset field must carry the full linear address. */
    ljmp  $0x18, $(long_entry - ap_trampoline_entry + TRAMP_BASE)

    .code64
long_entry:
    xorl  %eax, %eax
    movw  $0x10, %ax               /* flat data segment */
    movw  %ax, %ds
    movw  %ax, %es
    movw  %ax, %ss

    xorl  %ebp, %ebp
    movq  tramp_stack_top(%rip), %rsp   /* AP stack top from the BSP */

    movq  tramp_ap_main(%rip), %rax
    call  *%rax                    /* ap_main(), MS x64 ABI, never returns */

    /* Park the AP if ap_main ever returns (M4 scheduler hook). */
    cli
1:  hlt
    jmp   1b

    /* --- trampoline-local GDT --- */
    .balign 8
tramp_gdt:
    .quad 0                        /* 0x00: null descriptor */
    .quad 0x00CF9A008000FFFF       /* 0x08: 32-bit code, base 0x8000,
                                      G=1 D=1 (blob spans one page) */
    .quad 0x00CF92000000FFFF       /* 0x10: flat data, base 0, W=1 */
    .quad 0x00209A0000000000       /* 0x18: 64-bit code, L=1 D=0 */
tramp_gdt_end:
tramp_gdt_desc:
    .word tramp_gdt_end - tramp_gdt - 1
    .long (tramp_gdt - ap_trampoline_entry + TRAMP_BASE)

    /* --- BSP-patched slots: MUST remain the last 24 bytes of the
     * flat binary (kernel/smp.c computes them from the blob end) --- */
    .balign 8
    .globl tramp_cr3
tramp_cr3:
    .quad 0
    .globl tramp_stack_top
tramp_stack_top:
    .quad 0
    .globl tramp_ap_main
tramp_ap_main:
    .quad 0
