# Plano enxuto: Claude Desktop no SODOS (WINE.DLL)

Analise de 2026-11-19. Objetivo: chegar a janela do Claude Desktop com o menor
pedaco possivel do Windows implementado, e trocar o "roda, cai, adivinha" por
ciclos curtos com diagnostico exato.

## 1. O que medimos

**Binario.** `Claude.exe` 5.11492.5.0 = Electron 45.2.1 / Chromium 156
(250 MB). Importa ~1500 funcoes de 60 DLLs; faltam ~560 no WINE, mas a maior
parte esta fora do caminho ate a janela:

| Grupo | Faltam | Precisa para a janela? |
|---|---|---|
| user32 | 66 | sim (janela, monitores, DPI, clipboard, entrada) |
| gdi32 | 64 | parte (regioes, DIB, AlphaBlend; metafile/impressao nao) |
| kernel32 | 36 | parte (named pipes, ReadDirectoryChangesW) |
| ws2_32 | 21 | sim para a rede (getaddrinfo, WSAEventSelect) |
| crypt32 / ncrypt | 15 / 12 | parte (CryptProtectData para o safeStorage) |
| winhttp | 10 | nao (proxy: pode falhar) |
| dwmapi, shcore (DPI) | 3 + 2 | sim, como stubs que respondem |
| ffmpeg.dll | 45 | e uma DLL PE de verdade da pasta do app: o loader carrega |
| bluetooth, hid, winusb, setupapi, Media Foundation, impressao, dbghelp, UI Automation, pdh, wevtapi, tbs | ~130 | nao: ficam "ausentes" |

**JavaScript do app (`app.asar`, 88 MB).**
- Le um JSON de configuracao com `isHardwareAccelerationDisabled`; se `true`
  chama `app.disableHardwareAcceleration()` e acrescenta `disable-gpu`.
  Ou seja: GPU desligada **sem mexer no binario**.
- Chama `crashReporter.start({uploadToServer:false})` sempre: isso sobe o
  crashpad (processo `Claude.exe --type=crashpad-handler`) e um **named pipe**
  entre os dois. E o bloqueio atual (`CreateNamedPipe: Erro 50`).
- `requestSingleInstanceLock` (mutex + janela de mensagem), `autoUpdater`,
  `WebContentsView`, carrega `https://claude.ai` (31 referencias).

**Opcoes do Chromium presentes no binario:** `single-process`,
`in-process-gpu`, `no-sandbox`, `disable-gpu`, `disable-gpu-compositing`,
`enable-logging`, `NetworkServiceInProcess`, `no-zygote`.

**Onde estamos.** O processo principal ja passa: memoria virtual (W4),
seguranca, secoes, DirectWrite, ICU, snapshot do V8, inicio do Node e do JS
do app. Para no crashpad.

## 2. Principios do plano enxuto

1. **Reduzir o Windows necessario por configuracao, nao por codigo.** Um
   processo so (sem sandbox, sem processo de GPU, rede no mesmo processo,
   composicao por software). Cada processo filho a menos elimina IPC, handles
   entre processos e memoria compartilhada entre eles.
2. **Diagnostico exato antes de cada correcao.** O Chromium explica o que
   falta se pedirmos (`--enable-logging=stderr --v=1`); o WINE ja copia
   stdout/stderr na serial (`WINEDEBUG=2`). Nada de adivinhar pelo `int3`.
3. **Ciclo curto.** Hoje cada tentativa custa 4-5 min (desliga a VM, grava o
   disco, liga, espera o Electron). Meta: 1-2 min, sem reiniciar a VM.
4. **So o caminho critico.** Tudo da tabela marcado "nao" responde "ausente"
   (NULL no GetProcAddress, ERROR_CALL_NOT_IMPLEMENTED, E_NOTIMPL) e fica
   para depois.

## 3. Fases (cada uma termina num resultado verificavel)

### F0 - Bancada de teste (pequena; faz as outras andarem rapido)
- `wine/scripts/claude-run.sh`: um comando que sobe (ou reaproveita) a VM,
  troca o WINE.DLL **sem reiniciar** e roda o Claude com as opcoes da F1.
  Recolhe numa pasta: linhas FATAL/ERROR do Chromium, excecao com RVA e pilha,
  chamadas de API ausentes, `debug.log`, `screendump` em PNG, codigo de saida.
- Troca sem reiniciar: `SET WINEDLL=X:\WINE.DLL` (caminho alternativo do DLL,
  so diagnostico) com o arquivo vindo de um disco extra do QEMU em modo leitura
  (`-drive file=fat:wine/build`) ou de um `PUT` na API HTTP do SODOS.
- Opcoes do Chromium pelo lancador: `SET WINE_ARGS=...` acrescentado a linha de
  comando dos EXEs do Windows (e o `explorer.exe` interno passa a usa-lo).
- Relatorio estatico de cobertura (o desta analise) gerado pelo script, para
  ver o que muda a cada passo.

Pronto quando: um comando entrega a mensagem FATAL + captura de tela em < 2 min
por tentativa.

### F1 - Um processo so (pequena; experimento)
Configuracao alvo:
```
--no-sandbox --single-process --in-process-gpu --disable-gpu
--disable-gpu-compositing --enable-features=NetworkServiceInProcess
--enable-logging=stderr --v=1
```
mais `isHardwareAccelerationDisabled: true` no JSON de configuracao do app
(pasta `userData`, em `D:\WINE\USERS\SODOS\APPDATA\ROAMING\Claude`).
O WINE registra cada `CreateProcess` pedido.

Pronto quando: sabemos quais processos filhos ainda nascem (esperado: so o
crashpad). Se o `--single-process` nao funcionar no Electron 45, vale a F3b.

### F2 - Named pipes minimos (media)
`CreateNamedPipeW/A`, `ConnectNamedPipe`, `CreateFile("\\.\pipe\...")`,
`ReadFile/WriteFile` (tambem OVERLAPPED e com IOCP), `PeekNamedPipe`,
`WaitNamedPipe`, `SetNamedPipeHandleState`, `GetNamedPipeClient/ServerProcessId`,
`TransactNamedPipe`, `ImpersonateNamedPipeClient` (sim, sem efeito).
Tabela de pipes global no WINE.DLL (todos os processos estao no mesmo espaco
de enderecos); buffers com memoria do kernel (dono 0), porque a memoria de um
processo some quando ele termina.

Pronto quando: `PIPETEST.EXE` (servidor e cliente em processos diferentes)
passa e o crashpad registra o processo principal.

### F3 - Processo filho em paralelo (pequena)
Conferir que o pai continua rodando enquanto o filho (crashpad) roda
(`CreateProcess` hoje e atendido pela tarefa do loader com `k_exec`).
**F3b (so se a F1 falhar):** handles entre processos (`DuplicateHandle` para
outro processo), memoria compartilhada por handle e Mojo por named pipe para o
renderer e a GPU. E a parte mais cara do plano; a F1 existe para evita-la.

Pronto quando: o crashpad fica vivo e o processo principal segue ate criar a
janela.

### F4 - Primeira janela (media)
O que o Chromium usa para a janela `Chrome_WidgetWin_1`: monitores
(`GetMonitorInfo`, `EnumDisplayDevices/Settings`, `DisplayConfig*`), DPI
(`GetDpiForMonitor`, `GetDpiForWindow`), `dwmapi` (respondendo sem DWM),
janela sem moldura e `WM_NCCALCSIZE`, `ChangeWindowMessageFilterEx`, entrada
crua (`RegisterRawInputDevices`), clipboard listener, cursor.

Pronto quando: a captura mostra a janela do Claude (mesmo em branco).

### F5 - Desenho por software (media)
Composicao por software do Chromium: `CreateDIBSection` + `BitBlt` /
`StretchDIBits` / `UpdateLayeredWindow` / `GdiAlphaBlend`, regioes
(`CreateRectRgn`, `CombineRgn`, `GetRgnBox`), `GetDIBits`. A superficie do
WIN ja existe (dll/gdi.c, dll/user.c).

Pronto quando: aparece conteudo desenhado pelo Chromium (barra de titulo do
app, tela de carregamento).

### F6 - Rede e a interface do claude.ai (media)
`getaddrinfo`/`GetAddrInfoExW`/`freeaddrinfo`/`GetNameInfoW` (DNS),
`WSAEventSelect`/`WSAEnumNetworkEvents`/`WSACreateEvent` (o net do Chromium
espera sockets assim), `inet_ntop`. TLS e do BoringSSL dentro do Chromium;
a verificacao usa a Chrome Root Store embutida (CryptoAPI so como extra).
`winhttp` (proxy) pode falhar: o Chromium segue sem proxy.
`CryptProtectData` para o `safeStorage` do Electron (tokens de login):
precisa existir; no SODOS sem usuarios a protecao e so contra leitura casual
do arquivo, e isso tem de ficar escrito.

Pronto quando: a tela de login do claude.ai aparece na janela.

### F7 - Entrada, login e o PC de verdade
Teclado/mouse/rolagem/IME basico, abrir links externos (`ShellExecute` de
URL), login, e o teste no PC (memoria, tempo de abertura).

## 4. Fora do caminho (respondem "ausente")
Bluetooth, HID, WinUSB, SetupAPI/cfgmgr32, Media Foundation (camera/microfone),
impressao (winspool, PrintDlgEx, metafiles), dbghelp/MiniDumpWriteDump,
UI Automation/oleacc, PDH, Event Log, TPM (tbs), WinHTTP alem do minimo,
D3D11/DXGI/DirectComposition (GPU desligada), pastas monitoradas
(`ReadDirectoryChangesW` pode falhar no inicio).

## 5. Riscos
- **`--single-process` no Electron 45**: oficialmente sem suporte. Se quebrar,
  o renderer vira processo separado (F3b): handles e memoria compartilhada
  entre processos, Mojo por pipe. Testar na F1, antes de escrever codigo.
- **Crashpad** nao tem opcao para desligar; por isso os pipes vem cedo (F2).
  Servem tambem ao Mojo se a F3b for necessaria.
- **Velocidade**: QEMU aninhado no WSL2; o Electron leva ~1 min para chegar
  ao JS. O PC real deve ser bem mais rapido.
- **Seguranca**: `WINE_NOVERIFY`, `safeStorage` sem protecao real e sandbox
  desligado. Aceitavel para teste; tem de ficar visivel na documentacao.

## 6. Ordem e tamanho

| Fase | Tamanho | Depende de |
|---|---|---|
| F0 bancada | pequeno | - |
| F1 um processo | pequeno | F0 |
| F2 named pipes | medio | F0 |
| F3 filho em paralelo | pequeno (F3b: grande) | F2 |
| F4 primeira janela | medio | F3 |
| F5 desenho por software | medio | F4 |
| F6 rede + claude.ai | medio | F5 |
| F7 entrada, login, PC | medio | F6 |

F0 e F1 primeiro: sao baratas e decidem o tamanho do resto (com ou sem F3b).

## 7. Andamento (19/11/2026)

- **F0 feito.** `wine/scripts/claude-run.sh [segundos]` compila, troca o
  WINE.DLL na VM (FETCH, sem reiniciar), abre o Claude pelo WIN e grava
  `wine/build/claude-run/` (report.txt, serial.txt, tela.png). `FETCH.EXE`
  baixa por HTTP; `vm.sh launch` usa POST (caminhos longos).
- **F1: nao serve.** `--single-process` quebra no Electron 45 (CHECK na
  inicializacao do alocador). Seguimos com processos separados.
- **F2 feito.** `pipe.c`: named pipes entre processos (sincrono, OVERLAPPED,
  IOCP, mensagem/byte, pipe quebrado, CreatePipe anonimo). PIPETEST passa.
- **F3 feito.** CreateProcess abre o filho em paralelo como tarefa do WIN
  (`--wine-spawn=N`); pid dado ja no CreateProcess; o filho herda os handles
  do pai nos mesmos numeros. SPAWNTEST passa. Para o crashpad foi preciso:
  - cache de imagens grandes (`pe.c`, >= 8 MB, memoria compartilhada): o
    segundo Claude.exe carrega em segundos em vez de minutos;
  - a lista de handles herdados e tirada no proprio CreateProcess (o Chromium
    fecha o pipe do crashpad logo depois);
  - `GetFileInformationByHandleEx(FileNameInfo)` em pipe (`\nome`);
  - `WINE_ARGS` so no processo de cima (o crashpad-handler recusa opcoes).
  Resultado: navegador e crashpad-handler rodando juntos, sem erro, 5 min.
- Cuidado: trocar o WINE.DLL a quente com processo morto sem `wexit` deixa o
  cache (250 MB) perdido na VM; `vm.sh stop` antes de cada rodada.
- **F4 em andamento (19/11).** O navegador cria as janelas, sobe o Network
  Service e o renderer como processos separados. Corrigido no caminho:
  - pipes: leitura sincrona registrada como pendente, `TransactNamedPipe`
    atomico, `FlushFileBuffers` espera o outro lado ler;
  - `VirtualAlloc(MEM_RESERVE)` dentro de outra reserva agora falha (o V8
    tomava pedaco alheio; CHECK no VirtualFree de vez em quando);
  - SRW com futex (girar com yield deixava a thread principal minutos sem a
    trava do PartitionAlloc);
  - atomo de classe do `RegisterClassEx` (as janelas do Chromium nao abriam);
  - `GetTokenInformation(TokenPrivileges)` com os privilegios de um usuario
    comum; versao (`GetFileVersionInfo`) das DLLs embutidas: 10.0.19041;
  - WinRT HSTRING, DPAPI (sem protecao real), WSAEventSelect + vigia da rede,
    IPHLPAPI, DXGI/D3D11/DComp "sem suporte", ~100 APIs de user32/shell/etc.
    (`fill.c`), `NtQueryObject`, `NtQuerySection`;
  - heranca so dos handles da `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` (herdar
    tudo dividia ~1000 objetos e corrompia o heap do kernel), objetos e secoes
    em memoria sem dono, `DuplicateHandle` para dentro de um filho.
- Ferramentas novas: pid no prefixo do log (`[wine 268]`), registradores e
  textos na excecao que vai para o filtro do programa, dono de SRW preso,
  `memdis.sh`/`spin.sh` (scratchpad) para ver onde a CPU gira pela monitor.
- **Estabilidade com 4 processos (19/11, tarde).** O navegador, o crashpad, o
  Network Service e o renderer rodam juntos 10 min sem excecao e o Mojo troca
  mensagens pelos named pipes. Achados no caminho:
  - **heap do kernel corrompido**: `file.c` abria arquivo gravavel com
    `grow(f, size)` e o `k_fload` gravava o 0 final um byte depois do buffer
    (arquivos de 8192 bytes do cache/LevelDB). Diagnostico novo no `mm.c`:
    o "lista de livres corrompida" agora diz o elo ruim, o cabecalho em x e o
    bloco antes;
  - **FAT corrompido** com varios processos gravando: o `fat.c` cede a CPU no
    meio de gravacoes e outro programa entrava; `kexports.c` agora tem um dono
    do sistema de arquivos (`fs_enter`/`fs_leave`) nos `k_f*`;
  - memoria: pilhas de thread so reservadas (memoria virtual, 1 MB
    commitado), imagens acima de 4 GB (`k_hmalloc`), VirtualAlloc primeiro na
    memoria virtual (antes tudo ate 16 MB ia para a arena baixa e acabava);
  - linha de comando dos filhos ate 32 K (o renderer passa de 1 K) e argv do
    filho por processo (era estatico no WINE.DLL, um filho trocava o do outro);
  - `VirtualQuery` responde a protecao de cada secao da imagem e o que o
    programa pediu no `VirtualProtect` (base::ProtectedMemory);
  - trava global do WINE.DLL com futex (girar ocupava 3 CPUs).
- Disco da VM: se cair o kernel no meio de gravacao, refazer com
  `rebuild_vmdisk.sh` (scratchpad: disk.img base + mcopy de E:\WINE).
- Proximo (F4): a janela do Claude ainda nao aparece. O BrowserWindow e criado
  escondido e so aparece depois do primeiro quadro do renderer.
- **F4 feito (19/11, 09:06 na VM).** A janela "Claude" (BrowserWindow) aparece
  no desktop do WIN, ainda branca. Print: `wine/build/claude-janela-f4.png`.
  Ultimos ajustes ate aqui: `lstrcmpW(NULL)`, ponteiros invalidos nos
  handles GDI/menu/HSTRING (`wptr_ok`), oleacc/uiautomationcore/propsys
  embutidas, vistas de secao por processo (lista unica + dono), leituras de
  pipe pendentes canceladas quando o processo sai (o navegador copiava para
  a memoria do renderer morto), `ReadProcessMemory` (crashpad), e
  `--ipc-connection-timeout=300` nos testes (na VM o navegador demora mais
  que os 15 s padrao para ligar o filho).
- Proximo (F5): desenho por software (`SoftwareOutputDeviceWin`:
  CreateDIBSection com secao + BitBlt, ou UpdateLayeredWindow).
- **F5, primeiro quadro (19/11, tarde).** O compositor do navegador desenha na
  janela: `BitBlt` de 584x561 (t198) e a area pintada sai (252,252,251), a
  cor da pagina, no lugar do fundo (240,240,240) da janela do WIN. A pagina
  ainda esta vazia (sem rede). Para chegar ate aqui:
  - opcoes de teste: `--disable-gpu --disable-software-rasterizer
    --use-gl=disabled --disable-3d-apis --disable-webgl --disable-webgpu
    --disable-accelerated-2d-canvas --disable-gpu-rasterization` (sem o
    `--disable-software-rasterizer` o GPU em processo tentava SwiftShader e
    criava contexto GL num laco);
  - GDI: regioes retangulares, transformacao do mundo (identidade),
    `GetClipBox`, `GetDIBits`/`SetDIBits`, `CreateDIBSection` sobre secao;
  - `WINE_ARGS2`/`WINE_ARGS3`: o SET do kernel guarda 259 caracteres;
  - processo saindo nao deixa leitura de pipe pendente.
- Proximo (F6): rede. O Network Service nao mostra DNS nem conexao ainda;
  falta registro no `getaddrinfo`/`connect` e conferir TLS (o Chromium usa
  BoringSSL e a Chrome Root Store, nao a CryptoAPI).
- F6 (19/11/2026, tarde): **a interface do claude.ai aparece na VM**
  (`wine/build/claude-run/frente3.png`). A pagina carregou por h2 (952 KB,
  159 recursos, 77 scripts), mas o DOMContentLoaded veio aos ~308 s e o
  carregador do app mostra "Couldn't finish loading... assets-proxy" (lento,
  nao erro de TLS). O log do proprio app fica em
  `D:\WINE\USERS\SODOS\APPDATA\LOCAL\Claude\logs\main.log` (ler com mcopy com a
  VM parada). Correcoes desta etapa:
  - `NtSetInformationFile` (apagar ao fechar, renomear, tamanho):
    unlink/ftruncate/rotacao de log do Node davam ENOTSUP;
  - `claude-native-binding.node`: caminho `\?\` no carregador, DLLs mpr,
    ncrypt e wevtapi, ~40 funcoes; HSTRING no layout do Windows (o windows-rs
    monta os seus); `GetErrorInfo`/`SetErrorInfo` (oleaut32 #200-202);
  - `UnregisterWaitEx(h, evento)` sinaliza o evento quando nao ha callback
    rodando (o `ObjectWatcher::StopWatching` do Chromium travava a thread de
    rede depois do connect), sem corrida com o callback;
  - `WSANETWORKEVENTS` e `PROCESSENTRY32W` com `long` de 4 bytes;
  - `MapViewOfFile` da mesma secao duas vezes no mesmo processo: endereco
    proprio (`vmem_alias` no kernel), o `SharedMemoryTracker` indexa pelo
    endereco;
  - `DuplicateHandle` de um processo filho para o pai (broker do Mojo), com
    `DUPLICATE_CLOSE_SOURCE` na tabela do filho;
  - `NtQueryObject` com o `GrantedAccess` real (arquivo somente leitura);
  - kernel: `KT_MAX` 1024 threads; `flush_fat` restaura FAT[0]/FAT[1] (uma vez
    apareceram zerados na memoria e o disco da VM ficou ilegivel).
- Proximo: desempenho (o laco principal do Electron fica ~850 ms travado a
  cada segundo, segundo o log `event-loop-stall`), clicar "Try again" e testar
  no PC real.
