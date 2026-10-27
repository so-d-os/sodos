; boot.asm - stage1 (512 bytes)
; i386-friendly: usa BIOS, real mode, entra em protected mode, chama kernel 32-bit
org 0x7C00
bits 16

KERNEL_LOAD      equ 0x10000
%ifndef KERNEL_ENTRY
%define KERNEL_ENTRY 0x1026c
%endif
APP_LOAD         equ 0x58000   ; depois do kernel (que pode ir ate 0x58000)

%ifndef KERNEL_SECTORS
KERNEL_SECTORS   equ 20
%endif
APP_SECTORS      equ 4

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [BOOT_DRIVE], dl

    mov ax, 0x0003
    int 0x10

    mov si, msg
    call print

    call enable_a20

    ; Verifica suporte a LBA estendido
    call check_lba_support

    mov ax, KERNEL_LOAD >> 4
    mov es, ax
    xor bx, bx
    mov cx, KERNEL_SECTORS
    mov di, 1              ; LBA fixo: setor 1 (kernel)
    call read_sectors

    mov ax, APP_LOAD >> 4
    mov es, ax
    xor bx, bx
    mov cx, APP_SECTORS
    ; Carrega app logo após o kernel
    mov di, 1 + KERNEL_SECTORS
    call read_sectors

    cli
    lgdt [gdt_desc]

    ; Entra em protected mode
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; Limpa pipeline (necessário em processadores modernos com pipeline)
    ; O jmp far força uma limpeza completa do pipeline
    jmp CODE_SEL:pm_entry

bits 32
pm_entry:
    mov ax, DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ; Alinha stack a 16 bytes para compatibilidade com SSE/MMX em processadores modernos
    mov esp, 0x9FC00
    and esp, 0xFFFFFFF0  ; Alinha para 16 bytes

    push dword APP_SECTORS*512
    push dword APP_LOAD

    call dword KERNEL_ENTRY
    add esp, 8

hang:
    jmp hang

bits 16
print:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bh, 0
    mov bl, 0x07
    int 0x10
    jmp print
.done:
    ret

enable_a20:
    ; Método rápido (porta 0x92) - funciona na maioria dos sistemas modernos
    in al, 0x92
    test al, 2
    jnz .done
    or al, 2
    and al, 0xFE
    out 0x92, al
.done:
    ret

; Verifica suporte a INT 13h extensions (LBA)
check_lba_support:
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc .no_lba
    cmp bx, 0xAA55
    jne .no_lba
    test cl, 1
    jz .no_lba
    mov byte [LBA_SUPPORTED], 1
    ret
.no_lba:
    mov byte [LBA_SUPPORTED], 0
    ret

; Wrapper que escolhe entre LBA e CHS
; Entrada: ES:BX=buffer, CX=setores, DI=LBA
read_sectors:
    cmp byte [LBA_SUPPORTED], 0
    jne read_lba_extended
    jmp read_lba_chs

; LBA estendido (INT 13h AH=42h)
read_lba_extended:
    push ax
    push bx
    push cx
    push si
    push di
    
.next:
    mov word [DAP+2], 1
    mov [DAP+4], bx
    mov ax, es
    mov [DAP+6], ax
    xor eax, eax
    mov ax, di
    mov [DAP+8], eax
    mov dword [DAP+12], 0
    
    mov si, DAP
    mov ah, 0x42
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc disk_fail
    
    add bx, 512
    jnc .no_wrap_ext
    mov ax, es
    add ax, 0x1000
    mov es, ax
.no_wrap_ext:
    inc di
    loop .next
    
    pop di
    pop si
    pop cx
    pop bx
    pop ax
    ret

; CHS fallback - lê múltiplos setores
read_lba_chs:
    push ax
    push bx
    push cx
    push dx
    push si
    push di
    push es

.next:
    mov ax, di
    xor dx, dx
    mov si, 63
    div si
    inc dx
    mov cl, dl

    xor dx, dx
    mov si, 16
    div si
    mov dh, dl
    mov ch, al

    mov dl, [BOOT_DRIVE]
    mov ah, 0x02
    mov al, 0x01
    int 0x13
    jc disk_fail

    add bx, 512
    jnc .no_wrap_chs
    mov ax, es
    add ax, 0x1000
    mov es, ax
.no_wrap_chs:
    inc di
    loop .next

    pop es
    pop di
    pop si
    pop dx
    pop cx
    pop bx
    pop ax
    ret


disk_fail:
    mov si, msg_fail
    call print
    jmp $

BOOT_DRIVE db 0
LBA_SUPPORTED db 0

msg db "Boot OK. Loading...", 13,10,0
msg_fail db "Disk error!", 13,10,0

; Disk Address Packet para INT 13h extensão (AH=42h)
; Tamanho 16 bytes
DAP:
    db 16          ; tamanho
    db 0           ; reservado
    dw 1           ; número de setores (ajustado em runtime)
    dw 0           ; offset do buffer (ajustado em runtime)
    dw 0           ; segmento do buffer (ajustado em runtime)
    dd 0           ; LBA (low)
    dd 0           ; LBA (high - não usado aqui)

align 8
gdt:
    dq 0x0000000000000000  ; Null descriptor
    dq 0x00CF9A000000FFFF  ; Code segment: base=0, limit=4GB, 32-bit, executable, readable
    dq 0x00CF92000000FFFF  ; Data segment: base=0, limit=4GB, 32-bit, writable
    ; GDT compatível com processadores modernos (i386 até x86-64 em modo 32-bit)

gdt_desc:
    dw gdt_desc - gdt - 1
    dd gdt

CODE_SEL equ 0x08
DATA_SEL equ 0x10

; ------------------------------
; Tabela de partições MBR (64 bytes)
; Fica nos bytes 446-509 do setor
; ------------------------------
partition_table:
    ; Partição 1: Ativa, tipo 0x83 (Linux), começa no setor 1
    db 0x80                    ; Status: ativa
    db 0x00, 0x01, 0x00        ; CHS início (head=0, sector=1, cylinder=0)
    db 0x83                    ; Tipo: Linux filesystem (genérico)
    db 0xFF, 0xFF, 0xFF        ; CHS fim (máximo, não usamos)
    dd 1                       ; LBA start: setor 1 (onde o kernel começa)
    dd 24                      ; Sector count: 24 setores (kernel + app)

    ; Partições 2-4: Vazias
    times 16 db 0
    times 16 db 0
    times 16 db 0

times 510-($-$$) db 0
dw 0xAA55
