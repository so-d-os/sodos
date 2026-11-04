# 02 — Arquitetura do port

## 1. Camadas

```
+-------------------------------------------------------------+
|  Firefox / Gecko (C++/Rust)                                 |
|  browser, toolkit, netwerk, layout, gfx, dom, js            |
+-------------------------------------------------------------+
|  NSPR 4.35 (C)      NSS (C)      ICU (C/C++)   (M1..M4)     |
|  PR_Thread, PR_Lock, PR_Open, PR_Poll, PR_Now...            |
+-------------------------------------------------------------+
|  libsodos (SODOS.DLL) - este diretório, M0                  |
|  pthread, mutex/cond/rwlock/sem, TLS, mmap, poll, log, TCP  |
+-------------------------------------------------------------+
|  KERNEL do SODOS (KERNEL.IMP): k_malloc, k_fload, k_gui_*,  |
|  k_tcp_*, k_ms, k_yield, k_net_pending                      |
+-------------------------------------------------------------+
|  SOX (.EXE/.DLL) sobre FAT16, NE2000, VGA, teclado, mouse   |
+-------------------------------------------------------------+
```

A regra é: **nada acima da `libsodos` chama `k_*` diretamente**, exceto código
de teste. Assim, quando o kernel ganhar threads preemptivos, múltiplas
conexões ou `mmap` de verdade, a troca acontece em um lugar só.

## 2. ABI e toolchain

O SODOS executa imagens **SOX** montadas no endereço 0, com relocações de
32 bits (`sox.h`). Os programas usam a ABI do kernel (argumentos em
`edi, esi, edx, ecx, r8d, r9d`), e o NetSurf provou que `gcc -mx32` gera
código compatível: ponteiros de 32 bits em long mode, SSE2 disponível.

```
gcc -mx32 ...  ->  .o ELF32 x32  ->  c_compiler/build/link  ->  .DLL/.EXE SOX
```

Consequências:

- `sizeof(void*) == 4`, `size_t`/`long` de 32 bits; o espaço de endereço é
  limitado a 4 GB.
- O `link` do SODOS entende ELF x32, resolve imports contra `.DLL`/`.IMP` e
  gera os thunks `jmp [slot]`. Ele **não** roda `.init_array` nem
  construtores C++.
- Nomes de arquivo no FAT16 são 8.3: `SODOS.DLL`, `FBOOT.EXE` cabem.

### Por que x32 e não 64 bits agora

Migrar para ponteiros de 64 bits exigiria outro loader (relocações de 64
bits), reconstruir LIBCX/POSIXX/TLS/GUI e todos os `.EXE` do disco. O Gecko
compila em 32 bits (Windows x86 é suportado), então x32 é um alvo válido para
chegar ao NSPR/SpiderMonkey. A decisão fica registrada em
[01-avaliacao.md](01-avaliacao.md) §4; a saída de emergência é um alvo
`x86_64-sodos` (LP64) com um loader ELF64 novo, feito depois do M3 se o
SpiderMonkey exigir.

## 3. Threads: fibra cooperativa com pilha própria

`pthread_create` aloca uma pilha no heap do kernel (`k_malloc`, padrão de
256 KB) e monta o contexto inicial:

```
topo - 72 : r15 r14 r13 r12 rbp rbx   (48 bytes, callee-saved)
topo - 24 : endereço de sodos_thread_trampoline  (retorno do "ret")
topo - 16 : fn
topo -  8 : arg
```

`sodos_ctx_switch(save, new_sp)` empilha `rbx, rbp, r12-r15`, grava `rsp` em
`*save` e carrega `new_sp`. O primeiro `ret` cai no trampolim, que carrega
`fn` em `eax` e `arg` em **`edi`** (ABI x32) e chama a função. Ao retornar, o
valor de `eax` vai para `sodos_thread_finish`.

O escalonador é FIFO, com:

- fila de prontas (`run_h/run_t`);
- fila de dormindo ordenada por `wake_ms` (64 bits);
- uma thread corrente fora da fila;
- `sodos__park` para bloqueio sem fila (cond/sem usam o nó na pilha do
  waiter e `sodos__wake`).

### Regras que o código acima precisa seguir

1. **Toda espera passa pela `libsodos`.** `k_tcp_recv` bloqueia o processo
   inteiro; use `sodos_net_recv`/`poll`, que cedem.
2. **Nada de laço de espera com `k_ms` puro.** Use `sodos_sleep_ms`.
3. **Seções críticas são curtas** e não chamam nada que ceda (o mutex não é
   reentrante por outra thread; é recursivo para a mesma).
4. **Sem preempção**: uma thread que faz computação longa sem ceder trava as
   outras. O NSPR já cede nos pontos de I/O; o Gecko vai precisar de
   preempção no kernel (M1b) para o thread pool.

### O que muda quando o kernel tiver preempção

A API pública (`pthread_*`) não muda. `sodos_ctx_switch` vira “salvar
contexto no IRQ”, e as filas de prontas passam a ser do kernel. O port do
NSPR continua igual; o que muda é a latência e a garantia de progresso.

### NSPR sobre a libsodos (M1)

O NSPR não foi reescrito: ele compila no **modo pthreads** e usa a `libsodos`
como se fosse o glibc. O que muda é só a configuração do alvo:

| Peça | Papel |
|---|---|
| `sodos/nspr/include/prcpucfg.h` | ABI x32: `long`/ponteiro de 4 bytes, `int64` alinhado em 8 |
| `sodos/nspr/include/md/_sodos.h` | `XP_UNIX`, atomics por `__sync_*`, `poll`, `clock_gettime`, sem SysV/IPv6 |
| `sodos/nspr/include/md/prosdep.h` | seleciona `_sodos.h` em vez de `_linux.h` (shadow no include path) |
| `sodos/nspr/shim.c` | sinais, `select`→`poll`, `dlopen`→`k_dll_*`, DNS, stubs |
| `sodos/nspr/build.sh` | compila as 56 fontes com `sodos-cc` |
| `sodos/nspr/link.sh` | liga `PRTEST.EXE` com `link` (NSPR estático) |

No modo pthreads, `ptio.c`/`ptthread.c`/`ptsynch.c` substituem
`prfile.c`/`prsocket.c`/`prio.c`; o MD Unix (`unix.c`, `uxwrap.c`...) usa as
funções POSIX da `libsodos`/POSIXX/LIBCX. Soquetes de rede ainda são stubs
(`ENOSYS`) porque o kernel só tem uma conexão — M4.

### Runtime C++ (M2a)

O `link` do SODOS agora trata `.init_array` (tipo ELF `SHT_INIT_ARRAY`): as
entradas de todos os objetos viram uma tabela contígua depois de DATA e o
linker exporta `__init_array_start`/`__init_array_end` como símbolos de seção
(o loader soma a base). O `crt0` chama `__sodos_ctors()` antes de `main` e
`__sodos_dtors()` depois; a LIBCX fornece versões vazias e
`sodos/cxx/cxx_runtime.cpp` define as fortes no `.EXE`.

O runtime cobre `operator new/delete`, `__cxa_atexit` (destrutores globais),
`__cxa_guard_*` (static local), `__cxa_pure_virtual` e `__dso_handle`.
Compila-se com `sodos-cxx` (`g++ -mx32 -nostdinc -fno-exceptions -fno-rtti`),
que usa o g++ local de `vendor/gxx` quando não há `g++` no PATH.

Ainda **não** há biblioteca padrão: `std::string`, `std::vector`, `std::mutex`,
exceções e RTTI são o M2b.

### libc++ (M2b)

A libc++ 18.1.8 roda como **código do programa** (não DLL): os headers ficam em
`vendor/libcxx-18.1.8/include` e as fontes do runtime (`string.cpp`,
`algorithm.cpp`, `memory.cpp`, `chrono.cpp`...) são compiladas para `.o` e
ligadas no `.EXE`. `sodos-cxxlib` põe os headers da libc++ **antes** dos
headers C do SODOS (ela tem wrappers `<string.h>`/`<stdlib.h>` que fazem
`include_next`).

Configuração do alvo em `sodos/libcxx/include/__config_site`:

| Macro | Efeito |
|---|---|
| `_LIBCPP_ABI_NAMESPACE __1` | namespace ABI padrão |
| `_LIBCPP_HAS_NO_LOCALIZATION` | sem `<locale>` (por enquanto) |
| `_LIBCPP_HAS_NO_WIDE_CHARACTERS` | sem `wchar_t` |
| `_LIBCPP_HAS_CLOCK_GETTIME` | `steady_clock` usa `clock_gettime` |
| `_LIBCPP_HARDENING_MODE_NONE` | sem asserts extras |

Sem `_LIBCPP_HAS_NO_THREADS`, o `std::thread`/`mutex`/`condition_variable` da
libc++ usa os pthreads da `libsodos` (cooperativos) através da `SODOS.DLL`;
`memory.cpp` também precisa de `pthread_mutex_lock` para o singleton de
`std::shared_ptr`.

O linker precisa da seção `.init_array` (M2a) e de um `_lcxx_override` vazio
(`sodos/libcxx/override_stub.S`). `strtod`/`libm` vêm de `fpconv.c`/`math.c`
compilados com SSE2 no `.EXE`, porque a LIBCX é `-mno-sse` e deixa esses
arquivos vazios.

## 4. Memória

- `malloc/free` vêm da LIBCX (sobre `k_malloc`). O Gecko deve ser compilado
  com `--disable-jemalloc` no M2/M3 para usar esse caminho.
- `mmap` anônimo é um bloco de `k_malloc` zerado, com uma tabela
  `endereço → tamanho` para `munmap`. `mprotect` é no-op; `MAP_FIXED` e mapa
  de arquivo retornam `ENOSYS`.
- Sem guard pages: um estouro de pilha corrompe o heap (o mesmo já acontece
  no resto do SO).
- O heap do usuário vai do fim do kernel até o topo da RAM (4 GB no QEMU
  atual). `sodos_mem_info` lê `k_meminfo`.

## 5. Tempo

- Monotônico: `sodos_now_ms()` estende o `k_ms` de 32 bits para 64 bits,
  detectando o wrap. É a base de timers, `poll` e `PR_Now`.
- Parede: `time(NULL)` (RTC, segundos) + os ms do PIT; `sodos_wall_ms()`
  devolve epoch em ms.
- `nanosleep` da LIBCX é um laço de `k_ms` sem ceder; a `libsodos` não a
  usa: `sodos_sleep_ms` dorme na fila do escalonador e cede.

## 6. Arquivos e rede

- Arquivos continuam por `open/read/write` da POSIXX (arquivo inteiro em
  memória, limite de 64 MB). O log da `libsodos` usa `k_fload/k_fsave` com
  concatenação e rotação em 512 KB — provisório até um append real no FAT16.
- Rede: `sodos_net_connect/send/recv/close` sobre a única conexão do kernel.
  O handle é `SODOS_SOCK_FD_BASE` (1000) e serve para `poll`. Quando o kernel
  tiver uma tabela de TCBs, o handle passa a ser um índice real e `poll`
  ganha `POLLOUT`/timeout de kernel.

## 7. Build

```
firefox/Makefile
  sodos/sodos-cc  (gcc -mx32 + headers do SODOS)
    sodos/port/*.c -> build/obj/*.o
      link /dll /exportall -> build/out/SODOS.DLL
        link + x_crt0.o + t_fboot.o -> build/out/FBOOT.EXE
          soxrun -C build/run FBOOT.EXE
```

- As DLLs de runtime (`LIBCX.DLL`, `KERNEL.IMP`) precisam ser **regeneradas**
  quando o kernel ganha exports novos: `make -B -C ../c_compiler
  build/soxrun out/KERNEL.IMP`. O `soxrun` também precisa da tabela `ktab`
  atualizada, senão o import fica nulo e o programa cai no primeiro uso.
- O fuso/clock do WSL vs. Windows pode deixar arquivos “no futuro” e o make
  pular passos; se um símbolo novo não aparecer, use `make -B`.
- O alvo `make -C firefox` não entra no build normal do SO (o `make` da raiz
  continua gerando só `os.img`); `make firefox` é explícito.

## 8. Estrutura de arquivos

```
firefox/
  Makefile
  README.md
  docs/01-avaliacao.md 02-arquitetura.md 03-fases.md 04-construcao.md
  sodos/
    include/   pthread.h poll.h semaphore.h sys/mman.h sodos.h
    port/      sodos_init.c sodos_time.c sodos_thread.c sodos_sync.c
               sodos_mmap.c sodos_net.c sodos_poll.c sodos_internal.h
    tests/     t_fboot.c
    scripts/   fetch-nspr.sh fetch-gxx.sh soxdump.py
    nspr/      include/ shim.c build.sh link.sh tests/
    cxx/       cxx_runtime.cpp t_cxx.cpp build.sh
    sodos-cc   sodos-cxx
  vendor/      nspr-4.35/ (fora do git, baixado por setup.sh)
  mozconfig/   flags previstas do Gecko
  patches/     patches nossos
```
