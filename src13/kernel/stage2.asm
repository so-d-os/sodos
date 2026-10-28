; stage2.asm - segundo estagio do carregador BIOS do SODOS.
;
; Responsabilidades:
;   1. habilitar A20 e obter o mapa E820;
;   2. validar as regioes de destino do kernel e da aplicacao;
;   3. ler o kernel para um bounce buffer baixo e copia-lo para KERNEL_LOAD
;      usando um pequeno modo protegido (acima de 1 MiB o BIOS nao alcanca);
;   4. carregar a aplicacao/initrd em APP_LOAD;
;   5. preencher BootInfo v1 e entrar em protected mode chamando kernel_entry.
;
; Contrato entregue ao kernel (ver kernel/bootinfo.h e BOOT.md):
;   EBX = BootInfo*, paging off, IF=0, A20 on, ESP=0x9FC00, DF=0.
;
; Layout fisico usado aqui (o Makefile passa os valores):
;   0x04FF0  parametros do stage1 (boot_drive, kernel_lba)
;   0x05000  BootInfo (128 bytes)
;   0x05200  mapa E820 (64 entradas de 24 bytes)
;   0x07C00  stage1 (512 bytes)
;   0x07E00  este stage2
;   0x20000  bounce buffer (32 setores = 16 KiB)
;   0x0FC00  topo da pilha real do stage2 (stage2 termina no maximo em 0xEE00)
;   0x9FC00  topo da pilha entregue ao kernel

org 0x7E00
bits 16

%ifndef KERNEL_LOAD
%define KERNEL_LOAD 0x01000000
%endif
%ifndef KERNEL_ENTRY
%error "KERNEL_ENTRY nao definido pelo Makefile"
%endif
%ifndef KERNEL_SECTORS
%error "KERNEL_SECTORS nao definido pelo Makefile"
%endif
%ifndef KERNEL_SIZE
%define KERNEL_SIZE (KERNEL_SECTORS * 512)
%endif
%ifndef KERNEL_RESERVED
%define KERNEL_RESERVED (KERNEL_SECTORS * 512)
%endif
%ifndef KERNEL_MAX_SIZE
%define KERNEL_MAX_SIZE 0x0E000000
%endif
%ifndef APP_LOAD
%define APP_LOAD 0x00058000
%endif
%ifndef APP_SECTORS
%define APP_SECTORS 0
%endif
%ifndef APP_SIZE
%define APP_SIZE (APP_SECTORS * 512)
%endif
%ifndef APP_MAX_SIZE
%define APP_MAX_SIZE 0x00008000
%endif
%ifndef BOOTINFO_ADDR
%define BOOTINFO_ADDR 0x00005000
%endif
%ifndef E820_ADDR
%define E820_ADDR 0x00005200
%endif
%ifndef BOOT_PARAM_ADDR
%define BOOT_PARAM_ADDR 0x00004FF0
%endif

%define BOOTINFO_MAGIC 0x53444F53
%define BOOTINFO_VERSION 1
%define BOOTINFO_SIZE 128
%define BOOTINFO_FIRMWARE_BIOS 1
%define E820_MAX 64
%define E820_ENTRY_SIZE 24
%define BOUNCE_ADDR 0x00020000
%define BOUNCE_SEG (BOUNCE_ADDR >> 4)
%define BOUNCE_SECTORS 32
%define REAL_STACK_TOP 0x0000FC00
%define KERNEL_STACK_TOP 0x0009FC00

%if KERNEL_LOAD < 0x00100000
%error "KERNEL_LOAD deve ficar acima de 1 MiB"
%endif
%if KERNEL_SECTORS < 1
%error "KERNEL_SECTORS deve ser positivo"
%endif
%if KERNEL_RESERVED > KERNEL_MAX_SIZE
%error "KERNEL_RESERVED ultrapassa KERNEL_MAX_SIZE"
%endif
%if (KERNEL_LOAD + KERNEL_RESERVED) > 0x10000000
%error "kernel nao cabe na identidade de 256 MiB"
%endif
%if (APP_LOAD + APP_SECTORS * 512) > 0x00060000
%error "aplicacao ultrapassa o slot de 32 KiB em 0x58000"
%endif
%if APP_SECTORS * 512 > APP_MAX_SIZE
%error "APP_SECTORS acima de APP_MAX_SIZE"
%endif
%if APP_SECTORS > 64
%error "APP_SECTORS acima do maximo do slot"
%endif

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, REAL_STACK_TOP
    sti
    cld

    mov [boot_drive], dl
    mov si, msg_start
    call print

    ; Stage1 deixa {boot_drive, kernel_lba} em BOOT_PARAM_ADDR.  Se este
    ; stage2 for iniciado por outro caminho, assume LBA 1.
    mov eax, [BOOT_PARAM_ADDR + 4]
    test eax, eax
    jnz .have_lba
    mov eax, 1
.have_lba:
    mov [kernel_lba], eax

    call enable_a20
    call check_lba_support

    ; Zera BootInfo (0x5000..0x507F).  O mapa E820 comeca em 0x5200.
    xor ax, ax
    mov es, ax
    mov di, BOOTINFO_ADDR
    mov cx, BOOTINFO_SIZE / 2
    rep stosw

    call collect_e820
    call validate_memory
    call load_kernel
    call load_app
    call build_bootinfo

    mov si, msg_loaded
    call print
    jmp enter_kernel

; ---------------------------------------------------------------------------
; A20
; ---------------------------------------------------------------------------
enable_a20:
    mov ax, 0x2401
    int 0x15

    in al, 0x92
    test al, 2
    jnz .done
    or al, 2
    and al, 0xFE
    out 0x92, al
.done:
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax
    ret

; ---------------------------------------------------------------------------
; Deteccao de LBA estendido (INT 13h AH=41h)
; ---------------------------------------------------------------------------
check_lba_support:
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc .no
    cmp bx, 0xAA55
    jne .no
    test cl, 1
    jz .no
    mov bl, 1
    jmp .store
.no:
    xor bl, bl
.store:
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax
    mov [lba_supported], bl
    ret

; ---------------------------------------------------------------------------
; E820
; ---------------------------------------------------------------------------
collect_e820:
    mov dword [e820_count], 0
    xor ebx, ebx
.next:
    mov di, E820_ADDR
    mov ax, [e820_count]
    mov cx, E820_ENTRY_SIZE
    mul cx
    add di, ax
    mov [e820_cur], di

    xor ax, ax
    mov es, ax
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, E820_ENTRY_SIZE
    int 0x15
    jc .fail
    cmp eax, 0x534D4150
    jne .fail
    cmp ecx, 20
    jb .fail
    ; O BIOS pode ter alterado DS/ES; restaura antes de tocar na memoria.
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax

    cmp ecx, E820_ENTRY_SIZE
    jae .attr_ok
    mov di, [e820_cur]
    mov dword [di + 20], 0
.attr_ok:
    inc dword [e820_count]
    cmp dword [e820_count], E820_MAX
    jb .more
    test ebx, ebx
    jnz .fail
    jmp .done
.more:
    test ebx, ebx
    jnz .next
.done:
    xor ax, ax
    mov ds, ax
    mov es, ax
    ret
.fail:
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax
    mov si, msg_e820
    jmp fatal

; ---------------------------------------------------------------------------
; Validacao de [EAX, EDX) contra as entradas E820 tipo 1.
; Retorna CF=0 se encontrou uma entrada que cobre a faixa.
; ---------------------------------------------------------------------------
range_usable:
    mov [chk_start], eax
    mov [chk_end], edx
    mov esi, E820_ADDR
    mov ecx, [e820_count]
    test ecx, ecx
    jz .no
.next:
    cmp dword [esi + 16], 1          ; type == usable
    jne .skip
    cmp dword [esi + 4], 0           ; base acima de 4 GiB: ignora
    jne .skip
    mov ebx, [esi + 8]               ; length baixo
    test ebx, ebx
    jz .skip
    mov eax, [esi]                   ; base baixa
    cmp eax, [chk_start]
    ja .skip
    add eax, ebx
    jnc .have_end
    mov eax, 0xFFFFFFFF
.have_end:
    cmp eax, [chk_end]
    jae .found
.skip:
    add esi, E820_ENTRY_SIZE
    dec ecx
    jnz .next
.no:
    stc
    ret
.found:
    clc
    ret

validate_memory:
    mov eax, KERNEL_LOAD
    mov edx, KERNEL_LOAD + KERNEL_RESERVED
    call range_usable
    jc .bad_kernel

%if APP_SECTORS > 0
    mov eax, APP_LOAD
    mov edx, APP_LOAD + APP_SECTORS * 512
    call range_usable
    jc .bad_app
%endif
    ret
.bad_kernel:
    mov si, msg_bad_kernel
    jmp fatal
.bad_app:
    mov si, msg_bad_app
    jmp fatal

; ---------------------------------------------------------------------------
; Leitura de setores: EAX=LBA, ECX=quantidade; destino BOUNCE_ADDR.
; Usa LBA estendido quando disponivel; senao CHS com 63 setores/16 cabecas.
; CF=1 em erro.
; ---------------------------------------------------------------------------
read_sectors:
    cmp byte [lba_supported], 0
    je .chs

    mov [dap + 2], cx
    mov word [dap + 4], 0
    mov word [dap + 6], BOUNCE_SEG
    mov [dap + 8], eax
    mov dword [dap + 12], 0
    mov si, dap
    mov dl, [boot_drive]
    mov ah, 0x42
    int 0x13
    jc .err
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax
    clc
    ret

.chs:
    mov [chs_lba], eax
    mov [chs_left], ecx
    mov word [chs_off], 0
.chs_loop:
    mov eax, [chs_lba]
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
    mov ax, BOUNCE_SEG
    mov es, ax
    mov bx, [chs_off]
    mov ax, 0x0201
    int 0x13
    jc .chs_err
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax
    add word [chs_off], 512
    inc dword [chs_lba]
    dec dword [chs_left]
    jnz .chs_loop
    xor ax, ax
    mov ds, ax
    mov es, ax
    clc
    ret
.chs_err:
    xor ax, ax
    mov ds, ax
    mov es, ax
    stc
    ret

.err:
    push ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop ax
    stc
    ret

; ---------------------------------------------------------------------------
; Copia BOUNCE -> high memory via protected mode.
; Entrada: copy_src, copy_dst, copy_len (multiplo de 4).
; ---------------------------------------------------------------------------
copy_chunk_pm:
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_copy
bits 32
pm_copy:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esi, [copy_src]
    mov edi, [copy_dst]
    mov ecx, [copy_len]
    shr ecx, 2
    cld
    rep movsd
    ; Troca para um descritor de codigo 16 bits ANTES de desligar PE.  Assim
    ; o far jump final para o seletor 0 e decodificado como 16 bits.
    jmp 0x18:pm_copy_16
bits 16
pm_copy_16:
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    jmp 0x0000:pm_copy_done
pm_copy_done:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    sti
    ret

; ---------------------------------------------------------------------------
; Carrega [cur_lba, dest, remaining] setores para dest.
; ---------------------------------------------------------------------------
load_range:
    mov ecx, [remaining]
    test ecx, ecx
    jz .done
.loop:
    mov eax, ecx
    cmp eax, BOUNCE_SECTORS
    jbe .chunk
    mov eax, BOUNCE_SECTORS
.chunk:
    mov [chunk_sectors], eax
    mov eax, [cur_lba]
    mov ecx, [chunk_sectors]
    call read_sectors
    jc .disk_error

    mov eax, [chunk_sectors]
    shl eax, 9
    mov dword [copy_src], BOUNCE_ADDR
    mov edx, [dest]
    mov [copy_dst], edx
    mov [copy_len], eax
    call copy_chunk_pm

    mov eax, [chunk_sectors]
    shl eax, 9
    add dword [dest], eax
    mov eax, [chunk_sectors]
    add dword [cur_lba], eax
    sub dword [remaining], eax
    mov ecx, [remaining]
    jnz .loop
.done:
    clc
    ret
.disk_error:
    stc
    ret

load_kernel:
    mov eax, [kernel_lba]
    mov [cur_lba], eax
    mov dword [dest], KERNEL_LOAD
    mov dword [remaining], KERNEL_SECTORS
    call load_range
    jc .fail
    mov si, msg_kernel
    call print
    ret
.fail:
    mov si, msg_disk
    jmp fatal

load_app:
%if APP_SECTORS > 0
    mov eax, [kernel_lba]
    add eax, KERNEL_SECTORS
    mov [cur_lba], eax
    mov dword [dest], APP_LOAD
    mov dword [remaining], APP_SECTORS
    call load_range
    jc .fail
    mov si, msg_app
    call print
%endif
    ret
.fail:
    mov si, msg_disk
    jmp fatal

; ---------------------------------------------------------------------------
; BootInfo v1 (offsets iguais a kernel/bootinfo.h)
; ---------------------------------------------------------------------------
build_bootinfo:
    mov dword [BOOTINFO_ADDR + 0], BOOTINFO_MAGIC
    mov dword [BOOTINFO_ADDR + 4], BOOTINFO_VERSION
    mov dword [BOOTINFO_ADDR + 8], BOOTINFO_SIZE
    mov dword [BOOTINFO_ADDR + 12], BOOTINFO_FIRMWARE_BIOS

    mov dword [BOOTINFO_ADDR + 16], KERNEL_LOAD
    mov dword [BOOTINFO_ADDR + 20], 0
    mov dword [BOOTINFO_ADDR + 24], KERNEL_SIZE
    mov dword [BOOTINFO_ADDR + 28], 0
    mov dword [BOOTINFO_ADDR + 32], KERNEL_ENTRY
    mov dword [BOOTINFO_ADDR + 36], 0

    mov dword [BOOTINFO_ADDR + 40], APP_LOAD
    mov dword [BOOTINFO_ADDR + 44], 0
    mov dword [BOOTINFO_ADDR + 48], APP_SIZE
    mov dword [BOOTINFO_ADDR + 52], 0

    ; framebuffer: 0 = nao fornecido; o kernel inicializa o video como antes.
    mov dword [BOOTINFO_ADDR + 56], 0
    mov dword [BOOTINFO_ADDR + 60], 0
    mov dword [BOOTINFO_ADDR + 64], 0
    mov dword [BOOTINFO_ADDR + 68], 0
    mov dword [BOOTINFO_ADDR + 72], 0
    mov dword [BOOTINFO_ADDR + 76], 0
    mov dword [BOOTINFO_ADDR + 80], 0

    mov eax, [e820_count]
    mov [BOOTINFO_ADDR + 84], eax
    mov dword [BOOTINFO_ADDR + 88], E820_MAX
    mov dword [BOOTINFO_ADDR + 92], 0
    mov dword [BOOTINFO_ADDR + 96], E820_ADDR
    mov dword [BOOTINFO_ADDR + 100], 0

    movzx eax, byte [boot_drive]
    mov [BOOTINFO_ADDR + 104], eax
    mov dword [BOOTINFO_ADDR + 108], 0
    ret

; ---------------------------------------------------------------------------
; Entrada final no kernel (32-bit protected mode, paging off, IF=0)
; ---------------------------------------------------------------------------
enter_kernel:
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp 0x08:pm_kernel
bits 32
pm_kernel:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, KERNEL_STACK_TOP
    and esp, 0xFFFFFFF0
    cld
    mov ebx, BOOTINFO_ADDR
    mov eax, BOOTINFO_MAGIC
    call KERNEL_ENTRY
.hang:
    cli
    hlt
    jmp .hang

bits 16

; ---------------------------------------------------------------------------
; Impressao BIOS/VGA e utilitarios
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

print_hex32:
    push ax
    push cx
    push dx
    push di
    mov di, hexbuf
    mov ecx, 8
.loop:
    rol eax, 4
    mov edx, eax
    and edx, 0x0F
    mov dl, [hexdigits + edx]
    mov [di], dl
    inc di
    loop .loop
    mov byte [di], 0
    mov si, hexbuf
    call print
    pop di
    pop dx
    pop cx
    pop ax
    ret

fatal:
    call print
    cli
.hang:
    hlt
    jmp .hang

; ---------------------------------------------------------------------------
; Dados
; ---------------------------------------------------------------------------
align 4
gdt:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
    dq 0x00009A000000FFFF
gdt_desc:
    dw gdt_desc - gdt - 1
    dd gdt

dap:
    db 16
    db 0
    dw 1
    dw 0
    dw BOUNCE_SEG
    dd 0
    dd 0

boot_drive      db 0
lba_supported   db 0
align 4
e820_count      dd 0
kernel_lba      dd 1
cur_lba         dd 0
remaining       dd 0
dest            dd 0
chunk_sectors   dd 0
copy_src        dd 0
copy_dst        dd 0
copy_len        dd 0
chs_lba         dd 0
chs_left        dd 0
chs_off         dw 0
e820_cur        dw 0
chk_start       dd 0
chk_end         dd 0
hexbuf          times 9 db 0
hexdigits       db "0123456789ABCDEF"

msg_start       db "S2", 13, 10, 0
msg_kernel      db "K", 0
msg_app         db "A", 0
msg_loaded      db 13, 10, "S2: kernel + app loaded", 13, 10, 0
msg_e820        db 13, 10, "S2: E820 failed", 13, 10, 0
msg_bad_kernel  db 13, 10, "S2: kernel memory not usable (E820)", 13, 10, 0
msg_bad_app     db 13, 10, "S2: app memory not usable (E820)", 13, 10, 0
msg_disk        db 13, 10, "S2: disk read error", 13, 10, 0
