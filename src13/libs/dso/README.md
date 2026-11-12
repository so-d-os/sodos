# Bibliotecas dinamicas (ELF .so)

Programas LP64 podem ser ligados a bibliotecas compartilhadas com os nomes
padrao do Linux. Quem carrega e o kernel (`kernel/elf.c`): ele le o executavel
PIE, traz o que estiver em `DT_NEEDED`, reloca tudo e roda os construtores.

| Arquivo | Conteudo |
|---|---|
| `libc.so.6` | libc, arquivos (posix-lite), pthreads/mmap/rede (firefox/sodos/port), `dlopen` |
| `libm.so.6`, `libpthread.so.0`, `libdl.so.2`, `librt.so.1`, `libutil.so.1` | vazias: tudo esta na `libc.so.6`, como na musl; existem para `-lm` e `DT_NEEDED` resolverem |
| `libc++.so.1`, `libc++abi.so.1` | libc++ 18 (`make cxx`) |
| `libkernel.so` | so para o linker: os simbolos `k_*` saem do proprio kernel |
| `crt1.o`, `crtdso.o` | entrada do executavel e `__dso_handle` de cada modulo |

## Compilar

```
make            # build/sysroot/lib
make cxx        # libc++ (precisa de firefox/vendor/libcxx-18.1.8)
make check      # compila os testes e roda no QEMU (tests/run-qemu.sh)
```

`make disk-files` em `src13` copia as `.so` para `D:\LIB`.

## Ligar um programa

```
SODOS_DYNAMIC=1 x86_64-sodos-gcc64 -fPIC prog.c -o PROG.EXE -lm
SODOS_DYNAMIC=1 x86_64-sodos-gcc64 -fPIC -shared -Wl,-soname,libplug.so.1 plug.c -o libplug.so.1
```

Sem `SODOS_DYNAMIC=1` o driver continua gerando o executavel estatico (SOX2).
O codigo de quem entra em `.so` ou em executavel dinamico precisa de `-fPIC`.

## Onde o kernel procura

1. `DT_RUNPATH`/`DT_RPATH` do objeto que pede (`$ORIGIN` vale);
2. a pasta de quem pede e a pasta do executavel;
3. `\LIB` e `\USR\LIB` na unidade do executavel, na corrente, em `D:` e em `C:`.

Um nome com mais de 8.3 (`libpthread.so.0`) depende dos nomes longos da FAT.

## O que funciona e o que nao

- `dlopen`, `dlsym` (`RTLD_DEFAULT`, `RTLD_NEXT`), `dladdr`, `dl_iterate_phdr`,
  `RTLD_GLOBAL`/`RTLD_LOCAL`, hash SysV e GNU, `IRELATIVE`, `RELR`, copia de dados.
- TLS: todo modulo tem lugar fixo no bloco estatico da thread, tambem os
  abertos com `dlopen` enquanto couberem na folga de 64 KB. O driver compila
  com `-ftls-model=initial-exec`; o modelo general-dynamic (`__tls_get_addr`)
  tambem resolve.
- `dlclose` nao descarrega: a biblioteca fica ate o processo terminar.
- Nao ha versao de simbolo (`GLIBC_2.x`) nem ligacao preguicosa: tudo e resolvido
  na carga, e um simbolo que falta impede o programa de iniciar.
- So LP64. Programas x32 (SOX1) continuam com as DLLs SOX.
