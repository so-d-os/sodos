; cmd_shutdown.asm — desliga a maquina.
; QEMU (PIIX4, BIOS deixa o PM em 0x600): PM1a_CNT = 0x604, SLP_EN = 0x2000.
; Se essa escrita nao sair, tenta o porto antigo do Bochs (0xB004).

bits 64

section .text
global cmd_shutdown
extern puts_k

cmd_shutdown:
    lea rdi, [rel msg]
    call puts_k
    mov dx, 0x604
    mov ax, 0x2000
    out dx, ax
    mov dx, 0xB004
    mov ax, 0x2000
    out dx, ax
.hang:
    hlt
    jmp .hang

section .rodata
msg: db "Desligando...", 10, 0
