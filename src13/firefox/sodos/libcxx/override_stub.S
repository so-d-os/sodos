/* override_stub.S - o libc++ new.cpp percorre a secao _lcxx_override para
 * achar substituicoes de operator new/delete. O linker do SODOS nao cria
 * __start/__stop; definimos os dois no mesmo endereco (secao vazia). */
    .section .data
    .globl __start___lcxx_override
    .globl __stop___lcxx_override
__start___lcxx_override:
__stop___lcxx_override:
    .byte 0
