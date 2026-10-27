; Explorer. Cada janela tem o seu estado em rdx (ebp aqui).
; Estado: click +0, drive +4, sel +8, name +12, idx +44.
; API em rsi, ponteiros de 8 bytes:
; 0 titulo, 8 limpa, 16 item, 32 nome, 40 status, 48 seleciona, 64 unidade, 72 abre
; Entrada SysV: edi = msg, esi = api, edx = estado.
org 0x58000

start:
  push ebp
  push ebx
  mov ebx, esi
  mov ebp, edx
  cmp edi, 1
  je do_init
  cmp edi, 2
  je do_click
  jmp done

do_init:
  mov eax, [ebx]
  mov edi, title
  call eax
  mov eax, 67
  mov [ebp+4], eax
  call refresh
  jmp done

do_click:
  mov eax, [ebp]
  cmp eax, 0
  je click_c
  cmp eax, 1
  je click_d
  mov ecx, [ebp+8]
  cmp ecx, eax
  je click_open
  mov [ebp+8], eax
  mov edi, eax
  mov eax, [ebx+48]
  call eax
  mov eax, [ebp]
  sub eax, 2
  mov [ebp+44], eax
  mov edi, [ebp+4]
  mov esi, [ebp+44]
  mov edx, ebp
  add edx, 12
  mov ecx, 32
  mov eax, [ebx+32]
  call eax
  mov edi, ebp
  add edi, 12
  mov eax, [ebx+40]
  call eax
  jmp done

click_open:
  mov eax, [ebp+8]
  sub eax, 2
  mov edi, [ebp+4]
  mov esi, eax
  mov eax, [ebx+72]
  call eax
  jmp done

click_c:
  mov eax, 67
  mov [ebp+4], eax
  call refresh
  jmp done

click_d:
  mov edi, 68
  mov eax, [ebx+64]
  call eax
  cmp eax, 0
  je d_bad
  mov eax, 68
  mov [ebp+4], eax
  call refresh
  jmp done

d_bad:
  mov edi, lit_nomount
  mov eax, [ebx+40]
  call eax
  jmp done

done:
  xor eax, eax
  pop ebx
  pop ebp
  ret

refresh:
  mov eax, [ebx+8]
  call eax
  mov edi, lit_c
  mov eax, [ebx+16]
  call eax
  mov edi, lit_d
  mov eax, [ebx+16]
  call eax
  xor eax, eax
  mov [ebp+44], eax
nextf:
  mov edi, [ebp+4]
  mov esi, [ebp+44]
  mov edx, ebp
  add edx, 12
  mov ecx, 32
  mov eax, [ebx+32]
  call eax
  cmp eax, 0
  jne nomore
  mov edi, ebp
  add edi, 12
  mov eax, [ebx+16]
  call eax
  mov eax, [ebp+44]
  inc eax
  mov [ebp+44], eax
  jmp nextf
nomore:
  mov edi, lit_hint
  mov eax, [ebx+40]
  call eax
  mov eax, -1
  mov [ebp+8], eax
  mov edi, -1
  mov eax, [ebx+48]
  call eax
  ret

title:
  db "Explorer", 0
lit_c:
  db "[C:]", 0
lit_d:
  db "[D:]", 0
lit_hint:
  db "de novo abre ou executa", 0
lit_nomount:
  db "D: nao montado", 0
