/* crt1.S - entrada de um executavel ELF dinamico (ligado com a libc.so.6).
 *
 * O kernel ja carregou e relocou o programa e as bibliotecas e chama
 * _start(argc, argv) no estilo SysV: rdi = argc, rsi = argv. Quem monta o TLS,
 * roda os construtores (das bibliotecas e depois os do programa) e chama main
 * e a libc: __libc_start_main, com a assinatura da glibc. */
    .text
    .globl _start
    .type _start, @function
_start:
    xorl %ebp, %ebp
    movq %rsi, %rdx                     /* argv */
    movq %rdi, %rsi                     /* argc */
    andq $-16, %rsp
    movq %rsp, %rax
    pushq %rax                          /* mantem a pilha em 16 bytes */
    pushq %rax                          /* 7o argumento: topo da pilha */
    xorl %r9d, %r9d                     /* rtld_fini */
    xorl %r8d, %r8d                     /* fini */
    xorl %ecx, %ecx                     /* init */
    movq main@GOTPCREL(%rip), %rdi
    call __libc_start_main@PLT
    hlt
    .size _start, .-_start

    .section .note.GNU-stack,"",@progbits
