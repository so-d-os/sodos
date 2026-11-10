; ap_tramp.asm - trampolim dos APs (blob binario copiado para 0xA000).
;
; O BSP envia INIT-SIPI (vetor 0x0A -> CS=0x0A00, IP=0, linear 0xA000) e o AP
; comeca em modo real aqui. Parametros em 0xAF40, escritos pelo BSP antes do
; SIPI:
;   0xAF40 u32 cr3
;   0xAF48 u64 pilha (topo, alinhado a 16)
;   0xAF50 u64 entrada (ap_main)
;   0xAF58 u32 numero da cpu
;   0xAF60      GDTR do BSP (sgdt)  - 10 bytes
;   0xAF70      IDTR do BSP (sidt)  - 10 bytes
;
; GDT temporaria em 0xAF00: 0x08 = codigo 32, 0x10 = codigo 64, 0x18 = dados.
; No fim carrega a GDT/IDT do BSP e cai em ap_main com a pilha do AP.
bits 16
org 0xA000

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xAF00

    mov dx, 0x3F8          ; "A1": o AP comecou a executar
    mov al, 'A'
    out dx, al
    mov al, '1'
    out dx, al

    in al, 0x92            ; A20 (e nao aciona o fast reset)
    or al, 2
    and al, 0xFE
    out 0x92, al

    mov dx, 0x3F8          ; "A2": A20 ligado
    mov al, 'A'
    out dx, al
    mov al, '2'
    out dx, al

    lgdt [gdt_desc]

    mov dx, 0x3F8          ; "A3": GDT temporaria carregada
    mov al, 'A'
    out dx, al
    mov al, '3'
    out dx, al

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    mov dx, 0x3F8          ; "A4": PE ligado, vai para o codigo 32
    mov al, 'A'
    out dx, al
    mov al, '4'
    out dx, al

    jmp 0x08:pm32

bits 32
pm32:
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov dx, 0x3F8          ; "B2": modo protegido
    mov al, 'B'
    out dx, al
    mov al, '2'
    out dx, al

    mov eax, cr4
    or eax, 0x620          ; PAE | OSFXSR | OSXMMEXCPT
    mov cr4, eax

    mov ecx, 0x277         ; PAT igual ao do BSP
    mov eax, 0x00070106
    mov edx, 0x00070406
    wrmsr

    mov eax, [0xAF40]      ; CR3 do BSP (mesmas tabelas, identidade 0..4 GB)
    mov cr3, eax

    mov ecx, 0xC0000080    ; EFER.LME
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, 0x80000033    ; CR0: PE|MP|ET|NE|PG
    mov cr0, eax

    jmp 0x10:lm64

bits 64
lm64:
    mov dx, 0x3F8          ; "C3": long mode
    mov al, 'C'
    out dx, al
    mov al, '3'
    out dx, al

    mov ax, 0x18
    mov ds, ax
    mov es, ax

    lgdt [abs 0xAF60]      ; GDT definitiva do BSP
    ; Em 64 bits nao existe jmp ptr16:64 direto: empilha seletor+offset e retfq.
    mov rax, lm64b
    push qword 0x08
    push rax
    retfq

lm64b:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax

    lidt [abs 0xAF70]      ; IDT do BSP
    mov rsp, [abs 0xAF48]  ; pilha do AP
    mov edi, [abs 0xAF58]  ; numero da cpu (arg de ap_main)

    mov dx, 0x3F8          ; "D4": vai entrar em ap_main
    mov al, 'D'
    out dx, al
    mov al, '4'
    out dx, al

    mov rax, [abs 0xAF50]
    call rax               ; ap_main(cpu); ABI: call deixa rsp % 16 == 8
.hang:
    cli
    hlt
    jmp .hang

; ---- dados fixos em 0xAF00 (offset 0xF00 no blob) ----
times 0xF00 - ($ - $$) db 0

gdt:
    dq 0
    dq 0x00CF9A000000FFFF  ; 0x08: codigo 32 bits, base 0, limite 4 GB
    dq 0x00AF9A000000FFFF  ; 0x10: codigo 64 bits
    dq 0x00CF92000000FFFF  ; 0x18: dados
gdt_desc:
    dw 4 * 8 - 1
    dq 0xAF00

times 0xF80 - ($ - $$) db 0
