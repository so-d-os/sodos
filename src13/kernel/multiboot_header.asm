; multiboot_header.asm - cabeçalho Multiboot para GRUB

bits 32

section .multiboot
align 4

MBALIGN  equ 1<<0
MEMINFO  equ 1<<1
FLAGS    equ MBALIGN | MEMINFO
MAGIC    equ 0x1BADB002
CHECKSUM equ -(MAGIC + FLAGS)

dd MAGIC
dd FLAGS
dd CHECKSUM

section .text
align 4
global multiboot_entry
extern kmain

multiboot_entry:
    ; Configura stack semelhante ao boot.asm
    mov esp, 0x9FC00
    and esp, 0xFFFFFFF0

    ; Parâmetros para kmain (app_addr, app_size)
    ; Como não temos app em Multiboot, passamos 0
    push dword 0        ; app_size = 0
    push dword 0x58000  ; app_addr = APP_LOAD (mesmo do boot.asm)

    call kmain

.hang:
    jmp .hang
