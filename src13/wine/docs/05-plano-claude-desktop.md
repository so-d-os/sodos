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
- **Bancada em Linux com KVM (19/11, noite).** `scripts/claude-vmdisk.sh` monta
  o disco da VM sem o instalador: baixa o `.msix` "latest" e grava o pacote
  extraido em `D:\WINE\APPS\Claude_<versao>_x64__pzs8sxrjxfjjc` pelo mcopy
  (segundos, contra ~47 min do instalador em PIO). `scripts/qmon.py` fala com
  o monitor do QEMU onde nao ha `nc` (capturas e `vm.sh rips` voltaram).
  Os scripts de teste (zig) nao sao necessarios: `claude-run.sh` so compila o
  WINE.DLL.
- **O Chromium apagava D:\WINE\APPS (o proprio Claude instalado).** O cache de
  disco falha ("Unable to map Index file"), o Chromium renomeia `GPUCache` para
  `old_GPUCache_000` e apaga recursivamente; no disco o `old_GPUCache_000` tinha
  o conteudo de `D:\WINE` (`.` = cluster de `D:\WINE`) e a delecao levou APPS,
  SYSTEM32, TEMP... Causa: o `fat.c` monta a pasta que vai alterar no buffer
  estatico `sub_dir` e cede a CPU no meio da gravacao; so os `k_f*` dos
  programas pegavam a trava do FS, e o carregador de modulos, o `kernel/wine.c`
  (cabecalho do EXE de cada processo filho), o HOSTS do DNS, o shell etc. liam
  sem ela e trocavam o `sub_dir`; o `dir_flush` gravava a pasta errada. Agora a
  trava (`fs_lock`/`fs_unlock`, recursiva por dono) fica em toda entrada
  publica do `fs/drives.c`; a tela de EXCEPTION grava o CRASH.LOG sem ela
  (`fs_panic`). O `ffmpeg.dll` "nao encontrado" (erro 126) e as quedas dos
  filhos eram consequencia disso.
- **Network Service dado como morto.** O `CreateProcess` volta antes de a
  tarefa principal registrar o filho (`spawn_async`) e o Chromium duplica os
  handles do Mojo para ele logo em seguida: o `DuplicateHandle` nao achava o
  filho e falhava. `wdup_to_child` espera o registro enquanto o pedido daquele
  filho esta na fila (ate 5 s).
- **Renderer com ACCESS_VIOLATION na segunda vista de uma secao.** O
  `vmem_alias` usava o endereco da secao como fisico (memoria baixa 1:1), mas
  com o VirtualAlloc indo primeiro para a memoria virtual a secao pode estar
  acima de 1 TB: a PTE recebia um "fisico" alem do limite da CPU. Agora copia o
  quadro da PTE de origem (commitando se preciso).
- Resultado: navegador, crashpad, GPU, Network Service e um renderer juntos,
  sem excecao, a rede recebendo dados e a janela "Claude" aberta (ainda branca
  aos ~8 min). Disco da VM intacto depois da rodada (conferido com um walker
  de FAT16 no host). Proximo: desempenho (esperas de SRW de 5 s+ no renderer;
  `wine_gen` unico acorda todas as threads de todos os processos a cada
  sinal).

### 2026-11-20: primeiros pixels e esperas por objeto

- **Janela branca: o processo GPU nao conseguia desenhar na janela do
  browser.** Com `--disable-gpu` o viz (compositor) roda no processo GPU e
  pinta com `GetDC(hwnd do browser)` + `StretchBlt` + `ReleaseDC` (confirmado
  no Wine real: ~1100 blits em 5 min, so retangulos de dano). No WINE.DLL o
  HWND e ponteiro da `struct wwin` do proprio processo; o de outro processo
  caia no "DC da tela" e o blit sumia. Agora ha um registro global de janelas
  (`xwins` em `user.c`; o WINE.DLL e uma instancia so e a memoria e um espaco
  so): `GetDC`/`GetClientRect`/`IsWindow` aceitam HWND alheio; o DC desenha na
  superficie do dono e o `ReleaseDC` marca `present_pending` no gui do dono.
  Registro limpo no `win_destroy` e na saida do processo (`wgui_proc_exit`).
- **`wine_gen` unico -> 64 baldes.** Quem espera um objeto (ou varios no mesmo
  balde) dorme no futex do balde do endereco; `SetEvent`, `ReleaseMutex`,
  `ReleaseSemaphore`, timer, fim de thread/processo e post na porta IOCP usam
  `wsignal_obj(o)` (balde + global). Mensagens, pipes sincronos e o desenho
  entre processos usam `wsignal_global()`; o resto (`net.c`, pool de threads)
  continua `wsignal()` (tudo). Contador `waiters` por balde so poupa o
  `k_futex_wake` (geracao sobe sempre; ordem soma-waiters/le-geracao contra
  sobe-geracao/le-waiters evita acordar perdido).
- Medido (KVM, 4 CPUs): do inicio ao 1o renderer 2 min 10 s -> 32 s; 1o quadro
  na janela 1 min 17 s depois do inicio (antes: nunca). Sem excecao. A janela
  mostra os botoes do Electron e a barra cinza do topo; o conteudo do
  claude.ai ainda nao aparece.
- **Referencia: Wine 11 real no Linux** (WineHQ `wine-stable-amd64` +
  `wineserver` do pacote `wine-stable` extraido; o Wine 8.0 do Debian nao tem
  `ProcessPrng` em bcryptprimitives e o Claude para num `int3`). Abre e loga
  no claude.ai; janela em ~5 s. Rastro com
  `WINEDEBUG=+pid,+bitblt,+dc,+win`.
- Relatorio de excecao do WINE agora diz se o acesso era leitura/escrita e o
  estado da pagina (reserva, commit, protecao) para enderecos >= 1 TB
  (`wvm_note`), para o proximo crash na vista repetida de secao.
- Proximo: por que o conteudo do claude.ai nao chega (o renderer da pagina
  sai com 0 pelo retorno do `main`: o browser o dispensou?), comparar com o
  rastro do Wine real.
- **Conteudo do claude.ai chegou a janela** (rodada 01:12): o renderer 300
  carrega o claude.ai e sai com 0 (troca de processo normal do Chromium pelo
  COOP; o 308 assume), o JS da pagina inicializa (DatadogRUM, hCaptcha) e aos
  ~5 min o compositor apresenta o menu (≡) e o botao escuro da tela de login.
  So 5 quadros no total (`ReleaseDC de janela alheia`): o viz so desenha com
  dano, nao e falta de BeginFrame. O gargalo e o tempo da pagina no renderer
  (Wine real: ~5 s).
- `CreateDCW`/`CreateDCA` implementados (DC da tela); antes devolviam 0.
- O `--vmodule` chega aos filhos mas o Electron oficial nao tem VLOG.
- Processos no Wine real (+process): crashpad, GPU, Network, 4 renderers,
  `node.mojom.NodeService` (utilityProcess do app), mais 1 renderer, audio e
  video. No SODOS o NodeService ainda nao tinha subido quando a pagina
  apareceu.

### 2026-11-20 (madrugada): desempenho do kernel e textos

- **Amostragem das vCPUs** (`info registers -a` pelo monitor, 300 vezes;
  simbolizado com `addr2line -f -e kernel.elf`): a CPU0 passava ~70% em
  `drain_locked` (`kernel/ps2in.c`: `inb 0x64` a cada "tem tecla?" do laco do
  WIN; numa VM cada inb e saida para o QEMU) segurando o BKL, e as threads do
  Claude esperavam em `bkl_take`. Correcoes: com IRQ1/IRQ12 ligadas o
  `ps2in_pump` le o 8042 no maximo a cada 20 ms; o laco do WIN ocioso (sem
  entrada, sem tarefa com evento/ocupada, nada a desenhar) chama
  `kt_world_idle(1)` (`world_lend`: solta o BKL e empresta a CPU0) em vez de
  girar com `pause`.
- **Disco**: o cache HTTP do Chromium em D: (ATA por PIO, cli + BKL) parava o
  WIN a cada gravacao. O Electron ignora `--disk-cache-dir`; o WINE.DLL ganhou
  `SET WINEAPPDATA=Z:\WINEAPP` (AppData do perfil em outra pasta; o
  claude-run.sh liga por padrao, `APPDATA_DIR=` desliga). O SET do SODOS corta
  antes de 250 caracteres: o claude-run.sh divide os argumentos em partes de
  200.
- Resultado: 1o renderer 8 s, 1o quadro 17 s, tela de login ~1 min (antes:
  2 min, nunca, ~10 min).
- **Textos sumidos**: no renderer as fontes do sistema chegam pelo
  `DWriteFontCollectionProxy` do Chromium: `CreateCustomFontCollection` (vtable
  4, chave da familia) com arquivos de `CreateCustomFontFileReference` no
  loader dele. Os dois devolviam E_NOTIMPL; agora le cada arquivo pelo stream
  do loader do app (`foreign_load`, reconhece o mesmo arquivo pelo conteudo) e
  devolve uma colecao so com as familias. Rastro do Wine real (`+dwrite`)
  mostrou o resto que o Chromium usa: IDWriteFactory2 (CreateGlyphRunAnalysis
  v2), IDWriteFontFace1..5 (QIs recusados ainda; o texto sai sem eles).
- **ACCESS_VIOLATION do renderer saindo** (rva 0x240b428, escrita "fora de
  qualquer reserva"): o `wexit` soltava as vistas (e a reserva da vista
  repetida) com a thread de E/S do Mojo ainda escrevendo. Agora as vistas de
  quem sai vao para uma lista e so sao soltas 10 s depois; a reserva da vista
  repetida fica com o kernel (`vmem_release_pid`).
- Logs novos (WINEDEBUG=2): DirectWrite textura/contorno/QI recusado,
  `UnmapViewOfFile: solta vista repetida`, `[vmem] release de reserva alheia`,
  `[vmem] fim do pid`, `ReleaseDC de janela alheia, quadros`.
- Proximo: QIs do DirectWrite (Factory2, FontFace1..5), `CreateTextFormat` no
  browser, consoles do Claude.exe tapando a janela, ATA sem cli/BKL.
- **Login (2026-11-20, 02:xx)**: o browser caiu (int3, `rva 0x6c80952`) enquanto
  se digitava o e-mail: `SHGetSpecialFolderPathW` faltava e o gancho de falha de
  delay-load do Chromium aborta. Implementados `SHGetSpecialFolderPathW/A`.
- **Nomes longos**: export novo `k_dir_long` (kexports.c; `struct k_direntl`
  com name[256] em so.h): lista com drives_items e troca cada apelido NOME~N
  pelo nome longo (drives_long_name). `FindFirstFileW` do WINE usa ele. O
  k_dir antigo (name[32]) fica igual para os EXEs ja compilados. Limites como
  no Windows: nome 255, caminho 260 (DRV_PATH); `\\?\` ja era tirado no wpath.
- O Claude recusa abrir com `--log-net-log` ("refusing to start - a debugging
  or network-override switch is present"): sem NetLog do Chromium.
- **"Couldn't finish loading ... allow assets-proxy.anthropic.com"**: o shell do
  app desiste quando o claude.ai demora. Na rodada medida: 6,1 MB em ~80 s
  (~76 KB/s) entre o documento e os assets; e um "DNS falhou: claude.ai" com
  duas threads resolvendo juntas. `GetAddrInfoW` agora serializa o `k_dns` e
  tenta 3 vezes. Proximo: vazao da rede (NE2000 por PIO + slirp + poll de 5 ms
  do wnet_poll); as conexoes vao uma por vez para 160.79.104.10:443.
- **MSG_PEEK** (a causa do "Couldn't finish loading" e do erro de conexao no
  login): `recv(..., MSG_PEEK)` do WINE ignorava a flag e consumia o byte. O
  Chromium testa conexoes ociosas assim; com dado pendente (ticket do TLS 1.3,
  PING do HTTP/2) o registro TLS seguinte chegava quebrado, a conexao caia e o
  claude.ai abria uma nova por recurso (53 conexoes abertas e fechadas, ~76
  KB/s). Agora usa o peek do kernel (`k_sock_recv(..., 1)`); FIONREAD tambem.
  Depois: 10 conexoes, nenhuma fechada cedo, tela de login inteira.
- **Notebook (boot pelo GRUB, F8)**: `make version` + `make real` + `make send`
  (build 11). No SSD (D:): `CLAUDE.EXE` = instalador oficial (267 MB, PE32,
  baixado de api.anthropic.com/api/desktop/win32/x64/exe/latest/redirect),
  pacote ja extraido em `D:\WINE\APPS\Claude_5.11492.5.0_x64__pzs8sxrjxfjjc`,
  `D:\INSTCLAU.BAT` (roda o instalador) e `D:\RODACLAU.BAT` (abre o
  instalado), os dois com os SET da bancada e WINEAPPDATA=Z:\WINEAPP.
- Notebook: o Firefox do SSD ja e o build mais recente (FIREFOX.EXE de
  2026-11-16, 7390 arquivos iguais no make send). `D:\AUTOEXEC.BAT` ganhou
  `set FIREFOX_WORKFOLDER=D:\USER\DEFAULT\FIREFOX` (HOME/perfil, cache e
  TEMP do Firefox; o port cria as pastas). O AUTOEXEC.BAT do SSD nao vem do
  repositorio (make send nao o sobrescreve).
- **Primeiro teste no notebook (build 12, 2026-11-19 23:4x)**: o instalador
  oficial `CLAUDE.EXE` e PE32 (32 bits) e o WINE.DLL so roda x64 ("programa de
  32 bits (ainda nao suportado)"): INSTCLAU.BAT nao serve por ora. Com o
  RODACLAU.BAT o browser cai logo na partida (nao acontece na VM): o crashpad
  handler recebe o pedido de dump, chama NtSuspendProcess (sem
  implementacao) e ReadProcessMemory, falha e sai com 0xFFFF7002; o browser,
  sem resposta, sai com 0xFFFF7001 ("crash server did not respond,
  self-terminating"). Diferenca visivel no log: o notebook chama
  `IsUserCetAvailableInEnvironment` (CPU com CET), a VM nao. O SERIAL.LOG do PC
  real guarda so os primeiros 192 KB (serial.c SLOG_MAX), entao WINEDEBUG=2 nao
  serve la; agora as 3 primeiras excecoes que vao ao filtro do programa saem
  sempre (rip, modulo, rva, pilha), sem WINEDEBUG.
- **Segundo teste no notebook (build 12)**: chegou a todos os processos, mas
  em ~25 min (cada filho levava minutos para subir) e depois tudo fechou: o
  browser chamou `CreateRemoteThread` (o Chromium forca o dump de um renderer
  que nao responde, DumpAndCrashTargetProcess) e o stub de funcao ausente
  encerrava o processo (0xC0000139), levando os filhos junto. Agora
  CreateRemoteThread(Ex) cria a thread no proprio processo e recusa outro
  processo com ERROR_ACCESS_DENIED.
- **Lentidao no notebook = FAT32.DLL**: uma so posicao de leitura e uma so
  janela da FAT (16 KB). Com varios processos lendo arquivos grandes
  (Claude.exe 250 MB, app.asar, .pak) a posicao se perdia a cada chamada e
  cada leitura andava a cadeia desde o primeiro cluster, recarregando a janela.
  Agora: 8 posicoes por arquivo (fim e comeco da ultima leitura), e um cache
  so de leitura de 4 janelas de 16 KB para o fat_next (zerado a cada
  fat_set). Teste no host (2 arquivos de 120/90 MB lidos intercalados, 6000
  leituras de 64 KB): FAT lida 320 MB -> 12 MB, total 742 -> 410 MB, dados
  conferidos; escrita/rename/remove + fsck.fat ok. Build 13 no SSD.
- **Amostrador de CPU no kernel** (`smp prof on|off|save`, isr.c + cmd_smp.c):
  a cada tique (LAPIC 4 ms; PIT da CPU0 1 a cada 4) guarda cpu<<56 | pid<<48
  | rip num anel de 131072; com `on` uma thread fora da CPU0 grava sozinha a
  cada 60 s em D:\LOG\PROF0..4.BIN (o sistema travado nao deixa digitar). O
  RODACLAU.BAT liga. Analise: profan.py (nm do ELF certo: kernel.elf na VM,
  kernel_multiboot.elf no notebook).
- **Causa da lentidao no notebook (build 15)**: CPU0 99,9% em `bkl_take` e as
  outras em `kt_tlb_shootdown`. Quem tinha o BKL alterava paginas
  (VirtualAlloc/Protect) e esperava a confirmacao da TLB de todas as CPUs; a
  CPU0 esperava o BKL com interrupcoes desligadas e nao atendia a IPI, entao o
  dono girava ate o limite (50M voltas, segundos no hardware real) a cada
  operacao. `kt_tlb_poll()` atende pedido pendente de limpeza sem a IPI, nos
  lacos do `bkl_take` e do proprio `kt_tlb_shootdown`.
- **DirectWrite v2/v3** (o browser aborta sem eles): CHECK(factory2_) no
  MapCharacters do proxy de fontes -> IDWriteFactory1/2 (GetSystemFontFallback
  com MapCharacters sobre as fontes carregadas, FontFallbackBuilder,
  CreateGlyphRunAnalysis v2 com tons de cinza); CHECK(factory3_) no
  InitializeDirectWrite -> IDWriteFactory3 (so analise/params; conjuntos de
  fontes E_NOTIMPL, que o Chromium aceita). `CreateNumberSubstitution`
  devolvia E_NOTIMPL e o browser recursava ate estourar a pilha: tudo em ring
  0, a VM reiniciava (triple fault). Na VM: login com todos os textos, sem
  queda. Build 16 no SSD.
- **Build 17 (login no notebook)**: (1) tecla `.` e pontuacao: vk_of mandava o
  ASCII como VK ('.' = VK_DELETE, '!' = VK_PRIOR...); agora VK_OEM_* e os
  digitos para os simbolos com Shift. (2) URL no ShellExecute (login abre o
  navegador): a thread enfileira e a tarefa principal chama k_open_file (o
  LINKEXT entende `esquema://` como extensao: `HTTP,HTTPS=D:\FIREFOX\FIREFOX.EXE`
  no LINKEXT.CFG / "Abrir com"); sem associacao, SET WINEBROWSER ou o Firefox
  do SODOS. O argumento do programa aberto pelo WIN passou de 128 para 2048
  bytes. (3) Ctrl+V: o WINE pede k_gui_clip(1), Ctrl+letra vira Ctrl+tecla
  com GetKeyState de verdade (estado atualizado ao tirar a mensagem da fila),
  e o clipboard do WINE e o do SODOS (k_clip_get/set, UTF-16 para o
  programa; sequencia muda com o conteudo).
- **Login com Google ate o Firefox (2026-11-20, VM)**: "Continue with Google"
  abre `https://claude.com/cai/login/app-google-auth?...` e o Firefox do SODOS
  mostra o "Sign in with Google" (accounts.google.com). Corrigido:
  (1) o Electron (`shell.openExternal`) manda a URL entre aspas
  (`"https://..."`) e o `is_url` recusava; o ShellExecuteExW tira as aspas.
  (2) HKCR\http(s)\shell\open\command no registro (o comando e so para
  constar). (3) O navegador vem SO do LINKEXT (`SET LINKEXT=1` +
  `HTTP,HTTPS=D:\FIREFOX\FIREFOX.EXE` no LINKEXT.CFG; content/LINKEXT.CFG ja
  traz a linha); sem associacao, `SET WINEBROWSER` ou aviso no console (sem
  caminho fixo do Firefox). (4) WIN: TASK_MAX 8 -> 16, GUI_MAX 8 -> 16,
  WM_MAX 12 -> 24 — os 6+ processos do Claude enchiam a tabela e o Firefox
  nao abria ("janelas de programa cheias"). (5) Kernel: `add_to_history`
  copiava sem limite para a entrada de 128 bytes; o `SET WINE_ARGS=` de ~200
  caracteres do claude-run.sh transbordava em `mem_file_count` e o kernel
  caia em `find_mem_file` (#PF, CR2 0x240000000). (6) claude-run.sh: opcoes
  padrao sem `--single-process` (CHECK na partida, F1) e `SET LINKEXT=1`.
  Depois do clique o claude.ai tambem mostra um CAPTCHA na janela do Claude.
- **Captcha (hCaptcha) com imagens (20/11/2026, VM).** As imagens ficavam em
  branco porque o renderer caia com 0xe0000008 (sem memoria) em
  `AllocateLockedDiscardableMemoryWithRetryOrDie`: o navegador criava a secao
  de 4 MB do `DiscardableSharedMemory`, mas o `VirtualQuery` numa vista de
  `MapViewOfFile` respondia MEM_FREE com base 0, o `mapped_size()` saia
  absurdo e a alocacao era recusada. Correcoes:
  - `VirtualQuery` responde as vistas de secao (base, tamanho, MEM_MAPPED;
    `wview_query` em map.c);
  - heap do programa (`wheap_*` em util.c): malloc/HeapAlloc/GlobalAlloc/
    CoTaskMemAlloc/IMalloc com blocos >= 64 KB acima de 4 GB (k_hmalloc);
  - log: falha de commit no VirtualAlloc, enderecos recusados e, em excecao,
    pilha pelo unwind (.pdata) a partir de varios retornos candidatos;
  - `vm.sh keys` digita "@"; `NETFWD` abre portas extras (o Claude recusa
    `--remote-debugging-port`).
