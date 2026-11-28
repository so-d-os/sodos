# O que está na raiz de D: e o que dá para mover

Levantamento de 28/11/2026: 271 itens na raiz, dos quais 39 são pastas e 232 são arquivos.

## Como o SODOS acha um arquivo

- **Programa ou DLL pelo nome** (`cc`, `LIBC.DLL`): o kernel procura primeiro na raiz da
  unidade corrente, depois em `D:\` e `C:\`, depois em cada pasta do `PATH` e por último
  no `PATHSODOS` ([module.c:190-238](../kernel/module.c)). Um arquivo que vai para `D:\BIN`
  continua sendo achado se `D:\BIN` estiver no PATH. Por outro lado, se sobrar uma cópia
  antiga na raiz, é ela que roda, porque a raiz vem antes do PATH.
- **Caminho fixo no código** (`"D:\\EXPLORER.EXE"`): o PATH não ajuda. O arquivo fica na
  raiz ou o código muda.
- **`make send` / `install/install.sh`**: o `programas.lst` grava na raiz os EXEs, DLLs,
  OBJs, headers, fontes e exemplos do `c_compiler`. Também gravam na raiz o `config.lst`
  (AGENT.CFG) e o `wsl.lst` (WSL.EXE). Se você mover esses arquivos e não mudar o `.lst`,
  o próximo `make send` grava tudo de novo na raiz.

## 1. Fica na raiz: o código usa o caminho fixo

| Arquivo | Quem usa |
|---|---|
| AUTOEXEC.BAT | o kernel, no boot ([kernel.c:2137](../kernel/kernel.c)), e o `limpa` |
| SODOS.CFG | gpu.c, top/video.c, telas_cfg.c (placa de vídeo) |
| KEYB.CFG | cmd_keyb.c |
| START.BAT | `run=D:\START.BAT` no `D:\SODOS\WIN.CFG` (dá para mover se mudar o WIN.CFG) |
| AGENT.CFG | agent.c e ai_client.c procuram na pasta atual e depois em `D:\`, `U:\` e `C:\` |
| GITCONF.TXT | git.c lê na raiz da unidade do GIT.EXE |
| CRASH.LOG | o kernel grava na raiz quando um programa cai. Pode apagar: ele grava de novo |
| EXPLORER.EXE | kernel, WINE (picker.c, proc.c) e EXPLORER.LNK |
| BROWSER.EXE | visual.c e BROWSER.LNK |
| VIDPLAY.EXE, IMAGEVW.EXE | visual.c (abrir arquivos no desktop); o IMAGEVW também está no LINKEXT.CFG |
| WEBCAMV.EXE | cmd_usb.c |
| WSL.EXE | vt.c procura `D:\WSL.EXE` e depois `D:\SODOS\WSL.EXE`. Poderia ir para `D:\SODOS` |

### Kit do compilador: fica junto na raiz

O `cc` procura em `D:` os headers, o CRT0.OBJ, o LIBC.DLL e o KERNEL.IMP
([cc/main.c:146](../c_compiler/cc/main.c)), e os programas são compilados com `-I /d/ -L /d/`.

- CC.EXE, LINK.EXE, CRT0.OBJ, KERNEL.IMP, LIBC.DLL
- As DLLs que os programas importam na compilação: GUI.DLL, GLIB.DLL, IMG.DLL, TLS.DLL,
  POSIX.DLL, POSIXX.DLL, LIBCX.DLL, TERM.DLL
- Todos os `.H`. Padrão: ASSERT, CTYPE, CURSES, DIRENT, DLFCN, ERRNO, FCNTL, FLOAT,
  INTTYPES, LANGINFO, LIBGEN, LIMITS, LOCALE, MALLOC, MATH, NL_TYPES, SETJMP, SIGNAL,
  STDARG, STDBOOL, STDDEF, STDINT, STDIO, STDLIB, STRING, TERMIOS, TIME, UCHAR, UCONTEXT,
  UNISTD, WCHAR, WCTYPE. Do SODOS: SO, SOX, CC, GUI, GLIB, IMG, TLS, X509, CRYPTO, P256-M,
  P384, ROOTS, MATHX, CALCOPS, AI_CLIENT, BROWSER, CACHE, VEC, WIDGETS. E a pasta `SYS\`.

Para tirar o kit da raiz, seria preciso mudar o `cc` (por exemplo, procurar numa variável
`SET CCDIR=D:\DEV`) e o `programas.lst`.

## 2. Cópias: pode apagar da raiz

São iguais byte a byte às de `D:\SODOS`, e o sistema usa as de `D:\SODOS` primeiro
(PATHSODOS e `desk_find`):

AIPANEL.EXE, DISKSYNC.EXE, FONTADV.EXE, IMGBIG.DLL, LINKEXT.EXE, MENUCFG.EXE, TEMAS.EXE,
VIDEOCFG.EXE, WINTAB.EXE

## 3. Pode ir para `D:\BIN` (com `D:\BIN` no PATH)

Estes são achados pelo nome:

AGENT.EXE, BAT.EXE, COBRA.EXE, COMMAND.EXE, CURL.EXE, FFLAUNCH.EXE, GIT.EXE, HTTPD.EXE
(o boot acha pelo PATH), JS.EXE, LLAMA.EXE, MAKE.EXE, MC.EXE, NETSURF.EXE, PENDRIVE.EXE
(o boot acha pelo PATH), PLAY.EXE, PONTE.EXE, TELAS.EXE, TOP.EXE, TROCA.EXE, WGET.EXE,
WIDGETS.EXE, WIFI.EXE, XCOPY.EXE

BATs: INSTCLAU.BAT, RODACLAU.BAT, RODACHRM.BAT, GETD.BAT

Cuidados:
- O GITCONF.TXT continua na raiz de `D:`, que é a unidade do GIT.EXE.
- Mude o AUTOEXEC.BAT antes de mover: `set PATH=D:\BIN;D:\;D:\SODOS`.
- `CLAUDE.EXE` (267 MB) é só o instalador do Claude Desktop. Pode ir para `D:\BIN` ou
  para um arquivo morto; nesse caso, mude a linha `D:\CLAUDE.EXE` do INSTCLAU.BAT.

## 4. Testes e exemplos: para `D:\SRC\EXEMPLOS` ou apagar

HELLO.C/.EXE/.OBJ/.JS, HELLO2.EXE, HELLO64.C, HELLO64C.C/.EXE, OLA.C/.EXE/.OBJ,
XHELLO.C/.EXE, CRASHT.C/.EXE, FAT32T.C/.EXE, SIGT64.C/.EXE, SMPT.C/.EXE, SOCKT.C/.EXE,
SPECIAL.CPP/.EXE/.OBJ, TGLIB.C/.EXE, TTERM.C/.EXE, HTTPS.C/.EXE, MC.OBJ, DEMO.BAT

Vários vêm de `c_compiler/examples` pelo `programas.lst`: mude o `.lst` também.

## 5. Fontes `.C`/`.CPP` (para recompilar no SODOS): para `D:\SRC`

BAT.C, BROWSER.C, CACHE.CPP, CALC.C, CALCOPS.C, COMMAND.C, CRT0.C, CRYPTO.C, CTORS64.C,
CTYPE.C, DIVDI.C, DYNLOAD.C, FPCONV.C, GEN.C, GUI.C, HTML.C, HTTP.C, HTTPD.C, IMAGEVW.C,
IMG.C, IMG_BIG.C, INFLATE.C, JS.C, LEX.C, LINK.C, MAIN.C, MATH.C, MATHX.C, MISC.C,
P256-M.C, P384.C, PARSE.C, POSIX.C, PP.C, PRINTF.C, ROOTS.C, SCANF.C, SEARCH.C, SETJMP.C,
SETJMP64.C, SIGFAULT64.C, STDIO.C, STDLIB.C, STRING.C, TIME.C, TLS.C, TYPE.C, URL.C,
USEMATH.C, UTIL.C, VEC.CPP, VMAIN.CPP, WCHAR.C, WGET.C, WIDGETS.C, X509.C, YOUTUBE.C,
reg.c, BRDOM.JS

Os `.H` desses fontes ficam na raiz (seção 1), porque são do include path. Mude também
os destinos `D:\` do `programas.lst` para `D:\SRC\`.

## 6. Backups `.OLD`: para `D:\BKP` ou apagar

BROWSER.OLD, EXPLORER.OLD, GIT.OLD, LIBC.OLD, CHROME.SAV.OLD (33 MB), WINEAPP.SAV.OLD (24 MB)

## 7. Perfis salvos: movem se o BAT mudar

- WINEAPP.SAV (24 MB): `set WINEAPPSAVE=D:\WINEAPP.SAV` no RODACLAU.BAT. É o login do Claude.
- CHROME.SAV (33 MB): `set WINEUDSAVE=D:\CHROME.SAV` no RODACHRM.BAT (o instchrm.c gera esse BAT).

## 8. Dados soltos: para `D:\USER\DEFAULT` ou `D:\TEMP`, ou apagar

- Imagens: ALFA.PNG, FOTO.JPG, GIFT.GIF, LOGO.PNG, PROG.JPG
- Páginas: IMG.HTM, JS.HTM, LONGA.HTM, T.HTM, TESTE.HTM
- Textos: LEIA.TXT, NSCOOKIE.TXT (cookies do NetSurf; confira se ele grava na pasta
  atual), TMPH.TXT, PWD.TXT, T4.TXT e T5.TXT (os dois últimos estão vazios)
- YUY2 (80 KB, sem extensão): parece um quadro cru da webcam. Confira antes de apagar.
- Arquivos do Claude Desktop, criados quando a pasta atual era `D:\`: ccd-ids.json,
  dump-organizations.json, plan-usage-history.json, queue-purge-owed,
  remote-control-state.json e window-state.json. Pode apagar; podem voltar.

## 9. Estranhos: não mexer sem olhar

Há dois arquivos de 270336 bytes com o nome ilegível (`0?` e `????0?`, de 25/11). Parecem
entradas corrompidas da FAT. `move`, `del` e o 9p podem falhar neles. Vale conferir o disco
antes.

## Pastas

- **Não mexer**: BOOT, SODOS (PATHSODOS), WINE (o Claude roda daí), WSL (a VM e o Bash),
  USER, FIREFOX, TEMP, LOG, SOURCE, SYS (headers), WINEAPP.BIG, NET.
- **Conferir uma por uma**: AGENT, BAT, BKP, BKP.001, CLAUDE, DEVELOP, FILE, FOLDER, LIB,
  PRINTS, "Pastas pessoais", REPO, ROOT, SRC, UTILS, VIDEO, VIDEOS, temp1, z.

## Ordem sugerida

1. AUTOEXEC.BAT: `set PATH=D:\BIN;D:\;D:\SODOS`. Reiniciar.
2. Apagar as cópias da seção 2.
3. Mover as seções 6 e 8 (só dados, ninguém procura por eles).
4. Criar `D:\BIN` e mover a seção 3. Testar `git`, `make`, `top` e `wifi` e reiniciar uma
   vez, por causa do HTTPD e do PENDRIVE.
5. Seções 4 e 5, junto com a mudança no `programas.lst`.
6. Mover com `move` no SODOS ou pelo EXPLORER. `mv` de pasta pelo 9p da VM falha.
