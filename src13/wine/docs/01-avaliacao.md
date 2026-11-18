# 01 — Avaliação e plano: EXEs do Windows no SODOS

Primeira versão em 2026-11-18; revisada no mesmo dia depois de olhar as
mudanças ainda não publicadas do `src13` (WSL.EXE, teclado cru, NVMe cru).

## 1. O que o Wine exige do sistema

O Wine (desde a 5.x) tem duas metades:

| Metade | Formato | O que faz | O que pede ao SO |
|---|---|---|---|
| PE (`kernel32`, `kernelbase`, `ucrtbase`, `user32`, `gdi32`, `ntdll.dll`...) | DLLs PE (MinGW) | API Win32 | nada além de um loader PE |
| Unix (`ntdll.so`, `win32u.so`, `winex11.so`, `wineserver`) | ELF Linux | "kernel NT" sobre POSIX | `mmap(MAP_FIXED)`, `mprotect`, `SIGSEGV` com contexto (SEH), threads, `fork/exec`, `wineserver` por socket Unix com `SCM_RIGHTS`, X11/Wayland |

## 2. O que o SODOS tem (revisado)

- **Um espaço de endereços em identidade** (`kernel/mm.h`), `mmap` emulado sem
  `MAP_FIXED` e `mprotect` no-op (`firefox/sodos/port/sodos_mmap.c`).
- **Sem processos POSIX nem syscalls Linux**: compatibilidade de fonte
  (LIBC64 + `libsodos`), não de binário.
- **Novo, ainda não publicado: `WSL.EXE`** (`wsl/`): lê o Ubuntu do WSL2 do
  NTFS do Windows e sobe o kernel do WSL numa **VM AMD-V** (SVM + NPT,
  virtio-blk, console serial). Ou seja: binários Linux de verdade já rodam
  *dentro da VM*, com console de texto, sem rede e sem tela gráfica.
- **Exceções da CPU como sinais** para programas LP64 (`k_fault_entry`,
  `lib/sigfault64.c`): base para SEH no futuro.
- **GS é do kernel**: `gs:0`/`gs:8` são o bloco por CPU (`kthread.c`). O código
  do Windows x64 lê o TEB por `gs:0x30/0x58/0x60`.
- **Toolchain PE no WSL**: `~/zig/zig cc -target x86_64-windows-gnu` gera
  PE32+ com `DYNAMIC_BASE`.

## 3. Caminhos

| | Caminho | Situação |
|---|---|---|
| A | Wine do Ubuntu **dentro do WSL.EXE** (VM) | Possível agora para programas de console (`wine64 cmd.exe` na VM), mas o Wine precisa de X11/Wayland para janelas e a VM não tem tela. Fica como alternativa de teste/comparação. |
| B | Port de fonte do Wine inteiro (metade Unix com `x86_64-sodos64-gcc`) | Bloqueado pelos mesmos pré-requisitos (MAP_FIXED, mprotect, `wineserver` como processo). |
| **C** | **Loader próprio (`WINE.DLL`) + DLLs PE do Wine depois** | **Escolhido.** O SODOS faz o papel da metade Unix do Wine. |

## 4. Decisões do caminho C

- **Gate por `SET WINE`** (como o `LINKEXT`): sem `WINE=1` nada é carregado e
  um EXE do Windows é recusado com a mensagem "ligue com SET WINE=1". Com
  `WINE=1` o `WINE.DLL` só é carregado no primeiro EXE do Windows;
  `SET WINE=0` o tira da memória (ou quando o último EXE do Windows terminar).
- **O kernel não sabe nada de PE além de reconhecer `MZ`+`PE\0\0`**
  (`kernel/wine.c`): o `mod_exec` cria o processo (pid, pilha, argv) e chama a
  entrada do `WINE.DLL`. Imagem e alocações são do pid e somem no fim.
- **TEB pelo gs sem tirar o gs do kernel**: o bloco por CPU ganhou uma janela
  em `0x10..0x67` (`teb_win`), copiada do TEB da tarefa a cada troca
  (`task.teb`, `k_win_teb`). `gs:0`/`gs:8` (StackBase do TEB) continuam do
  kernel; o código do Windows quase nunca os lê direto.
- **Estado do processo por `gs:0x60`**: o PEB é o começo da `struct wproc`;
  dois EXEs do Windows em janelas diferentes do WIN não dividem estado.
- **Funções embutidas com `ms_abi`** dentro do `WINE.DLL` (kernel32, ntdll,
  ucrt/msvcrt, user32). Import que falta vira stub que diz o nome e encerra.

## 5. Marcos

| Marco | Entrega | Teste | Estado |
|---|---|---|---|
| W0 | Gate `SET WINE`, loader PE32+, TEB/PEB, TLS, kernel32/ucrt minimos | `HELLOW.EXE`, `MSGBOXW.EXE` (zig) no QEMU | feito em 2026-11-18 |
| W1 | GS = TEB real, `KUSER_SHARED_DATA`, memoria virtual (reserva/commit), threads, eventos/timers/IOCP, arquivos, excecoes vetorizadas, processos filhos | runtime do Go 1.25 (instalador do Claude), `WRT.EXE` | feito em 2026-11-18 |
| W1b | Winsock TCP sobreposto, registro em memoria, crypt32 (cadeias, assinante), COM/WinRT minimo, PackageManager + ZIP | download HTTPS de 299 MB e instalacao de MSIX pelo instalador do Claude | feito em 2026-11-18 (ver 02-claude-setup.md) |
| W3a | user32/gdi32 proprios: uma janela, mensagens, timers, GDI em superficie | splash do instalador do Claude no WIN | feito em 2026-11-18 |
| W2 | DLLs PE do Wine (`ucrtbase`, `msvcrt`, `kernelbase`) no lugar das embutidas | | |
| W3b | Controles, varias janelas, texto do GDI em bitmap, dialogos (`MessageBox` de verdade), ListView/Toolbar/StatusBar/Rebar, menus guardados | instalador grafico do WinRAR + `WinRAR.exe` (ver 04-winrar.md) | feito em 2026-11-18 (falta desenhar menus e icones) |
| W1c | SEH por tabela (`RtlVirtualUnwind`, `__C_specific_handler`), `throw` do C++ | EXEs MSVC/MinGW com excecoes | |
| W4 | Kernel: paginas por processo, bases fixas | EXEs sem `.reloc`, 32 bits (WoW64), instaladores Squirrel/NSIS | |
| W5 | O que um app Electron/Chromium pede (GPU, sandbox, IPC, COM) | Claude Desktop instalado | |

Pendencias de seguranca: verificador de X.509 (raizes + RSA/ECDSA) e
Authenticode, para aposentar o `WINE_NOVERIFY`.
