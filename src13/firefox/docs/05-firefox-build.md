# 05 — Tentativa de build do Firefox 153.6.0esr (2026-11-03)

Este documento registra a tentativa real de baixar e configurar o Firefox
para o SODOS, com os erros exatos e o que precisa ser ajustado. O objetivo
não era “passar o configure” a qualquer custo, e sim descobrir a ordem das
barreiras.

## Fonte baixada

| Item | Valor |
|---|---|
| Versão | Firefox **153.6.0esr** (a ESR atual em 2026-11-03) |
| Tarball | `vendor/firefox-153.6.0esr.source.tar.xz` (~802 MB) |
| Extraído | 4,7 GB (em `$HOME/ffsrc/firefox-153.6.0esr`, ext4) |
| Script | `sodos/scripts/fetch-firefox.sh` |

O tarball fica em `vendor/` (ignorado pelo git). A extração no `C:` (DrvFs)
foi abandonada: com milhares de arquivos pequenos, o `tar` ficou 12+ min em
estado `D` sem progresso; no ext4 extraiu em poucos minutos. Use
`FIREFOX_DIR=$HOME/ffsrc/firefox-153.6.0esr`.

## Como o teste foi feito

```sh
./mach configure            # (travou em futex; ver abaixo)
# alternativa que funcionou para diagnóstico: configure direto no objdir
mkdir ~/ffsrc/ff-obj && cd ~/ffsrc/ff-obj
MOZCONFIG=~/ffsrc/firefox-153.6.0esr/mozconfig-sodos \
  ~/ffsrc/firefox-153.6.0esr/configure --target=x86_64-sodos
```

O `./mach configure` ficou 5+ min em `futex_wait_queue_me` com um filho
`[python] <defunct>` e log vazio; o `configure` direto no objdir deu os
erros abaixo sem travar.

## Barreiras encontradas (em ordem)

### 1. `config.sub` não conhece o OS `sodos`

```
checking for target system type...
Executing: .../build/autoconf/config.sub x86_64-sodos
Invalid configuration 'x86_64-sodos': OS 'sodos' not recognized
```

**Ajuste:** adicionar `sodos*` à lista de OS aceitos em
`build/autoconf/config.sub`. O patch [0001](patches/0001-firefox-sodos-target.patch)
faz isso; depois `config.sub x86_64-sodos` devolve `x86_64-pc-sodos`.

### 2. `init.configure` rejeita o OS

```
ERROR: Unknown OS: sodos
```

**Ajuste:** o `build/moz.configure/init.configure` tem um `elif` por OS;
adicionamos `sodos` mapeando `canonical_os = canonical_kernel = "SODOS"`.

### 3. `constants.py` não tem `SODOS` nos enums

```
ERROR: 'SODOS' is not a valid value for Kernel
```

**Ajuste:** `python/mozbuild/mozbuild/configure/constants.py` precisa de
`"SODOS"` em `OS.POSSIBLE_VALUES`, `Kernel.POSSIBLE_VALUES` e em
`kernel_preprocessor_checks` (`"SODOS": "__SODOS__"`).

### 4. Não havia linker de destino — resolvido com o driver

```
checking for target linker...
DEBUG: Executing: `.../x86_64-sodos-gcc -Wl,--version`
ERROR: Failed to find an adequate linker
```

O `sodos-cc` compilava x32, mas não tinha como ligar um `conftest`
executável; o link do SODOS é o `link` + SOX, que o configure não conhece.

**Feito:** [toolchain/](../toolchain/README.md) com
`x86_64-sodos-gcc`/`g++` (driver em `sodos-gcc.py`). Ele:

- compila com `sodos-cc`/`sodos-cxxlib` (x32, headers do SODOS/libc++);
- liga com `c_compiler/build/link` + `x_crt0.o` + LIBCX/KERNEL/POSIXX/SODOS;
- em C++ inclui `cxx_runtime.o`, `LIBCXX.LIB`, `fpconv.o` e `mathx.o`;
- responde `-Wl,--version` como `GNU ld (SODOS link) 1.0` e
  `-dumpmachine` como `x86_64-pc-sodos`;
- no modo `-E` define `__SODOS__` e remove `__linux__` (é o que o configure
  lê para descobrir o kernel); compilar/ligar mantém `__linux__` porque a
  libc++ usa isso para pthreads.

Com ele o configure passa de:

```
checking for the target C compiler... .../x86_64-sodos-gcc
checking whether the target C compiler can be used... yes
checking for the target C++ compiler... .../x86_64-sodos-g++
checking whether the target C++ compiler can be used... yes
checking for target linker... bfd
```

Teste: `make -C src13/firefox toolchain` (compila C/C++, liga SOX, roda no
`soxrun` e confere `__SODOS__` sem `__linux__`).

### 5. O ABI x32/ILP32 não é LP64 — barreira atual

```
checking for 64-bit OS... no
ERROR: The target compiler does not agree with configure about the target bitness.
```

O CPU é `x86_64` (bitness 64), mas o SODOS usa **x32**: ponteiros e `long` de
4 bytes. O Firefox assume LP64 em `x86_64`; o configure detecta a
inconsistência e para. Isso não é um detalhe de flag: `sizeof(void*) == 8`
aparece em `mozglue`, `mfbt`, `xpcom`, `NSPR`, Rust e no formato de várias
estruturas.

Opções:

1. **Portar o Gecko para x32/ILP32** — correto para o SODOS atual, porém
   grande; o alvo Rust `x86_64-unknown-linux-gnux32` é Tier 3 e o Firefox não
   o suporta oficialmente.
2. **Mudar a ABI do SODOS para LP64** — mudança profunda no kernel,
   `libc`, SOX e todo o userland; não é um passo de port.
3. **Alvo i386/32-bit** — o SODOS roda em long mode e não tem compatibilidade
   i386; exigiria outro kernel.

Recomendação: antes de decidir, compilar **`mfbt`** (headers C++ do Gecko) e
um subconjunto de `mozglue` com o toolchain atual para medir quantos
`static_assert(sizeof(void*) == 8)` e suposições de ponteiro aparecem.

### 5.1 Primeiro marco LP64: SOX2 + `HELLO64.EXE`

O caminho LP64 começou, em paralelo ao x32:

- `c_compiler/sox.h`: `SOX_MAGIC2` (`"SOX2"`) e `SOX_FLAG_64`; na SOX2 cada
  reloc da lista aponta para um campo de **8 bytes** e cada slot de import
  tem 8 bytes. Offsets/tamanhos/entrada continuam `u32` (imagem < 4 GB).
- `kernel/module.c`: aceita SOX1 e SOX2; reloca com 64 bits, preenche slots
  de 8 bytes, monta argv LP64 e chama a entrada com o mesmo `mod_call_entry`
  (SysV: `rdi=argc`, `rsi=argv`).
- `c_compiler/link/link64.c`: linker ELF64 → SOX2 (`R_X86_64_64`,
  `R_X86_64_PC32`/`PLT32`; thunk `jmp [rip]` + slot).
- `c_compiler/lib/crt0_64.S` e `lib/hello64.c`; alvo `make -C c_compiler hello64`.

Resultado no QEMU:

```
[mod] hdr HELLO64.EXE sox2
HELLO64: ABI LP64 no SODOS
[mod] retornou
```

O `HELLO64.EXE` tem 342 bytes, usa só `k_write`/`k_exit` e não depende de
LIBCX64. O FBOOT x32 continuou 17/17, então o loader aceita as duas ABIs.
LIBCX64, libc++64 e NSPR64 já rodam; falta o build do Gecko.

### 5.2 LIBCX64 estática: printf, malloc e argv LP64

A mesma `lib/` do `c_compiler` foi compilada com `gcc -m64 -mcmodel=large`
(`string`, `ctype`, `stdlib`, `stdio`, `printf`, `fpconv`, `math`, `time`,
`misc`) e ligada estaticamente com `crt0_64libc` no `HELLO64C.EXE`:

```
HELLO64C: argc=1 argv0=D:\HELLO64C.EXE
```

O hello usa `malloc`, `snprintf` e `printf`; o `argv` chega com ponteiros de
64 bits. Para isso, dois headers do SODOS passaram a ser condicionais:

- `stddef.h`: `size_t`/`ptrdiff_t`/`ssize_t` = `unsigned long`/`long` quando
  `__SIZEOF_POINTER__ == 8` (x32 continua `unsigned int`/`int`).
- `stdint.h`: `intptr_t`/`uintptr_t`/`intmax_t`/`uintmax_t` idem.

Sem isso, `libc++` e `printf` faziam cast de ponteiro para `unsigned int`.

Alvo: `make -C c_compiler hello64c`.

A **libc++64** (`sodos/libcxx64/`) compila, liga e roda no QEMU:

```
STL64: libc++ LP64 sort=1 map=7
THRT64: counter=800 atomic=200 val=42
```

(containers, `std::sort`, `std::thread`, mutex, condvar e atomic; sem
locale/iostreams/exceções ainda).

### 5.3 O que já compila da árvore do Firefox

Até agora, só peças isoladas:

| Peça | Estado |
|---|---|
| `mfbt/Vector.h` (template C++) | compila, liga e **roda no QEMU** (x32/libc++) |
| `mfbt/Assertions.cpp` | compila (precisa de `<emmintrin.h>` + `-include stdint.h`); link puxa `Printf`/double-conversion |
| `nsprpub` da árvore do Firefox 153 | **NSPR 4.39**: 56 arquivos, `PRTEST` 16/16 no QEMU (x32 e LP64) |
| `libc++64` | `STLTEST64`/`THRTEST64` rodam: containers + threads/mutex/condvar/atomic |
| `build/`/`configure` do Firefox | passa compilador/bitness/linker/pkg-config; para em Rust |
| `libxul`/browser/SpiderMonkey | não |

O NSPR da árvore (não o standalone 4.35) compila com:

```sh
NSPR_SRC=$HOME/ffsrc/firefox-153.6.0esr/nsprpub sh sodos/nspr/build.sh
sh sodos/nspr/link.sh --run
```

Resultado:

```
PRTEST - NSPR 4.39 sobre libsodos
...
PRTEST: 16 ok, 0 falhas
```

### 5.4 NSPR 4.39 LP64 — verde

O mesmo `NSPR_SRC` com `NSPR_BITS=64` compila os 56 arquivos com
`sodos-cc64` e liga o `PRTEST64.EXE` estático (NSPR 4.39 + libsodos64 +
LIBCX64). Ajustes feitos:

- `prcpucfg.h` condicional para `__SIZEOF_POINTER__ == 8`: `PR_BYTES_PER_LONG`,
  `PR_BYTES_PER_WORD`, `PR_BYTES_PER_WORD_LOG2=3`, alinhamentos e `IS_64`.
- `sodos-cc64` (`-m64 -mcmodel=large -mstackrealign -msse2`).
- `sodos/port/*.c` e `libs/posix-lite/posix.c` compilados em 64-bit.
- `link64` com `MAX_SEC=4096` e suporte a `R_X86_64_PC64`.
- **Bug do `link64` corrigido**: os símbolos globais eram resolvidos antes
  do layout da `.bss`, então globais como `_pr_sleeplock` ficavam no
  endereço 8. Agora os endereços são recalculados depois da bss.

Resultado no QEMU:

```
PRTEST - NSPR 4.39 sobre libsodos
...
PASS NSPR: condvar com timeout
PASS NSPR: PR_Read devolve o conteudo
PASS NSPR: PR_snprintf
PASS NSPR: PR_GetCurrentThread

PRTEST: 16 ok, 0 falhas
```

Inclusive `PR_Cleanup`. Reprodução:

```sh
NSPR_BITS=64 NSPR_SRC=$HOME/ffsrc/firefox-153.6.0esr/nsprpub \
  sh sodos/nspr/build.sh
NSPR_SRC=$HOME/ffsrc/firefox-153.6.0esr/nsprpub sh sodos/nspr/link64.sh
# copie build/nspr64/PRTEST64.EXE para o D: e rode no QEMU
```

Ou `make -C src13/firefox nspr64`.

### 5.5 Configure LP64

Com `x86_64-sodos-gcc64`/`g++64`, o configure agora passa dos compiladores e
do bitness:

```
checking for rustc... /home/valmor/.cargo/bin/rustc
checking for cargo... /home/valmor/.cargo/bin/cargo
checking rustc version... 1.99.1
checking cargo version... 1.99.1
ERROR: Don't know how to translate x86_64-pc-sodos for rustc
```

Ou seja: a barreira x32 vs LP64 caiu; agora falta o **alvo Rust
`x86_64-sodos`** (target JSON + `core`/`std`; o rustc só conhece
`x86_64-unknown-linux-gnu`) ou uma integração equivalente no Gecko. Depois
vêm toolkit/GTK e as bibliotecas restantes.

Para enxergar além do Rust sem `std`, há um patch **temporário de sonda**:

```sh
cd $HOME/ffsrc/firefox-153.6.0esr
patch -p1 < src13/firefox/patches/0002-sodos-rust-bringup.patch
FIREFOX_DIR=$PWD sh src13/firefox/sodos/scripts/configure-firefox64.sh
```

Ele mapeia `SODOS` para `x86_64-sodos` e pula o `assert_rust_compile` (que
usa `println!`). Com ele o configure do browser passa Rust/cbindgen/clang/
libclang/Node e para no **toolkit**:

```
checking for gtk+-wayland-3.0 >= 3.14 xkbcommon >= 0.4.1... no
```

Para não depender de GTK, o `--enable-application=js` (SpiderMonkey) foi
testado com a mesma sonda e o **configure terminou** (`config.status`,
Makefiles). O `gmake` começou a compilar ICU e parou nas primeiras barreiras
reais de código:

- `mozilla/Assertions.h`: faltava `#include <stdint.h>` (patch
  `patches/0003-firefox-sodos-build.patch`);
- libc++: `<locale.h>`/`<wchar.h>` desabilitados, mas a ICU inclui os
  headers C (fallback `include_next` + `wchar.h`/`NULL` C++ na libc);
- `rustc`: `could not find specification for target "x86_64-sodos"` — o
  cargo ainda não usa o target JSON; falta `std`.

Depois dessas correções, o `gmake` compilou **toda a ICU/common** e o único
erro restante é o target Rust do cargo. Isso **não** torna o build Rust real:
`std` para `x86_64-sodos` continua faltando.

O `std` foi portado nos `cfg` (`sys/alloc`, `sys/io/error`, `sys/random`,
`sys/thread_local`/TLS single-thread e PAL `unsupported`). O teste
`sodos/rust/stdtest` compila com `std` e roda:

```
STD64: std main ok
```

(`println!` ainda não escreve; o `k_dbg` confirma o `main`.) Os wrappers
`sodos/rust/tools/{cargo,rustc}` mapeiam `x86_64-sodos` para o JSON e usam
`build-std=std,panic_abort`.

No `gmake` do SpiderMonkey:

- todo o C++/ICU compila;
- o vendor do `std` foi copiado do `rust-src`, o cargo passou dos checksums
  e **`libjs_static.a` + `libjsrust.a` foram gerados**;
- o link final do shell **passou**: `dist/bin/js` (41,9 MB) gerado com
  GOT, COMDAT/`main` e os stubs de runtime;
- `.init_array/.fini_array` agora são coletados contiguamente pelo `link64`
  e chamados pelo `crt0_64.S` (`__sodos_ctors`/`__sodos_dtors`);
- a execução no QEMU chega ao carregamento:
  `[mod] exec JS.EXE base=03A3A300 entry=0 size=027D9A80`;
- o GDB no frame de `isr_dispatch` confirma o modo usuário
  (`CS=0x8`, `SS=0x10`); o `#UD` cai dentro da tabela de dados
  `std::locale::__imp::classic_locale_imp_`;
- removi o `-fno-rtti` que ainda estava no wrapper `sodos-cxxlib64` e
  reconstruí a libc++64 inteira; o crash mudou de `0x61E0C23` para
  `0x61E6D82` (offset `0x27A94F2`);
- o `GOTPC64` ignorava o addend (`+0x9`), deixando `r15 = GOT-9`;
  corrigido, o `js` passou de `setlocale` e chega a `InitOptionParser`
  (marcadores `0123abc`);
- o crash atual é `#GP` no primeiro `addMultiStringOption`, com
  `RIP=0x2474FF8548F88948` (bytes de `basename` usados como ponteiro) e
  `RSP=0x133FB68`; o alvo agora é `opt_new`/`malloc` e o ponteiro de
  função usado na primeira opção.

Para chegar até aqui também foram necessários:

- responder a versão do linker (`-Wl,--version`);
- expandir arquivos de resposta `@lista` do configure;
- `pkg-config` local (`vendor/tools`);
- `--disable-audio-backends` (o browser puxava `libpulse`);
- Rust no `$HOME` (`sodos/scripts/fetch-rust.sh`), cbindgen;
- Node 20 local, shim de clang (Zig/libc++) e libclang do wheel Python;
- `readlink`/`tzname` na libc e `sodos/port` no link do `g++64` (threads).

Comando completo:

```sh
FIREFOX_DIR=$HOME/ffsrc/firefox-153.6.0esr \
  sh sodos/scripts/configure-firefox64.sh
```

Ou seja: o compilador, a libc, a libc++ e o NSPR já rodam em **LP64**; o que
falta é o build do Gecko (configure/toolkit/Rust) e o restante das
bibliotecas.

### 5.6 Rust `no_std` no SODOS

Dois níveis já funcionam:

- alvo embutido **`x86_64-unknown-none`** (`make rust64`);
- alvo **`x86_64-sodos`** customizado (`make rust64-custom`), com
  `core`/`compiler_builtins` compilados via `-Z build-std` (nightly +
  `rust-src`).

O objeto do crate é ligado pelo `link64` e roda no QEMU:

```
RUST64: add=42 vec=10
```

O teste usa `core`, `alloc` (`Vec`) e `compiler_builtins`; o staticlib é
combinado com `ld -r` e usa `malloc/free` da LIBCX64 como `GlobalAlloc`.

```sh
sh sodos/scripts/fetch-rust.sh
rustup toolchain install nightly --profile minimal --component rust-src
make -C src13/firefox rust64-custom
```

Pendências para o Firefox:

- `std` para `x86_64-sodos` (syscalls/pthreads/fs do SODOS);
- ensinar o `rust.configure` a usar o target JSON; hoje ele para em
  `Don't know how to translate x86_64-pc-sodos for rustc`.

### 6. Ferramentas de host: `llvm-objdump`, `pkg-config`, `libpulse`

Para enumerar o resto da cadeia foi usada uma **sonda** (compiladores do host
com `-D__SODOS__ -U__linux__`, só para o configure; não gera binário SODOS):

```
checking for llvm-objdump... not found
ERROR: Cannot find llvm-objdump
```

Com um `llvm-objdump` de fachada, o próximo é:

```
ERROR: *** The pkg-config script could not be found.
```

Com o `pkgconf` local (`vendor/tools`), o configure chega às bibliotecas de
áudio do browser:

```
checking for libpulse... no
ERROR: Package libpulse was not found in the pkg-config search path.
```

`--disable-pulseaudio` conflita com o `--enable-audio-backends` implícito do
browser (`Cannot add '--enable-pulseaudio' ...`), então desligar áudio exige
mexer no `--enable-audio-backends`/toolkit, não só numa flag. Depois disso vêm
GTK3/cairo/pixman/freetype/harfbuzz/fontconfig.

O gcc 13 também não tem algumas flags que o Firefox espera do clang
(`-Wno-error=atomic-alignment`, `-Wno-character-conversion`,
`-fdiagnostics-absolute-paths`); o configure tolera, mas o build real é
testado com clang.

## O que o Gecko exige além disso

1. **Toolchain de destino completa** (parcial): o driver `x86_64-sodos-gcc`
   já compila e liga executáveis SOX, incluindo C++. Faltam `libgcc`,
   `crtbegin/crtend`, TLS estático, exceções e um `-shared` real para
   `libxul.so`.
2. **ABI x32/ILP32**: o Gecko assume LP64 em `x86_64`; ver a seção 5.
3. **Rust para o alvo**: Firefox é ~30% Rust (`style`, `webrender`, xpcom).
   `x86_64-unknown-linux-gnux32` é Tier 3; para `sodos` seria um alvo custom
   (`rustc --target x86_64-sodos`), com `std` cross-compilado.
4. **Sistema de build**: `configure`/`mach` geram `config.status`,
   `old-configure`, Makefiles e usam `pkg-config`, `llvm-objdump`, `ccache`,
   Python e Node.
5. **Bibliotecas de sistema**: GTK3/X11/Wayland (ou um toolkit `cairo-sodos`),
   cairo, pixman, freetype, harfbuzz, fontconfig, dbus/glib, NSS, ICU,
   zlib, libpng, libjpeg, sqlite, libvpx, dav1d... O SODOS não tem nada disso
   como biblioteca compartilhada; teriam que ser portadas/estáticas.
6. **Kernel/VM**: threads preemptivos, `mmap` de arquivo/páginas sob demanda,
   sinais, processos, `poll` de vários fds, W^X para o JIT, sandbox desligada.
7. **Saída**: o Gecko produz `libxul.so`/ELF com `dlopen`, TLS e `rpath`; o
   SODOS carrega SOX (endereço 0, relocações de 32 bits). O driver já gera
   SOX, mas não uma `.so` ELF.

## Patches já preparados

`patches/0001-firefox-sodos-target.patch`:

- `build/autoconf/config.sub`: aceita `sodos`.
- `build/moz.configure/init.configure`: `sodos` → `SODOS`.
- `python/mozbuild/mozbuild/configure/constants.py`: `SODOS` nos enums e no
  check de preprocessador.

Aplicação (na árvore extraída):

```sh
cd "$HOME/ffsrc/firefox-153.6.0esr"
patch -p1 < /caminho/src13/firefox/patches/0001-firefox-sodos-target.patch
```

## Próximo caminho recomendado

1. **Decidir a ABI**: x32/ILP32 é a barreira atual. O caminho mais barato para
   medir o tamanho do problema é compilar `mfbt` e `mozglue` com o toolchain
   atual e contar as suposições de `sizeof(void*) == 8`. Se forem poucas e
   isoladas, portar o Gecko para x32 é viável; se estiverem no layout de
   `JS::Value`, `nsTArray` e IPC, a ABI LP64 vira pré-requisito.
2. **Não começar pelo browser**: o primeiro alvo deve ser o **SpiderMonkey**
   (`js/src`), que é C++/Rust menor e não precisa de GTK/cairo.
   `--enable-project=js --disable-jit --without-intl-api`.
3. **Toolchain**: o driver `x86_64-sodos-gcc` já liga `conftest` SOX; falta
   `libgcc`/`crtbegin`/TLS/exceções e um `-shared` real para bibliotecas.
4. **Portar `mfbt`** (headers C++ do Gecko) e `mozglue` sobre a libc++ atual;
   isso dá um teste de compilação de código real do Firefox sem todo o
   browser.
5. **NSS/ICU** e as bibliotecas de imagem/fonte vêm depois do motor JS.

## Evidência

Logs completos ficam fora do git (em `$HOME/ffsrc`):

- `configure.log` — primeira tentativa (config.sub).
- `configure-direct.log` — objdir (Unknown OS).
- `configure-sodos.log` — mach (travou).
- `configure-linux4.log` — alvo Linux (llvm-objdump).
- `ff-tc/configure.log` — toolchain SODOS (para no ABI x32).

Reprodução: `sodos/scripts/fetch-firefox.sh` + o patch acima + `toolchain/`.

## Execucao do shell JS (marco)

`JS.EXE` roda JavaScript no SODOS. Um arquivo de teste (`T.JS`) imprime:

```
SpiderMonkey no SODOS!
2
soma 1..100 = 5050
sort = 1,3,5,9
obj = sodos 13
json = {"a":[1,2,3],"b":"x"}
regex = 12|34
fib(15) = 610
math = 1.414214
data = 1970-01-01T00:00:00.000Z
```

Comando (o kernel entrega a linha ao `shell_exec_line`, que passa argv ao
modulo):

```sh
curl -s -X POST --data-binary 'D:\\JS.EXE D:\\T.JS' http://127.0.0.1:8797/exec
```

### Bugs de plataforma encontrados nesta fase

Todos foram reproduzidos e corrigidos; ficam aqui porque explicam sintomas que
voltam a aparecer em outros programas grandes.

1. **`link64`: `R_X86_64_GOT64` gravava `got_base + off`** (deveria ser so
   `off`). No modelo large o codigo e
   `movabs $off,%rax; mov (%r15,%rax),%rax` com `r15 = _GLOBAL_OFFSET_TABLE_`.
   Com o valor errado, **toda** leitura de dado via GOT caia em memoria fora da
   imagem. No GC isso fazia `Arena::FirstThingOffset/ThingSize` voltarem 0, o
   GC alocava o primeiro "cell" em `arena+0` e sobrescrevia
   `Arena::firstFreeSpan`/`Arena::next` -> `#GP` em
   `js::gc::ArenaLists::refillFreeListAndAllocate`.
2. **`link64`: `SHN_XINDEX`** (objetos com mais de 0xFF00 secoes): simbolos com
   `st_shndx == 0xFFFF` resolviam para o endereco 0, colapsando 11.568 vtables
   em um unico endereco.
3. **`crt0_64.S` nao repassava `argc/argv`** ao `main` (os ctors C++ destroem
   `rdi/rsi`).
4. **`libc/wchar.c`: `wcsrtombs(NULL, ...)` retornava 0** em vez do tamanho
   necessario. `JS::EncodeUtf8ToNarrow` usa isso para converter o nome do
   arquivo, entao `js arquivo.js` abria uma string vazia ("can't open ...").
   `mbsrtowcs` tinha o mesmo defeito.
5. **`libc/stdio.c`: `fileno()` devolvia -1**, e `js::ReadCompleteFile` chama
   `fstat(fileno(fp))` para descobrir o tamanho. Fallback em
   `js/src/util/CompleteFile.cpp` (fseek/ftell) quando o `fstat` falha —
   versionado em `patches/0004-js-shell-sodos-file-io.patch`.
6. **Pilha do modulo**: o `link64` passou a pedir 16 MB (`h.stack_size`) e o
   `crt0` registra o topo em `__sodos_stack_top`; `pthread_attr_getstack`
   reporta esse intervalo (o SpiderMonkey usa para o limite de recursao).
7. **`mmap` do shim**: alinhamento em 2 MB, `munmap` de sub-faixas (o GC
   desmapeia cabeca/cauda para alinhar) e commit `MAP_FIXED`.
8. **`/dev/urandom` sintetico** + `getrandom`/`arc4random` (o
   `mozilla::RandomUint64` chamava em laco).
9. **`malloc` 16-alinhado** e cabecalho de 16 bytes (o `umalloc` do kernel
   devolve 4-alinhado porque `sizeof(struct mm_hdr)` = 12).

### Como depurar

- A serial (COM1) continua sendo o canal: `-serial file:...`.
- Marcadores de avanco: `SODOS_MARK` em `js/src/shell/js.cpp` e os `!AB/!FB/!MP`
  da libc/shim (temporarios).
- GDB: com `-accel tcg,thread=multi` o stub aceita conexao mesmo com o modulo
  rodando de interrupcoes desligadas; `-s` abre em `:1234`. Breakpoints uteis:
  `mod_call_entry` (para pegar a base do modulo) e `isr_dispatch` (frame da
  excecao).
- O binario e' SOX2: `build/img.bin` (extraido do EXE) + `link64.map` permitem
  achar a funcao de qualquer RIP: `objdump -D -b binary -m i386:x86-64`.

### JIT

O backend JIT esta **ligado** e executa codigo gerado: o SODOS nao tem NX, entao
basta `mmap` RW (o shim alinha em 2 MB) e o `InitializeJit` reserva
`MaxCodeBytesPerProcess` = 2044 MB. Teste (`T2.JS`):

```
aquecendo...
loop(1000000) = 1000000
r = 8999994 em 2 ms      <-- 3 milhoes de iteracoes
fim
```

`mprotect`/`madvise` sao no-ops no shim (sem protecao por pagina), o que e
suficiente para W^X "de fachada".
