; kernel_entry.asm - entra em long mode e chama kmain(BootInfo*).
;
; Contrato de entrada v1 (BIOS stage2 e futuro UEFI):
;   EBX = endereco fisico de struct BootInfo, com magic BOOTINFO_MAGIC
;   paging desligado, interrupcoes desligadas, A20 ligado
;   CS=0x08; DS=ES=FS=GS=SS=0x10; ESP=0x9FC00 alinhado a 16
;   DF limpo; EAX/ECX/EDX indefinidos
;
; O ponto de entrada continua sendo kernel_entry.  Qualquer carregador que
; respeite o contrato entrega o mesmo BootInfo ao mesmo kmain() 64 bits.
; Paginas de 2 MB, identidade, 4 GB: diretorios 0x3000 (GB 0), pd_gb12 (GB 1
; e GB 2, no .data do kernel) e 0x6000 (GB 3); ver mm.c.  GB 1/2 ficavam em
; 0x8000/0x9000, mas o smp.c copia o trampolim dos APs para 0x8000: 1-2 GB
; passavam a apontar para lixo (#PF em 0x40000000 no umalloc_aligned).  Tabelas em 0x1000/0x2000.  No GB 3
; o buraco PCI fica UC pelos MTRRs do firmware; o framebuffer Bochs em
; 0xE0000000 e o candidato 0xFD000000 ganham UC explicito. GDT em 0x4000.
; 0x8000-0x9FFF era o stage2; ele ja terminou quando chegamos aqui.
; BootInfo fica em 0x5000 e o mapa E820 em 0x5200; nada aqui os sobrescreve.

bits 32
section .text
global kernel_entry
extern kmain

kernel_entry:
    test ebx, ebx
    jz .no_bootinfo
    cmp dword [ebx], 0x53444F53
    jne .no_bootinfo
    mov ebp, ebx          ; cpuid abaixo clobbera EBX; EBP preserva BootInfo

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

    mov ecx, 512
    mov eax, 0x83
    mov edi, 0x3000
.fill:
    mov [edi], eax
    mov dword [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill

    ; 1 GB..3 GB: pd_gb12 (dois diretorios contiguos, 1024 entradas).
    mov ecx, 1024
    mov eax, 0x40000083
    mov edi, pd_gb12
.fill12:
    mov [edi], eax
    mov dword [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill12
    mov eax, pd_gb12 + 3
    mov dword [0x2008], eax
    mov dword [0x200C], 0
    mov eax, pd_gb12 + 0x1000 + 3
    mov dword [0x2010], eax
    mov dword [0x2014], 0

    ; 3 GB..4 GB: identidade inteira (RAM abaixo do buraco PCI em notebooks)
    ; e o LFB (16 MB em 0xE0000000 e em 0xFD000000) marcado UC por cima.
    ; Sem isto o win le 0xE0000000 e toma page fault.
    mov ecx, 512
    mov eax, 0xC0000083
    mov edi, 0x6000
.fill3:
    mov [edi], eax
    mov dword [edi+4], 0
    add eax, 0x200000
    add edi, 8
    loop .fill3
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

    ; Framebuffer entregue pelo carregador (GOP via GRUB): pode estar em
    ; qualquer lugar (0xD0000000 numa AMD, BAR alto acima de 4 GB...).
    ; Mapeia 64 MB a partir de fb_addr, write-combining (PAT 1 = WC abaixo).
    ; GB 0..3 usam os diretorios 0x3000/pd_gb12/pd_gb12+0x1000/0x6000; acima, 0x7000.
    mov eax, [ebp + 56]
    mov edx, [ebp + 60]
    mov ecx, eax
    or ecx, edx
    jz .fbi_done
    cmp edx, 0x80                 ; >= 512 GB: fora do PDPT unico
    jae .fbi_done
    mov ecx, eax
    shr ecx, 30
    mov esi, edx
    shl esi, 2
    or ecx, esi                   ; ECX = indice no PDPT (GB)
    mov edi, 0x3000
    test ecx, ecx
    jnz .fbi_not0
    cmp eax, 0x10000000           ; abaixo de 256 MB: memoria do kernel, nao vira WC
    jb .fbi_done
    jmp .fbi_map
.fbi_not0:
    mov edi, pd_gb12
    cmp ecx, 1
    je .fbi_map
    mov edi, pd_gb12 + 0x1000
    cmp ecx, 2
    je .fbi_map
    mov edi, 0x6000
    cmp ecx, 3
    je .fbi_map
    push eax
    push ecx
    xor eax, eax
    mov edi, 0x7000
    mov ecx, 1024
    rep stosd
    pop ecx
    pop eax
    mov edi, 0x7000
    mov dword [0x2000 + ecx*8], 0x7003
    mov dword [0x2004 + ecx*8], 0
.fbi_map:
    mov esi, eax
    shr esi, 21
    and esi, 511                  ; primeira entrada no diretorio
    and eax, 0xFFE00000
    or eax, 0x8B                  ; P | RW | PWT | PS -> PAT 1 (WC)
    mov ecx, 32
.fbi_loop:
    cmp esi, 512
    jae .fbi_done
    mov [edi + esi*8], eax
    mov [edi + esi*8 + 4], edx
    add eax, 0x200000
    inc esi
    loop .fbi_loop
.fbi_done:

    ; PAT: entrada 1 (PWT=1, PCD=0) vira write-combining; o resto e o padrao.
    ; So o framebuffer acima usa PWT sozinho.
    mov ecx, 0x277
    mov eax, 0x00070106
    mov edx, 0x00070406
    wrmsr

    ; GDT: nulo, codigo 64 (0x08), dados (0x10)
    mov dword [0x4000], 0
    mov dword [0x4004], 0
    mov dword [0x4008], 0x00000000
    mov dword [0x400C], 0x00209A00
    mov dword [0x4010], 0x00000000
    mov dword [0x4014], 0x00009200
    mov word  [0x4100], 23
    mov dword [0x4102], 0x4000

    ; CR4: PAE, OSFXSR e OSXMMEXCPT (SSE para programas; o kernel e -mno-sse).
    ; CR0: MP=1, EM=0, TS=0, NE=1. Todo x86-64 tem SSE2.
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax
    mov eax, cr0
    and eax, ~((1 << 2) | (1 << 3))
    or eax, (1 << 1) | (1 << 5)
    mov cr0, eax
    fninit

    ; AVX para programas (LLAMA): CR4.OSXSAVE e XCR0 = x87|SSE|AVX, so se o
    ; CPU tem XSAVE e AVX (CPUID.1:ECX bits 26 e 28).  O kernel nao usa AVX e o
    ; escalonador e cooperativo, entao nao ha xsave na troca de tarefa.
    mov eax, 1
    cpuid
    and ecx, (1 << 26) | (1 << 28)
    cmp ecx, (1 << 26) | (1 << 28)
    jne .no_avx
    mov eax, cr4
    or eax, (1 << 18)
    mov cr4, eax
    xor ecx, ecx
    xgetbv
    or eax, 7
    xsetbv
.no_avx:

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

.no_bootinfo:
    mov dx, 0x3F8
    mov al, 'B'
    out dx, al
.no_bootinfo_hang:
    cli
    hlt
    jmp .no_bootinfo_hang

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
    cld

    mov dx, 0x3F8
    mov al, 'L'
    out dx, al

    mov edi, ebp          ; RDI = BootInfo* (zero-extend de EBP)
    call kmain

.hang:
    cli
    hlt
    jmp .hang

; Diretorios de pagina de GB 1 e GB 2. No .data (nao no .bss): o kmain zera o
; BSS depois que a paginacao ja usa estas tabelas.
section .data
align 4096
pd_gb12:
    times 1024 dq 0
