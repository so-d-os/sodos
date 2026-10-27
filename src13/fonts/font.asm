; Le um TTF e desenha uma frase.
; fontasm fonte.ttf "frase"|@arquivo pixels saida.raw
; Escreve NAME, FULL, VERSION, COPYRIGHT, TRADEMARK, LICENSE na saida padrao
; e um alfa de 8 bits (SOFONT) no arquivo.
bits 64
default rel

section .data
tag_name db 'name'
tag_head db 'head'
tag_hhea db 'hhea'
tag_maxp db 'maxp'
tag_cmap db 'cmap'
tag_glyf db 'glyf'
tag_loca db 'loca'
tag_hmtx db 'hmtx'
k_name db 'NAME',0
k_full db 'FULL',0
k_ver  db 'VERSION',0
k_copy db 'COPYRIGHT',0
k_tm   db 'TRADEMARK',0
k_lic  db 'LICENSE',0
hdr    db 'SOFONT',10
usage  db 'uso: fontasm fonte.ttf frase pixels saida.raw',10

section .bss
file    resq 1
flen    resd 1
cmap4   resq 1
glyf    resq 1
loca    resq 1
hmtx    resq 1
loca_long resd 1
upem    resd 1
ascent  resd 1
nhmtx   resd 1
nglyph  resd 1
ppem    resd 1
reqpx   resd 1
dy_base resd 1
pen_x   resd 1
pen_y   resd 1
img     resq 1
img_w   resd 1
img_h   resd 1
ne      resd 1
px      resd 2048
py      resd 2048
pon     resd 2048
gflags  resb 2048
ex0     resd 4096
ey0     resd 4096
ex1     resd 4096
ey1     resd 4096
xs      resd 512
textb   resb 512
lineb   resb 640
nbest   resb 512
ntmp    resb 512
statb   resb 160
tptr    resq 1
dbg     resd 1

section .text
global _start

ru16:
    movzx eax, byte [rdi]
    shl eax, 8
    movzx ecx, byte [rdi+1]
    or eax, ecx
    ret

ri16:
    call ru16
    movsx eax, ax
    ret

ru32:
    movzx eax, byte [rdi]
    shl eax, 24
    movzx ecx, byte [rdi+1]
    shl ecx, 16
    or eax, ecx
    movzx ecx, byte [rdi+2]
    shl ecx, 8
    or eax, ecx
    movzx ecx, byte [rdi+3]
    or eax, ecx
    ret

; rsi = tag (4 bytes). rax = ponteiro, rdx = tamanho. rax=0 se faltar.
table_find:
    push rbx
    push r12
    mov rbx, [file]
    cmp dword [flen], 12
    jb .no
    lea rdi, [rbx+4]
    call ru16
    mov ecx, eax
    lea r12, [rbx+12]
.lp:
    test ecx, ecx
    jz .no
    mov eax, [rsi]
    cmp eax, [r12]
    jne .nx
    lea rdi, [r12+8]
    call ru32
    mov r8d, eax
    lea rdi, [r12+12]
    call ru32
    mov edx, eax
    mov eax, r8d
    add rax, rdx
    cmp eax, [flen]
    ja .no
    mov rax, rbx
    add rax, r8
    pop r12
    pop rbx
    ret
.nx:
    add r12, 16
    dec ecx
    jmp .lp
.no:
    xor eax, eax
    xor edx, edx
    pop r12
    pop rbx
    ret

; edi = name id, rsi = chave. Imprime "CHAVE\ttexto\n".
name_line:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 104
    mov r15d, edi
    mov r14, rsi
    lea rsi, [tag_name]
    call table_find
    test rax, rax
    jz .empty
    mov rbx, rax
    mov r12d, edx
    cmp r12d, 6
    jb .empty
    lea rdi, [rbx+2]
    call ru16
    mov r13d, eax
    lea rdi, [rbx+4]
    call ru16
    mov [rbp-100], eax
    xor r8d, r8d
    mov byte [nbest], 0
.rec:
    cmp r8d, r13d
    jge .out
    mov eax, r8d
    imul eax, 12
    lea rdi, [rbx+6]
    add rdi, rax
    lea rax, [rdi+12]
    mov rcx, rbx
    add rcx, r12
    cmp rax, rcx
    ja .out
    push r8
    push rdi
    add rdi, 6
    call ru16
    pop rdi
    pop r8
    cmp eax, r15d
    jne .nr
    push r8
    mov r9, rdi
    lea rdi, [r9+8]
    call ru16
    mov r10d, eax
    lea rdi, [r9+10]
    call ru16
    mov esi, eax
    pop r8
    mov eax, [rbp-100]
    add eax, esi
    mov ecx, eax
    add ecx, r10d
    cmp ecx, r12d
    ja .nr
    mov ecx, eax
    mov rax, rbx
    add rax, rcx
    mov rdi, rax
    mov esi, r10d
    mov eax, r8d
    imul eax, 12
    lea r9, [rbx+6]
    add r9, rax
    push r8
    push rdi
    push rsi
    mov rdi, r9
    call ru16
    mov r11d, eax
    pop rsi
    pop rdi
    pop r8
    lea r10, [ntmp]
    xor ecx, ecx
    test esi, esi
    jz .got
    cmp r11d, 3
    je .u16
    test r11d, r11d
    jz .u16
.mac:
    cmp ecx, esi
    jge .got
    cmp ecx, 510
    jge .got
    movzx eax, byte [rdi+rcx]
    inc ecx
    cmp al, 10
    je .sp
    cmp al, 13
    je .sp
    cmp al, 9
    je .sp
    cmp al, 32
    jb .mac
    mov [r10], al
    inc r10
    jmp .mac
.sp:
    mov byte [r10], ' '
    inc r10
    jmp .mac
.u16:
    cmp ecx, esi
    jge .got
    cmp ecx, 510
    jge .got
    movzx eax, byte [rdi+rcx]
    shl eax, 8
    movzx edx, byte [rdi+rcx+1]
    or eax, edx
    add ecx, 2
    cmp eax, 10
    je .sp16
    cmp eax, 13
    je .sp16
    cmp eax, 9
    je .sp16
    cmp eax, 32
    jb .u16
    cmp eax, 255
    ja .u16
    mov [r10], al
    inc r10
    jmp .u16
.sp16:
    mov byte [r10], ' '
    inc r10
    jmp .u16
.got:
    mov byte [r10], 0
    mov rax, r10
    lea rdx, [ntmp]
    sub rax, rdx
    lea rcx, [nbest]
    xor edx, edx
.bl:
    cmp byte [rcx+rdx], 0
    je .bln
    inc edx
    cmp edx, 511
    jb .bl
.bln:
    cmp eax, edx
    jle .nr
    lea rsi, [ntmp]
    lea rdi, [nbest]
    mov ecx, eax
    inc ecx
    rep movsb
.nr:
    inc r8d
    jmp .rec
.empty:
    mov byte [nbest], 0
.out:
    lea rdi, [lineb]
    mov rsi, r14
.ck:
    mov al, [rsi]
    test al, al
    jz .ct
    mov [rdi], al
    inc rsi
    inc rdi
    jmp .ck
.ct:
    mov byte [rdi], 9
    inc rdi
    lea rsi, [nbest]
.cv:
    mov al, [rsi]
    test al, al
    jz .cn
    mov [rdi], al
    inc rsi
    inc rdi
    jmp .cv
.cn:
    mov byte [rdi], 10
    inc rdi
    lea rax, [lineb]
    sub rdi, rax
    mov rdx, rdi
    mov rax, 1
    mov rdi, 1
    lea rsi, [lineb]
    syscall
    add rsp, 104
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

load_font:
    push rbx
    push r12
    lea rsi, [tag_head]
    call table_find
    test rax, rax
    jz .bad
    cmp edx, 54
    jb .bad
    mov rbx, rax
    lea rdi, [rbx+18]
    call ru16
    cmp eax, 16
    jb .bad
    mov [upem], eax
    lea rdi, [rbx+50]
    call ri16
    mov [loca_long], eax
    lea rsi, [tag_hhea]
    call table_find
    test rax, rax
    jz .bad
    cmp edx, 36
    jb .bad
    mov rbx, rax
    lea rdi, [rbx+4]
    call ri16
    mov [ascent], eax
    lea rdi, [rbx+34]
    call ru16
    test eax, eax
    jz .bad
    mov [nhmtx], eax
    lea rsi, [tag_maxp]
    call table_find
    test rax, rax
    jz .bad
    lea rdi, [rax+4]
    call ru16
    mov [nglyph], eax
    lea rsi, [tag_glyf]
    call table_find
    test rax, rax
    jz .bad
    mov [glyf], rax
    lea rsi, [tag_loca]
    call table_find
    test rax, rax
    jz .bad
    mov [loca], rax
    lea rsi, [tag_hmtx]
    call table_find
    test rax, rax
    jz .bad
    mov [hmtx], rax
    lea rsi, [tag_cmap]
    call table_find
    test rax, rax
    jz .bad
    mov rbx, rax
    mov r12d, edx
    lea rdi, [rbx+2]
    call ru16
    mov ecx, eax
    xor r8d, r8d
.cm:
    cmp r8d, ecx
    jge .bad
    mov eax, r8d
    shl eax, 3
    lea rdi, [rbx+4]
    add rdi, rax
    add rdi, 4
    push rcx
    push r8
    call ru32
    pop r8
    pop rcx
    cmp eax, r12d
    jae .cmn
    lea rdi, [rbx]
    add rdi, rax
    push rcx
    push r8
    push rdi
    call ru16
    pop rdi
    pop r8
    pop rcx
    cmp eax, 4
    jne .cmn
    mov [cmap4], rdi
    xor eax, eax
    pop r12
    pop rbx
    ret
.cmn:
    inc r8d
    jmp .cm
.bad:
    mov eax, 1
    pop r12
    pop rbx
    ret

; edi = codepoint -> eax glyph
glyph_id:
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov dword [dbg], 222
    mov r15d, edi
    mov rbx, [cmap4]
    test rbx, rbx
    jz .z
    lea rdi, [rbx+6]
    call ru16
    mov r12d, eax
    mov r13d, eax
    shr r13d, 1
    mov [dbg], r13d
    lea r14, [rbx+14]
    lea rax, [r14+r12]
    add rax, 2
    mov r8, rax
    lea r9, [r8+r12]
    lea r10, [r9+r12]
    xor r11d, r11d
.lp:
    cmp r11d, r13d
    jge .z
    mov eax, r11d
    shl eax, 1
    lea rdi, [r14+rax]
    push r8
    push r9
    push r10
    push r11
    call ru16
    pop r11
    pop r10
    pop r9
    pop r8
    cmp r15d, eax
    ja .nx
    mov eax, r11d
    shl eax, 1
    lea rdi, [r8+rax]
    push r8
    push r9
    push r10
    push r11
    call ru16
    pop r11
    pop r9
    pop r10
    pop r8
    cmp r15d, eax
    jb .z
    mov r13d, eax
    mov eax, r11d
    shl eax, 1
    lea rdi, [r9+rax]
    push r8
    push r10
    push r11
    call ri16
    pop r11
    pop r10
    pop r8
    mov ebx, eax
    mov eax, r11d
    shl eax, 1
    lea rdi, [r10+rax]
    push r10
    push r11
    call ru16
    pop r11
    pop r10
    test eax, eax
    jnz .range
    mov eax, r15d
    add eax, ebx
    and eax, 0xFFFF
    jmp .done
.range:
    mov eax, r11d
    shl eax, 1
    lea rdi, [r10+rax]
    push r11
    call ru16
    pop r11
    mov ecx, r15d
    sub ecx, r13d
    shl ecx, 1
    mov edx, r11d
    shl edx, 1
    lea rdi, [r10+rdx]
    add rdi, rax
    add rdi, rcx
    mov rax, [file]
    mov edx, [flen]
    add rax, rdx
    lea rdx, [rdi+2]
    cmp rdx, rax
    ja .z
    call ru16
    test eax, eax
    jz .z
    add eax, ebx
    and eax, 0xFFFF
    jmp .done
.nx:
    inc r11d
    jmp .lp
.z:
    xor eax, eax
.done:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; edi = glyph -> eax offset
glyph_off:
    mov eax, [nglyph]
    cmp edi, eax
    ja .z
    mov rsi, [loca]
    cmp dword [loca_long], 0
    je .sh
    mov eax, edi
    shl eax, 2
    lea rdi, [rsi+rax]
    jmp ru32
.sh:
    mov eax, edi
    shl eax, 1
    lea rdi, [rsi+rax]
    call ru16
    shl eax, 1
    ret
.z:
    xor eax, eax
    ret

; eax unidades -> eax pixels
scale_s:
    movsxd rax, eax
    movsxd rcx, dword [ppem]
    imul rax, rcx
    cqo
    movsxd rcx, dword [upem]
    idiv rcx
    ret

advance_of:
    push rbx
    mov ebx, edi
    cmp ebx, [nhmtx]
    jl .in
    mov ebx, [nhmtx]
    dec ebx
.in:
    mov rsi, [hmtx]
    mov eax, ebx
    shl eax, 2
    lea rdi, [rsi+rax]
    call ru16
    call scale_s
    pop rbx
    ret

add_edge:
    cmp dword [ne], 4096
    jge .r
    cmp esi, ecx
    je .r
    mov eax, [ne]
    lea r8, [ex0]
    mov [r8+rax*4], edi
    lea r8, [ey0]
    mov [r8+rax*4], esi
    lea r8, [ex1]
    mov [r8+rax*4], edx
    lea r8, [ey1]
    mov [r8+rax*4], ecx
    inc dword [ne]
.r:
    ret

; edi,esi,edx,ecx,r8d,r9d = x0,y0,x1,y1,x2,y2
quad:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 104
    mov [rbp-100], edi
    mov [rbp-104], esi
    mov [rbp-108], edx
    mov [rbp-112], ecx
    mov [rbp-116], r8d
    mov [rbp-120], r9d
    mov r12d, edi
    mov r13d, esi
    mov ebx, 1
.lp:
    cmp ebx, 8
    jg .d
    mov ecx, 8
    sub ecx, ebx
    mov eax, ecx
    imul eax, ecx
    mov r14d, eax
    mov eax, ecx
    imul eax, ebx
    shl eax, 1
    mov r15d, eax
    mov eax, ebx
    imul eax, ebx
    mov [rbp-124], eax
    mov eax, r14d
    imul eax, [rbp-100]
    mov ecx, r15d
    imul ecx, [rbp-108]
    add eax, ecx
    mov ecx, [rbp-124]
    imul ecx, [rbp-116]
    add eax, ecx
    cdq
    mov ecx, 64
    idiv ecx
    mov [rbp-128], eax
    mov eax, r14d
    imul eax, [rbp-104]
    mov ecx, r15d
    imul ecx, [rbp-112]
    add eax, ecx
    mov ecx, [rbp-124]
    imul ecx, [rbp-120]
    add eax, ecx
    cdq
    mov ecx, 64
    idiv ecx
    mov edi, r12d
    mov esi, r13d
    mov edx, [rbp-128]
    mov ecx, eax
    mov r12d, edx
    mov r13d, eax
    call add_edge
    inc ebx
    jmp .lp
.d:
    add rsp, 104
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; edi = s, esi = e  (indices inclusivos)
flatten:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 104
    cmp esi, edi
    jl .d
    mov r12d, edi
    mov r13d, esi
    mov eax, esi
    sub eax, edi
    inc eax
    mov r14d, eax
    xor ebx, ebx
    mov dword [rbp-100], -1
.fon:
    cmp ebx, r14d
    jge .non
    mov eax, r12d
    add eax, ebx
    lea r8, [pon]
    cmp dword [r8+rax*4], 0
    jne .got
    inc ebx
    jmp .fon
.got:
    mov [rbp-100], ebx
.non:
    cmp dword [rbp-100], 0
    jl .d
    mov eax, r12d
    add eax, [rbp-100]
    lea r8, [px]
    mov r15d, [r8+rax*4]
    lea r8, [py]
    mov r11d, [r8+rax*4]
    mov [rbp-104], r15d
    mov [rbp-108], r11d
    mov [rbp-112], r15d
    mov [rbp-116], r11d
    mov dword [rbp-120], 0
    mov ebx, 1
.lp:
    cmp ebx, r14d
    jg .d
    mov eax, [rbp-100]
    add eax, ebx
    cmp eax, r14d
    jb .nm
    sub eax, r14d
.nm:
    add eax, r12d
    lea r8, [px]
    mov r15d, [r8+rax*4]
    lea r8, [py]
    mov r11d, [r8+rax*4]
    lea r8, [pon]
    mov r10d, [r8+rax*4]
    cmp ebx, r14d
    jne .use
    mov r15d, [rbp-104]
    mov r11d, [rbp-108]
    mov r10d, 1
.use:
    test r10d, r10d
    jz .off
    cmp dword [rbp-120], 0
    je .lin
    mov edi, [rbp-112]
    mov esi, [rbp-116]
    mov edx, [rbp-124]
    mov ecx, [rbp-128]
    mov r8d, r15d
    mov r9d, r11d
    mov [rbp-112], r15d
    mov [rbp-116], r11d
    mov dword [rbp-120], 0
    call quad
    jmp .ni
.lin:
    mov edi, [rbp-112]
    mov esi, [rbp-116]
    mov edx, r15d
    mov ecx, r11d
    mov [rbp-112], r15d
    mov [rbp-116], r11d
    mov dword [rbp-120], 0
    call add_edge
    jmp .ni
.off:
    cmp dword [rbp-120], 0
    je .first
    mov eax, [rbp-124]
    add eax, r15d
    sar eax, 1
    mov r8d, eax
    mov eax, [rbp-128]
    add eax, r11d
    sar eax, 1
    mov r9d, eax
    mov edi, [rbp-112]
    mov esi, [rbp-116]
    mov edx, [rbp-124]
    mov ecx, [rbp-128]
    mov [rbp-112], r8d
    mov [rbp-116], r9d
    mov [rbp-124], r15d
    mov [rbp-128], r11d
    call quad
    jmp .ni
.first:
    mov dword [rbp-120], 1
    mov [rbp-124], r15d
    mov [rbp-128], r11d
.ni:
    inc ebx
    jmp .lp
.d:
    add rsp, 104
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

scale_edges:
    push rbx
    xor ebx, ebx
.lp:
    cmp ebx, [ne]
    jge .d
    lea r8, [ex0]
    mov eax, [r8+rbx*4]
    call scale_s
    add eax, [pen_x]
    mov r12d, eax
    lea r8, [ey0]
    mov eax, [r8+rbx*4]
    call scale_s
    mov ecx, [dy_base]
    sub ecx, eax
    sub ecx, [pen_y]
    mov r13d, ecx
    lea r8, [ex1]
    mov eax, [r8+rbx*4]
    call scale_s
    add eax, [pen_x]
    mov r14d, eax
    lea r8, [ey1]
    mov eax, [r8+rbx*4]
    call scale_s
    mov ecx, [dy_base]
    sub ecx, eax
    sub ecx, [pen_y]
    shl r12d, 8
    shl r13d, 8
    shl r14d, 8
    shl ecx, 8
    lea r8, [ex0]
    mov [r8+rbx*4], r12d
    lea r8, [ey0]
    mov [r8+rbx*4], r13d
    lea r8, [ex1]
    mov [r8+rbx*4], r14d
    lea r8, [ey1]
    mov [r8+rbx*4], ecx
    inc ebx
    jmp .lp
.d:
    pop rbx
    ret

fill_glyph:
    push rbx
    push r12
    push r13
    push r14
    push r15
    xor r12d, r12d
.y:
    cmp r12d, [img_h]
    jge .d
    xor r13d, r13d
    mov eax, r12d
    shl eax, 8
    add eax, 128
    mov r14d, eax
    xor ebx, ebx
.e:
    cmp ebx, [ne]
    jge .sort
    lea r8, [ey0]
    mov esi, [r8+rbx*4]
    lea r8, [ey1]
    mov edi, [r8+rbx*4]
    cmp esi, edi
    je .en
    lea r8, [ex0]
    mov eax, [r8+rbx*4]
    lea r8, [ex1]
    mov ecx, [r8+rbx*4]
    cmp esi, edi
    jl .ord
    xchg esi, edi
    xchg eax, ecx
.ord:
    cmp r14d, esi
    jl .en
    cmp r14d, edi
    jge .en
    cmp r13d, 512
    jge .en
    mov edx, ecx
    sub edx, eax
    mov ecx, r14d
    sub ecx, esi
    movsxd r8, edx
    movsxd r9, ecx
    imul r8, r9
    mov edx, edi
    sub edx, esi
    movsxd r9, edx
    mov rax, r8
    cqo
    idiv r9
    add eax, [rsp]
    ; eax was x0 which I lost. Recalculate below.
    jmp .en
.en:
    inc ebx
    jmp .e
.sort:
    ; preenchimento refeito no segundo passe, abaixo, se nx ficou em r13
    ; O passe acima esta incompleto de proposito? Nao: reescrevo o miolo.
    xor r12d, r12d
    jmp .d
.d:
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    ret

; A rotina acima foi deixada como rascunho e substituida por fill2.
fill2:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 16
    xor r12d, r12d
.y:
    cmp r12d, [img_h]
    jge .done
    xor r13d, r13d
    mov eax, r12d
    shl eax, 8
    add eax, 128
    mov r14d, eax
    xor ebx, ebx
.e:
    cmp ebx, [ne]
    jge .sort
    lea r8, [ey0]
    mov esi, [r8+rbx*4]
    lea r8, [ey1]
    mov edx, [r8+rbx*4]
    lea r8, [ex0]
    mov eax, [r8+rbx*4]
    lea r8, [ex1]
    mov ecx, [r8+rbx*4]
    cmp esi, edx
    je .en
    cmp esi, edx
    jl .up
    xchg esi, edx
    xchg eax, ecx
.up:
    cmp r14d, esi
    jl .en
    cmp r14d, edx
    jge .en
    cmp r13d, 512
    jge .en
    mov r15d, eax
    mov eax, ecx
    sub eax, r15d
    mov ecx, r14d
    sub ecx, esi
    movsxd r8, eax
    movsxd r9, ecx
    imul r8, r9
    mov eax, edx
    sub eax, esi
    movsxd r9, eax
    mov rax, r8
    cqo
    idiv r9
    add eax, r15d
    lea r8, [xs]
    mov [r8+r13*4], eax
    inc r13d
.en:
    inc ebx
    jmp .e
.sort:
    mov ebx, 1
.si:
    cmp ebx, r13d
    jge .span
    lea r8, [xs]
    mov eax, [r8+rbx*4]
    mov r15d, ebx
.sj:
    test r15d, r15d
    jz .sp
    lea r8, [xs]
    mov ecx, [r8+r15*4-4]
    cmp ecx, eax
    jle .sp
    mov [r8+r15*4], ecx
    dec r15d
    jmp .sj
.sp:
    lea r8, [xs]
    mov [r8+r15*4], eax
    inc ebx
    jmp .si
.span:
    xor ebx, ebx
.sl:
    mov eax, ebx
    inc eax
    cmp eax, r13d
    jge .yn
    lea r8, [xs]
    mov eax, [r8+rbx*4]
    sar eax, 8
    mov r15d, eax
    lea eax, [rbx+1]
    mov eax, [r8+rax*4]
    sar eax, 8
    mov r11d, eax
    cmp r15d, 0
    jge .a0
    xor r15d, r15d
.a0:
    cmp r11d, [img_w]
    jle .b0
    mov r11d, [img_w]
.b0:
    mov ecx, r15d
.px:
    cmp ecx, r11d
    jge .sn
    movsxd r8, dword [img_w]
    movsxd r9, r12d
    imul r8, r9
    movsxd r9, ecx
    add r8, r9
    add r8, [img]
    mov byte [r8], 255
    inc ecx
    jmp .px
.sn:
    add ebx, 2
    jmp .sl
.yn:
    inc r12d
    jmp .y
.done:
    add rsp, 16
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; rdi = bytes do glifo, esi = tamanho. Desenha contornos simples.
emit_simple:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 104
    cmp esi, 10
    jl .z
    mov r12, rdi
    mov r13d, esi
    mov rdi, r12
    call ri16
    test eax, eax
    jle .z
    cmp eax, 64
    jg .z
    mov [rbp-100], eax
    mov eax, [rbp-100]
    dec eax
    shl eax, 1
    lea rdi, [r12+10]
    add rdi, rax
    push rdi
    call ru16
    pop rdi
    inc eax
    test eax, eax
    jle .z
    cmp eax, 2048
    jg .z
    mov [rbp-104], eax
    mov eax, [rbp-100]
    shl eax, 1
    lea r14, [r12+10]
    add r14, rax
    mov rdi, r14
    call ru16
    mov ecx, eax
    lea r14, [r14+2]
    add r14, rcx
    mov rax, r12
    add rax, r13
    cmp r14, rax
    ja .z
    xor ebx, ebx
    xor r15d, r15d
    mov dword [rbp-108], 0
.fl:
    cmp ebx, [rbp-104]
    jge .fx
    cmp dword [rbp-108], 0
    je .fr
    dec dword [rbp-108]
    lea r8, [gflags]
    mov al, [rbp-109]
    mov [r8+rbx], al
    inc ebx
    jmp .fl
.fr:
    mov rax, r12
    add rax, r13
    cmp r14, rax
    jae .z
    movzx ecx, byte [r14]
    inc r14
    lea r8, [gflags]
    mov [r8+rbx], cl
    test cl, 8
    jz .fn
    cmp r14, rax
    jae .z
    movzx edx, byte [r14]
    inc r14
    mov [rbp-108], edx
    mov [rbp-109], cl
.fn:
    inc ebx
    jmp .fl
.fx:
    xor ebx, ebx
    xor r15d, r15d
.xl:
    cmp ebx, [rbp-104]
    jge .yl
    lea r8, [gflags]
    movzx ecx, byte [r8+rbx]
    mov r11d, ecx
    mov rax, r12
    add rax, r13
    test cl, 2
    jz .xlng
    cmp r14, rax
    jae .z
    movzx eax, byte [r14]
    inc r14
    test cl, 16
    jnz .xp
    neg eax
.xp:
    add r15d, eax
    jmp .xs
.xlng:
    test cl, 16
    jnz .xs
    lea rdx, [r14+2]
    cmp rdx, rax
    ja .z
    mov rdi, r14
    call ri16
    add r14, 2
    add r15d, eax
.xs:
    lea r8, [px]
    mov [r8+rbx*4], r15d
    lea r8, [pon]
    mov eax, r11d
    and eax, 1
    mov [r8+rbx*4], eax
    inc ebx
    jmp .xl
.yl:
    xor ebx, ebx
    xor r15d, r15d
.ylp:
    cmp ebx, [rbp-104]
    jge .con
    lea r8, [gflags]
    movzx ecx, byte [r8+rbx]
    mov r11d, ecx
    mov rax, r12
    add rax, r13
    test cl, 4
    jz .ylng
    cmp r14, rax
    jae .z
    movzx eax, byte [r14]
    inc r14
    test cl, 32
    jnz .yp
    neg eax
.yp:
    add r15d, eax
    jmp .ys
.ylng:
    test cl, 32
    jnz .ys
    lea rdx, [r14+2]
    cmp rdx, rax
    ja .z
    mov rdi, r14
    call ri16
    add r14, 2
    add r15d, eax
.ys:
    lea r8, [py]
    mov [r8+rbx*4], r15d
    inc ebx
    jmp .ylp
.con:
    mov dword [ne], 0
    xor ebx, ebx
    xor r15d, r15d
.cl:
    cmp ebx, [rbp-100]
    jge .sc
    mov eax, ebx
    shl eax, 1
    lea rdi, [r12+10]
    add rdi, rax
    call ru16
    mov edi, r15d
    mov esi, eax
    mov r15d, eax
    inc r15d
    call flatten
    inc ebx
    jmp .cl
.sc:
    call scale_edges
    call fill2
    mov eax, 1
    jmp .out
.z:
    xor eax, eax
.out:
    add rsp, 104
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; edi = glyph, esi = depth
emit_glyph:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 120
    cmp esi, 4
    jg .ok
    mov ebx, edi
    mov r12d, esi
    mov edi, ebx
    call glyph_off
    mov r13d, eax
    mov edi, ebx
    inc edi
    call glyph_off
    cmp eax, r13d
    jbe .ok
    sub eax, r13d
    mov r14d, eax
    mov rax, [glyf]
    mov edx, [flen]
    add rdx, [file]
    mov rcx, rax
    add rcx, r13
    add rcx, r14
    cmp rcx, rdx
    ja .bad
    cmp r14d, 10
    jl .bad
    mov rdi, rax
    add rdi, r13
    mov rsi, rdi
    call ri16
    test eax, eax
    js .comp
    mov rdi, rsi
    mov esi, r14d
    call emit_simple
    jmp .out
.comp:
    mov [rbp-152], rsi
    lea r15, [rsi+10]
    mov ebx, 1
.cm:
    test ebx, ebx
    jz .ok
    mov rsi, [rbp-152]
    mov rax, rsi
    add rax, r14
    lea rdx, [r15+4]
    cmp rdx, rax
    ja .ok
    mov rdi, r15
    call ru16
    mov [rbp-100], eax
    lea rdi, [r15+2]
    call ru16
    mov [rbp-104], eax
    add r15, 4
    test dword [rbp-100], 1
    jz .by
    lea rdx, [r15+4]
    cmp rdx, rax
    ja .ok
    mov rdi, r15
    call ri16
    mov [rbp-108], eax
    lea rdi, [r15+2]
    call ri16
    mov [rbp-112], eax
    add r15, 4
    jmp .sc
.by:
    lea rdx, [r15+2]
    cmp rdx, rax
    ja .ok
    movsx eax, byte [r15]
    mov [rbp-108], eax
    movsx eax, byte [r15+1]
    mov [rbp-112], eax
    add r15, 2
.sc:
    mov eax, [rbp-100]
    test eax, 8
    jz .sc2
    add r15, 2
    jmp .xy
.sc2:
    test eax, 64
    jz .sc3
    add r15, 4
    jmp .xy
.sc3:
    test eax, 128
    jz .xy
    add r15, 8
.xy:
    test dword [rbp-100], 2
    jz .nore
    mov eax, [rbp-108]
    call scale_s
    mov ecx, eax
    mov eax, [rbp-112]
    mov [rbp-116], ecx
    call scale_s
    mov edx, eax
    mov ecx, [rbp-116]
    add [pen_x], ecx
    add [pen_y], edx
    mov edi, [rbp-104]
    mov esi, r12d
    inc esi
    push rcx
    push rdx
    call emit_glyph
    pop rdx
    pop rcx
    sub [pen_x], ecx
    sub [pen_y], edx
    jmp .more
.nore:
    mov edi, [rbp-104]
    mov esi, r12d
    inc esi
    call emit_glyph
.more:
    mov ebx, [rbp-100]
    and ebx, 32
    jmp .cm
.ok:
    mov eax, 1
    jmp .out
.bad:
    xor eax, eax
.out:
    add rsp, 120
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

die:
    mov rax, 60
    syscall

_start:
    mov rax, [rsp]
    cmp rax, 5
    je .args
    mov rax, 1
    mov rdi, 2
    lea rsi, [usage]
    mov rdx, 48
    syscall
    mov rdi, 1
    jmp die
.args:
    mov rbx, [rsp+16]
    mov r12, [rsp+24]
    mov r13, [rsp+32]
    mov r14, [rsp+40]
    mov rax, 2
    mov rdi, rbx
    xor esi, esi
    xor edx, edx
    syscall
    cmp rax, 0
    jl .e1
    mov r15, rax
    mov rax, 5
    mov rdi, r15
    lea rsi, [statb]
    syscall
    mov rsi, [statb+48]
    cmp rsi, 12
    jb .e1
    cmp rsi, 16*1024*1024
    ja .e1
    mov [flen], esi
    mov rax, 9
    xor edi, edi
    mov rdx, 1
    mov r10, 2
    mov r8, r15
    xor r9d, r9d
    syscall
    cmp rax, 0
    jl .e1
    mov [file], rax
    mov rax, 3
    mov rdi, r15
    syscall
    mov rax, [file]
    cmp dword [rax], 0x66637474
    jne .sf
    lea rdi, [rax+12]
    call ru32
    add [file], rax
    sub [flen], eax
.sf:
    mov edi, 1
    lea rsi, [k_name]
    call name_line
    mov edi, 4
    lea rsi, [k_full]
    call name_line
    mov edi, 5
    lea rsi, [k_ver]
    call name_line
    mov edi, 0
    lea rsi, [k_copy]
    call name_line
    mov edi, 7
    lea rsi, [k_tm]
    call name_line
    mov edi, 13
    lea rsi, [k_lic]
    call name_line
    call load_font
    test eax, eax
    jnz .e2
    mov rdi, r13
    xor eax, eax
.at:
    movzx ecx, byte [rdi]
    cmp cl, '0'
    jb .atd
    cmp cl, '9'
    ja .atd
    imul eax, eax, 10
    sub ecx, '0'
    add eax, ecx
    inc rdi
    jmp .at
.atd:
    cmp eax, 8
    jge .amin
    mov eax, 8
.amin:
    cmp eax, 96
    jle .amax
    mov eax, 96
.amax:
    mov [reqpx], eax
    shl eax, 1
    mov [ppem], eax
    movzx eax, byte [r12]
    cmp al, '@'
    jne .copy
    inc r12
    mov rax, 2
    mov rdi, r12
    xor esi, esi
    syscall
    cmp rax, 0
    jl .e1
    mov r15, rax
    mov rax, 0
    mov rdi, r15
    lea rsi, [textb]
    mov rdx, 511
    syscall
    mov r8, rax
    mov rax, 3
    mov rdi, r15
    syscall
    cmp r8, 0
    jle .emptyt
    mov byte [textb+r8], 0
.tr:
    cmp r8, 0
    jle .emptyt
    mov al, [textb+r8-1]
    cmp al, 10
    je .tr1
    cmp al, 13
    jne .tdone
.tr1:
    dec r8
    mov byte [textb+r8], 0
    jmp .tr
.emptyt:
    mov byte [textb], 0
    xor r8d, r8d
.tdone:
    movzx edi, byte [textb]
    add edi, 100
    mov rax, 60
    syscall
    lea r12, [textb]
    jmp .sz
.copy:
    lea rdi, [textb]
    mov ecx, 0
.cp:
    mov al, [r12+rcx]
    mov [rdi+rcx], al
    test al, al
    jz .cpf
    inc ecx
    cmp ecx, 500
    jb .cp
    mov byte [rdi+rcx], 0
.cpf:
    lea r12, [textb]
.sz:
    mov eax, [ppem]
    mov ecx, eax
    shr ecx, 1
    add eax, ecx
    add eax, 16
    mov [img_h], eax
    mov dword [img_w], 16
    xor ecx, ecx
.wl:
    cmp byte [r12+rcx], 0
    je .we
    mov eax, [ppem]
    add [img_w], eax
    inc ecx
    jmp .wl
.we:
    cmp dword [img_w], 64
    jge .w1
    mov dword [img_w], 64
.w1:
    cmp dword [img_w], 4096
    jle .w2
    mov dword [img_w], 4096
.w2:
    and dword [img_w], 0xFFFFFFFE
    and dword [img_h], 0xFFFFFFFE
    mov eax, [img_w]
    imul eax, [img_h]
    movsxd rsi, eax
    mov rax, 9
    xor edi, edi
    mov rdx, 3
    mov r10, 0x22
    mov r8, -1
    xor r9d, r9d
    syscall
    cmp rax, 0
    jl .e1
    mov [img], rax
    mov eax, [ascent]
    call scale_s
    add eax, 4
    mov [dy_base], eax
    mov dword [pen_x], 4
    mov dword [pen_y], 0
    mov [tptr], r12
.gl:
    mov rsi, [tptr]
    movzx eax, byte [rsi]
    test al, al
    jz .wr
    mov eax, [pen_x]
    mov ecx, [img_w]
    sub ecx, 4
    cmp eax, ecx
    jge .wr
    cmp al, 0x80
    jae .u2
    inc rsi
    mov [tptr], rsi
    mov edi, eax
    jmp .draw
.u2:
    movzx ecx, byte [rsi]
    movzx edx, byte [rsi+1]
    test edx, edx
    jz .badu
    and ecx, 0x1F
    shl ecx, 6
    and edx, 0x3F
    or ecx, edx
    add rsi, 2
    mov [tptr], rsi
    mov edi, ecx
    jmp .draw
.badu:
    inc rsi
    mov [tptr], rsi
    jmp .gl
.draw:
    cmp edi, 10
    je .gl
    push rdi
    call glyph_id
    mov ebx, eax
    pop rdi
    mov edi, [dbg]
    mov rax, 60
    syscall
    test ebx, ebx
    jz .adv
    push rdi
    mov edi, ebx
    xor esi, esi
    call emit_glyph
    pop rdi
.adv:
    push rdi
    mov edi, ebx
    call advance_of
    mov r11d, eax
    mov eax, [ppem]
    xor edx, edx
    mov ecx, 5
    div ecx
    cmp r11d, eax
    jge .adok
    mov eax, [rsp]
    cmp eax, ' '
    jne .adw
    mov eax, [ppem]
    xor edx, edx
    mov ecx, 3
    div ecx
    mov r11d, eax
    jmp .adok
.adw:
    mov r11d, [ppem]
    shr r11d, 1
.adok:
    add [pen_x], r11d
    pop rdi
    jmp .gl
.wr:
    mov eax, [img_w]
    shr eax, 1
    mov r12d, eax
    mov eax, [img_h]
    shr eax, 1
    mov r13d, eax
    mov eax, r12d
    imul eax, r13d
    movsxd rsi, eax
    mov r15, rsi
    mov rax, 9
    xor edi, edi
    mov rdx, 3
    mov r10, 0x22
    mov r8, -1
    xor r9d, r9d
    syscall
    cmp rax, 0
    jl .e1
    mov rbx, rax
    xor r8d, r8d
.dy:
    cmp r8d, r13d
    jge .wh
    xor r9d, r9d
.dx:
    cmp r9d, r12d
    jge .dny
    mov eax, r8d
    shl eax, 1
    imul eax, [img_w]
    mov ecx, r9d
    shl ecx, 1
    add eax, ecx
    movsxd rax, eax
    add rax, [img]
    movzx ecx, byte [rax]
    movzx edx, byte [rax+1]
    add ecx, edx
    mov edx, [img_w]
    movsxd rdx, edx
    movzx edx, byte [rax+rdx]
    add ecx, edx
    mov edx, [img_w]
    movsxd rdx, edx
    movzx edx, byte [rax+rdx+1]
    add ecx, edx
    shr ecx, 2
    mov eax, r8d
    imul eax, r12d
    add eax, r9d
    movsxd rax, eax
    mov [rbx+rax], cl
    inc r9d
    jmp .dx
.dny:
    inc r8d
    jmp .dy
.wh:
    mov rax, 2
    mov rdi, r14
    mov rsi, 577
    mov rdx, 420
    syscall
    cmp rax, 0
    jl .e1
    mov r14, rax
    lea rdi, [lineb]
    lea rsi, [hdr]
    mov ecx, 7
    rep movsb
    mov eax, r12d
    call itoa_at
    mov byte [rdi], ' '
    inc rdi
    mov eax, r13d
    call itoa_at
    mov byte [rdi], 10
    inc rdi
    lea rdx, [lineb]
    sub rdi, rdx
    mov rdx, rdi
    mov rax, 1
    mov rdi, r14
    lea rsi, [lineb]
    syscall
    mov rax, 1
    mov rdi, r14
    mov rsi, rbx
    mov rdx, r15
    syscall
    mov rax, 3
    mov rdi, r14
    syscall
    xor edi, edi
    jmp die
.e1:
    mov rdi, 1
    jmp die
.e2:
    mov rdi, 2
    jmp die

; eax = numero, rdi = destino. rdi avanca. Preserva rbx.
itoa_at:
    push rbx
    push r12
    sub rsp, 16
    mov r12, rdi
    mov ecx, 10
    lea rbx, [rsp+15]
    mov byte [rbx], 0
    test eax, eax
    jnz .d
    dec rbx
    mov byte [rbx], '0'
    jmp .c
.d:
    xor edx, edx
    div ecx
    add dl, '0'
    dec rbx
    mov [rbx], dl
    test eax, eax
    jnz .d
.c:
    mov al, [rbx]
    mov [r12], al
    inc rbx
    inc r12
    test al, al
    jnz .c
    mov rdi, r12
    dec rdi
    add rsp, 16
    pop r12
    pop rbx
    ret
