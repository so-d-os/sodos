# EXEs do Windows no SODOS (`WINE.DLL`)

Loader e API do Windows para programas x64 (PE32+). Ligado só com
`SET WINE=1`. Plano e decisões: [docs/01-avaliacao.md](docs/01-avaliacao.md).
O que foi preciso para o instalador do Claude Desktop:
[docs/02-claude-setup.md](docs/02-claude-setup.md).
7-Zip instalado e rodando: [docs/03-7zip.md](docs/03-7zip.md).
WinRAR instalado pelo instalador gráfico, `Rar.exe` e a janela do `WinRAR.exe`: [docs/04-winrar.md](docs/04-winrar.md).
Google Chrome baixado do Google pelo `INSTCHRM.EXE` e aberto pelo `D:\RODACHROME.BAT`: [docs/06-chrome.md](docs/06-chrome.md).

```
SET WINE=1          liga (nada e carregado ainda)
D:\HELLOW.EXE a b   primeiro EXE do Windows: carrega PATHSODOS\DRV\WINE.DLL
SET WINE=0          descarrega (ou no fim do ultimo EXE do Windows rodando)
```

## Consumo com o WINE desligado: zero

- Sem `WINE=1`, um EXE do Windows dá
  `programa do Windows: ligue com SET WINE=1` e nenhum arquivo é lido.
- Com `WINE=1` e nenhum EXE do Windows rodando, nada é carregado.
- O `WINE.DLL` (340 KB), a página `KUSER_SHARED_DATA` (0x7FFE0000) e as
  threads só existem enquanto há EXE do Windows; a base do GS só troca para
  threads do Windows; o relógio do Windows só é atualizado (um `if` no IRQ do
  timer) com essa página alocada.
- Medido no QEMU com `FREE` e `DLLS`: depois de rodar EXEs e dar `SET WINE=0`,
  heap do kernel e heap alto voltam aos valores de antes.

## Variáveis (SET)

| | |
|---|---|
| `WINE=1` | liga |
| `WINEDEBUG=1` | registra na serial o que o programa pede e falta (`GetProcAddress`, registro, arquivos, COM) |
| `WINE_NOVERIFY=1` | **inseguro, opt-in**: aceita certificados TLS e assinaturas de arquivos sem conferir (o SODOS não tem autoridades raiz nem Authenticode). Sem ela, HTTPS e verificação de assinatura falham, como devem. Cada uso é avisado na serial. |
| `WINE_SIGTEST=1` | só diagnóstico: devolve a assinatura do próprio EXE para qualquer arquivo |

## O que funciona

- **Processo**: loader PE32+ com relocações, imports/exports/forwarders, TLS,
  TEB/PEB reais (GS = TEB), `KUSER_SHARED_DATA`, linha de comando, ambiente,
  processos filhos (`CreateProcess`, `ShellExecuteEx`) com handle e código de
  saída.
- **Memória**: `VirtualAlloc`/`Free`/`Query` com reserva e commit, heaps.
- **Threads e sincronização**: `CreateThread` (k_threads em várias CPUs),
  eventos, mutex, semáforos, timers, `WaitFor*`, portas de conclusão (IOCP),
  `WaitOnAddress`, seções críticas, exceções vetorizadas (faltas da CPU).
- **Arquivos**: `CreateFile`, leitura/escrita, pastas, `FindFirstFile`,
  atributos; temporários grandes ficam em RAM.
- **Rede**: Winsock TCP cliente com E/S sobreposta (`ConnectEx`, `WSARecv`,
  `WSASend`), DNS (`GetAddrInfoW`). TLS é do programa (ex.: Go).
- **Janelas**: uma janela por processo no WIN: classes, fila de mensagens,
  timers, GDI em superfície de 32 bits (`CreateDIBSection`, `StretchBlt`,
  `Rectangle`, `FillRect`).
- **Registro** em memória, **CRT** (ucrt/msvcrt), **COM/WinRT** mínimo
  (HSTRING, ativação), **PackageManager** (instala `.msix` extraindo-o),
  **crypt32** (lojas, cadeias, assinante de PKCS#7/Authenticode).
- Runtime do **Go** 1.25 completo (escalonador, GC, netpoll, TLS 1.3).

EXE x64 sem `.reloc` com base baixa (0x400000) carrega no endereço pedido
(o kernel cede a RAM da tela em memória enquanto ele roda).

Ainda não: SEH por tabela/exceções C++, EXE de 32 bits (precisa de memória
virtual e WoW64), diálogos, várias janelas, controles (`comctl32`),
texto do GDI dentro de bitmaps, UDP/servidor, serviços, COM de verdade.

## Pastas do "Windows"

Criadas sob demanda em `D:\WINE`: `TEMP`, `SYSTEM32`, `USERS\SODOS\...`,
`PROGRAM FILES`, `PROGRAMDATA`, `APPS` (pacotes MSIX instalados).

## Arquivos

| | |
|---|---|
| `../kernel/wine.c`, `wine.h` | gate `SET WINE`, reconhece PE, carrega/descarrega o DLL, página KUSER |
| `../kernel/kthread.c` | base do GS = TEB nas threads do Windows (`k_win_teb`) |
| `dll/main.c` | processo: TEB/PEB, thread principal, espera do `ExitProcess` |
| `dll/pe.c` | loader PE, módulos, recursos |
| `dll/vm.c` | memória virtual sem tabelas de páginas |
| `dll/obj.c` | handles, esperas, threads, timers, IOCP |
| `dll/exc.c` | faltas da CPU → exceções do Windows, contexto |
| `dll/file.c` | arquivos, pastas, temporários em RAM |
| `dll/net.c` | ws2_32 sobre `k_sock_*` |
| `dll/gui.c` | user32 + gdi32 sobre `k_gui_*` |
| `dll/proc.c` | processos filhos |
| `dll/reg.c`, `sys.c`, `misc.c`, `k32.c`, `crt.c` | registro, ntdll/advapi/shell, kernel32, CRT |
| `dll/crypt.c`, `sig.c` | certificados, cadeias, assinante de arquivos |
| `dll/winrt.c`, `pkg.c`, `zip.c` | COM/WinRT, PackageManager, ZIP/inflate |
| `dll/tables.c` | lista de DLLs embutidas e busca de funções |
| `tests/` | EXEs de teste (zig `x86_64-windows-gnu`) |

## Compilar e testar (WSL)

```
make -C wine                 # build/WINE.DLL (+ c_compiler/out/DRV) e build/tests/*.EXE
make os.img                  # kernel
sh wine/scripts/qemu-test.sh -c 'SET WINE=1' -c 'D:\HELLOW.EXE um "dois tres"'
sh wine/scripts/try.sh 'D:\WRT.EXE'            # compila, roda com WINEDEBUG, mostra serial
sh wine/scripts/win-test.sh start 'D:\X.EXE'   # dentro do desktop (WIN); shot/stop
```

O disco do QEMU é ATA em PIO: gravar custa ~0,26 MB/s nesse ambiente
(virtualização aninhada no WSL2). No PC o SODOS usa NVMe.
