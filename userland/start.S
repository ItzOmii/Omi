/* Process entry. The kernel has already put argc at (%rsp) and argv right
 * after it; all this has to do is hand them to C and exit with its status. */

    .text
    .globl _start
_start:
    xorq %rbp, %rbp
    movq (%rsp), %rdi              /* argc */
    leaq 8(%rsp), %rsi             /* argv */
    andq $-16, %rsp                /* SysV: keep the stack aligned */
    call cmain
    movq %rax, %rdi
    movq $2, %rax                  /* SYS_EXIT - returns only if that fails */
    int  $0x80
1:  jmp 1b
