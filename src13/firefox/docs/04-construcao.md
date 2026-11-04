# 04 — Construção, testes e depuração

## Pré-requisitos

No WSL (igual ao resto do projeto): `gcc` com suporte a `-mx32`, `make`,
`nasm`, `qemu-system-x86_64`, `mtools` (para copiar para o `disk.img`).
O `c_compiler` precisa estar compilado:

```sh
cd src13/c_compiler && make        # gera build/link, build/soxrun, out/*.DLL
```

## Comandos

```sh
cd src13

make firefox                 # build/out/SODOS.DLL
make firefox-test            # FBOOT.EXE + soxrun
make firefox-test ARGS=net   # + GET HTTP
make firefox-nspr            # NSPR 4.35 (vendor/nspr-4.35)
make firefox-nspr-test       # PRTEST.EXE + soxrun
make firefox-cxx-test        # runtime C++ + CXXTEST.EXE
make firefox-libcxx-test     # libc++ + STLTEST.EXE
make firefox-disk            # copia os EXEs/DLLs para D:

# equivalente:
make -C firefox all
make -C firefox test
make -C firefox nspr
make -C firefox nspr-test
make -C firefox cxx-test
make -C firefox libcxx-test
make -C firefox clean
```

O `make nspr` pressupõe `vendor/nspr-4.35` (rode `./setup.sh` na primeira vez).
O `build.sh` limpa `build/nspr/obj` e recompila as 56 fontes; o `link.sh` liga
o `PRTEST.EXE` com o linker do SODOS e copia as DLLs para `build/nspr/run`.

## C++ (M2a)

O WSL do projeto tem `gcc` mas normalmente **não tem `g++`/`cc1plus`**. O
`sodos/scripts/fetch-gxx.sh` baixa o `.deb` do `g++-13-x86-64-linux-gnu` e
extrai em `vendor/gxx` (sem root); o `sodos-cxx` usa esse compilador quando
não acha `g++` no PATH. Para refazer:

```sh
sh sodos/scripts/fetch-gxx.sh
make cxx-test
```

O build é `g++ -mx32 -nostdinc -fno-exceptions -fno-rtti` e o link é o
`link` do SODOS (sem libstdc++). O `crt0` chama `__sodos_ctors()` antes de
`main`; os construtores vêm da tabela `.init_array` que o linker monta.
Se um construtor não rodar, confira no `link /map` se a seção
`.init_array` existe e se `__init_array_start/end` foram definidos.

## libc++ (M2b)

A libc++ é baixada por `sodos/scripts/fetch-libcxx.sh` (padrão 18.1.8) e
compilada por `sodos/libcxx/build.sh`:

```sh
sh sodos/scripts/fetch-libcxx.sh
make libcxx-test
```

O build compila `string.cpp`, `algorithm.cpp`, `memory.cpp`, `chrono.cpp`,
`thread.cpp`, `mutex.cpp`, `condition_variable.cpp` etc. com
`-std=c++20 -D_LIBCPP_BUILDING_LIBRARY`, gera `build/libcxx/LIBCXX.LIB`
(estática SOX) e liga em `STLTEST.EXE` (containers) e `THRTEST.EXE`
(threads). A saída do compilador fica em `build/libcxx/build.log`; o terminal
só mostra erro ou o resultado dos testes (`make libcxx-test` compila uma vez e
depois usa `build.sh --run-only`). Se um símbolo `std::` ficar indefinido, o
arquivo correspondente da libc++ ainda não está na lista `SRCS` do `build.sh`
(ex.: `iostream.cpp`, `locale.cpp`, `atomic.cpp` são fases seguintes).

## O que o build faz

1. `sodos/sodos-cc` compila cada `sodos/port/*.c` com:
   `-mx32 -msse2 -mno-red-zone -ffreestanding -fno-pic -fno-pie
   -nostdinc -isystem sodos/include -isystem ../c_compiler/include`.
2. `link /dll /exportall` junta os objetos com `LIBCX.DLL` e `KERNEL.IMP` e
   gera `SODOS.DLL` (SOX, entry `DllMain`).
3. `link` junta `x_crt0.o` + `t_fboot.o` + `SODOS.DLL` + `LIBCX.DLL` +
   `KERNEL.IMP` e gera `FBOOT.EXE`.
4. `soxrun -C build/run FBOOT.EXE` roda no Linux, sem QEMU.

## Rodar dentro do SODOS (QEMU)

```sh
# com os.img e disk.img prontos
cd src13
make firefox-disk   # copia SODOS.DLL/FBOOT.EXE/LIBCX.DLL para D: (fecha o QEMU antes)
make run
```

O alvo usa a partição do `disk.img` em `@@512` (MBR com a FAT16 começando no
setor 1). Para copiar à mão, o mesmo offset:

```sh
mcopy -o -i disk.img@@512 firefox/build/out/FBOOT.EXE ::/FBOOT.EXE
mcopy -o -i disk.img@@512 firefox/build/out/SODOS.DLL ::/SODOS.DLL
mcopy -o -i disk.img@@512 c_compiler/out/LIBCX.DLL    ::/LIBCX.DLL
```

No prompt (ou pela API HTTP em `http://127.0.0.1:8797`):

```
D:\FBOOT.EXE
D:\FBOOT.EXE net
type D:\LOG\FIREFOX.LOG
```

As DLLs são procuradas na raiz da unidade (`D:\`). O log fica em
`D:\LOG\FIREFOX.LOG` e é criado sozinho.

## Problemas comuns

### `link: simbolo indefinido: k_yield` (ou outro `k_*` novo)

O `KERNEL.IMP` está velho. Ele é gerado a partir de `kernel/kexports.c`:

```sh
make -B -C c_compiler build/soxrun out/KERNEL.IMP
```

### O programa cai no primeiro `sodos_sleep_ms` / `k_yield`

O `soxrun` tem uma tabela própria de exports (`ktab` em
`tools/soxrun.c`). Se o kernel ganhou um `k_*` e o `soxrun` não, o loader
deixa o slot nulo e a chamada salta para 0. Adicione a entrada em `ktab` e
recompile o `soxrun` com `-B`.

### O `make` diz “Nothing to be done” mas o código mudou

Clock skew entre Windows e WSL. Use `make -B` ou toque no arquivo.

### `soxrun: nao achei LIBCX.DLL`

O `soxrun -C <dir>` procura as DLLs no diretório do `.EXE`. Copie
`LIBCX.DLL`, `SODOS.DLL` e `KERNEL.IMP` para lá (o alvo `test` faz isso).

### Crash dentro de uma thread

Rode com `gdb`:

```sh
gdb -batch -ex run -ex bt --args build/../c_compiler/build/soxrun -C build/run FBOOT.EXE
```

Lembre que o código é x32: `info registers`, `x/8i $pc` e `bt` funcionam, mas
`p sizeof(void*)` é 4. Para inspecionar o SOX, use `link /map` no link ou o
script `sodos/scripts/soxdump.py`.

### A pilha da thread estourou

O padrão é 256 KB. Aumente com `pthread_attr_setstacksize` (mínimo 16 KB) ou
`DEFAULT_STACK` em `sodos_thread.c`. Não há guard page: o estouro corrompe o
heap silenciosamente.

## Depurar o kernel com os exports novos

```sh
cd src13
make -B            # reconstrói kernel.bin/os.img com k_yield/k_net_pending
nm kernel.elf | grep -E 'k_yield|k_net_pending'
grep -E 'k_yield|k_net_pending' c_compiler/out/KERNEL.IMP
```

Se o símbolo está no `kernel.elf` mas não no `KERNEL.IMP`, rode o alvo do
`c_compiler` de novo; o `.IMP` é gerado por `sed` em `kexports.c`.

## Convenções para código novo na `libsodos`

- Prefixo `sodos_` para API nova; `pthread_*`/`poll`/`mmap`/`sem_*` só para
  POSIX que ainda não exista em outra DLL.
- Nada de `printf` em caminho quente; use `sodos_log` (que tem nível).
- Toda função que pode esperar precisa ceder (`sodos__block/park/sleep`).
- Não chame `k_tcp_*` de dentro do port: passe por `sodos_net_*`.
- Comente em português, como o resto do `src13`.
