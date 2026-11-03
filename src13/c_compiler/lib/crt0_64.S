/* crt0_64.S - entrada de um programa SOX2 (ABI LP64).
 *
 * O kernel chama _start(argc, argv) no estilo SysV: rdi=argc, rsi=argv.
 * A pilha ja e a do programa; alinhamos e chamamos main. */
    .text
    .globl _start
    .type _start, @function
_start:
    xorq %rbp, %rbp
    andq $-16, %rsp
    call main
    movl %eax, %edi
    call k_exit
    hlt
    .size _start, .-_start

    .section .note.GNU-stack,"",@progbits
