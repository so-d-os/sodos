/* setjmp64.c - setjmp/longjmp LP64 (System V x86-64).
 *
 * jmp_buf (8 palavras): rbx, rbp, r12, r13, r14, r15, rsp e rip do chamador.
 * Antes usava __builtin_setjmp dentro de uma funcao C: o contexto salvo era o
 * quadro do proprio setjmp, que morre no retorno, e longjmp sempre devolvia 1
 * (o QEMU distingue COROUTINE_YIELD/TERMINATE/ENTER pelo valor).
 */
#include <setjmp.h>

__asm__(
  ".text\n"
  ".globl setjmp\n"
  ".type setjmp,@function\n"
  "setjmp:\n"
  "  movq %rbx, 0(%rdi)\n"
  "  movq %rbp, 8(%rdi)\n"
  "  movq %r12, 16(%rdi)\n"
  "  movq %r13, 24(%rdi)\n"
  "  movq %r14, 32(%rdi)\n"
  "  movq %r15, 40(%rdi)\n"
  "  leaq 8(%rsp), %rdx\n"     /* rsp do chamador depois do retorno */
  "  movq %rdx, 48(%rdi)\n"
  "  movq (%rsp), %rdx\n"      /* endereco de retorno */
  "  movq %rdx, 56(%rdi)\n"
  "  xorl %eax, %eax\n"
  "  ret\n"
  ".size setjmp, .-setjmp\n"

  ".globl longjmp\n"
  ".type longjmp,@function\n"
  "longjmp:\n"
  "  movl %esi, %eax\n"
  "  testl %eax, %eax\n"
  "  jnz 1f\n"
  "  incl %eax\n"              /* longjmp(b, 0) devolve 1 */
  "1:\n"
  "  movq 0(%rdi), %rbx\n"
  "  movq 8(%rdi), %rbp\n"
  "  movq 16(%rdi), %r12\n"
  "  movq 24(%rdi), %r13\n"
  "  movq 32(%rdi), %r14\n"
  "  movq 40(%rdi), %r15\n"
  "  movq 48(%rdi), %rsp\n"
  "  jmpq *56(%rdi)\n"
  ".size longjmp, .-longjmp\n");
