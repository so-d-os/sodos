/*
 * eco.S - guest multiboot interativo: ecoa o que chega na COM1, em
 * maiusculas, para testar a entrada do teclado do SODOS ate o guest
 * (-serial stdio). Enter pula linha; "q" sai do QEMU pelo isa-debug-exit
 * (codigo 33).
 *
 *   QEMU.EXE -M isapc -m 16 -kernel ECO.ELF -serial stdio
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
    mov $msg, %esi
    call puts
loop:
    mov $0x3FD, %dx          /* LSR bit 0: chegou byte */
1:  in %dx, %al
    test $1, %al
    jz 1b
    mov $0x3F8, %dx
    in %dx, %al
    cmp $'q', %al
    je sair
    cmp $'\r', %al
    je enter
    cmp $'a', %al
    jb 2f
    cmp $'z', %al
    ja 2f
    sub $0x20, %al
2:  call putc
    jmp loop
enter:
    mov $crlf, %esi
    call puts
    jmp loop
sair:
    mov $bye, %esi
    call puts
    mov $0xF4, %dx           /* isa-debug-exit */
    mov $0x10, %al
    out %al, %dx
3:  hlt
    jmp 3b

/* putc: %al na COM1, esperando o THR vazio (LSR bit 5) */
putc:
    push %edx
    mov %al, %ah
    mov $0x3FD, %dx
1:  in %dx, %al
    test $0x20, %al
    jz 1b
    mov $0x3F8, %dx
    mov %ah, %al
    out %al, %dx
    pop %edx
    ret

/* puts: string em %esi terminada em 0 */
puts:
    lodsb
    test %al, %al
    jz 1f
    call putc
    jmp puts
1:  ret

    .section .rodata
msg:
    .ascii "Eco do guest i386 (QEMU dentro do SODOS).\r\n"
    .ascii "Digite: o guest devolve em maiusculas. q sai.\r\n> "
    .byte 0
crlf:
    .ascii "\r\n> "
    .byte 0
bye:
    .ascii "\r\nTchau.\r\n"
    .byte 0
