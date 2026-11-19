/*
 * APCORE STARTUP TRAMPOLINE
 *
 * A startup IPI delivers an application processor into real mode at vector:0 -
 * here 0x0800:0x0000, physical 0x8000 - because real mode is the only state
 * firmware can guarantee a core is in. Everything else is this file's job:
 * 16-bit to protected mode, protected to long mode, and a call into the kernel.
 *
 * It is linked at 0x8000 so every absolute address inside it is already correct
 * for where it will live; the three values that are not known at build time
 * (page tables, stack, entry point) sit in a parameter block after the code and
 * are patched by the kernel before the first IPI.
 *
 * Only one page: the whole blob has to fit below 1 MB and inside a single frame,
 * because the identity mapping set up for the mode switch covers exactly it.
 */

.section .text
.globl ap_image_start
.globl ap_params_pml4
.globl ap_params_stack
.globl ap_params_entry
.globl ap_params_cpu

ap_image_start:
        /* The startup IPI enters at the first byte, so the parameter block the
         * kernel patches cannot be in front of the code unless something jumps
         * over it. Two bytes, then 62 bytes of values at fixed offsets. */
        .byte 0xEB, 0x3E                /* jmp ap_code (0x8000 + 64) */
        .space 6
ap_params_pml4:  .quad 0                /* +8  */
ap_params_stack: .quad 0                /* +16 */
ap_params_entry: .quad 0                /* +24 */
ap_params_cpu:   .quad 0                /* +32 */
ap_params_idtr:                         /* +40: limit (2), base (8), packed */
        .space 16
        .space 8                        /* -> code begins at +64 */
ap_code:
.code16
        cli
        xorw    %ax, %ax
        movw    %ax, %ds
        movw    %ax, %es
        movw    %ax, %ss
        movl    $0x7C00, %esp           /* a stack below the blob is gone; above is fine */
        data32 lgdt gdt_desc

        movl    %cr0, %eax
        orl     $0x00000001, %eax       /* PE */
        movl    %eax, %cr0
        ljmp    $0x08, $ap_pm32

.code32
ap_pm32:
        movw    $0x10, %ax
        movw    %ax, %ds
        movw    %ax, %es
        movw    %ax, %ss
        movw    %ax, %fs
        movw    %ax, %gs

        movl    %cr4, %eax
        orl     $0x00000020, %eax       /* PAE: required before long mode */
        movl    %eax, %cr4

        movl    (ap_params_pml4 - ap_image_start) + 0x8000, %eax
        movl    %eax, %cr3

        movl    $0xC0000080, %ecx       /* IA32_EFER */
        rdmsr
        orl     $0x00000900, %eax       /* LME | NXE */
        wrmsr
        /* NXE is not optional here: the kernel marks its data pages no-execute,
         * and with EFER.NXE clear that bit is reserved - every store to .data or
         * .bss faults while code and the stack keep working, which looks like a
         * memory-corruption bug and is really an encoding one. */

        movl    %cr0, %eax
        andl    $~0x60000000, %eax      /* cache on: CD and NW are set at reset */
        orl     $0x80010021, %eax       /* PG | WP | NE | PE, as the BSP runs */
        movl    %eax, %cr0

        movl    %cr4, %eax
        orl     $0x00000620, %eax       /* PAE | OSFXSR | OSXMMEXCPT: SSE works */
        movl    %eax, %cr4

        /* The very next fetch happens with paging on, which is why the kernel
         * identity-maps this page before starting us. */
        ljmp    $0x18, $ap_pm64

.code64
ap_pm64:
        /* The table above is only good enough to get into long mode. Every gate
         * in the kernel's IDT names selector 0x08, and 0x08 here is a 32-bit
         * segment, so an AP that keeps this table takes a #GP while an interrupt
         * is being delivered and triple-faults the machine. Adopt the kernel's
         * GDT and run on its selectors instead. */
        movq    $0x8032, %rax           /* 0x8000 + 50: the kernel GDTR copy */
        lgdt    (%rax)
        pushq   $0x08                   /* kernel code, 64-bit in that table */
        leaq    ap_pm64_kernel(%rip), %rax
        pushq   %rax
        lretq

ap_pm64_kernel:
        movw    $0x10, %ax              /* kernel data */
        movw    %ax, %ds
        movw    %ax, %es
        movw    %ax, %ss
        movw    %ax, %fs
        movw    %ax, %gs

        movq    ap_params_stack, %rsp
        xorq    %rbp, %rbp
        /* The marker goes out before %rax is loaded: outb needs %al, and %al is
         * the low byte of the entry point. Writing it afterwards called into the
         * middle of the function instead of its first instruction. */
        /* An AP with no IDT triple-faults on its first exception and takes the
         * machine down with it; the same table as every other core turns a fault
         * into a dump. */
        movq    $0x8028, %rax           /* 0x8000 + 40 */
        lidt    (%rax)

        movq    ap_params_cpu, %rdi     /* ap_main(cpu_index) */
        movq    ap_params_entry, %rax
        callq   *%rax

        /* ap_main parks instead of returning; if it ever does, stop cleanly. */
ap_dead:
        cli
        hlt
        jmp     ap_dead

/* ---- the tables the switch needs: flat, 4 GB, present ---- */
.p2align 4
gdt:
        .quad 0x0000000000000000        /* null */
        .quad 0x00CF9A000000FFFF        /* 0x08 code32: D=1 */
        .quad 0x00CF92000000FFFF        /* 0x10 data */
        .quad 0x00AF9A000000FFFF        /* 0x18 code64: L=1 */
gdt_end:
gdt_desc:
        .word gdt_end - gdt - 1
        .long gdt
.globl ap_image_end
ap_image_end: