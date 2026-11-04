# Toolchain `x86_64-sodos`

Driver que faz o papel de `x86_64-sodos-gcc`/`g++`: compila C/C++ na ABI
**x32** do SODOS e liga uma imagem **SOX** com o `link` do `c_compiler`.
Não é um gcc cruzado completo (não compila libgcc/crt/ld para o alvo), mas é
o suficiente para o `configure` do Firefox e para programas C/C++ simples.

## Uso

```sh
export PATH=$PWD/toolchain/bin:$PATH
x86_64-sodos-gcc hello.c -o D:\\HELLO.EXE
x86_64-sodos-g++ hello.cpp -o D:\\HELLO.EXE
x86_64-sodos-gcc64 hello64.c -o D:\\HELLO64.EXE   # SOX2/LP64 (C)
x86_64-sodos-g++64 hello.cpp -o D:\\HELLO64.EXE   # SOX2/LP64 (C++/libc++64)
```

No configure do Firefox:

```sh
CC=x86_64-sodos-gcc CXX=x86_64-sodos-g++ \
  ./configure --target=x86_64-sodos
```

## O que ele faz

- `-c`/`-E`/`-S`/`-M` delegam para `sodos-cc`/`sodos-cxxlib` (`gcc -mx32`,
  headers do SODOS/libc++).
- No link, compila cada `.c`/`.cpp` para um `.o` temporário e chama
  `c_compiler/build/link` com `x_crt0.o`, `LIBCX.DLL`, `KERNEL.IMP`,
  `POSIXX.DLL`, `SODOS.DLL` e, em C++, `cxx_runtime.o` + `LIBCXX.LIB`.
- `-shared` vira `/dll /exportall` no `link` (experimental).
- `-Wl,--version` responde `GNU ld (SODOS link) 1.0` para o configure
  reconhecer um linker; `-dumpmachine` responde `x86_64-pc-sodos`.
- No modo `-E`, define `__SODOS__` e remove `__linux__` (é assim que o
  configure descobre o kernel); compilar/ligar mantém `__linux__` porque a
  libc++ o usa para detectar pthreads.

## ABI LP64 (SOX2)

Além do x32, o SODOS agora tem o começo da ABI LP64:

- `c_compiler/link/link64.c` liga objetos `gcc -m64` em uma imagem **SOX2**;
- `c_compiler/lib/crt0_64.S` + `lib/hello64.c` geram `out/HELLO64.EXE`;
- o loader do kernel aceita SOX1 (x32) e SOX2 (LP64) no mesmo SO.

```sh
make -C c_compiler hello64
# copie HELLO64.EXE para o D: e rode D:\HELLO64.EXE
```

Ainda não há `LIBCX64`/libc++64; o hello usa só `k_write`/`k_exit` (e `k_dbg`
para a serial). O `x86_64-sodos-gcc64` compila C LP64 e liga SOX2 pelo
`link64`; `g++64` existe e liga a libc++64 estática (`STLTEST64`/`G++64`).

Para usar `printf`/`malloc` em 64-bit, o alvo do `c_compiler` compila a libc
estática:

```sh
make -C c_compiler hello64c   # out/HELLO64C.EXE
```

## Limites (importantes)

- A saída é **SOX**, não ELF; o loader do SODOS carrega SOX em endereço 0.
- A ABI é **x32/ILP32**: ponteiros e `long` de 4 bytes sobre x86_64. O Firefox
  assume LP64 em `x86_64`; o configure chega a
  `checking for 64-bit OS... no` e para em
  `The target compiler does not agree with configure about the target bitness`.
- Sem `libgcc`, `crtbegin/crtend`, `-shared` real, TLS estático nem exceções.
- `-lm` é ignorada; `math.c`/`fpconv.c` com SSE2 entram no link.
- Bibliotecas de terceiros (NSS/ICU/GTK...) precisam ser portadas para SOX.

## Teste

```sh
make -C .. toolchain    # ou sh check.sh
```
