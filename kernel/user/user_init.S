/* The first user-mode program.
 *
 * kernel/ring3.c copies this blob into a read-only user page and enters it with
 * rdi = mode at CPL 3. There is no libc here: every service is an `int 0x80`
 * with the syscall number in rax, arguments in rdi / rsi / rdx, and the result
 * back in rax.
 *
 *   mode 0  say hello, yield twice, say it again, exit(7)
 *   mode 1  read a kernel address, which must fault and get this task killed
 *
 * The strings live in .text (same page, same mapping) so the program needs no
 * relocations beyond the rip-relative leas.
 */

    .text
    .globl _start
_start:
    movq  %rdi, %r12                 /* mode */
    testq %r12, %r12
    jne   .Lprobe

    /* write(1, "hello from ring 3\n", 18) */
    movq  $1, %rax
    movq  $1, %rdi
    leaq  msg1(%rip), %rsi
    movq  $18, %rdx
    int   $0x80

    /* yield twice: the scheduler must take us off the cpu and bring us back.
       rax comes back holding the return value, so reload it every time. */
    movq  $3, %rax
    int   $0x80
    movq  $3, %rax
    int   $0x80

    /* write(1, "second syscall, still ring 3\n", 29) */
    movq  $1, %rax
    movq  $1, %rdi
    leaq  msg2(%rip), %rsi
    movq  $29, %rdx
    int   $0x80

    /* exit(7) */
    movq  $2, %rax
    movq  $7, %rdi
    int   $0x80

1:  jmp   1b                        /* exit does not return */

.Lprobe:
    /* Kernel text is mapped supervisor-only in this address space, so this
     * load takes a #PF. If it instead returns, isolation is broken and the
     * task exits with code 9 to say so. */
    movabsq $0xFFFFFFFF80000000, %rax
    movq  (%rax), %rax

    movq  $2, %rax
    movq  $9, %rdi
    int   $0x80
2:  jmp   2b

msg1:
    .ascii "hello from ring 3\n"
msg2:
    .ascii "second syscall, still ring 3\n"