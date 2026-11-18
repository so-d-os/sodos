# Threads do Firefox em várias CPUs (FFTHREADS)

Desde 2026-11-15 as pthreads do Firefox (LP64) são **k_threads do kernel**
(`kernel/kthread.c`) e rodam nas outras CPUs. Antes, `sodos_thread.c` fazia
pthreads cooperativas em user-level: todas as threads (principal, compositor,
socket, JS helpers, compilação de wasm, decodificadores, rayon do Stylo/WebRender)
dividiam o "mundo" preso na CPU0, e o TOP/`smp` mostrava as APs paradas.

## O switch

| `SET FFTHREADS=` | Efeito |
|---|---|
| (não definido) ou `1` | k_threads quando há mais de uma CPU (padrão) |
| `0` | volta às pthreads cooperativas (tudo na CPU0, como antes) |
| `2` | força k_threads mesmo com uma CPU (teste) |

O modo é decidido uma vez, na primeira chamada de pthread
(`sodos__sched_init`), e sai na serial:

```
[ffthr] pthreads = k_threads (SET FFTHREADS=0 desliga)
[ffthr] pthreads cooperativas
```

Com k_threads, `sysconf(_SC_NPROCESSORS_*)`, `get_nprocs` e
`sched_getaffinity` devolvem o número de CPUs do kernel (no cooperativo,
1); é isso que faz o SpiderMonkey, o Stylo e o rayon do WebRender criarem
uma thread por CPU.

## Como funciona

- **Criação** (`sodos_thread.c`): `pthread_create` aloca a `sodos_thread`, um
  bloco TLS próprio (`sodos__tls_new`) e chama `k_thread_create` (pilha pedida
  + 32 KB, mínimo 128 KB: IRQs e código do kernel usam a mesma pilha). A
  thread entra em `kt_entry`, aponta o FS para o seu TLS (`k_thread_tls`) e
  guarda o ponteiro da `sodos_thread` no TCB, em `%fs:0x40`
  (`pthread_self`/`sodos__self` leem dali; a principal tem 0 e usa
  `main_thread`).
- **Fim**: destrutores de chave, `finished=1`, `k_thread_exit`. `pthread_join`
  faz `k_thread_join` (só depois disso o TLS é liberado: o código do kernel
  que a thread roda até o fim lê o guarda de pilha em `%fs:0x28`). Threads
  destacadas vão para uma lista e são colhidas no próximo `pthread_create`.
- **Sincronização** (`sodos_sync.c`): sem mudar o layout dos tipos do
  `pthread.h` (o Firefox foi compilado com ele):
  - mutex: 32 bits baixos de `wait_head` = futex 0/1/2 (livre, preso, preso
    com espera); `owner`/`count` dão recursão e `EPERM`;
  - cond: `head` = sequência, `tail` = quantas esperam; o
    `pthread_cond_wait` nunca devolve erro por acordar sem sinal (a libc++
    sem exceções aborta em qualquer retorno ≠ 0);
  - sem: `value` é o próprio futex; `pthread_once` usa um futex por once;
  - o futex da libc (Rust std, parking_lot) vai direto a `k_futex_wait/wake`.
- **Lock grande do port** (`sodos_big_lock`, recursivo): protege o que o
  port tem de estado global — fds da posix-lite, soquetes, pipes, shm, mmap,
  log. Cada função pública desses arquivos começa com `SODOS_BIG_GUARD`
  (`posix.c` usa `PX_GUARD`, com símbolos fracos). `sodos_sleep_ms`/`sched_yield`
  soltam o lock enquanto a thread espera (o `poll` e os laços de soquete
  dormem com ele na mão).
- **libc LP64** (`c_compiler/lib`): `malloc`/`free` com lock (futex), stdio
  com lock recursivo (dono = `k_thread_self`), `errno` por thread
  (`__sodos_errno` no port) e `clock_gettime` sem o `sched_yield` a cada 64
  chamadas.
- **Kernel**: `KT_MAX` 64 → 256; `k_ms`, `k_futex_wait` e `k_thread_yield`
  deixaram de pegar o BKL no thunk quando quem chama é uma thread (o futex já
  soltava logo em seguida; o relógio vem de `net_ms_fast`, ticks do IRQ0 +
  ajuste). O mundo continua pegando o BKL nessas chamadas.
- **Janela (`k_gui_*`) chamada por thread** (2026-11-17): a thread do
  compositor do Gecko chama `k_gui_pixels` de outra CPU. O kernel escolhia a
  janela por `gui_ix`/`task_cur`, que são do mundo: com outro programa
  rodando na CPU0 os pixels iam para a janela dele, ou eram descartados (a
  janela do Firefox piscava com faixas velhas). Agora `gui_thread_enter`
  (`video/visual.c`, chamado em `kernel/kexports.c`) aponta para a janela
  aberta pelo programa da thread (`gui_win.pid`) enquanto ela segura o BKL.
  `k_gui_open`/`k_gui_close` continuam só do programa principal.
- **posix-lite**: `FD_MAX` 64 → 256 em LP64. Com as threads em paralelo o
  SQLite mantinha ~60 conexões abertas (places/favicons com 12 cada) e o
  `open("/dev/urandom")` do `nsID::GenerateUUID` falhava (abort). Quando a
  tabela esgota, a serial lista quem está segurando os fds
  (`[posix] fds esgotados: ...`).

A thread principal continua sendo uma tarefa do WIN no mundo (CPU0): quando
ela espera um futex, o kernel roda o desktop (`gui_coop_pause`), então o WIN
não trava.

## Medições (QEMU/KVM, `-smp 4 -m 8192`, 2026-11-15)

Mesmo FIREFOX.EXE, mesmo disco, `-snapshot`; tempo até a tela parar de mudar
(capturas a cada 1 s).

| | cooperativo (`FFTHREADS=0`) | k_threads |
|---|---|---|
| Abrir o Firefox até o Google pronto | 8,2 s | 8,2 s |
| `sistema.c8x.app` (Flutter/wasm) até a tela de login | 25,8 s | **12,6–13,7 s** |
| Uso das APs (`smp`) durante a carga | 0% | 20–30% cada |

O Google não muda: a abertura é limitada pela rede e pela thread principal.
Em repouso, com a página parada, as APs ficam em ~15% cada: threads girando
no BKL (o mundo segura o BKL ~75% do tempo dentro do laço do WIN) e o
`poll` da thread de soquete, que dorme em fatias de 4 ms e lê a placa a cada
volta.

### Digitação em app Flutter (C8X, campo CPF)

Tempo da tecla até o dígito aparecer (screendump pelo QMP, que não custa nada
ao guest; o `/win/printscreen` do httpd comprime PNG na CPU0 e infla a medida
para 8–20 s):

| | cooperativo | k_threads |
|---|---|---|
| janela 1600×820 | 1,0–1,8 s | ~0,9 s |
| janela maximizada | — | 1,3–2,3 s |
| busca do Google (HTML comum) | 0,1–1 s | — |

A causa não é o caminho da tecla no port: **não há WebGL** (`Failed to create
WebGL context`, `FEATURE_FAILURE_EGL_LOAD_3`), então o Flutter cai no CanvasKit
em software e rasteriza cada quadro em wasm na thread principal. O custo cresce
com a área da janela, e o caret piscando (um quadro a cada 500 ms) deixa a CPU0
em ~93% em código JIT mesmo parado; as APs ficam ociosas. O JIT do SODOS está
na velocidade do host (microbenchmark de loop: wasm 333 ms vs 300 ms no
Firefox do Linux, JS 304 vs 298 ms), então o ganho de verdade é ter WebGL (GL
em software, p.ex. Mesa llvmpipe, que rasteriza em threads nas APs).

Com o DevTools aberto, clicar no app Flutter não tira o foco do teclado da
busca do Inspector (o Flutter cancela o `pointerdown`); feche o DevTools antes
de digitar.

## Limites conhecidos / próximos passos

- `gettid()` devolve 1 para todas as threads, então o SpiderMonkey usa a base
  da pilha principal em toda thread (`GetNativeStackBaseImpl`). Já era assim
  no cooperativo; as pilhas das k_threads ficam acima da principal e o limite
  de recursão nunca dispara nelas. Corrigir pede `gettid` por thread e
  `pthread_getattr_np`/`pthread_attr_getstack` com a pilha real.
- BKL: as threads ainda giram esperando o mundo (`bkl_spin` no `smp`).
  Próximo ganho: o laço ocioso do WIN soltar o BKL, ou as threads dormirem
  (hlt + IPI no `kt_bkl_breathe`) em vez de girar.
- Um panic do Rust no rasterizador de glifos do WebRender
  (`FontContext::add_font`, thread rayon) apareceu uma vez em ~10 aberturas.
  A thread morre sozinha (`exit` numa k_thread encerra só ela) e o Firefox
  continua. O motivo agora sai na serial (`[stk] motivo: ...`).
- O crash log (`D:\LOG\FIREFOX.LOG`) não é gravado quando o abort acontece
  numa k_thread; a pilha e o motivo saem só na serial.

## Teste

```
# kernel novo (KT_MAX 256, k_ms/futex sem BKL) + FIREFOX.EXE religado
make -C src13 os.img
sh src13/firefox/sodos/scripts/link-firefox64.sh   # FIREFOX_OUT=... para outro destino
```

No SODOS: `win`, abrir o Firefox, carregar uma página pesada e olhar `smp`
(ou o TOP): as APs aparecem com uso. `SET FFTHREADS=0` antes de abrir o
Firefox para comparar.
