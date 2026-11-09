# QEMU no SODOS

Port do QEMU (emulador de sistema, TCG) para rodar **dentro** do SODOS. Mesma
organização do [port do Firefox](../firefox/README.md): o código upstream não
entra no git; `setup.sh` baixa o QEMU para `vendor/` e aplica `patches/`.

Alvo: `qemu-system-i386` com TCG, compilado pelo toolchain LP64 do SODOS
(`x86_64-sodos-gcc64`, imagem SOX2) do `firefox/toolchain`.

## Marcos

| Marco | Objetivo | Estado |
|---|---|---|
| **Q0** | baixar o QEMU, cross-build com meson, `configure` do `i386-softmmu` e lista do que falta | **feito (2026-11-09)** |
| Q1 | portar glib, zlib e (para tela) pixman; ucontext (`makecontext`/`swapcontext`) | — |
| Q2 | `QEMU.EXE --version` no SODOS | — |
| Q3 | bootar um kernel multiboot mínimo pela serial (`-M isapc -nographic`) | — |
| Q4 | backend de tela `ui/sodos.c` numa janela do SODOS | — |

## Q0: resultado

O `configure` do QEMU 9.2.0 roda até o fim com o alvo x86_64-sodos. Com
`--without-default-features --without-default-devices`, as dependências
**obrigatórias** que faltam no SODOS são só duas:

1. **glib-2.0 ≥ 2.66** (a de verdade; o `libs/glib` é um subconjunto do MC e
   não serve). O QEMU inclui `glib.h` em todo arquivo via `qemu/osdep.h`, então
   nada compila antes dela.
2. **zlib**.

`scripts/probe-deps.sh` acha isso dando um `.pc` falso para cada dependência
que o meson recusa, até o configure passar. Com essas duas preenchidas, o
resumo do meson diz:

- `TCG backend: native (x86_64)`: o gerador de código x86-64 do QEMU, como o
  JIT do SpiderMonkey (que já roda no SODOS);
- `coroutine backend: sigaltstack`: o meson não achou `makecontext` e caiu no
  backend de sinais, que é pior para o SODOS. No Q1 implementar
  `makecontext`/`swapcontext` (o `ucontext.h` já existe) e forçar
  `--with-coroutine=ucontext`;
- `pixman: NO`, `SDL/VNC: NO`, `slirp: NO`: sem tela e sem rede no começo
  (`-nographic`). O pixman entra quando houver o backend de tela;
- `fdt` e `softfloat` vêm como subprojetos internos;
- funções que a libsodos LP64 já cobre: `accept4`, `fallocate`, `getrandom`,
  `gettid`, `posix_memalign`, `sched_getcpu`, `sendfile`; faltam `ppoll`,
  `preadv`, `dup3`, `timerfd_create`, `strchrnul`, `openpty` (todas
  opcionais).

O `host_os` é `linux`: o driver mantém `__linux__` ao compilar (como no
Firefox), e a libsodos imita a ABI Linux. Os pontos que isso puxa (eventfd,
signalfd, `/proc`) aparecem no Q1/Q2.

## Riscos conhecidos

- **Threads cooperativas** (o M1b, preempção, segue pendente): a thread da vCPU
  pode monopolizar a CPU. Plano: `-accel tcg,thread=single` e fazer
  `mutex_unlock`/`cond_wait` cederem a vez.
- **Memória**: o kernel mapeia 256 MiB em identidade; guest de 16–32 MB.
- **Velocidade**: o SODOS costuma rodar no QEMU, então é emulação aninhada.

## Como rodar (WSL)

```sh
cd src13/qemu
sh setup.sh                    # QEMU 9.2.0 + ninja/pkgconf/venv locais (sem root)
sh scripts/configure-qemu.sh   # para em "Dependency glib-2.0 not found" ate o Q1
sh scripts/probe-deps.sh       # lista todas as dependencias obrigatorias
```

O build fica em `~/sodos-build/qemu-9.2.0` (ext4 do WSL; no `/mnt/c` o meson
acusa *clock skew*). Mude com `QEMU_BUILD_DIR=...`. O log do configure fica em
`build/configure.log`.

## Estrutura

| Caminho | O que é |
|---|---|
| `setup.sh` | baixa ferramentas e QEMU |
| `scripts/fetch-tools.sh` | `ninja`, `pkgconf` e um venv com pip/setuptools em `vendor/tools`, via `apt-get download` (o WSL não tem root nem `ensurepip`) |
| `scripts/fetch-qemu.sh` | baixa o tarball, confere o SHA-256 (`vendor/SHA256SUMS`) e aplica `patches/` |
| `scripts/configure-qemu.sh` | configure com o toolchain do SODOS e um pkg-config que só enxerga `sodos/sysroot` |
| `scripts/probe-deps.sh` | sonda de dependências obrigatórias |
| `sodos/cross-sodos.ini` | arquivo de máquina extra do meson (`needs_exe_wrapper`) |
| `sodos/sysroot/` | onde as libs portadas (glib, zlib...) vão instalar `.pc`, headers e objetos |
| `patches/0001-*` | deixa o configure somar o `cross-sodos.ini` |

## Mudança no toolchain

`firefox/toolchain/sodos-gcc.py` passou a responder `-m64 --version` (o meson
põe a flag de ABI antes) e a imprimir o `--version` completo do gcc (o meson
reconhece o gcc pela linha do copyright).
