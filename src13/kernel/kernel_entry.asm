; kernel_entry.asm - entra em long mode e chama kmain.
; O boot (boot.asm) fica em 32 bits e faz call cdecl:
;   [esp+4] = app_addr, [esp+8] = app_size.
; Paginas de 2 MB, identidade, 256 MB (igual a QEMU -m 256 e ao probe de mm.c).
; Tabelas em 0x1000/0x2000/0x3000. O diretorio 0x6000 cobre o framebuffer
; Bochs em 0xE0000000 e o candidato 0xFD000000. GDT em 0x4000. Args em 0x5000.

bits 32
section .text
global kernel_entry
extern kmain

kernel_entry:
    mov eax, [esp+4]
    mov [0x5000], eax
    mov eax, [esp+8]
    mov [0x5004], eax

    mov dx, 0x3F8
    mov al, 'P'
    out dx, al

    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no64
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no64

    xor eax, eax
    mov edi, 0x1000
    mov ecx, (4096 * 3) / 4
    rep stosd

    mov dword [0x1000], 0x2003
    mov dword [0x1004], 0
    mov dword [0x2000], 0x3003
    mov dword [0x2004], 0

    mov ecx, 128
    mov eax, 0x83
    mov edi, 0x3000
.fill:
    mov [edi], eax
    mov dword [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill

    ; 3 GB..4 GB: so o LFB (16 MB em 0xE0000000 e em 0xFD000000).
    ; Sem isto o win le 0xE0000000 e toma page fault.
    xor eax, eax
    mov edi, 0x6000
    mov ecx, 1024
    rep stosd
    mov dword [0x2018], 0x6003
    mov dword [0x201C], 0

    mov ecx, 8
    mov eax, 0xE000009B
    mov edi, 0x6000 + 256*8
.fb_e:
    mov [edi], eax
    mov dword [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .fb_e

    mov ecx, 8
    mov eax, 0xFD00009B
    mov edi, 0x6000 + 488*8
.fb_fd:
    mov [edi], eax
    mov dword [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .fb_fd

    ; GDT: nulo, codigo 64 (0x08), dados (0x10)
    mov dword [0x4000], 0
    mov dword [0x4004], 0
    mov dword [0x4008], 0x00000000
    mov dword [0x400C], 0x00209A00
    mov dword [0x4010], 0x00000000
    mov dword [0x4014], 0x00009200
    mov word  [0x4100], 23
    mov dword [0x4102], 0x4000

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov eax, 0x1000
    mov cr3, eax

    lgdt [0x4100]

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax

    jmp 0x08:lm_entry

.no64:
    mov dx, 0x3F8
    mov al, 'N'
    out dx, al
.no64_hang:
    cli
    hlt
    jmp .no64_hang

bits 64
lm_entry:
    mov ax, 0x10
    mov ss, ax
    mov ds, ax
    mov es, ax
    xor ax, ax
    mov fs, ax
    mov gs, ax

    mov rsp, 0x9FC00

    mov dx, 0x3F8
    mov al, 'L'
    out dx, al

    mov edi, [qword 0x5000]
    mov esi, [qword 0x5004]
    call kmain

.hang:
    cli
    hlt
    jmp .hang
