# Port do Dart (e do Flutter web) para o SODOS

Objetivo: compilar apps Flutter web **dentro do SODOS**. O Flutter web em si já
roda no Firefox do port (`firefox/docs/07-webgl.md`); faltava a toolchain.

Versão: **Dart 3.13.2**, a do Flutter 3.47.2 instalado no host
(`C:\dev\programs\flutter\flutter`). Fonte em `~/dartsrc/sdk` (WSL, fora do git).

## Estado (2026-11-17)

| Marco | Resultado no SODOS (QEMU, KVM, 4 CPUs) |
|---|---|
| Shim epoll/timerfd/eventfd | `EPOLLTST.EXE` 26/26 |
| D0 `DARTAOT.EXE` | `hello.dart` AOT: `print`, `Future.delayed` (151 ms para 150), `Isolate.run` em outra thread, `stdout` |
| D2 `dart2js` | `main.js` **idêntico** ao do host (2 s) |
| D3 app Flutter web | `flutter create` padrão: fase 1 (fontes → `app.dill`, 110 s) e fase 2 (→ `main.dart.js`, 16 s); `main.dart.js` **idêntico byte a byte** ao do `flutter build web` do Windows |

Não feito ainda: montar o bundle `build/web` (index.html, flutter.js, assets,
CanvasKit) no SODOS e abrir no Firefox de lá; `dart run`/JIT; `flutter_tools`.

## Estratégia

O alvo compila **como Linux x86-64** com o toolchain LP64 do port do Firefox
(`firefox/toolchain/bin/x86_64-sodos-g++64`), e o SODOS oferece a ABI glibc pela
libsodos (`firefox/sodos/port/`). Usa-se o **`dartaotruntime_product`**
(`DART_PRECOMPILED_RUNTIME`, sem JIT nem compilador na VM): o SDK distribui as
ferramentas como snapshots AOT, e o mesmo runtime roda o `dart2js`.

Os snapshots precisam ser **linux-x64 do mesmo commit** (o do Windows usa a ABI
de chamada do Windows; outra versão tem outro formato). `build-dart.sh sdk`
compila esse SDK de referência no WSL.

## Arquivos

| Caminho | O quê |
|---|---|
| `scripts/fetch-dart.sh` | depot_tools + `gclient sync` do Dart 3.13.2 em `~/dartsrc` (~7 GB) |
| `scripts/build-dart.sh` | aplica `patches/`, gera `out/SodosX64` (GN) e liga `build/DARTAOT.EXE`; `sdk` faz também o SDK linux-x64 de referência |
| `toolchain/bin/x86_64-sodos64-*` | `x64_toolchain_prefix` do GN: gcc/g++ LP64 do SODOS + `-I port/include -include sodos_dart_compat.h`; no link, `build/port/*.o` |
| `port/include/sodos_dart_compat.h` | `std::atomic_ref` (a libc++ do port é a 18), funções `*at`, constantes do glibc, renomes `sodos_*` |
| `port/sodos_epoll.c` | `epoll`, `timerfd`, `eventfd` e fds de diretório (faixa 5000) sobre o `poll` do port |
| `port/sodos_dart_libc.c` | `openat64`/`fdopendir`/`fstatat`... com dirfd, `getcwd` em estilo `/d/...`, `inet_pton`, pilha por thread, `getrandom` pelo `syscall` |
| `patches/dart-0001-sodos.patch` | `ThreadId` = ponteiro (`pthread_t` do port); snapshot pelo loader de ELF próprio da VM (sem `dlopen`) |
| `scripts/qemu-test.sh` | sobe o SODOS numa cópia do `disk.img`, põe arquivos em `D:\` e roda comandos por `POST /exec` |
| `scripts/stage-flutter-web.py` | monta app + pacotes (só os fontes do `app.dill.deps`) + SDK web com `package_config.json` reescrito para `/d/FL` |
| `tests/` | `t_epoll.c`, `hello.dart`, `fl_hello/` (app de `flutter create`) |

Mudanças no port compartilhado (valem também para o Firefox):

- `sodos_mmap.c`: `mmap` de **arquivo** com `MAP_FIXED` dentro de uma reserva não
  copiava o conteúdo (agora copia, como sem `MAP_FIXED`);
- `posix-lite`: `getcwd(NULL, 0)` aloca (POSIX.1-2008); `fstat` de 0–2 é
  `S_IFCHR` (concorda com `isatty`);
- `sodos_thread.c`: `sodos_thread_stack()` (limites da pilha de cada k_thread);
- `sodos_pipe.c`/`sodos_poll.c`: ganchos fracos para a faixa 5000; `SOCK_FD` com
  teto 5000; stubs de `epoll`/`eventfd` fracos (no Firefox continuam `ENOSYS`).

## Como reproduzir (WSL)

```sh
sh dart/scripts/fetch-dart.sh            # uma vez
sh dart/scripts/build-dart.sh sdk        # DARTAOT.EXE + SDK linux-x64 de referência
```

Dart puro:

```sh
SDK=~/dartsrc/sdk/out/ReleaseX64/dart-sdk
$SDK/bin/dart compile aot-snapshot dart/tests/hello.dart -o dart/build/test/HELLO.AOT
sh dart/scripts/qemu-test.sh dart/build/DARTAOT.EXE dart/build/test/HELLO.AOT -- 'D:\DARTAOT.EXE D:\HELLO.AOT a b'
```

App Flutter web (depois de um `flutter build web` no host, que gera o
`.dart_tool/flutter_build/*/main.dart` e o `app.dill.deps`):

```sh
S=dart/build/flfs
python3 -I dart/scripts/stage-flutter-web.py dart/tests/fl_hello $S/FL /d/FL
mkdir -p $S/DART/snapshots && cp dart/build/DARTAOT.EXE $S/DART/
cp ~/dartsrc/sdk/out/ReleaseX64/dart-sdk/bin/snapshots/dart2js_aot.dart.snapshot $S/DART/snapshots/
TIMEOUT=1800 sh dart/scripts/qemu-test.sh $S/DART $S/FL -g FL/build/main.dart.js \
  -c 'D:\DART\DARTAOT.EXE /d/DART/snapshots/dart2js_aot.dart.snapshot @/d/FL/d2j-cfe.args' \
  -c 'D:\DART\DARTAOT.EXE /d/DART/snapshots/dart2js_aot.dart.snapshot @/d/FL/d2j-js.args'
# saida em dart/build/qemu-out/main.dart.js
```

## Detalhes que custaram

- **Caminhos**: o Dart acha que está no Linux e só `/...` é absoluto. O
  posix-lite aceita o estilo MSYS (`/d/X` = `D:\X`), e o `getcwd` do Dart devolve
  esse estilo, então `Uri.base` e caminhos relativos ficam coerentes.
- **Linha de comando**: o shell do SODOS aceita 240 caracteres, e a URL do
  `GET /exec` do httpd é ainda mais curta. Argumentos longos vão em `@arquivo`
  (o `dart2js` lê um por linha); o `qemu-test.sh` usa `POST /exec`.
- **Pilha**: o `pthread_attr_getstack` do port só conhecia a pilha do processo;
  os isolates rodam em k_threads e o `ASSERT(HasStackHeadroom())` disparava.
- **Entropia**: `getrandom` é o PRNG do posix-lite, sem entropia do kernel.
  Serve para compilar; não serve para criptografia (`Random.secure`).

## Próximos passos

| Marco | Falta |
|---|---|
| D3b bundle web | bundle em `D:\FLWEB` (o `build/web` do app com o `main.dart.js` feito no SODOS). No QEMU (2026-11-17): `file:///d/FLWEB/index.html` carrega (o Firefox quer `/d/`, não `D:/`), mas a página fica em branco, provavelmente porque o Flutter usa `fetch()` e o Firefox não faz `fetch` de `file://`. Falta: servir por HTTP (o kernel não aceita conexões de entrada; o teste por `10.0.2.2` foi interrompido) e ligar `devtools.console.stdout.content` para ver o console da página. A 1ª execução do Firefox novo num perfil antigo aborta (`mozalloc_abort`); a 2ª sobe |
| D1 `dart:io` | testar arquivos, diretórios, timers e `Process.*` com erro limpo |
| D4 VM completa (JIT) | `dart run`, `flutter_tools`; precisa de processos (`fork`/`exec`) e de `listen`/`accept` no kernel |
