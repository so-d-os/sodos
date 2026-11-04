/* init_array_stub.S - tabela .init_array vazia para o link64 (SOX2).
 *
 * O runtime C++ espera __init_array_start/__init_array_end; o linker x32
 * sintetiza esses simbolos, o link64 ainda nao. Aqui os dois apontam para o
 * mesmo endereco: os testes nao usam construtores globais. */
    .section .init_array,"aw"
    .globl __init_array_start
    .globl __init_array_end
__init_array_start:
__init_array_end:
    .section .note.GNU-stack,"",@progbits
