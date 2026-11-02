; multiboot_header.asm - carregador Multiboot2 para o GRUB.
;
; O GRUB (BIOS ou UEFI) entrega o kernel em 0x01000000 e salta aqui em
; modo protegido de 32 bits, com paginacao desligada. Este stub traduz a
; informacao do Multiboot2 para o BootInfo v1 em 0x5000 e o mapa em 0x5200,
; depois chama kernel_entry com EBX = BootInfo*. O kernel C nao muda.
;
; Firmware fica UEFI: o GRUB desta maquina ja encerrou os boot services.
; boot_drive fica 0 porque nao ha unidade BIOS neste caminho.

bits 32

section .multiboot progbits alloc
align 8
header_start:
    dd 0xE85250D6
    dd 0
    dd header_end - header_start
    dd -(0xE85250D6 + (header_end - header_start))

    align 8
    dw 1
    dw 0
    dd 16
    dd 6
    dd 8

    ; Pede um framebuffer linear de 32 bpp (largura/altura 0 = o GRUB escolhe).
    ; Sem esta tag o GRUB EFI responde "no suitable video mode found" e o
    ; kernel fica sem tela: no UEFI nao ha modo texto em 0xB8000.
    align 8
    dw 5
    dw 1
    dd 20
    dd 0
    dd 0
    dd 32

    align 8
    dw 0
    dw 0
    dd 8
header_end:

section .data
align 4
mb_count: dd 0
mb_entsz: dd 0
mb_next:  dd 0

section .text
align 4
global multiboot_entry
extern kernel_entry
extern __bss_end

MB2_MAGIC equ 0x36d76289
BOOTINFO  equ 0x00005000
MEMMAP    equ 0x00005200
MBI_TMP   equ 0x00060000
MBI_MAX   equ 32768

multiboot_entry:
    cli
    mov esp, 0x9FC00
    and esp, 0xFFFFFFF0
    cld

    cmp eax, MB2_MAGIC
    jne .bad_magic
    test ebx, ebx
    jz .bad_magic

    mov esi, ebx
    mov ecx, [esi]
    cmp ecx, 8
    jb .bad_magic
    cmp ecx, MBI_MAX
    ja .bad_magic

    mov edi, MBI_TMP
    rep movsb

    xor eax, eax
    mov edi, BOOTINFO
    mov ecx, (128 + 64 * 24) / 4
    rep stosd

    mov dword [mb_count], 0
    mov dword [BOOTINFO + 0], 0x53444F53
    mov dword [BOOTINFO + 4], 1
    mov dword [BOOTINFO + 8], 128
    mov dword [BOOTINFO + 12], 2
    mov dword [BOOTINFO + 16], 0x01000000
    mov eax, __bss_end
    sub eax, 0x01000000
    mov [BOOTINFO + 24], eax
    mov dword [BOOTINFO + 32], kernel_entry
    mov dword [BOOTINFO + 88], 64
    mov dword [BOOTINFO + 96], MEMMAP

    mov esi, MBI_TMP
    mov ebp, [esi]
    add ebp, MBI_TMP
    add esi, 8

.tag_loop:
    cmp esi, ebp
    jae .tags_done
    mov ecx, [esi + 4]
    cmp ecx, 8
    jb .tags_done
    mov eax, esi
    add eax, ecx
    add eax, 7
    and eax, ~7
    cmp eax, ebp
    ja .tags_done
    mov [mb_next], eax
    movzx eax, word [esi]
    cmp eax, 0
    je .tags_done
    cmp eax, 6
    je .mmap
    cmp eax, 8
    je .fb
    jmp .advance

.mmap:
    mov eax, [esi + 8]
    cmp eax, 20
    jb .advance
    mov [mb_entsz], eax
    lea edi, [esi + 16]
    mov ebx, esi
    add ebx, [esi + 4]
.ment:
    cmp edi, ebx
    jae .advance
    mov eax, edi
    add eax, 20
    cmp eax, ebx
    ja .advance
    mov ecx, [mb_count]
    cmp ecx, 64
    jae .advance
    imul eax, ecx, 24
    add eax, MEMMAP
    mov ecx, [edi]
    mov [eax], ecx
    mov ecx, [edi + 4]
    mov [eax + 4], ecx
    mov ecx, [edi + 8]
    mov [eax + 8], ecx
    mov ecx, [edi + 12]
    mov [eax + 12], ecx
    mov ecx, [edi + 16]
    cmp ecx, 1
    jb .mres
    cmp ecx, 5
    jbe .mstore
.mres:
    mov ecx, 2
.mstore:
    mov [eax + 16], ecx
    mov dword [eax + 20], 0
    inc dword [mb_count]
    add edi, [mb_entsz]
    jmp .ment

.fb:
    mov ecx, [esi + 4]
    cmp ecx, 30
    jb .advance
    cmp byte [esi + 29], 1
    jne .advance
    movzx eax, byte [esi + 28]
    cmp eax, 24
    jb .advance
    mov eax, [esi + 8]
    mov [BOOTINFO + 56], eax
    mov eax, [esi + 12]
    mov [BOOTINFO + 60], eax
    mov eax, [esi + 20]
    mov [BOOTINFO + 64], eax
    mov eax, [esi + 24]
    mov [BOOTINFO + 68], eax
    mov eax, [esi + 16]
    mov [BOOTINFO + 72], eax
    movzx eax, byte [esi + 28]
    mov [BOOTINFO + 76], eax
    mov dword [BOOTINFO + 80], 1
    cmp ecx, 38
    jb .advance
    movzx eax, byte [esi + 32]
    movzx ebx, byte [esi + 36]
    cmp eax, ebx
    jb .advance
    mov dword [BOOTINFO + 80], 2
    jmp .advance

.advance:
    mov esi, [mb_next]
    jmp .tag_loop

.tags_done:
    mov eax, [mb_count]
    mov [BOOTINFO + 84], eax
    mov ebx, BOOTINFO
    call kernel_entry

.hang:
    cli
    hlt
    jmp .hang

.bad_magic:
    mov dx, 0x3F8
    mov al, 'M'
    out dx, al
    mov edi, 0xB8000
    mov eax, 0x4F4D4F4D
    mov [edi], eax
    jmp .hang
