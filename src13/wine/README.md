# EXEs do Windows no SODOS (`WINE.DLL`)

Loader de programas do Windows x64 (PE32+). Ligado só com `SET WINE=1`.
Plano e decisões: [docs/01-avaliacao.md](docs/01-avaliacao.md).

```
SET WINE=1          liga (o WINE.DLL ainda nao e carregado)
D:\HELLOW.EXE a b   primeiro EXE do Windows: carrega PATHSODOS\DRV\WINE.DLL
SET WINE=0          descarrega (ou no fim do ultimo EXE do Windows rodando)
```

Sem `WINE=1`, um EXE do Windows dá `programa do Windows: ligue com SET WINE=1`
e nenhum arquivo é lido. O DLL é procurado em `PATHSODOS\DRV`,
`D:\SODOS\DRV` e `Z:\SODOS\DRV`. DLLs PE que não são embutidas vêm da pasta
do EXE ou de `PATHSODOS\WINE\SYSTEM32`.

## Estado (W0, 2026-11-18)

Roda programas de **console** x64 com relocações (MinGW/zig, MSVC com
`/DYNAMICBASE`): argv/linha de comando, `printf` do ucrt, TLS (`.tls` e
`TlsAlloc`), heap, `GetProcAddress`/`LoadLibrary`, `MessageBox` (no console).
Função que falta aparece como `wine: funcao nao implementada: DLL!Nome`.

Ainda não: arquivos, threads, SEH/exceções C++, janelas, EXE de 32 bits ou
sem `.reloc` (precisam de memória virtual, marco W4).

## Arquivos

| | |
|---|---|
| `../kernel/wine.c`, `../kernel/wine.h` | gate `SET WINE`, reconhece PE, carrega/descarrega o DLL |
| `../kernel/kthread.c` | janela do TEB em `gs:0x10..0x67` (`k_win_teb`) |
| `dll/pe.c` | loader PE: seções, relocações, imports/exports, forwarders, TLS |
| `dll/main.c` | processo: TEB/PEB, linha de comando, entrada, `ExitProcess` |
| `dll/k32.c` | kernel32/kernelbase/ntdll/user32 embutidos |
| `dll/crt.c` | ucrtbase/api-ms-win-crt/msvcrt embutidos (printf com `va_list` MS) |
| `tests/` | EXEs de teste (zig `x86_64-windows-gnu`) |

## Compilar e testar (WSL)

```
make -C wine                 # build/WINE.DLL (+ c_compiler/out/DRV) e build/tests/*.EXE
make os.img                  # kernel com wine.c
sh wine/scripts/qemu-test.sh -c 'SET WINE=1' -c 'D:\HELLOW.EXE um "dois tres"'
```

O `make disk-files` do Makefile principal leva o `WINE.DLL` para
`D:\SODOS\DRV` junto com os outros DLLs de `c_compiler/out/DRV`.
