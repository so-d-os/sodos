# NetSurf no SODOS

Port do navegador [NetSurf](https://www.netsurf-browser.org/) 3.11 (frontend
framebuffer) para o SODOS. Gera um `NETSURF.EXE` (SOX, gcc `-mx32 -msse2`) que
usa a `LIBCX.DLL` e a `POSIXX.DLL` e desenha numa janela do WIN.

## Fase 1 (estado atual)

- HTML + CSS (libcss, libdom, hubbub), layout completo do NetSurf
- URLs `file:` com unidade: `file:///D:/PAGINA.HTM`
- `about:`, `data:` e `resource:` (paginas internas embutidas no exe)
- teclado, clique, arrastar, roda do mouse, barra de enderecos, formularios
- fonte bitmap interna; decodificadores GIF e BMP ligados (ainda nao testados)
- sem rede (http/https e a fase 2), sem PNG/JPEG, sem JavaScript

## Como compilar

```sh
sodos/setup.sh   # baixa netsurf-all-3.11.tar.gz, extrai em tree/ e aplica o patch
sodos/build.sh   # compila tudo com sodos/sodos-cc
sodos/link.sh    # junta em sodos/obj/NETSURF.EXE com o linker do SODOS
```

Precisa de `c_compiler` ja compilado (`make -C ../../c_compiler`): usa o
`build/link`, o `build/x_crt0.o`, a `out/LIBCX.DLL`, a `out/POSIXX.DLL` e o
`out/KERNEL.IMP`. O `setup.sh` extrai o `gperf` de um `.deb` (sem root) se ele
nao estiver instalado.

## Rodar

No SODOS, copie `NETSURF.EXE`, `LIBCX.DLL` e `POSIXX.DLL` para a raiz de uma
unidade (os DLLs sao procurados na raiz) e abra pelo WIN, ou pelo Command:

```
D:\NETSURF.EXE file:///D:/T.HTM
```

Sem argumento abre a pagina de boas-vindas embutida.

No Linux, pelo `soxrun` (a janela vira um `.ppm` a cada quadro):

```sh
cd sodos/run && cp ../obj/NETSURF.EXE ../../../../c_compiler/out/*.DLL .
SOX_GUI=800x600 SOX_GUI_OUT=quadro SOX_GUI_EVENTS="wait:2000,wheel:5,wait:500,close" \
  ../../../../c_compiler/build/soxrun NETSURF.EXE file:///T.HTM
python3 topng.py quadro.ppm quadro.png
```

`SOX_GUI_EVENTS` aceita `wait:ms`, `click:x:y`, `drag:x:y`, `up`, `key:c`,
`key:ENTER` (e `TAB`, `BS`, `ESC`, setas...), `wheel:n[:x:y]` e `close`.

## Arquivos

| Arquivo | O que e |
|---|---|
| `netsurf-3.11-sodos.patch` | mudancas na arvore oficial (abaixo) |
| `sodos-cc` | gcc `-mx32 -msse2 -nostdinc` com os headers do SODOS |
| `include/` | headers POSIX que faltam no SODOS (iconv, regex, zlib, getopt...) |
| `shim/iconv.c` | iconv: UTF-8, ASCII, Latin-1/9, Windows-1252, UTF-16, UCS-4 |
| `shim/posix_extra.c` | scandir, mmap (copia), fstatat, getopt_long, inet_pton, stubs |
| `genres.py` | embute `default.css`, `Messages` etc. no exe (FAT 8.3) |

O `link.sh` tambem compila `lib/math.c`, `lib/fpconv.c` e `lib/printf.c` do
`c_compiler` com SSE2 e liga junto: a LIBCX e `-mno-sse` e nao tem `double`.

O patch:

- `libnsfb/src/surface/sodos.c`: superficie nova sobre `k_gui_pixels`,
  `k_gui_present` e `k_gui_event`. O cursor e o do kernel. Antes de apresentar
  ela le os eventos pendentes, porque `gui_present` redesenha a tela toda e
  teclas se perdiam. Ela tambem espera com timeout 0 + `k_ms`, porque aberto
  pela area de trabalho o programa e uma tarefa cooperativa e la o
  `k_gui_event` ignora o timeout.
- registro manual das superficies (`nsfb_sodos_register`): o linker do SODOS
  nao roda `.init_array`.
- sem SDL/X/VNC/Wayland, sem a ligacao expat da libdom e sem libsvgtiny.
- `utils/file.c`: `file:///D:/X` vira `D:/X` e vice-versa.
- `frontends/framebuffer`: recursos e `Messages` vem do exe, superficie `sodos`
  por padrao, cookies em `NSCOOKIE.TXT`.

## Pendencias conhecidas

- Aberto pela area de trabalho, digitacao muito rapida (rajadas < ~100 ms por
  tecla) perde teclas: o kernel redesenha a tela inteira a cada quadro da
  tarefa e nao le o PS/2 nesse meio tempo. Em velocidade normal funciona.
- Ctrl+letra chega como letra simples: sem selecionar tudo/copiar/colar.
- Teclas fora do ASCII (acentos do ABNT2) ainda nao viram caracteres.
