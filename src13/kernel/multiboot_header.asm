; multiboot_header.asm - entrada legada para GRUB/Multiboot.
;
; O caminho principal do SODOS e o BIOS em dois estagios (boot.asm + stage2.asm).
; Este arquivo mantem a ISO opcional do GRUB usando o MESMO contrato de entrada:
; EBX aponta para um BootInfo v1 e a transicao para long mode fica em
; kernel_entry.  O mapa E820 nao e preenchido aqui (GRUB nao o converte), entao
; mm.c usa o fallback de probe; por isso este caminho e considerado legado.

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

section .data
align 8
bootinfo_mb:
    dd 0x53444F53          ; magic "SODS"
    dd 1                   ; version
    dd 128                 ; size
    dd 0                   ; firmware desconhecido
    dq 0x01000000          ; kernel_phys
    dq 0                   ; kernel_size (desconhecido)
    dq 0                   ; kernel_entry (kernel_entry descobre pelo link)
    dq 0                   ; app_phys
    dq 0                   ; app_size
    dq 0                   ; fb_addr
    dd 0, 0, 0, 0, 0       ; fb_width/height/pitch/bpp/format
    dd 0, 0, 0             ; mem_count, mem_capacity, flags
    dq 0                   ; mem_map_addr
    dq 0                   ; boot_drive
    dq 0, 0                ; reserved

section .text
align 4
global multiboot_entry
extern kernel_entry

multiboot_entry:
    cli
    mov esp, 0x9FC00
    and esp, 0xFFFFFFF0
    cld
    mov ebx, bootinfo_mb
    call kernel_entry
.hang:
    cli
    hlt
    jmp .hang
