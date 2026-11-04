# libc++64 (SOX2/LP64)

libc++ 18.1.8 compilada com `-m64` para o SODOS, sem locale/exceções ainda.
Containers, algoritmos, `std::thread`, `mutex`, `condition_variable` e
`atomic` rodam no QEMU:

```
STL64: libc++ LP64 sort=1 map=7
THRT64: counter=800 atomic=200 val=42
```

## Como reproduzir

```sh
make -C c_compiler hello64c
sh sodos/libcxx64/build.sh
# copie build/libcxx64/STLTEST64.EXE para o D: como STL64.EXE
# e THRTEST64.EXE como THRT64.EXE
```

O `link64` gera SOX2; o `soxrun` não executa 64-bit, então o teste roda no
QEMU.

## Configuração

- `__config_site` próprio: ABI `__1`, sem locale/wchar.
- `sodos-cxxlib64` (`g++ -m64 -mcmodel=large`, libc++ headers).
- `init_array_stub64` + `override_stub64` para o `link64`.
- libc64 estática (`x64_*.o`), `posix64` e `sodos/port` (pthreads) para
  `std::thread`/mutex/condvar.

## Pendências

- locale/iostreams;
- exceções/RTTI;
- `-shared`/DLL64 (hoje a libc++ é estática no `.EXE`).
