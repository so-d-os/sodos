; Explorer. Janela desenhada pelo WIN. Mouse chega como MSG_CLICK.
; asm explorer.asm  depois  win  e Iniciar, Explorer
; API: 0 titulo, 4 limpa, 8 item, 16 nome, 20 status, 24 seleciona, 32 unidade, 36 abre
; Comentario ao lado: tudo depois de ';' e ignorado, exceto dentro de aspas.
org 0x58000                          ; codigo fixo no slot 0x58000

start:                               ; entrada cdecl: msg, api, arg
  push ebp                           ; guarda o frame de quem chamou
  mov ebp, esp                       ; ebp passa a apontar os argumentos
  mov ebx, [ebp+12]                  ; ebx = tabela da API (preservado nas calls)
  mov eax, [ebp+8]                   ; eax = codigo da mensagem
  cmp eax, 1                         ; 1 = abertura da janela
  je do_init                         ; monta titulo, unidade e lista
  cmp eax, 2                         ; 2 = clique do mouse
  je do_click                        ; trata a linha clicada
  xor eax, eax                       ; mensagem desconhecida devolve 0
  pop ebp                            ; restaura o frame do chamador
  ret                                ; volta ao gerenciador de janelas

do_init:                             ; primeira chamada, ao abrir o Explorer
  mov eax, [ebx]                     ; eax = set_title, deslocamento 0
  push title                         ; argumento: texto da barra
  call eax                           ; pede ao WIN o titulo da janela
  add esp, 4                         ; retira o argumento da pilha
  mov eax, 67                        ; 67 e a letra C
  mov edx, drive                     ; edx = endereco da unidade corrente
  mov [edx], eax                     ; comeca listando C:
  call refresh                       ; limpa e preenche a lista
  xor eax, eax                       ; retorno 0
  pop ebp                            ; restaura ebp
  ret                                ; fim do init

do_click:                            ; arg em [ebp+16] e o indice da linha
  mov eax, [ebp+16]                  ; eax = linha clicada
  mov edx, click                     ; edx = onde guardar esse indice
  mov [edx], eax                     ; memoriza, ecx se perde nas calls
  cmp eax, 0                         ; linha 0 e [C:]
  je click_c                         ; troca para a unidade C
  cmp eax, 1                         ; linha 1 e [D:]
  je click_d                         ; troca para a unidade D
  mov edx, sel                       ; edx = selecao anterior
  mov ecx, [edx]                     ; ecx = indice ja marcado
  cmp ecx, eax                       ; segundo clique na mesma linha?
  je click_open                      ; sim: abre o arquivo
  mov [edx], eax                     ; nao: esta passa a ser a selecao
  mov eax, [ebx+24]                  ; eax = select
  mov edx, click                     ; relê o indice salvo
  mov ecx, [edx]                     ; ecx = linha a destacar
  push ecx                           ; argumento de select
  call eax                           ; pede o realce dessa linha
  add esp, 4                         ; limpa o argumento
  mov edx, click                     ; indice da linha de novo
  mov eax, [edx]                     ; eax = linha
  sub eax, 2                         ; tira [C:] e [D:] do indice
  mov edx, idx                       ; edx = indice do arquivo
  mov [edx], eax                     ; guarda o indice do nome
  mov edx, drive                     ; edx = unidade corrente
  mov ecx, [edx]                     ; ecx = letra (67 ou 68)
  push 32                            ; tamanho do buffer do nome
  push name                          ; destino do nome
  push eax                           ; indice do arquivo na unidade
  push ecx                           ; letra da unidade
  mov eax, [ebx+16]                  ; eax = fname
  call eax                           ; copia o nome para name
  add esp, 16                        ; quatro argumentos
  mov eax, [ebx+20]                  ; eax = status
  push name                          ; mostra o nome na barra de baixo
  call eax                           ; atualiza o status
  add esp, 4                         ; limpa o argumento
  xor eax, eax                       ; retorno 0
  pop ebp                            ; restaura ebp
  ret                                ; fim do primeiro clique

click_open:                          ; segundo clique no mesmo arquivo
  mov edx, sel                       ; edx = selecao
  mov eax, [edx]                     ; eax = linha selecionada
  sub eax, 2                         ; indice do arquivo, sem [C:] e [D:]
  mov edx, drive                     ; edx = unidade corrente
  mov ecx, [edx]                     ; ecx = letra
  push eax                           ; indice do arquivo
  push ecx                           ; letra da unidade
  mov eax, [ebx+36]                  ; eax = open_file
  call eax                           ; pede ao WIN abrir no bloco de notas
  add esp, 8                         ; dois argumentos
  xor eax, eax                       ; retorno 0
  pop ebp                            ; restaura ebp
  ret                                ; o WIN abre o notepad depois do retorno

click_c:                             ; clique em [C:]
  mov eax, 67                        ; letra C
  mov edx, drive                     ; endereco da unidade corrente
  mov [edx], eax                     ; passa a listar C:
  call refresh                       ; redesenha a lista
  xor eax, eax                       ; retorno 0
  pop ebp                            ; restaura ebp
  ret                                ; fim

click_d:                             ; clique em [D:]
  push 68                            ; pergunta se D (68) esta montada
  mov eax, [ebx+32]                  ; eax = drive_ok
  call eax                           ; 0 se nao montada
  add esp, 4                         ; limpa o argumento
  cmp eax, 0                         ; montada?
  je d_bad                           ; nao: avisa na barra de status
  mov eax, 68                        ; letra D
  mov edx, drive                     ; endereco da unidade corrente
  mov [edx], eax                     ; passa a listar D:
  call refresh                       ; redesenha a lista
  xor eax, eax                       ; retorno 0
  pop ebp                            ; restaura ebp
  ret                                ; fim

d_bad:                               ; D: nao esta montada
  mov eax, [ebx+20]                  ; eax = status
  push lit_nomount                   ; texto do aviso
  call eax                           ; mostra "D: nao montado"
  add esp, 4                         ; limpa o argumento
  xor eax, eax                       ; retorno 0
  pop ebp                            ; restaura ebp
  ret                                ; fica na unidade anterior

refresh:                             ; reconstroi as linhas da janela
  mov eax, [ebx+4]                   ; eax = clear
  call eax                           ; apaga a lista anterior
  mov eax, [ebx+8]                   ; eax = add
  push lit_c                         ; primeira linha: [C:]
  call eax                           ; acrescenta a linha
  add esp, 4                         ; limpa o argumento
  mov eax, [ebx+8]                   ; eax = add de novo
  push lit_d                         ; segunda linha: [D:]
  call eax                           ; acrescenta a linha
  add esp, 4                         ; limpa o argumento
  xor ecx, ecx                       ; primeiro arquivo e o indice 0
  mov edx, idx                       ; edx = contador de arquivos
  mov [edx], ecx                     ; zera o contador
nextf:                               ; um arquivo por volta
  mov edx, drive                     ; unidade que esta sendo listada
  mov eax, [edx]                     ; eax = letra
  mov edx, idx                       ; contador
  mov ecx, [edx]                     ; ecx = indice deste arquivo
  push 32                            ; tamanho do buffer
  push name                          ; onde fname grava o nome
  push ecx                           ; indice
  push eax                           ; letra
  mov eax, [ebx+16]                  ; eax = fname
  call eax                           ; 0 se existe, diferente de 0 se acabou
  add esp, 16                        ; quatro argumentos
  cmp eax, 0                         ; ainda ha arquivo?
  jne nomore                         ; nao: encerra a lista
  mov eax, [ebx+8]                   ; eax = add
  push name                          ; nome recem lido
  call eax                           ; poe o arquivo na lista
  add esp, 4                         ; limpa o argumento
  mov edx, idx                       ; contador
  mov ecx, [edx]                     ; ecx = indice atual
  inc ecx                            ; proximo arquivo
  mov [edx], ecx                     ; guarda o novo indice
  jmp nextf                          ; repete ate fname falhar
nomore:                              ; lista completa
  mov eax, [ebx+20]                  ; eax = status
  push lit_hint                      ; dica de uso
  call eax                           ; "clique de novo abre"
  add esp, 4                         ; limpa o argumento
  mov eax, -1                        ; nenhuma linha marcada
  mov edx, sel                       ; edx = selecao
  mov [edx], eax                     ; limpa a selecao interna
  mov eax, [ebx+24]                  ; eax = select
  push -1                            ; -1 tira o realce
  call eax                           ; nenhuma linha destacada
  add esp, 4                         ; limpa o argumento
  ret                                ; refresh e call interno, sem pop ebp

title:                               ; texto da barra da janela
  db "Explorer", 0                   ; titulo terminado em zero
lit_c:                               ; rotulo da unidade C
  db "[C:]", 0                       ; linha 0 da lista
lit_d:                               ; rotulo da unidade D
  db "[D:]", 0                       ; linha 1 da lista
lit_hint:                            ; dica depois de montar a lista
  db "clique de novo abre", 0        ; segundo clique abre o arquivo
lit_nomount:                         ; aviso quando D nao existe
  db "D: nao montado", 0             ; mostrado na barra de status
drive:                               ; unidade corrente
  dd 67                              ; 67 = C, 68 = D
sel:                                 ; linha do ultimo clique
  dd -1                              ; -1 = nada selecionado
click:                               ; indice do clique atual
  dd 0                               ; sobrevive as calls que estragam ecx
idx:                                 ; indice do arquivo na unidade
  dd 0                               ; 0 e o primeiro nome, sem contar [C:]
name:                                ; buffer do nome lido por fname
  dd 0, 0, 0, 0, 0, 0, 0, 0          ; 32 bytes, oito dwords zeradas
