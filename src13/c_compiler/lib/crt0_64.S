/* crt0_64.S - entrada de um programa SOX2 (ABI LP64).
 *
 * O kernel chama _start(argc, argv) no estilo SysV: rdi=argc, rsi=argv.
 * Guardamos argc/argv em registradores callee-saved (os construtores C++
 * destroem os de argumento), registramos o topo da pilha para o runtime,
 * rodamos os ctors, e so entao chamamos main. */
    .data
    .globl __sodos_stack_top
__sodos_stack_top:
    .quad 0

    .text
    .globl _start
    .type _start, @function
_start:
    xorq %rbp, %rbp
    andq $-16, %rsp
    movq %rsp, __sodos_stack_top(%rip)
    movq %rdi, %rbx          /* argc */
    movq %rsi, %r12          /* argv */
    call __sodos_ctors
    movq %rbx, %rdi
    movq %r12, %rsi
    call main
    movl %eax, %ebx          /* codigo de saida (dtors preservam rbx) */
    call __sodos_dtors
    movl %ebx, %edi
    call k_exit
    hlt
    .size _start, .-_start

    .section .note.GNU-stack,"",@progbits
