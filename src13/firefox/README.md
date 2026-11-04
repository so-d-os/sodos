# Firefox no SODOS

Port do Firefox/Gecko para o SODOS. Este diretório é o projeto de port: a
camada de plataforma, o build, os patches sobre o upstream e a documentação.
O código do Firefox em si não é copiado para o repositório; `setup.sh` baixa a
árvore oficial para `vendor/` (fora do git).

> **Estado em 2026-11-03 (M0 + M1 + M2a + M2b parcial):** a fundação
> (`libsodos`), o **NSPR 4.35**, o **runtime C++ mínimo** e o **núcleo da
> libc++** (containers, algoritmos, `string`, `chrono`, `std::thread`/`mutex`)
> estão portados e testados. O Firefox **ainda não compila**: faltam
> iostreams/locale e exceções da libc++, NSS/ICU, VM com páginas sob demanda,
> múltiplas conexões e preempção no kernel. O plano completo está em
> [docs/03-fases.md](docs/03-fases.md).

## O que já funciona

**M0 — `SODOS.DLL` + `FBOOT.EXE`** (17 testes, 18 com rede): threads
cooperativos com pilha própria, mutex/cond/rwlock/TLS, `mmap` anônimo, tempo,
log, `poll` e uma conexão TCP.

**M1 — NSPR 4.35 + `PRTEST.EXE`** (16 testes): `PR_Init`, `PR_Now`,
`PR_Sleep`, locks, condição (com timeout), `PR_CreateThread`/`PR_JoinThread`,
TLS do NSPR, `PR_Open/Read/Write/GetFileInfo/Delete`, `PR_Malloc` e
`PR_snprintf`.

**M2a — runtime C++ + `CXXTEST.EXE`** (7 testes): o `link` agora monta a
tabela `.init_array` e o `crt0` chama os construtores globais; `new/delete`,
`__cxa_atexit`, `__cxa_guard_*`, herança virtual, templates e métodos
virtuais funcionam sem libstdc++.

```
$ make cxx-test
CXXTEST - runtime C++ minimo do SODOS
PASS cxx: construtor global roda antes de main
PASS cxx: new + heranca virtual
PASS cxx: override de metodo virtual
PASS cxx: new[]
PASS cxx: template
PASS cxx: static local com guard
PASS cxx: operador sobrecarregado
CXXTEST: 7 ok, 0 falhas
```

**M2b — libc++ 18.1.8 + `STLTEST.EXE`** (23 testes): a libc++ compila para
x32+SODOS e o teste cobre `vector`, `string` (find/substr/to_string),
`map`/`unordered_map` (com hash), `set`, `list`, `deque`, `array`, `tuple`,
`unique_ptr`/`shared_ptr`, `function`/lambda, `numeric`, `string_view`,
`optional`, `variant`, `chrono` e `stoi`/`stof`.

**`THRTEST.EXE`** (4 testes) usa os pthreads da `libsodos` por baixo:
`std::thread` + `join`, `std::mutex`/`lock_guard`,
`std::condition_variable::wait` com predicado, `std::atomic::fetch_add` e
`std::this_thread::sleep_for`.

```
$ make libcxx-test
STLTEST: 23 ok, 0 falhas
THRTEST: 4 ok, 0 falhas
```

```
$ make libcxx-test
STLTEST - libc++ sobre o SODOS
PASS stl: vector + sort
PASS stl: string + operador +=
PASS stl: map
PASS stl: unordered_map + hash
PASS stl: unique_ptr
PASS stl: shared_ptr
PASS stl: function + lambda
PASS stl: optional / variant / chrono
...
STLTEST: 23 ok, 0 falhas
```

Os quatro testes rodam no `soxrun` e dentro do SODOS (QEMU), inclusive pela API
HTTP (`D:\STLTEST.EXE`, `D:\CXXTEST.EXE`, `D:\PRTEST.EXE`, `D:\FBOOT.EXE`).

## Tentativa de build do Firefox (2026-11-03)

O fonte do **Firefox 153.6.0esr** foi baixado (`vendor/firefox-153.6.0esr.source.tar.xz`,
~802 MB) e o configure foi rodado com alvo `x86_64-sodos`. O alvo é
reconhecido depois do patch [0001-firefox-sodos-target.patch](patches/0001-firefox-sodos-target.patch)
e o toolchain [toolchain/](toolchain/README.md) faz o configure aceitar
compilador C, C++ e linker. Ele para na ABI:

```
checking for 64-bit OS... no
ERROR: The target compiler does not agree with configure about the target bitness.
```

O SODOS é **x32/ILP32** (ponteiros de 4 bytes) e o Firefox assume LP64 em
`x86_64`. O primeiro passo do caminho LP64 já funciona: **SOX2 + `HELLO64.EXE`**
e **`HELLO64C.EXE` com LIBCX64 estática** (`printf`, `malloc`, `argv` LP64)
rodam no QEMU, e o loader continua aceitando os `.EXE` x32 (FBOOT 17/17).
Da árvore do Firefox já compilam o **NSPR 4.39** (56 arquivos, PRTEST 16/16
no x32 e no LP64) e templates do `mfbt`; a **libc++64** também roda
(`STL64` e `THRT64`, com threads). O configure LP64 já passa
compilador/bitness/linker/pkg-config e para no **toolkit (GTK)**; com o
patch de sonda, o configure do **SpiderMonkey (`--enable-application=js`)**
termina, o **C++/ICU compila**, o `gmake` gera **`libjs_static.a` +
`libjsrust.a`** e o **shell `dist/bin/js` é linkado** (41,9 MB) com GOT,
COMDAT, ctors/dtors e stubs de runtime. No QEMU ele passa por `_start`, `.init_array`, `main`, `PreInit()`,
`setlocale` e chega a `InitOptionParser`; o crash atual é um `#GP` no
primeiro `addMultiStringOption`, com o ponteiro `0x2474FF8548F88948`
(bytes de `basename` usados como endereço). O browser/libxul ainda não.

O relatório completo (barreiras, logs e o que falta) está em
[docs/05-firefox-build.md](docs/05-firefox-build.md). Diagnóstico:
`sodos/scripts/configure-firefox.sh`.

## Como compilar e testar

Tudo roda no WSL, como o restante do projeto. O `c_compiler` precisa estar
compilado antes (`make -C ../c_compiler`).

```sh
cd src13
make -C firefox                 # build/out/SODOS.DLL
make -C firefox test            # FBOOT.EXE + soxrun
make -C firefox test ARGS=net   # inclui o GET HTTP (precisa de rede)
make -C firefox nspr            # NSPR 4.35 (vendor/, baixado por setup.sh)
make -C firefox nspr-test       # PRTEST.EXE + soxrun
make -C firefox cxx-test        # runtime C++ + CXXTEST.EXE
make -C firefox libcxx-test     # libc++ + STLTEST.EXE
make -C firefox nspr64          # NSPR 4.39 LP64 + PRTEST64
make -C firefox libcxx64        # libc++64 + STLTEST64
make -C firefox rust64          # Rust no_std -> RUST64.EXE
make -C firefox rust64-custom   # target x86_64-sodos + core/compiler_builtins
make -C firefox toolchain       # valida x86_64-sodos-gcc/g++
```

Na raiz do `src13`: `make firefox`, `make firefox-test`, `make firefox-nspr`,
`make firefox-nspr-test`, `make firefox-cxx`, `make firefox-cxx-test`,
`make firefox-libcxx`, `make firefox-libcxx-test` e `make firefox-disk`. O
`setup.sh` baixa o NSPR, a libc++ e (quando possível) um `g++` local sem root.

## Estrutura

| Caminho | O que é |
|---|---|
| `sodos/include/` | headers POSIX que o SODOS não tem (`pthread.h`, `poll.h`, `sys/mman.h`, `sys/socket.h`, `netdb.h`...) |
| `sodos/port/` | a `libsodos`: init/log, tempo, threads, sincronização, mmap, rede, poll |
| `sodos/nspr/include/` | config do NSPR para o alvo (`prcpucfg.h` x32, `md/_sodos.h`, `md/prosdep.h`) |
| `sodos/nspr/shim.c` | cola do alvo: sinais, `select`, `dlopen`, DNS, stubs de socket |
| `sodos/nspr/build.sh` | compila as 56 fontes do NSPR com o `sodos-cc` |
| `sodos/nspr/link.sh` | liga `PRTEST.EXE` com o linker do SODOS e roda |
| `sodos/nspr/tests/t_nspr.c` | teste de fumaça do NSPR |
| `sodos/cxx/cxx_runtime.cpp` | runtime C++ mínimo (`new`, `__cxa_*`, ctors) |
| `sodos/cxx/t_cxx.cpp` | teste do C++ (CXXTEST.EXE) |
| `sodos/libcxx/include/` | `__config_site`, `__assertion_handler`, `features.h` do alvo |
| `sodos/libcxx/build.sh` | compila a libc++ e gera `LIBCXX.LIB` + `STLTEST.EXE` |
| `sodos/libcxx/t_stl.cpp` | teste da libc++ (23 casos) |
| `sodos/libcxx/t_stl_thread.cpp` | `std::thread`/mutex/condvar/atomic (THRTEST) |
| `toolchain/` | `x86_64-sodos-gcc/g++`: compila x32 e liga SOX (`make toolchain`) |
| `sodos/tests/t_fboot.c` | teste de fumaça da `libsodos` |
| `docs/` | avaliação, arquitetura, fases e construção |
| `vendor/` | árvores upstream baixadas (NSPR, Firefox ESR); fora do git |
| `mozconfig/` | flags previstas para o `configure` do Gecko (ainda não usáveis) |
| `patches/` | patches nossos sobre o upstream |

## Como o NSPR foi portado

- `prcpucfg.h` próprio com a ABI **x32 ILP32** (`long`/ponteiro de 4 bytes).
- `md/_sodos.h` no lugar do `_linux.h`, e um `md/prosdep.h` que o seleciona.
- Compilação no modo **pthreads** do NSPR: `ptio.c`/`ptthread.c` substituem
  `prfile.c`/`prsocket.c`, e o MD Unix usa a `libsodos` em vez do glibc.
- `shim.c` cobre o que a LIBCX/POSIXX não têm: `sigaction` (no-op), `fcntl`,
  `select` sobre `poll`, `dlopen` sobre `k_dll_*`, `gethostbyname` via `k_dns`,
  `uname`, `getrlimit` e stubs de processo/socket.
- Soquetes de rede do NSPR ainda são stubs (`ENOSYS`): o kernel tem uma
  conexão TCP por vez e isso é o **M4**.

## Como a libc++ foi portada

- `sodos/libcxx/include/__config_site`: ABI `__1`, sem locale/wchar/threads,
  hardening desligado, `clock_gettime` disponível.
- `features.h` próprio: a libc++ inclui `<features.h>` em `__linux__`; o SODOS
  não é glibc, então `__GLIBC_PREREQ` é 0.
- Headers C do SODOS ganharam `extern "C"` (stdio, stdlib, string, math,
  time, ctype, locale, errno, inttypes...), `int_least*`/`INT64_C` no
  `stdint.h`, `max_align_t`, `timespec_get`, `uchar.h` e `setvbuf`/`fgetpos`.
- Runtime compilado da libc++: `string.cpp`, `algorithm.cpp`, `stdexcept.cpp`,
  `exception.cpp`, `verbose_abort.cpp`, `new*.cpp`, `typeinfo.cpp`,
  `memory.cpp`, `functional.cpp`, `hash.cpp`, `vector.cpp`, `optional.cpp`,
  `any.cpp`, `chrono.cpp`, `system_error.cpp`, `error_category.cpp`.
- O linker do SODOS ganhou a seção `.init_array` (M2a) e um stub
  `_lcxx_override` vazio para o `new.cpp` da libc++.
- `strtod`/`strtof`/`strtold` e `libm` vêm de `fpconv.c`/`math.c` compilados
  com SSE2 no `.EXE` (a LIBCX é `-mno-sse` e deixa esses arquivos vazios).

## Limites atuais (por que ainda não é o Firefox)

- **Sem preempção**: as threads trocam só em chamadas que cedem. O NSPR cabe,
  o Gecko (thread pool/GC) não fecha sem a troca por IRQ de timer (M1b).
- **Uma conexão TCP por vez** no kernel; `PR_Socket`/`PR_Poll` de rede ainda
  não funcionam (M4).
- **`mmap` é heap**: sem páginas sob demanda, sem mapear `omni.ja`/fontes.
- **libc++ parcial**: containers/algoritmos/`string`/`chrono`/threads
  funcionam, mas ainda faltam iostreams, locale, exceções e
  `std::filesystem`. O Gecko usa iostreams? pouco, mas usa exceções e locale.
- **Sem NSS/ICU/jemalloc**: TLS hoje é o cliente do `c_compiler/tls`, não NSS.
- **Sem build dentro do SODOS**: `mach`/clang/rust não existem no SO; o port é
  cross-build no host. Levar o build para dentro do SO é o M7.

## Próximos passos

1. **ABI x32 vs LP64** — SOX2, LIBCX64, libc++64 e NSPR 4.39 já rodam em
   LP64; agora reavaliar o configure do Firefox (Rust/toolkit) e o NSPR64 no
   build do Gecko.
2. **M2b (continuação)** — iostreams/locale, exceções simplificadas e
   `std::filesystem`.
3. **M1b** — threads preemptivos no kernel (hoje tudo é polling; ligar o
   timer/IRQ exige tornar o kernel reentrante).
4. **M3** — SpiderMonkey (`JS.EXE`) com `--disable-jit` no começo.

Detalhes e critérios em [docs/03-fases.md](docs/03-fases.md).

## Licenças

O código deste diretório segue a GPLv3 do SODOS (raiz do repositório). O NSPR
baixado em `vendor/` é MPL 2.0; o Firefox/Gecko é MPL 2.0; a libc++ é Apache
2.0 com exceção LLVM. MPL 2.0 é compatível com a GPLv3 para distribuição
conjunta, mas os arquivos upstream mantêm o cabeçalho original e o `vendor/`
não é versionado aqui.
