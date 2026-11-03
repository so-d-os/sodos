/* setjmp.c - setjmp/longjmp para a libc do gcc -mx32 (LIBCX). No cc sao
 * gerados inline pelo compilador e este arquivo fica vazio. */
#if defined(__GNUC__) && defined(__x86_64__)
__asm__(
    ".text\n"
    ".globl setjmp\n"
    ".type setjmp,@function\n"
    "setjmp:\n"
    "  mov %rbx, (%edi)\n"
    "  mov %rbp, 8(%edi)\n"
    "  mov %r12, 16(%edi)\n"
    "  mov %r13, 24(%edi)\n"
    "  mov %r14, 32(%edi)\n"
    "  mov %r15, 40(%edi)\n"
    "  lea 8(%rsp), %rdx\n"
    "  mov %rdx, 48(%edi)\n"
    "  mov (%rsp), %rdx\n"
    "  mov %rdx, 56(%edi)\n"
    "  xor %eax, %eax\n"
    "  ret\n"
    ".globl longjmp\n"
    ".type longjmp,@function\n"
    "longjmp:\n"
    "  mov %esi, %eax\n"
    "  test %eax, %eax\n"
    "  jnz 1f\n"
    "  inc %eax\n"
    "1:\n"
    "  mov (%edi), %rbx\n"
    "  mov 8(%edi), %rbp\n"
    "  mov 16(%edi), %r12\n"
    "  mov 24(%edi), %r13\n"
    "  mov 32(%edi), %r14\n"
    "  mov 40(%edi), %r15\n"
    "  mov 48(%edi), %rsp\n"
    "  jmp *56(%edi)\n");
#else
typedef int setjmp_inline_no_cc;
#endif
