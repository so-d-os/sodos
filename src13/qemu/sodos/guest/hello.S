/*
 * hello.S - guest multiboot minimo para testar o QEMU dentro do SODOS.
 * Escreve na COM1 (0x3F8) e sai do QEMU pelo isa-debug-exit (porta 0xF4):
 * o QEMU termina com codigo (0x10 << 1) | 1 = 33.
 *
 *   QEMU.EXE -M isapc -m 16 -kernel HELLO.ELF -serial file:SAIDA.TXT
 *            -device isa-debug-exit,iobase=0xf4,iosize=4 -display none
 */
    .set MB_MAGIC, 0x1BADB002
    .set MB_FLAGS, 0
    .section .multiboot, "a"
    .align 4
    .long MB_MAGIC, MB_FLAGS, -(MB_MAGIC + MB_FLAGS)

    .text
    .code32
    .globl _start
_start:
    mov $0x90000, %esp
    /* contador de instrucoes: prova que a CPU emulada roda um laco */
    xor %ecx, %ecx
1:  inc %ecx
    cmp $1000000, %ecx
    jne 1b
    mov $msg, %esi
2:  lodsb
    test %al, %al
    jz 4f
    mov %al, %bl
3:  mov $0x3FD, %dx          /* LSR: espera THR vazio (bit 5) */
    in %dx, %al
    test $0x20, %al
    jz 3b
    mov $0x3F8, %dx
    mov %bl, %al
    out %al, %dx
    jmp 2b
4:  mov $0xF4, %dx           /* isa-debug-exit */
    mov $0x10, %al
    out %al, %dx
5:  hlt
    jmp 5b

    .section .rodata
msg:
    .ascii "Ola! Guest i386 rodando no QEMU dentro do SODOS.\r\n"
    .ascii "multiboot ok, serial 16550 ok, laco de 1e6 iteracoes ok.\r\n"
    .byte 0
