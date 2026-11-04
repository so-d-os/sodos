# Tetris para o SODOS WIN

Jogo em C++ no subconjunto suportado pelo compilador nativo do SODOS.
Usa `GUI.DLL`, sem SDL, STL, Linux ou runtime C++ externo.

## Compilar no host

Na raiz do repositorio:

```sh
make -C src/testris
```

Gera `src/testris/TETRIS.EXE` em formato SOX (nome FAT 8.3).
Requer GCC e Make para construir as ferramentas do SDK.

## Instalar e abrir

Com a VM desligada e um `src13/disk.img` FAT ja preparado:

```sh
mcopy -o -i src13/disk.img src/testris/TETRIS.EXE ::TETRIS.EXE
```

Mantenha `GUI.DLL` e `LIBC.DLL` do mesmo SDK na raiz de `D:`
(o fluxo normal `make disk-files` em `src13` instala essas bibliotecas).
Inicie o SODOS, execute `win` e abra `D:\TETRIS.EXE` pelo Explorer.
Nao execute no shell de texto: a API canvas exige o WIN.

## Controles

- Esquerda/direita: mover.
- Cima: girar (com ajustes horizontais junto das paredes).
- Baixo: queda suave, 1 ponto por celula.
- Espaco: queda instantanea, 2 pontos por celula.
- P: pausar/continuar; R: reiniciar; Esc ou X da janela: sair.

Tabuleiro 10x20, sete pecas, previa da proxima peca e fim de jogo
quando uma nova peca nao cabe. Linhas valem 100/300/500/800 pontos,
multiplicados pelo nivel. A cada dez linhas a queda acelera.
A pontuacao nao e persistida. A rotacao e simples, nao implementa SRS.
O desenho se adapta ao tamanho da janela; aumente-a se necessario.
