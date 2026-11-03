# NetSurf no SODOS

Port do navegador [NetSurf](https://www.netsurf-browser.org/) 3.11 (frontend
framebuffer) para o SODOS. Gera um `NETSURF.EXE` (SOX, gcc `-mx32 -msse2`) que
usa a `LIBCX.DLL` e a `POSIXX.DLL` e desenha numa janela do WIN.

## Estado atual

- HTML + CSS (libcss, libdom, hubbub), layout completo do NetSurf
- `http:` e `https:` (fase 2): fetcher proprio sobre `k_tcp_*` e o cliente
  TLS 1.3/1.2 do `c_compiler/tls`, com redirecionamento, cookies, `chunked`,
  `Content-Length` e POST de formulario (urlencoded)
- URLs `file:` com unidade: `file:///D:/PAGINA.HTM`
- `about:`, `data:` e `resource:` (paginas internas embutidas no exe)
- teclado, clique, arrastar, roda do mouse, barra de enderecos, formularios
- imagens: PNG (com transparencia) e JPEG (inclusive progressivo) pelo `img.c`
  da IMG.DLL ligado no exe; GIF, BMP e ICO pelas libs do NetSurf
- fonte bitmap interna; sem JavaScript

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
D:\NETSURF.EXE https://www.netsurf-browser.org/
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

## Log

O NetSurf grava `X:\LOG\NETSURF.LOG`, onde X e a unidade do exe (no pendrive,
`U:\LOG\NETSURF.LOG`), e cria a pasta se faltar. Cada execucao comeca com uma
linha `=== NetSurf iniciado ... ===`; depois vem cada pedido (`GET`, `HTTP 200
... (ms)`, `ok ...: bytes, ms`) e os erros (`ERRO url: motivo`, com o alerta
TLS ou a falha de DNS/TCP). O filtro e avisos de tudo mais o categoria `fetch`
em INFO. Acima de 256 KB o arquivo vira `NETSURF.OLD` e comeca outro.

Na LIBCX o `fflush` regrava o arquivo inteiro, entao avisos e erros vao para o
disco na hora e o resto a cada ~2 s e ao fechar. `NETSURF.EXE -v url` manda o
log para o console em vez do arquivo.

## Arquivos

| Arquivo | O que e |
|---|---|
| `netsurf-3.11-sodos.patch` | mudancas na arvore oficial (abaixo) |
| `sodos-cc` | gcc `-mx32 -msse2 -nostdinc` com os headers do SODOS |
| `include/` | headers POSIX que faltam no SODOS (iconv, regex, zlib, getopt...) |
| `shim/iconv.c` | iconv: UTF-8, ASCII, Latin-1/9, Windows-1252, UTF-16, UCS-4 |
| `shim/posix_extra.c` | scandir, mmap (copia), fstatat, getopt_long, inet_pton, stubs |
| `run/qmp.py` | controla o QEMU (teclas, mouse, screenshot) nos testes |
| `genres.py` | embute `default.css`, `Messages` etc. no exe (FAT 8.3) |

O `link.sh` tambem compila `lib/math.c`, `lib/fpconv.c` e `lib/printf.c` do
`c_compiler` com SSE2 e liga junto: a LIBCX e `-mno-sse` e nao tem `double`.
O `img.c` (`c_compiler/img`) entra com `-DIMG_MAXS=2048 -DIMG_MAX_IN=8192`:
imagens de ate 8192 px, reduzidas a 2048 px de lado (a DLL reduz a 800).
O cliente TLS (`c_compiler/tls/*.c`) entra no exe do mesmo jeito: o
`TLS.DLL` e do `cc`, e assim nao ha mistura de ABI.

O patch:

- `libnsfb/src/surface/sodos.c`: superficie nova sobre `k_gui_pixels`,
  `k_gui_present` e `k_gui_event`. O cursor e o do kernel. Antes de apresentar
  ela le os eventos pendentes, porque `gui_present` redesenha a tela toda e
  teclas se perdiam. Ela tambem espera com timeout 0 + `k_ms`, porque aberto
  pela area de trabalho o programa e uma tarefa cooperativa e la o
  `k_gui_event` ignora o timeout.
- `content/fetchers/sodos.c`: fetcher de `http` e `https`. O kernel tem uma
  conexao TCP por vez, entao os fetches entram numa fila e rodam um por vez,
  com `Connection: close`. Cada poll le por ate ~60 ms; quando ha tecla ou
  clique o `k_tcp_*` devolve -3 e a interface responde antes de continuar.
  Conexao, handshake ou envio interrompidos sao refeitos depois de 100 ms
  (no notebook o laco sem espera chegou a abrir 825 conexoes em 1,4 s), e a
  superficie esvazia a fila de eventos do kernel a cada leitura para que o
  fetch nao seja interrompido a toa.
- `content/handlers/image/sodos_img.c`: handler de `image/png` e `image/jpeg`.
  Decodifica quando o download termina e entrega o bitmap ao `image_cache`; o
  tamanho na pagina e o da imagem original mesmo quando ela chega reduzida.
- registro manual das superficies (`nsfb_sodos_register`): o linker do SODOS
  nao roda `.init_array`.
- sem SDL/X/VNC/Wayland, sem a ligacao expat da libdom e sem libsvgtiny.
- `utils/file.c`: `file:///D:/X` vira `D:/X` e vice-versa.
- `frontends/framebuffer`: recursos e `Messages` vem do exe, superficie `sodos`
  por padrao, cookies em `NSCOOKIE.TXT`.

## Pendencias conhecidas

- Paginas com muitos recursos demoram: um fetch por vez e uma conexao nova
  por recurso (o site do NetSurf leva ~17 s no QEMU). Keep-alive e o proximo
  passo. TLS 1.3 e 1.2 (este so com ECDHE e AES-128-GCM: servidor que so tem
  CBC ou AES-256 recusa com alerta 40), sem gzip e sem envio de arquivo
  (multipart).
- Sem WebP, SVG e PNG entrelacado (Adam7). O GIF fica alinhado pelo topo
  da linha em vez da base.
- Aberto pela area de trabalho, cliques muito curtos (~50 ms, so em teste
  automatico) perdem o soltar e o link nao abre; clique normal funciona.

- Aberto pela area de trabalho, digitacao muito rapida (rajadas < ~100 ms por
  tecla) perde teclas: o kernel redesenha a tela inteira a cada quadro da
  tarefa e nao le o PS/2 nesse meio tempo. Em velocidade normal funciona.
- Ctrl+letra chega como letra simples: sem selecionar tudo/copiar/colar.
- Teclas fora do ASCII (acentos do ABNT2) ainda nao viram caracteres.
