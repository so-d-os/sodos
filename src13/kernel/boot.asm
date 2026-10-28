; boot.asm - stage1 (512 bytes) do carregador BIOS do SODOS.
;
; Responsabilidades minimas:
;   - escolher modo texto e imprimir "S1";
;   - carregar stage2.bin em 0x7E00 via LBA estendido (ou CHS como fallback);
;   - deixar {boot_drive, kernel_lba} em 0x4FF0 para o stage2;
;   - saltar para o stage2.  O kernel e a aplicacao NAO sao tocados aqui.
;
; O stage2 faz A20, E820, carrega o kernel acima de 1 MiB e entrega o BootInfo.
; Layout: setor 0 = este codigo; setores 1..N = stage2; depois kernel; depois app.

org 0x7C00
bits 16

%ifndef STAGE2_SECTORS
%define STAGE2_SECTORS 8
%endif
%ifndef PART_SECTORS
%define PART_SECTORS 128
%endif
%if STAGE2_SECTORS < 1
%error "STAGE2_SECTORS deve ser positivo"
%endif
%if STAGE2_SECTORS > 56
%error "stage2 grande demais para o buffer de 0x7E00..0xEE00"
%endif

STAGE2_LOAD     equ 0x7E00
BOOT_PARAM_ADDR equ 0x4FF0

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld

    mov [boot_drive], dl
    mov ax, 0x0003
    int 0x10

    mov si, msg_start
    call print

    ; Contrato com o stage2: unidade e LBA inicial do kernel.
    movzx eax, byte [boot_drive]
    mov [BOOT_PARAM_ADDR], eax
    mov eax, 1 + STAGE2_SECTORS
    mov [BOOT_PARAM_ADDR + 4], eax

    call check_lba
    cmp byte [lba_ok], 0
    je .chs

    ; LBA estendido: um setor por chamada, buffer em 0x07E0:offset.
    mov word [dap + 2], 1
    mov word [dap + 4], 0
    mov word [dap + 6], STAGE2_LOAD >> 4
    mov dword [dap + 8], 1
    mov dword [dap + 12], 0
    mov cx, STAGE2_SECTORS
.lba_loop:
    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc disk_fail
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax
    add word [dap + 4], 512
    inc dword [dap + 8]
    loop .lba_loop
    jmp .go

    ; CHS: 63 setores por trilha, 16 cabecas; um setor por chamada.
.chs:
    mov word [cur_off], 0
    mov dword [cur_lba], 1
    mov cx, STAGE2_SECTORS
.chs_loop:
    push cx
    mov eax, [cur_lba]
    xor edx, edx
    mov ebx, 63
    div ebx
    inc edx
    mov cl, dl
    xor edx, edx
    mov ebx, 16
    div ebx
    mov dh, dl
    mov ch, al
    mov al, ah
    and al, 0x03
    shl al, 6
    or cl, al
    mov dl, [boot_drive]
    mov ax, STAGE2_LOAD >> 4
    mov es, ax
    mov bx, [cur_off]
    mov ax, 0x0201
    int 0x13
    jc disk_fail
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax
    add word [cur_off], 512
    inc dword [cur_lba]
    pop cx
    loop .chs_loop

.go:
    mov dl, [boot_drive]
    jmp 0x0000:STAGE2_LOAD

; ---------------------------------------------------------------------------
print:
    push ax
    push bx
    push si
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bh, 0
    mov bl, 0x07
    int 0x10
    jmp .loop
.done:
    pop si
    pop bx
    pop ax
    ret

check_lba:
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .no
    cmp bx, 0xAA55
    jne .no
    test cl, 1
    jz .no
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax
    mov byte [lba_ok], 1
    ret
.no:
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax
    mov byte [lba_ok], 0
    ret

disk_fail:
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax
    mov si, msg_fail
    call print
.hang:
    cli
    hlt
    jmp .hang

; ---------------------------------------------------------------------------
; Dados
; ---------------------------------------------------------------------------
boot_drive db 0
lba_ok     db 0
cur_off    dw 0
cur_lba    dd 0

align 4
dap:
    db 16
    db 0
    dw 1
    dw 0
    dw STAGE2_LOAD >> 4
    dd 0
    dd 0

msg_start db "S1", 13, 10, 0
msg_fail  db "S1: disk error", 13, 10, 0

; ---------------------------------------------------------------------------
; Tabela de particoes MBR (64 bytes em 0x1BE).  A particao descreve os dados
; reais da imagem (stage2 + kernel + app); o SODOS nao a usa para montar C:.
; ---------------------------------------------------------------------------
times 446 - ($ - $$) db 0
partition_table:
    db 0x80
    db 0x00, 0x01, 0x00
    db 0x83
    db 0xFF, 0xFF, 0xFF
    dd 1
    dd PART_SECTORS

    times 16 db 0
    times 16 db 0
    times 16 db 0

times 510 - ($ - $$) db 0
dw 0xAA55
