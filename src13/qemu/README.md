# QEMU no SODOS

Port do QEMU (emulador de sistema, TCG) para rodar **dentro** do SODOS. Mesma
organização do [port do Firefox](../firefox/README.md): o código upstream não
entra no git; os scripts baixam QEMU, glib e zlib para `vendor/` e aplicam
`patches/`.

Alvo: `qemu-system-i386` com TCG, compilado pelo toolchain LP64 do SODOS
(`x86_64-sodos-gcc64`, imagem SOX2) do `firefox/toolchain`.

## Marcos

| Marco | Objetivo | Estado |
|---|---|---|
| **Q0** | baixar o QEMU, cross-build com meson, `configure` do `i386-softmmu` e lista do que falta | **feito (2026-11-09)** |
| **Q1** | portar glib, zlib e ucontext (`makecontext`/`swapcontext`); configure com as libs reais | **feito (2026-11-09)** |
| **Q2** | compilar e ligar o QEMU inteiro; `QEMU.EXE --version` no SODOS | **feito (2026-11-09)** |
| **Q3** | bootar um kernel multiboot mínimo pela serial (`-M isapc`) | **feito (2026-11-09)** |
| **Q4a** | `-serial stdio` no console do SODOS (saída e teclado); QEMU no `make disk-files` (`D:\UTILS\QEMU`) | **feito (2026-11-09)** |
| **Q4b** | bootar um sistema de verdade: FreeDOS 1.3 (disquete) com tela e teclado pela serial | **feito (2026-11-09)** |
| **Q4c** | tela de texto do guest no console do SODOS (`ui/sodos.c`): EDIT do FreeDOS | **feito (2026-11-09)** |
| **Q5** | QEMU numa janela do WIN (gráfico, teclado e mouse crus); Linux com X (Tiny Core, Xvesa) | **feito (2026-11-09)** |

## Q5: resultado

O QEMU abre como **janela do WIN** e roda um **Linux com X**: o Tiny Core 15
(kernel 6.6.8, BusyBox, servidor X do projeto X.Org — o Xvesa, sobre o VBE
da VGA — com FLWM e wbar) boota dentro do QEMU dentro do SODOS. Na VM de
teste, em que o próprio SODOS roda em TCG, o X sobe em ~11 minutos.

No WIN, abra o `D:\UTILS\QEMU\LINUX.BAT` (ou rode o `QEMU.EXE` pelo
Command): com o WIN aberto o `-display curses` vira janela. Fechar a janela
desliga o QEMU. O `DOS.BAT` também abre numa janela no WIN (e no console de
texto fora dele).

**Janela** (`sodos/ui/sodos.c`): a superfície da VGA (texto ou gráfico,
32 bpp) vai para a janela com `k_gui_pixels`, ampliada por um fator inteiro
quando cabe (720x400 do DOS vira 1440x800) ou reduzida para caber.

**Teclado e mouse crus** (kernel, `video/visual.c`): `k_gui_raw(1)` faz a
janela com foco receber `GUI_EV_SCAN` (cada byte do teclado PS/2: apertar,
soltar, E0, Ctrl/Alt/Shift) e `GUI_EV_MOVE` (todo movimento do rato, com os
botões). O QEMU repassa os scancodes ao i8042 do guest, um por vez
(`qemu_input_event_send_key_delay`; sem isso o DOS perdia o Alt de Alt+F), e
o movimento como deltas do rato PS/2. Ctrl+F1..F7 seguem com o kernel. A fila
de eventos por janela passou de 16 para 64, e movimentos seguidos se juntam.
Vale nos dois caminhos do WIN: programa como tarefa (Command) e direto.

**Linux** (`scripts/make-tinycore.sh`): baixa a ISO TinyCore 15.0 (25 MB,
confere o MD5 publicado) e tira `VMLINUZ` e `CORE.GZ` com o
`scripts/isoextract.py` (leitor ISO 9660 mínimo). O `LINUX.BAT` usa
`-kernel/-initrd` (pula o menu do isolinux), `-append "loglevel=3 cde
noapic"` e a ISO como CD (extensões do X em `cde/`). O `noapic` é preciso: a
máquina `isapc` anuncia um IOAPIC sem linhas e o kernel caía ao configurá-lo.
A VGA ISA tem o Bochs VBE com framebuffer linear, o bastante para o Xvesa.
Memória do guest 128 MB, `tb-size=64` (o padrão reservaria 1 GB para o JIT).

Limites: o mouse é relativo (PS/2), então o cursor do guest pode se afastar
do do WIN; o Xorg completo (`Xorg-7.7.tcz`) precisaria de rede no guest.
Velocidade: é emulação dentro de emulação. Com o `make run` em KVM (o WSL tem
`/dev/kvm`; falta o usuário no grupo `kvm`) tudo fica várias vezes mais rápido.

## Q4c: resultado

Programas de tela cheia rodam: o **FreeDOS Edit** abre, edita, salva e sai,
com a tela do guest no console de texto do SODOS (`D:\UTILS\QEMU\DOS.BAT`).

O backend `sodos/ui/sodos.c` (copiado para `vendor/.../ui/` pelo
`configure-qemu.sh`) se registra como o display `curses` (`-display curses`):
a VGA entrega a tela de texto em células e cada linha alterada vai para a
tela 80x25 do SODOS com `k_vput`, no formato da VGA; o cursor vai por
`k_cursor`. O `patches/0003` liga o arquivo no build (com a propriedade
`sodos` do cross file, sem a biblioteca curses) e aloca a superfície de vídeo
em memória anônima (o SODOS não tem `memfd` nem `mmap` compartilhado).

Teclado: cada tecla do `k_trygetc` vira aperta+solta no i8042 do guest
(layout US). O kernel não informa Ctrl/Alt, então:

| Tecla no SODOS | No guest |
|---|---|
| **F11** e depois uma tecla | Ctrl + tecla (F11 S = salvar no EDIT) |
| **F12** e depois uma tecla | Alt + tecla (F12 F = menu Arquivo, F12 X = sair) |
| **F12 F12** | desliga o QEMU gravando os discos |
| Ctrl+C | o kernel mata o QEMU na hora |

Discos: a posix-lite guarda a imagem em memória e só grava no `fsync`/`close`.
O backend pede `bdrv_flush_all()` a cada 2 s, então um Ctrl+C perde no máximo
os últimos 2 s de escrita; F12 F12 grava tudo. O disquete agora traz o
`EDIT.EXE` (pacote `edit.zip` do repositório FreeDOS 1.3, GPL; licença em
`A:\FREEDOS\DOC\EDIT.TXT`). A versão só serial (sercon, sem VGA) ficou em
`DOSSER.BAT`.

Limites: só modo texto (modos gráficos do guest não aparecem); a tela mostra
as primeiras 25 linhas e 80 colunas; caracteres fora do ASCII que o teclado
ABNT2 gera (ç, acentos) não são enviados.

## Q4b: resultado

O **FreeDOS 1.3** boota no QEMU dentro do SODOS e chega ao `A:\>` em menos
de 40 s na VM de teste; `dir`, `ver`, `mem`, `type`... respondem no console:

```
D:\UTILS\QEMU> dos
SeaBIOS (version rel-1.16.3-0-ga6ed6b701f0a-prebuilt.qemu.org)
Booting from Floppy...
FreeDOS kernel 2043 (build 2043 OEM:0xfd) [compiled May 13 2021]
FreeCom version 0.85a - WATCOMC - XMS_Swap [Jul 10 2021 19:28:06]
FreeDOS 1.3 no QEMU dentro do SODOS (tela e teclado pela serial).
A:\>mem
Conventional          639K       137K       502K
Extended (XMS)     15,360K    15,360K         0K
```

Sem backend de tela, a tela vai pela serial: o SeaBIOS tem o **sercon**, que,
com a porta da COM1 em `etc/sercon-port` (fw_cfg, `SERCON.BIN` = `0x3F8`), manda
a tela de texto do BIOS (int 10h) para a serial e lê o teclado de lá (int 16h).
Tudo que o DOS escreve pelo BIOS aparece; programas que escrevem direto na
memória de vídeo (B800h), não. O aviso `externally provided fw_cfg item names
should be prefixed with "opt/"` é esperado (o SeaBIOS lê esse nome exato).

O disquete (`scripts/make-freedos.sh`, 1,44 MB) sai do `x86BOOT.img` oficial da
FD13-FloppyEdition, sem o menu de idioma e sem o instalador; o
`make disk-files` grava `FREEDOS.IMG`, `SERCON.BIN` e `DOSSER.BAT` (no Q4b era o `DOS.BAT`) em
`D:\UTILS\QEMU`. `-drive` usa `file.driver=file` explícito: senão o QEMU lê
`D:\...` como o protocolo `D:`.

O que precisou mudar (quatro deles valem para todo programa LP64):

1. **Disquete** (`sodos/devices/i386-sodos.mak`): `CONFIG_FDC_ISA=y`. O PC só
   faz `imply`, e com `--without-default-devices` não entrava.
2. **vCPU em laço sem E/S** (`patches/0002`, `translator.c` e `cpu-exec.c`): o
   `yield()` do SeaBIOS espera a IRQ num laço só de saltos diretos, que rodava
   para sempre em TBs encadeados. Agora cada TB desconta 64 da parte baixa do
   `icount_decr` (sem uso fora do icount) e, a cada ~1000 TBs, sai para o
   `cpu_exec_loop`, que cede a vez.
3. **Mutex sem fila furada** (`sodos_sync.c`): o `unlock` passa o mutex direto
   ao primeiro da fila. Antes a vCPU soltava e pegava a BQL de novo antes do
   laço principal (acordado) rodar; o timer do disquete atrasava 10 s e o
   SeaBIOS desistia ("could not read the boot disk").
4. **`poll` cede a vez** passada a fatia de ~10 ms mesmo com fd pronto
   (`sodos_poll.c`): o `aio_poll` aninhado da leitura do disquete via sempre o
   pipe do notificador pronto e nunca deixava o worker do thread pool rodar.
   A fatia agora conta desde a última troca de thread (`sodos_thread.c`).
5. **Sono**: `clock_nanosleep` respeita `TIMER_ABSTIME` (antes dormia o
   uptime inteiro) e `nanosleep`/`usleep` LP64 dormem cooperativos em vez de
   espera ocupada, que prendia todas as threads.
6. **ANSI no console** (`posix.c`): em modo raw, o `write` em 1/2 tira as
   sequências `ESC [ ...` que o console do SODOS não interpreta;
   `ESC [ 2 J` limpa a tela.

## Q4a: resultado

O `make disk-files` (raiz do `src13`) agora grava o QEMU em `D:\UTILS\QEMU`
(`scripts/copy-qemu-disk.sh`; sem build do QEMU ele só avisa): `QEMU.EXE`, o
firmware (`bios.bin`, `vgabios*.bin`, `multiboot*.bin`, `linuxboot*.bin`...),
os guests de teste e três `.BAT` (`sodos/disk/`):

| No SODOS | O que faz |
|---|---|
| `D:\UTILS\QEMU\HELLO.BAT` | guest escreve pela serial no console e sai (código 33), ~4 s |
| `D:\UTILS\QEMU\ECO.BAT` | guest interativo: o que você digita volta em maiúsculas; `q` sai |
| `D:\UTILS\QEMU\SERIAL.BAT arq.elf` | boota um kernel multiboot com a serial no console |
| `D:\UTILS\QEMU\DOS.BAT` | FreeDOS 1.3 na tela de texto, com EDIT (Q4c) |
| `D:\UTILS\QEMU\DOSSER.BAT` | FreeDOS 1.3 só pela serial (Q4b) |
| `D:\UTILS\QEMU\LINUX.BAT` | Tiny Core Linux com X numa janela do WIN (Q5) |

Ctrl+C encerra o QEMU (regra do kernel para programas de console). Ainda não
há tela: só aparece o que o guest escreve na COM1.

```
D:\UTILS\QEMU> eco
Eco do guest i386 (QEMU dentro do SODOS).
Digite: o guest devolve em maiusculas. q sai.
> OLA SODOS
> XYZ
Tchau.
```

O que precisou mudar:

1. **Console como terminal** (`c_compiler/lib/misc.c`): `tcgetattr`/`tcsetattr`
   nos fds 0–2 funcionam (antes falhavam). Sem `ICANON` no fd 0, o `read(0)`
   da posix-lite lê tecla a tecla pelo `k_trygetc`, sem esperar a linha, e
   converte Enter em CR, Backspace em DEL e setas em `ESC [ A..D`.
2. **`poll` com o console** (`firefox/sodos/port/sodos_poll.c`): fd 0 dá
   `POLLIN` quando há tecla (em modo raw) e fds 1/2 dão `POLLOUT`. Antes eram
   `POLLNVAL`, e o QEMU caía num `read(0)` que bloqueava no kernel.
3. **vCPU cooperativa** (`patches/0002-sodos-coop-yield.patch` e
   `sodos_coop_due`/`sodos_coop_yield` na libsodos): o kernel não preempta, e
   um guest em laço de espera (`in` na serial) roda em TBs encadeados que
   nunca voltam ao `cpu_exec_loop`. Agora, a cada ~10 ms, o acesso a
   dispositivo (`memory_region_dispatch_*`) e o `lookup_tb_ptr` dos saltos
   indiretos pedem a saída do TB, e o `cpu_exec_loop` (fora da BQL) cede a
   vez ao laço principal, que lê o teclado e roda os timers.

## Q3: resultado

Um guest i386 boota no QEMU **dentro do SODOS** (`scripts/run-guest.sh`):

```
D:\QEMU.EXE -L D:\QEMU -M isapc -m 16 -accel tcg,tb-size=16 -kernel D:\HELLO.ELF
            -serial file:D:\SAIDA.TXT -device isa-debug-exit,iobase=0xf4,iosize=4
            -display none -vga none -monitor none -nic none
== D:\SAIDA.TXT (3 s)
Ola! Guest i386 rodando no QEMU dentro do SODOS.
multiboot ok, serial 16550 ok, laco de 1e6 iteracoes ok.
[mod] saiu QEMU.EXE rc=00000021
```

SeaBIOS (`D:\QEMU\bios.bin`) inicializa a máquina `isapc`, a option ROM
`multiboot_dma.bin` carrega o `HELLO.ELF` (`sodos/guest/hello.S`), o TCG
executa um laço de 10⁶ iterações, a saída passa pela serial 16550 e o guest
desliga o QEMU pelo `isa-debug-exit` (código 33 = `0x21`). Cada execução
leva 3–4 s, contando o boot do SeaBIOS em emulação aninhada.

Os dispositivos vêm de `sodos/devices/i386-sodos.mak` (`isapc`, que puxa
i8259, PIT, RTC, teclado, serial e IDE ISA, mais `isa-debug-exit` e VGA ISA),
passado com `--with-devices-i386=sodos`.

Bugs da base LP64 encontrados e corrigidos no caminho:

1. **futex**: `QemuEvent` usa `syscall(SYS_futex)`, que era `ENOSYS`
   (`abort()` em `qemu_event_wait`). Agora o `syscall()` LP64
   (`c_compiler/lib/misc.c`) implementa `FUTEX_WAIT`/`FUTEX_WAKE` sobre as
   threads cooperativas: a espera cede a vez até o valor mudar (com timeout);
   o wake não precisa fazer nada.
2. **`mmap` de arquivo**: o `load_elf` usa `GMappedFile`. O `mmap` da libsodos
   agora atende arquivo para leitura e `MAP_PRIVATE` copiando o conteúdo
   (`pread`) para uma região anônima; `MAP_SHARED` gravável continua
   `ENOSYS`.
3. **`setjmp`/`longjmp`** (`c_compiler/lib/setjmp64.c`): eram uma função C em
   volta de `__builtin_setjmp`, que salva o quadro do próprio `setjmp` (morto
   no retorno), e o `longjmp` sempre devolvia 1. As corrotinas do QEMU
   pulavam para lixo (`#GP` em `coroutine_trampoline`). Agora são assembly e
   devolvem o valor pedido (`COTEST` confere 3, 2 e 1).
4. **escalonador** (`firefox/sodos/port/sodos_thread.c`): `resched()` só
   acordava as threads dormentes quando a fila de prontas estava vazia. Uma
   thread que só cede a vez (a do RCU, esperando o futex) se reencontrava
   sempre na fila e a thread principal, dormindo no `poll`, nunca acordava.
   Agora a fila de sono é processada a cada troca.

Ferramentas de depuração novas: o `abort()` LP64 imprime na serial quem o
chamou; `vm.sh mon '<comando>'` fala com o monitor do QEMU de fora (`info
registers`, `x/...`); `scripts/vm-stack.sh` lista as palavras da pilha da
thread atual que caem dentro do `QEMU.EXE` (um backtrace heurístico, com o
ELF guardado por `SODOS_KEEP_ELF=1`).

No Q3, `-serial stdio` ainda travava; resolvido no Q4a.

## Q2: resultado

`qemu-system-i386` compila e liga inteiro (SOX2 de 10 MB, `tls=512`) e roda
no SODOS:

```
D:\QEMU.EXE --version
QEMU emulator version 9.2.0
Copyright (c) 2003-2024 Fabrice Bellard and the QEMU Project developers
[mod] saiu QEMU.EXE rc=00000000
```

`-M help` e `-device help` também rodam (o QOM inicializa e lista as CPUs
x86). Só aparece a máquina `none`, porque o configure usa
`--without-default-devices`.

O que foi preciso:

- **Headers UAPI do Linux** (`scripts/fetch-uapi.sh`): `linux-libc-dev` em
  `sodos/sysroot/include-uapi`, com `-idirafter`. Os headers do SODOS têm
  prioridade e a UAPI só preenche o que falta (`linux/types.h`, `linux/fs.h`,
  `linux/vfio.h`...). `linux/netfilter*` fica de fora (nomes que só diferem
  na caixa não cabem no NTFS).
- **headers do SODOS**: termios completo (valores do Linux); `SIGIO`, `SI_*`;
  `u_char`/`u_short`; `st_rdev` e `S_IFCHR`...; `IOV_MAX`, `SOMAXCONN`,
  `struct ucred`; `_IO*`/`TIOCM*` em `sys/ioctl.h`; `CLONE_*`,
  `PR_SET_PDEATHSIG`, `TMPFS_MAGIC`, `POSIX_FADV_*`, `IN_MULTICAST`;
  `pthread_cleanup_push/pop`; `features.h` e `glob.h` mínimos.
- **funções novas** (`firefox/sodos/port/sodos_posix_extra.c`):
  `cfset*speed`/`cfmakeraw`/`tcflush`..., `getopt`, `flockfile`, `fstatfs`,
  `posix_fadvise`, `inet_aton`, `glob`, `fmaf` e stubs `ENOSYS` (`openpty`,
  `ptsname`, `daemon`, `clone`, `mincore`...).

O `struct stat` LP64 ganhou `st_rdev` no fim: tudo que chama `stat()` precisa
ser recompilado com o header novo (glib e zlib foram; os testes passam de
novo).

## Q1: resultado

As dependências rodam **dentro do SODOS** (VM de teste, `scripts/test.sh`):

```
== D:\ZTEST.EXE     compress2/uncompress/crc32                       ZTEST: ok (zlib 1.3.1)
== D:\CTXTEST.EXE   makecontext (8 args), swapcontext, siglongjmp
                    para fora da corrotina, uc_link                  CTXTEST: ok
== D:\GTEST.EXE     GHashTable, GString/printf 64 bits, printf posicional,
                    UTF-8 + g_convert (iconv), GRegex (pcre2), 2 GThreads + GMutex,
                    GMainLoop com idle + 3 timeouts de 50 ms (151 ms) GTEST: ok (glib 2.76.6)
```

- **zlib 1.3.1** (`scripts/build-zlib.sh`): compilada direto, sem configure.
- **glib 2.76.6** (`scripts/build-glib.sh`): meson cruzado com
  `sodos/meson-cross.ini` + `sodos/cross-sodos.ini`. Instala `libglib-2.0.a`,
  `libgmodule-2.0.a`, `libgnulib.a`, `libcharset.a`, `libpcre2-8.a`
  (subprojeto) e `libintl.a` (proxy) em `sodos/sysroot`. O patch
  `patches/glib-2.76.6/0001-sodos.patch` desliga o futex (o `syscall()` do
  SODOS é `ENOSYS`; o glib usa mutex/cond do pthread) e não configura gobject
  (libffi) nem gio (resolver DNS). O printf do glib é o do gnulib embutido (o
  da libc não tem `%n$`).
- **ucontext** em `firefox/sodos/port/sodos_ucontext.c` (LP64): troca
  callee-saved, rsp/rip, registradores de argumento, MXCSR e x87 CW. O QEMU
  configura com `coroutine backend: ucontext`.
- **iconv mínimo** em `firefox/sodos/port/sodos_iconv.c`: UTF-8, ASCII,
  Latin-1, UTF-16 e UTF-32.

Correções na base LP64 encontradas no caminho:

- `c_compiler/include/limits.h`: `LONG_MAX`/`ULONG_MAX` eram de 32 bits também
  no LP64 (o glib quebrava em `G_MAXSIZE == SIZE_MAX`). Agora vêm de
  `__LONG_MAX__`; `SSIZE_MAX` novo.
- headers `sys/poll.h`, `utime.h`, `iconv.h`; `rewinddir` (`libs/posix-lite`),
  `getpwnam_r`; protótipos de `fallocate` e `gethostname`.

Limite conhecido: `off_t` é 32 bits em toda a libc (FAT16, arquivos < 2 GB).
Basta para imagens de guest pequenas.

## Q0: resultado

Com `--without-default-features --without-default-devices`, as dependências
**obrigatórias** do QEMU 9.2.0 são só **glib-2.0 ≥ 2.66** e **zlib**.
`scripts/probe-deps.sh` acha isso dando um `.pc` falso para cada dependência
que o meson recusa, até o configure passar. O `host_os` é `linux`: o driver
mantém `__linux__` ao compilar (como no Firefox) e a libsodos imita a ABI
Linux. O backend do TCG é o nativo x86-64, como o JIT do SpiderMonkey.

## Próximos passos

1. Mouse absoluto (usb-tablet ou vmmouse) para o cursor do guest seguir o do
   WIN.
2. Rede no guest (slirp) para o Tiny Core baixar extensões, como o Xorg 7.7.
3. KVM no QEMU de fora (`make run`) para a emulação aninhada ficar usável.

## Riscos conhecidos

- **Threads cooperativas** (o M1b, preempção, segue pendente): funcionam para
  o FreeDOS (vCPU, RCU, worker de E/S e laço principal se revezam). A vCPU
  cede a vez a cada ~10 ms: em E/S, em salto indireto e a cada ~1000 TBs.
  O Firefox usa a mesma libsodos (mutex FIFO, `poll` e sono novos) e não foi
  retestado nesta etapa.
- **Memória**: o Linux usa 128 MB de guest mais ~100 MB do QEMU (JIT de 64 MB).
- **Velocidade**: o SODOS costuma rodar no QEMU, então é emulação aninhada.

## Como rodar (WSL)

```sh
cd src13/qemu
sh setup.sh                    # QEMU 9.2.0 + ninja/pkgconf/venv locais (sem root)
sh scripts/build-zlib.sh       # zlib -> sodos/sysroot
sh scripts/build-glib.sh       # glib -> sodos/sysroot
sh scripts/fetch-uapi.sh       # headers UAPI do Linux -> sodos/sysroot/include-uapi
sh scripts/test.sh             # ZTEST/CTXTEST/GTEST na VM de teste
sh scripts/configure-qemu.sh
sh scripts/build-qemu.sh       # alvo padrão: qemu-system-i386
sh scripts/vm.sh put ~/sodos-build/qemu-9.2.0/qemu-system-i386 QEMU.EXE
sh scripts/vm.sh start
sh scripts/vm.sh exec 'D:\QEMU.EXE --version'
sh scripts/run-guest.sh        # Q3: guest multiboot no QEMU dentro do SODOS
sh scripts/copy-qemu-disk.sh ~/sodos-build/vm/disk.img   # Q4a: D:\UTILS\QEMU na VM
sh scripts/vm.sh stop
```

`scripts/vm.sh` sobe uma **segunda** instância do SODOS (cópias dos discos em
`~/sodos-build/vm`, porta 8798, sem janela) e grava binários no D: dela com
`mcopy`. Assim os testes não exigem fechar o QEMU de uso diário (porta 8797).

Os builds ficam em `~/sodos-build/` (`qemu-9.2.0`, `glib-2.76.6`,
`zlib-1.3.1`), no ext4 do WSL: no `/mnt/c` o meson acusa *clock skew* e o
ninja é lento. Mude com `SODOS_BUILD_ROOT` ou `QEMU_BUILD_DIR`. Os logs ficam
em `build/`.

## Estrutura

| Caminho | O que é |
|---|---|
| `setup.sh` | baixa ferramentas e QEMU |
| `scripts/env.sh` | PATH, sysroot, pkg-config do alvo e meson (usado por todos os scripts) |
| `scripts/fetch-tools.sh` | `ninja`, `pkgconf` e um venv com pip/setuptools em `vendor/tools`, via `apt-get download` (o WSL não tem root nem `ensurepip`) |
| `scripts/fetch-qemu.sh` | baixa o tarball do QEMU, confere o SHA-256 (`vendor/SHA256SUMS`) e aplica `patches/` |
| `scripts/fetch-src.sh` | o mesmo para outros tarballs; aplica `patches/<dir>/` |
| `scripts/fetch-uapi.sh` | headers UAPI do Linux (`linux-libc-dev`) para o `-idirafter` |
| `scripts/build-zlib.sh`, `build-glib.sh` | libs estáticas em `sodos/sysroot` |
| `scripts/build-port64.sh` | recompila a libsodos LP64 (`firefox/sodos/port`) e o `posix64.o` |
| `scripts/configure-qemu.sh`, `build-qemu.sh` | configure e ninja do QEMU |
| `scripts/probe-deps.sh` | sonda de dependências obrigatórias (Q0) |
| `scripts/vm.sh`, `scripts/test.sh` | VM de teste (com `mon`, o monitor de fora) e testes de `sodos/tests` |
| `scripts/run-guest.sh` | Q3: copia QEMU, firmware e guest para o D: e roda |
| `scripts/vm-stack.sh` | backtrace heurístico do programa travado na VM de teste |
| `scripts/copy-qemu-disk.sh` | chamado pelo `make disk-files`: QEMU, firmware, guests, FreeDOS e `.BAT` em `D:\UTILS\QEMU` |
| `scripts/make-tinycore.sh`, `scripts/isoextract.py` | Tiny Core 15 (ISO, `VMLINUZ`, `CORE.GZ`) para o `LINUX.BAT` |
| `scripts/make-freedos.sh` | disquete FreeDOS 1.3 que boota no `A:\>` e o `SERCON.BIN` (baixa a FD13-FloppyEdition, SHA-256 em `vendor/SHA256SUMS`) |
| `sodos/devices/i386-sodos.mak` | dispositivos do `qemu-system-i386` (`isapc`) |
| `sodos/guest/` | guests de teste multiboot (`hello.S`, `eco.S`, `hello.ld`, `build.sh`) |
| `sodos/disk/` | `.BAT` que vão para `D:\UTILS\QEMU` (`HELLO`, `ECO`, `SERIAL`, `DOS`, `DOSSER`, `LINUX`) |
| `sodos/ui/sodos.c` | `-display curses`: janela do WIN (gráfico, entrada crua) ou console de texto |
| `sodos/cross-sodos.ini` | propriedades do meson para o alvo (`needs_exe_wrapper`, glib) |
| `sodos/meson-cross.ini` | cross file das libs (compilador, pkg-config, host) |
| `sodos/sysroot/` | libs portadas: `.pc`, headers e `.a` (gerados, fora do git) |
| `sodos/tests/` | `ztest.c`, `ctxtest.c`, `cotest.c`, `gtest.c`, `elfread.c` |
| `patches/0001-*` | deixa o configure do QEMU somar o `cross-sodos.ini` |
| `patches/0003-*` | liga o `ui/sodos.c` no build e aloca a superfície sem `memfd` |
| `patches/0002-*` | vCPU do TCG cede a vez a cada ~10 ms: E/S, salto indireto e contagem de TBs (threads cooperativas) |
| `patches/glib-2.76.6/` | glib sem futex e sem gobject/gio |

## Mudanças no toolchain

`firefox/toolchain/sodos-gcc.py`:

- responde `-m64 --version` (o meson põe a flag de ABI antes) e imprime o
  `--version` completo do gcc (o meson reconhece o gcc pela linha do
  copyright);
- no link LP64, resolve `-lfoo` para `libfoo.a` dos diretórios `-L`;
- no link, opções de compilação com valor separado (`-idirafter DIR`,
  `-isystem DIR`...) não são mais lidas como arquivo de entrada;
- o link LP64 inclui a `libgcc.a` do gcc (`__divti3` de `__int128`).

O `firefox/toolchain/check.sh` termina com segfault no `soxrun` (x32); isso já
acontecia antes destas mudanças.
