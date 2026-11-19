# 02 — O instalador do Claude Desktop no SODOS

Registro do que o instalador (`Claude Setup.exe`, o `win32-bootstrapper` em
Go 1.25) pede ao sistema e de como cada pedido foi atendido. Testado no QEMU
em 2026-11-18 com o arquivo `Claude Setup(2).exe` de 2026-10-01.

## O que o instalador faz

1. Sobe o runtime do Go: reserva gigabytes de endereço (`VirtualAlloc`), cria
   threads, timers e uma porta de conclusão; lê o relógio direto de
   `KUSER_SHARED_DATA` (0x7FFE0008) e o TLS direto de `gs:[0x1480+n]`.
2. Detecta o sistema: versão/edição (registro), arquitetura, elevação
   (token), S Mode, sideloading, serviços em conflito.
3. Abre a janela de progresso: splash 400×300 animado (GDI, DIB de 32 bits).
4. Baixa o pacote: HTTPS (TLS 1.3 do Go sobre Winsock sobreposto) de
   `api.anthropic.com/.../msix/latest/redirect` para `%TEMP%` (~300 MB).
5. Relança a si mesmo "elevado" (`ShellExecuteEx runas ... --elevated
   --msix-path ...`) e espera o filho.
6. O filho confere as assinaturas (`WinVerifyTrust`, `CryptQueryObject` no
   próprio EXE e no `.msix`) e exige que o **certificado do pacote seja o
   mesmo do instalador**.
7. Instala: `PackageManager.FindPackages`, `Uri.CreateUri(file:///...)`,
   `PackageManager.AddPackageAsync`.

## Como o SODOS atende

| Pedido | Solução |
|---|---|
| GS = TEB | a base do GS de uma thread do Windows é o TEB; `TEB+0`/`TEB+8` guardam CPU e tarefa para o kernel (`kthread.c`) |
| `KUSER_SHARED_DATA` | página alocada em 0x7FFE0000 só com EXE do Windows rodando; o IRQ do timer atualiza |
| reserva de memória | bloco de RAM de verdade, commit zera sob demanda (`vm.c`) |
| elevação | o token diz "elevado": no SODOS todo programa é administrador |
| relançar a si mesmo | processo filho aninhado (`k_exec`) com handle e código de saída (`proc.c`) |
| arquivo de 300 MB em `%TEMP%` | fica em RAM, visível ao processo filho (`file.c`) |
| certificado TLS / assinaturas | sem autoridades raiz nem Authenticode: falha por padrão; `SET WINE_NOVERIFY=1` aceita sem conferir e avisa. O assinante é lido de verdade (PKCS#7 do PE e do `AppxSignature.p7x`) |
| `AddPackageAsync` | extrai o `.msix` (ZIP) para `D:\WINE\APPS\<nome completo>` (`pkg.c`, `zip.c`) |

## Resultado do teste

- Captura do splash no desktop do SODOS: [img/claude-setup-sodos.png](img/claude-setup-sodos.png).
- Detecção, janela, download (299 MB em ~2,5 min pela rede do SODOS), filho
  "elevado" e leitura das assinaturas funcionam.
- **Este instalador (1º/out/2026) recusa o pacote atual**: ele foi assinado
  com o certificado da Anthropic válido de 2025-10-14 a 2026-10-20 (série
  ...12d31a50); o MSIX "latest" vem assinado com o certificado renovado,
  2026-10-08 a 2027-10-28 (série ...2ec875b8), mesma identidade
  ("Anthropic, PBC", subject e emissor idênticos), outra chave. A mensagem é
  a do próprio instalador: "MSIX signer does not match bootstrapper". É do
  arquivo, não do SODOS; precisa de um `Claude Setup.exe` atual.
- Com o diagnóstico `WINE_SIGTEST=1` (mesma assinatura para os dois) o fluxo
  segue até o fim: o pacote `Claude_5.11492.5.0_x64__pzs8sxrjxfjjc` (174 arquivos,
  733 MB, `claude.exe` de 250 MB) é extraído sem falhas e o log do instalador
  termina com "Claude Setup completed successfully", código de saída 0. No
  QEMU a extração levou 47 min por causa do disco ATA em PIO (0,26 MB/s).
- O aplicativo instalado é Electron (Chromium): **não roda** no `WINE.DLL`
  atual. Rodá-lo é outro projeto (GPU, sandbox, COM, dezenas de DLLs).

## Como repetir

```
cp "/mnt/c/Users/<u>/Downloads/Claude Setup.exe" wine/build/tests/CLAUDE.EXE
NOVERIFY=1 sh wine/scripts/win-test.sh start      # desktop + instalador
sh wine/scripts/win-test.sh shot                  # captura, serial, ClaudeSetup.log
sh wine/scripts/win-test.sh stop
```

## PC real, 2026-11-18 (noite)

- No PC (UEFI) o instalador foi ate "Claude Setup completed successfully": o
  pacote ficou em `D:\WINE\APPS\Claude_5.11492.5.0_x64__pzs8sxrjxfjjc`.
- O relogio dos EXEs (pagina KUSER em 0x7FFE0000) estava parado em 2065: o
  endereco nao e RAM do heap no PC. O kernel agora mapeia uma pagina do heap
  ali (`mm_alias_page`); `SET WINE_KUSERMAP=1` forca esse caminho no QEMU e o
  `KUSER.EXE` confere (data certa, 1500 ms em 1500 ms).
- "explorer.exe shell:AppsFolder\Claude_pzs8sxrjxfjjc!Claude" falhava: o Go
  achava o `D:\EXPLORER.EXE` do SODOS na pasta atual e recusava. Agora o WINE
  poe um marcador em `D:\WINE\SYSTEM32\EXPLORER.EXE` (no PATH) e resolve o
  pedido pelo `AppxManifest.xml` (tambem `claude://` pelo `uap:Protocol`).
  Teste: `EXPLTEST.EXE` com um pacote falso.
- O `Claude.exe` (Electron, 250 MB) carrega, mas o V8 reserva 4 GB e depois
  64 GB de espaco de enderecos (`VirtualAlloc` so de reserva) e o SODOS nao
  tem memoria virtual por processo: para ai. E o marco W4/W5.

## W4: memoria virtual (2026-11-18)

- `kernel/vmem.c`: regiao a partir de 1 TB (PML4[2] em diante) com tabelas de
  4 KB sob demanda. Reserva = so enderecos; commit = quadros zerados (blocos de
  2 MB do heap alto); `VirtualProtect` liga/desliga presente/escrita;
  decommit/release devolvem os quadros. Dono = pid; o `exec_release` solta
  tudo do processo. Sem reservas vivas, quadros, tabelas e entradas do PML4
  voltam ao heap (WINE desligado = custo zero). Exports `k_vm_*`.
- `wine/dll/vm.c`: reserva sem commit de 64 MB ou mais, reserva em endereco
  pedido e o que nao cabe na RAM vao para essa regiao.
- Corrigidos no caminho: retorno dos handlers vetorizados comparado como
  `LONG` de 32 bits (antes nenhum `EXCEPTION_CONTINUE_EXECUTION` escrito em C
  funcionava); `int3` vira `EXCEPTION_BREAKPOINT` so em threads do Windows;
  `GetProcAddress` da ntdll devolve uma funcao `STATUS_NOT_IMPLEMENTED` para
  Nt/Zw que faltam; ordinais do ws2_32; `procmisc.c` com ~80 funcoes de
  processo/thread/jobs/psapi/winmm que o Chromium chama no inicio.
- `VMTEST.EXE`: 64 GB reservados em 0x10000000000, commit/consulta/protecao,
  falha em pagina sem acesso tratada pelo handler, decommit/recommit zerado,
  4 GB alinhados em endereco pedido, 256 MB em ~110 ms. `VMTEST OK`.
- `Claude.exe` agora passa da reserva do V8 e chega ao codigo de seguranca do
  Chromium, que pede as funcoes de descritor de seguranca do advapi32 uma a
  uma (`GetSecurityDescriptorControl` e a proxima) e para num `CHECK`
  (int3 no rva 0x6c80952). Isso ja e o marco W5.

## W5, primeira parte (2026-11-18)

O que o processo principal do Claude.exe (Chromium/Electron) ja passa no QEMU:

- Reserva de 64 GB do V8 (W4), seguranca do advapi32, ICU e snapshot do V8
  mapeados, window station/desktop, ETW, topologia de CPU, AppModel.
- `dll/sec.c`: SID, ACL/ACE, descritor de seguranca absoluto e self-relative,
  SDDL basico, TRUSTEE/EXPLICIT_ACCESS, token com grupos/integridade/DACL
  padrao. Dados de verdade (o Chromium le de volta o que monta); acesso sempre
  permitido. Fica antes das outras tabelas e substitui os stubs antigos.
- `dll/map.c`: CreateFileMapping/MapViewOfFile. Secao de arquivo (lida para a
  memoria; vista com escrita grava de volta) e anonima com nome
  (OpenFileMapping). Teste `MAPTEST.EXE`.
- `dll/safearray.c`: SAFEARRAY e VarBstrCmp. `dll/ordtab.c`: ordinal -> nome de
  oleaut32, uxtheme, comctl32, shell32, shlwapi, ws2_32 e ole32 (gerado das
  tabelas .spec do Wine); qualquer ordinal cai na funcao de mesmo nome.
- `GetProcAddress`: Nt/Zw que faltam respondem STATUS_NOT_IMPLEMENTED; o que
  falta em kernel32/user32/advapi32/gdi32 devolve 0 com
  ERROR_CALL_NOT_IMPLEMENTED; falhas por ordinal vao para a serial com a DLL.
- Diagnostico: `int3` vira EXCEPTION_BREAKPOINT e o relatorio da serial lista
  enderecos de retorno da pilha (rva); o Chromium escreve LOG(FATAL) em
  `app\debug.log`.

Proximo bloqueio: **DirectWrite**. `DWriteCreateFactory` ainda devolve
E_NOTIMPL e o Chromium faz CHECK logo depois (rva 0x33484f6): ele exige
IDWriteFactory (colecao de fontes do sistema, FontFace com glifos/metricas,
TextFormat/TextLayout e rasterizacao de glifos para o Skia). Depois disso
vem a criacao dos processos filhos (GPU, renderer) com IPC por pipes.

## W5, segunda parte (2026-11-19)

Capturas da tela tiradas pelo monitor do QEMU (`screendump`; a API HTTP fica
ocupada pelo /exec do Claude): o Claude ainda nao abre janela.

O que entrou e o que o Claude.exe ja passa:

- **DirectWrite** (`dll/ttf.c`, `dll/dwrite.c`): fabrica, colecao do sistema
  com as fontes de SODOS\FONTS (nomes do Windows -> Inter/Noto/JetBrains),
  familia/fonte/face, metricas, cmap 4/12, tabelas OpenType, contorno para o
  ID2D1SimplifiedGeometrySink e rasterizacao (IDWriteGlyphRunAnalysis).
  Teste `DWTEST.EXE` (desenha "SODOS" em ASCII).
- **Corrupcao de memoria achada com watchpoint (gdb do QEMU, `QEMU_EXTRA=-s`)**:
  callbacks de TLS/DllMain rodavam na pilha do kernel; o `__chkstk` deles
  "sondava" a memoria ate o StackLimit e zerava um byte por pagina (codigo do
  WINE.DLL). Agora todo codigo do Windows da thread roda na pilha do Windows.
- **Pagina 0 fora do mapa** enquanto roda EXE do Windows (`mm_null_guard`):
  ponteiro NULL vira ACCESS_VIOLATION em vez de ler/estragar a tabela da BIOS.
- **StackBase no gs:[8]**: o kernel passou a guardar a tarefa em TEB+0x18
  (`struct cpu.curr` no offset 0x18); TEB+8/+0x10 sao os limites da pilha (o
  V8 confere o rsp contra eles).
- **Sincronizacao**: EnterCriticalSection perdia o acordar de um segundo
  esperador (mutex de futex corrigido: 0/1/2); SRW com leitores de verdade;
  espera de futex sem prazo nao devolve mais "tempo esgotado" quando acorda
  sem wake (o Chromium chamava abort() quando SleepConditionVariableCS
  INFINITE devolvia FALSE).
- NUL como dispositivo de caractere; console para o libuv; handle real do
  proprio processo (DuplicateHandle/OpenProcess); NtQueryInformationFile/
  NtQueryVolumeInformationFile/NtQueryDirectoryFile (`dll/nt.c`);
  PEB.LoaderLock; RtlAddGrowableFunctionTable; QueueUserWorkItem.
- Diagnostico com `WINEDEBUG=2`: stdout/stderr copiados na serial (o FATAL do
  Chromium aparece ali), esperas longas e pilha do ExitProcess.

Agora: o JS principal do Electron roda (app.asar lido, realpath ok) e para no
crashpad: `CreateNamedPipe: Erro do Windows 50`. Proximo: **named pipes**
(servidor/cliente, overlapped, IOCP) e processos filhos do Chromium (crashpad,
GPU, renderer) conversando por eles.
