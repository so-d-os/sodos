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
| Q4b | backend de tela `ui/sodos.c` numa janela do SODOS; bootar um sistema de verdade | — |

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

## Q4b: próximos passos

1. Backend de tela `ui/sodos.c` (DisplayChangeListener) numa janela do WIN,
   com o pixman portado; teclado/mouse da janela para o guest.
2. Bootar um sistema de verdade (FreeDOS ou o próprio SODOS) de uma imagem
   de disco pequena.

## Riscos conhecidos

- **Threads cooperativas** (o M1b, preempção, segue pendente): funcionam para
  o guest de teste (vCPU, RCU e laço principal se revezam). A vCPU cede a vez
  a cada ~10 ms quando acessa dispositivo ou faz salto indireto; um laço
  puramente de cálculo, só com saltos diretos, ainda atrasa o laço principal.
- **Memória**: o kernel mapeia 256 MiB em identidade; guest de 16–32 MB.
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
| `scripts/copy-qemu-disk.sh` | chamado pelo `make disk-files`: QEMU, firmware, guests e `.BAT` em `D:\UTILS\QEMU` |
| `sodos/devices/i386-sodos.mak` | dispositivos do `qemu-system-i386` (`isapc`) |
| `sodos/guest/` | guests de teste multiboot (`hello.S`, `eco.S`, `hello.ld`, `build.sh`) |
| `sodos/disk/` | `.BAT` que vão para `D:\UTILS\QEMU` (`HELLO`, `ECO`, `SERIAL`) |
| `sodos/cross-sodos.ini` | propriedades do meson para o alvo (`needs_exe_wrapper`, glib) |
| `sodos/meson-cross.ini` | cross file das libs (compilador, pkg-config, host) |
| `sodos/sysroot/` | libs portadas: `.pc`, headers e `.a` (gerados, fora do git) |
| `sodos/tests/` | `ztest.c`, `ctxtest.c`, `cotest.c`, `gtest.c`, `elfread.c` |
| `patches/0001-*` | deixa o configure do QEMU somar o `cross-sodos.ini` |
| `patches/0002-*` | vCPU do TCG cede a vez a cada ~10 ms (threads cooperativas) |
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
