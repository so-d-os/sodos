# 03 — Fases do port

Cada fase tem um **artefato** e um **critério de aceite** verificável no
`soxrun` ou no QEMU. Nada é “feito” enquanto o critério não passa.

## M0 — Fundação de plataforma  ✅ (2026-11-03)

**Artefato:** `SODOS.DLL` (`sodos/port/`) + `FBOOT.EXE`.

**Feito:**

- `pthread_create/join/detach/exit`, `pthread_self`, atributos;
- mutex (normal/recursivo/errorcheck), cond com `timedwait`, rwlock, once,
  TLS com destrutores, semáforos;
- `mmap` anônimo/`munmap`, `mprotect` no-op;
- tempo monotônico de 64 bits, `sleep_ms` cooperativo, `sodos_wall_ms`;
- `poll` sobre a conexão do kernel + timeouts;
- `sodos_net_connect/send/recv/pending/close`;
- log em `X:\LOG\FIREFOX.LOG` com rotação;
- kernel: `k_yield`, `k_net_pending` e `k_exe_drive` exportados; `soxrun`
  acompanha.

**Critério:** `FBOOT` com 17 ok/0 falhas e, com `ARGS=net`, um `HTTP/1.1 200`
de verdade. **Cumprido** (18 ok no `soxrun` e no QEMU).

## M1 — NSPR 4.35 sobre a libsodos  ✅ (2026-11-03, núcleo)

**Objetivo:** a camada que o Firefox usa para tudo (threads, locks, tempo,
arquivos, rede, DNS, processos) compilando e rodando sobre o SODOS.

**Artefato:** `vendor/nspr-4.35/` + `sodos/nspr/` (config do alvo, `shim.c`,
`build.sh`, `link.sh`) + `PRTEST.EXE`. O NSPR é ligado **estático** no teste
por enquanto; virar `NSPR4.DLL` é trabalho do M2.

**Feito:**

1. `prcpucfg.h` próprio com a ABI x32 ILP32 e `md/_sodos.h` no lugar do
   `_linux.h`; um `md/prosdep.h` no include path seleciona o alvo.
2. Build no modo **pthreads** do NSPR: 56 fontes (`ptio.c`, `ptthread.c`,
   `ptsynch.c`, MD Unix, misc, linking, memory...), 0 erros.
3. `shim.c`: `sigaction` (no-op), `fcntl`/`lockf`, `select` sobre `poll`,
   `dlopen` sobre `k_dll_*`, `gethostbyname` via `k_dns`, `uname`,
   `getrlimit`/`getrusage`, `environ` e stubs de processo.
4. Sistema: `errno.h` completo, `struct stat` com `st_atime`/`st_ctime`,
   `fcntl.h`, `signal.h`, `sys/select.h`, `netinet/*`, `netdb.h`, `sys/wait.h`
   etc.; POSIXX preenche os campos novos do `stat`.
5. `PRTEST` cobre init/tempo, locks, condição com timeout, 4 threads com
   join/mutex/TLS e `PR_Open/Read/Write/GetFileInfo`.

**Critério:** `PRTEST` 16/16 no `soxrun` **e** dentro do SODOS no QEMU.
**Cumprido.**

**Fica para o M4:** `PR_Socket`/`PR_Poll` de rede, `PR_CreatePipe`,
`PR_NewProcess`/fork e a suíte oficial `pr/tests` (o `PRTEST` é o nosso
subconjunto). O `select` já existe, mas o kernel só tem uma conexão TCP.

**Risco remanescente:** o NSPR assume `poll` de vários fds; o `shim` emula com
a conexão única até o kernel ganhar a tabela de TCBs (M4).

## M1b — Preempção no kernel (paralelo, bloqueado por arquitetura)

**Objetivo:** troca de contexto por IRQ de timer, com pilha de kernel por
thread, fila de prontos e bloqueio real. É o que transforma a fibra do M0 em
thread de verdade.

**Bloqueio atual:** o kernel **não habilita interrupções** (`sti`); teclado,
PIT, ATA e rede são todos polling (`kernel.c` comenta "Sem sti"). Ligar o
timer exige: handler de IRQ0, salvar/restaurar contexto no frame da IRQ e
tornar o kernel reentrante (as rotinas de ATA/rede/GUI não são). É um projeto
de kernel por si só, não um alvo de uma sessão.

**Alternativa intermediária:** threads cooperativos de kernel com
`k_thread_yield`/`k_sleep`, que já funcionam com o modelo atual, mas não dão
a preempção que o thread pool do Gecko espera.

## M2 — Runtime C++ e mozglue

### M2a — Runtime mínimo + `.init_array`  ✅ (2026-11-03)

**Feito:**

- `c_compiler/link` agora reconhece `.init_array` (SHT_INIT_ARRAY) e monta uma
  tabela contígua no fim de DATA, exportando `__init_array_start` e
  `__init_array_end` como símbolos de seção (o loader soma a base).
- `crt0` chama `__sodos_ctors()` antes de `main` e `__sodos_dtors()` depois;
  a LIBCX tem versões vazias (import) e o runtime C++ define as fortes no
  próprio `.EXE`.
- `sodos/cxx/cxx_runtime.cpp`: `operator new/delete/new[]/delete[]`,
  `__cxa_atexit`/`__cxa_finalize` (destrutores globais), `__cxa_guard_*`
  (static local), `__cxa_pure_virtual`, `__stack_chk_fail`, `__dso_handle`.
- `sodos-cxx` (g++ `-mx32 -nostdinc -fno-exceptions -fno-rtti`); o WSL do
  projeto não tem `g++`, então `sodos/scripts/fetch-gxx.sh` baixa e extrai o
  pacote `.deb` em `vendor/gxx` (sem root).
- **`CXXTEST.EXE`: 7/7** no `soxrun` e no QEMU (construtor global, herança
  virtual, `new[]`, template, static local com guard, operador).

### M2b — Biblioteca padrão C++ (libc++ 18.1.8) — núcleo ✅

**Feito (2026-11-03):**

- libc++ 18.1.8 baixada para `vendor/libcxx-18.1.8` e compilada com
  `sodos-cxxlib` (`g++ -mx32 -std=c++20 -nostdinc -fno-exceptions -fno-rtti`).
- `__config_site` próprio: ABI `__1`, sem locale/wchar/threads, hardening
  desligado, `clock_gettime` disponível; `features.h` falso (não é glibc).
- Headers C do SODOS ganharam `extern "C"`; `stdint.h` ganhou
  `int_least*`/`INT64_C`; `max_align_t`, `timespec_get`, `uchar.h`,
  `setvbuf`/`setfpos`, `strcoll`/`strxfrm`, `aligned_alloc`.
- Runtime da libc++: `string.cpp`, `algorithm.cpp`, `stdexcept.cpp`,
  `exception.cpp`, `verbose_abort.cpp`, `new*.cpp`, `typeinfo.cpp`,
  `memory.cpp`, `functional.cpp`, `hash.cpp`, `vector.cpp`, `optional.cpp`,
  `any.cpp`, `chrono.cpp`, `system_error.cpp`, `error_category.cpp`.
- Stub vazio da seção `_lcxx_override` para o `new.cpp`; `strtod`/`libm` com
  SSE2 compilados no `.EXE` (a LIBCX é `-mno-sse`).
- **`STLTEST.EXE`: 23/23** no `soxrun` e no QEMU: `vector`, `string`
  (find/substr/to_string), `map`, `unordered_map` (hash), `set`, `list`,
  `deque`, `array`, `tuple`, `unique_ptr`/`shared_ptr`, `function`/lambda,
  `numeric`, `string_view`, `optional`, `variant`, `chrono`, `stoi`/`stof`.
- **`THRTEST.EXE`: 4/4**: `std::thread` + `join`, `std::mutex`/`lock_guard`,
  `std::condition_variable::wait` com predicado, `std::atomic::fetch_add` e
  `std::this_thread::sleep_for` sobre os pthreads cooperativos da `libsodos`.

**Ainda pendente no M2b:**

- iostreams (`iostream.cpp`, `ios.cpp`, `locale.cpp`) e locale.
- exceções/RTTI (exigem libc++abi + unwinder DWARF; hoje `-fno-exceptions`).
- `std::filesystem` (precisa de `statvfs`, symlinks, `mmap` de arquivo).

**Critério do M2 (completo):** um binário C++ com `std::vector`, `std::string`,
`std::thread`, `std::mutex`, `static` com construtor e exceção roda no
`soxrun` e no QEMU. Hoje o critério vale sem `std::thread` e sem exceção.

## M3 — SpiderMonkey (shell `js`)

**Objetivo:** primeiro componente grande do Firefox rodando: o motor JS.

**Trabalho:**

- `js/src` com `--disable-jit` (ou JIT com W^X depois do `mprotect` real),
  `--without-intl-api` no começo;
- alocador: `--disable-jemalloc`, `mmap` anônimo;
- `js` shell (não o browser) como `JS.EXE`;
- testes: `js -e 'print(1+1)'`, `test262` reduzido, `JSON`, `Date`, `RegExp`.

**Critério:** `JS.EXE` avalia um script com objetos, arrays, closures e
exceções; o mesmo script roda no `js` do host com a mesma saída.

## M4 — Rede de verdade: NSS + múltiplos sockets

**Objetivo:** HTTPS com NSS e mais de uma conexão simultânea.

**Trabalho:**

- kernel: tabela de TCBs, portas, `poll`/`select` por socket, timeout,
  `shutdown`, DNS concorrente;
- NSS: `nss` + `nspr` + `freebl` (AES, SHA, RSA/ECDHE) e a loja de
  certificados;
- `PR_Poll` real, `PR_GetAddrInfoByName`, HTTP/1.1 e HTTP/2 (nghttp2).

**Critério:** `prtest` de socket com 8 conexões simultâneas; um GET HTTPS
com certificado verificado pelo NSS dentro do SODOS.

## M5 — Gecko: gfx, widget e layout

**Objetivo:** uma janela do SODOS desenhando HTML/CSS do Gecko.

**Trabalho:**

- backend `widget/sodos`: janela `k_gui_open`, eventos `k_gui_event`,
  `nsWindow`/`nsLookAndFeel` mínimos;
- `gfx`: `cairo`/`pixman` software, `nsIDeviceContextSpec`, fontes TTF do
  `fonts/` (harfbuzz depois);
- `layout`/`dom`/`style`/`content` sem e10s (processo único), sem sandbox;
- `omni.ja` desempacotado ou lido do FAT16 (sem mmap de arquivo ainda).

**Critério:** uma página local (`file:///D:/T.HTM`) com CSS, imagem e JS
aparece na janela do SODOS, com scroll e clique em link.

## M6 — Browser Firefox

**Objetivo:** a UI do Firefox (chrome) e navegação real.

**Trabalho:**

- `browser/` com `--enable-application=browser`; XUL/HTML do chrome;
- perfil em `D:\FIREFOX`, cache, downloads, histórico;
- TLS/NSS, cookies, redirecionamento, formulários;
- teclado ABNT2 e mouse (o WIN já entrega os eventos).

**Critério:** abrir `https://www.netsurf-browser.org/` no `FIREFOX.EXE`
dentro do SODOS, com imagens, e navegar por um link.

## M7 — Compilar dentro do SODOS (longo prazo)

**Objetivo:** o pedido literal “compilar o Firefox no sistema”.

Hoje o SODOS tem `CC.EXE`/`LINK.EXE` (C, sem C++), não tem Python, Rust,
clang nem `mach`. Para isso seria preciso portar:

- clang/LLVM ou um gcc C++ para x32+SODOS;
- Python 3 (para `mach`/`configure`) e Node;
- rustc/LLVM para o alvo `sodos` (Tier 3 custom);
- um filesystem com arquivos grandes e memória suficiente (o build usa
  GBs de RAM/disco).

É um projeto do tamanho do port inteiro. A rota realista é **cross-build no
host** (WSL) e levar para o SO o binário; o M7 fica como objetivo separado.

## Ordem prática

```
M0 (feito) -> M1 (NSPR) -> M2 (C++) -> M3 (SpiderMonkey) -> M4 (NSS/rede)
                                  \-> M1b (preempção no kernel, em paralelo)
M5 (gfx/widget) -> M6 (browser) -> M7 (build no SO, opcional/longo)
```

## Critérios gerais (valem para todas as fases)

1. `make -C src13` continua gerando `os.img` sem erros.
2. `make -C firefox test` continua 100% verde.
3. Nenhuma API `k_*` nova entra sem linha no `KERNEL.IMP` e no `soxrun`.
4. Todo teste novo roda no `soxrun` **e** no QEMU (quando envolver kernel).
5. `docs/` é atualizado na mesma mudança.
