# Driver de placa de video (aceleracao grafica)

Estado (2026-11): **etapas 1 a 4 feitas**. O kernel acha, carrega, testa,
suspende e desliga um driver de placa; o `VIRTGPU.DLL` acelera o QEMU com
`-device virtio-vga` (`make run-virtio`); o kernel diz ao driver quais
areas mudaram, entao so elas vao para a placa; e a placa desenha o cursor,
que sai do quadro (medidas abaixo). Para o notebook (Radeon Barcelo) ha o
`AMDGOP.DLL`, configurado pelo `U:\SODOS.CFG`, que ja rodou no hardware
(2026-11-08): copia por quadro 8 vezes menor que a do quadro inteiro.

## Regra principal

> Sem um driver da placa **existente, operacional e ativo**, o SODOS roda
> exatamente como hoje: nenhum byte de memoria a mais para graficos e nenhuma
> instrucao a mais por pixel.

Na pratica:

- O driver **nao fica dentro do kernel**. Ele e um `.DLL` separado em
  `D:\SODOS\DRV\`, carregado so quando a placa dele e encontrada no PCI.
  Sem a placa, o arquivo nem e lido do disco.
- O caminho atual por CPU (`fill_rect`, `gui_blit_win`, `gfx_copy_back`,
  `gfx_clear`, `gfx_desk_clear` em `video/visual.c`) **nao e reescrito** e
  **nao passa a ser chamado por tabela de funcoes**. Sem driver, executa as
  mesmas instrucoes de hoje.
- Superficies, filas de comandos, listas de areas alteradas e qualquer
  estrutura do renderizador vivem **dentro do DLL**, alocadas por ele so
  depois que a placa respondeu.

## O que o kernel ganha (opcao A)

Esta e a escolha feita: um ponteiro testado uma vez por quadro. Todo custo
fixo fica listado aqui, medido, para nao crescer sem ninguem ver.

| Item | Custo sem driver | Onde |
|---|---|---|
| Teste `if (gpu_drv)` | 1 comparacao por quadro, so quando o quadro foi montado em RAM | `gfx_frame_end` |
| Teste `!gfx_for_gpu` | 1 comparacao por troca de modo | `gfx_buf_init` |
| `gfx_gpu_try()` | por troca de modo do WIN: `gpu_wanted()` le o PCI (ate 64 leituras de configuracao) e compara com a tabela | `gfx_session_run`, volta do Ctrl+F7 |
| `gpu_suspend()` / `gpu_close()` | 1 chamada que volta na hora, ao sair do WIN ou ir para uma tela do DOS | `win_vt_away`, fim do WIN |
| `gpu_cfg_line()` | as linhas `gpu...` do `VIDEO.CFG`, que o WIN ja le | `win_load_cfg` |
| Codigo e textos de `gpu.c` + chamadas (etapas 1 e 2) | **3.552 bytes** no `kernel.bin` (402.472 -> 406.024) | `video/gpu.c` |
| Variaveis (etapas 1 e 2) | 72 bytes de BSS (`gpu_drv`, estado do carregador, `gfx_for_gpu`); couberam no alinhamento | `video/gpu.c`, `video/visual.c` |
| **Etapa 3 (areas):** `if (gpu_drv)` | 1 teste por `gui_present` de programa, 1 por movimento do cursor fora de quadro, 2 por passo de arrasto | `gui_present`, `cursor_draw`, `dc_present` |
| Etapa 3: `need_draw` 2 | `win_api_dirty |= 2` no lugar de `= 1` (mesmo custo); 2 comparacoes quando ha `win_api_dirty`; 1 atribuicao por quadro (`gfx_frame_dmg`) | laco do WIN |
| Etapa 3: codigo | **1.088 bytes** no `kernel.bin` (406.024 -> 407.112) | `video/visual.c` |
| Etapa 3: variaveis | 192 bytes de BSS (lista de 10 areas, cursor mostrado) | `video/visual.c` |
| **Etapa 4 (cursor):** `if (gpu_hwcur)` | 1 teste por desenho do cursor (`cursor_redraw_at`, uma vez por quadro; `cursor_draw`, uma vez por movimento) e 1 por passo de arrasto | `video/visual.c` |
| Etapa 4: codigo | **928 bytes** no `kernel.bin` (407.112 -> 408.040) | `video/visual.c`, `video/gpu.c` |
| Etapa 4: variaveis | 16 bytes de BSS (`gpu_hwcur` e alinhamento) | `video/gpu.c` |
| **Notebook:** `gpu_wanted()` | segue as pontes PCI (no QEMU: so o barramento 0; no notebook: ate o 4); por troca de modo do WIN | `video/gpu.c` |
| Notebook: `SODOS.CFG` | uma leitura por abertura do WIN (D: e depois U:; o U: so e montado para isso uma vez por boot) | `video/gpu.c` |
| Notebook: codigo | **7.872 bytes** no `kernel.bin` (408.040 -> 415.912): leitura do `SODOS.CFG`, busca nas pontes e `GPU.LOG` | `video/gpu.c` |
| Notebook: variaveis | 352 bytes de BSS (ate 4 placas do `SODOS.CFG`); o buffer do `GPU.LOG` (8 KB) so existe com driver para tentar | `video/gpu.c` |

O teste nunca entra em funcoes chamadas por pixel ou por glifo (`put_pixel`,
`gui_glyph`, `gui_pixels`). Se uma etapa futura quiser acelerar
`fill_rect` ou `blit` com a placa, isso adiciona um teste por **chamada**
(nao por pixel) e precisa ser decidido e registrado nesta tabela antes de
entrar no codigo.

A alternativa recusada (opcao B) era o driver reescrever a primeira
instrucao das funcoes de desenho com um salto, como o ftrace do Linux: custo
zero absoluto, mas fragil (patch com interrupcoes desligadas, enderecos das
funcoes expostos ao driver). Fica registrada caso a opcao A um dia pese.

## Ciclo de vida

```
WIN abre (ou volta com Ctrl+F7)
  |
  win_load_cfg(): gpu=0? gpudrv=?
  gfx_gpu_try()
  |
  gpu_wanted(): placa de video no PCI esta na tabela (ou gpudrv=)?
  |-- nao, ou gpu=0, ou ja falhou neste WIN -> fim. gpu_drv = 0.
  |
  quadro em pagina dupla? remonta em RAM (gfx_for_gpu)
  |
  mod_dll_load("D:\SODOS\DRV\<nome>.DLL"), simbolo gpu_driver
  |-- falhou (arquivo, formato, ABI) -> log, quadro como antes, fim.
  |
  drv->init(&kapi, &modo)            driver liga a placa, aloca o que precisa
  drv->selftest()                    prova que a placa responde
  |-- erro -> drv->fini(), mod_dll_free(), quadro como antes, fim.
  |
  gpu_drv = drv                      so aqui o kernel passa a usar o driver
```

| Evento | O que acontece |
|---|---|
| Cada quadro | `gfx_frame_end` chama `gpu_present()` em vez de `gfx_copy_back()` |
| `present` devolve erro | `gpu_drv = 0` primeiro, o mesmo quadro sai por `gfx_copy_back()`, `fini` + `mod_dll_free` |
| Ctrl+F1..F12 (WIN ao fundo) | `suspend()`: o driver devolve a tela ao modo texto e guarda o que reservou. Sem `suspend`, e descarregado |
| Ctrl+F7 (WIN volta) | `resume(modo)` com o modo reprogramado; se falhar, descarrega e fica na CPU |
| Fim do WIN | `fini` + `mod_dll_free` |

Um driver que falhou nao e tentado de novo ate o WIN abrir outra vez
(evita ler o disco a cada Ctrl+F7 com um driver quebrado).

O DLL e carregado no contexto do kernel (`cur_pid == 0`), entao nao e
liberado quando um programa termina (`module.c` so libera DLLs ligadas a um
pid).

## Interface kernel <-> driver

Em `video/gpu.h`, usado pelo kernel e pelo fonte do driver.
`abi` permite ao kernel recusar um driver compilado para outra versao.

```c
#define GPU_ABI 1

struct gpu_kapi {              /* o que o kernel oferece; nada de libc */
  int abi;
  void (*log)(const char* s);
  void (*log_hex)(uint32_t v);
  uint32_t (*pci_read32)(uint32_t bus, uint32_t dev, uint32_t fun, uint32_t off);
  void (*pci_write32)(uint32_t bus, uint32_t dev, uint32_t fun, uint32_t off, uint32_t v);
  void* (*alloc)(uint32_t bytes, uint32_t align);   /* fisico = virtual */
  void (*free)(void* p);
  uint32_t (*ms)(void);                             /* relogio para timeouts */
};

struct gpu_mode {              /* placa achada e quadro que o kernel usa */
  uint32_t w, h, pitch;
  void* back;                  /* back buffer em RAM (gfx_back), 32 bpp */
  volatile uint8_t* fb;        /* framebuffer linear */
  uint32_t fb_pitch;
  uint32_t pci_id, pci_bus, pci_dev, pci_fun;
};

struct gpu_drv {
  int abi;
  const char* name;
  uint32_t caps;               /* GPU_CAP_PRESENT obrigatorio */
  int (*init)(const struct gpu_kapi* k, const struct gpu_mode* m);
  int (*selftest)(void);
  int (*present)(const struct gpu_rect* rects, int n);   /* rects = 0: tela toda; areas so com GPU_CAP_DAMAGE */
  void (*fini)(void);
  int (*suspend)(void);                       /* opcional */
  int (*resume)(const struct gpu_mode* m);    /* opcional */
  int (*cursor_set)(const uint32_t* argb, int w, int h, int hot_x, int hot_y);  /* GPU_CAP_CURSOR */
  int (*cursor_move)(int x, int y);                                            /* GPU_CAP_CURSOR */
};

const struct gpu_drv* gpu_driver(void);   /* unico simbolo exportado */
```

Tudo que o driver pede por `kapi.alloc` e contado e aparece no TOP.

Regras para o DLL, porque ele roda no contexto do kernel:

- formato SOX2 (ABI LP64, a mesma do kernel), ligado com
  `link64 /dll /export:gpu_driver`;
- compilado com `gcc -m64 -mno-sse -mno-mmx -mno-red-zone -mcmodel=large
  -ffreestanding` (o kernel nao salva SSE e as interrupcoes usam a mesma
  pilha); regra `DRVCC` em `c_compiler/Makefile`;
- sem libc e sem `k_*` de programa: so o que vem em `gpu_kapi`;
- nao pode bloquear sem timeout. Toda espera usa `k->ms()` e devolve erro.

## Como o driver se encaixa no quadro

`gfx_buf_init()` escolhe entre dois jeitos de montar o quadro:

- **pagina dupla Bochs** (`gfx_dbuf`): desenha direto na outra metade da VRAM
  e troca o `Y_OFFSET`, quando a VRAM tem altura virtual 2*h;
- **back buffer em RAM** (`gfx_back`, em `GFX_BACK_PHYS`): desenha na RAM e
  `gfx_copy_back()` copia o quadro inteiro para o framebuffer.

O driver usa o back buffer em RAM, entao `gfx_gpu_try()` remonta o quadro em
RAM (`gfx_for_gpu`) antes de chamar o driver, e volta a escolha de sempre se
o driver nao entrar. Sem back buffer em RAM (`gfx_ram_ok()` falhou), o driver
nao e carregado. `gfx_frame_begin` nao muda.

No **virtio-gpu 2D** (QEMU `-device virtio-vga`, `drivers/virtgpu/virtgpu.c`):

| Plano do renderizador | Comando virtio-gpu |
|---|---|
| criar superficie | `RESOURCE_CREATE_2D` (B8G8R8X8) |
| ligar a memoria | `RESOURCE_ATTACH_BACKING` com as paginas de `gfx_back` |
| escolher a tela | `SET_SCANOUT` |
| `upload` + `present` | `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` com o retangulo |
| `submit/fence` | fila de controle virtio com `VIRTIO_GPU_FLAG_FENCE` |
| cursor | fila de cursor (`UPDATE_CURSOR`/`MOVE_CURSOR`) |

Como o back buffer ja e a memoria do recurso, o guest **nao copia** os
8 MB do quadro (1920x1080x4): quem le e o QEMU. A CPU do SODOS faz menos
trabalho que no caminho atual.

O `virtio-vga` tambem responde ao Bochs VBE, entao, se o driver falhar, o
caminho atual continua funcionando na mesma VM.

Detalhes do `VIRTGPU.DLL`:

- virtio 1.0 moderno: acha as capabilities PCI `0x09` (comum e notificacao)
  no BAR2 (no QEMU, `0xFE800000`, abaixo de 4 GB; BAR acima de 4 GB e
  recusado porque o kernel so mapeia ate 4 GB);
- negocia so `VIRTIO_F_VERSION_1` (sem virgl, sem EDID); uma fila (controlq)
  de 16 entradas, em polling, timeout de 1 s;
- `init`: recurso 2D `B8G8R8X8` do tamanho da tela, `ATTACH_BACKING` com o
  `gfx_back` (contiguo) e `SET_SCANOUT`; `selftest`: `GET_DISPLAY_INFO`;
- `suspend`: reset do dispositivo (a parte VGA volta a mostrar o texto);
  `resume`: liga de novo e remonta a tela, reaproveitando a memoria;
- `present`: um `TRANSFER_TO_HOST_2D` por area (com o `offset` da area no
  back buffer) e um `RESOURCE_FLUSH` com a uniao; fila de 64 entradas;
- cursor: segunda fila (cursorq, indice 1); `cursor_set` cria o recurso 2
  (64x64 `B8G8R8A8`, uma vez por reset), copia a imagem e manda
  `UPDATE_CURSOR`; `cursor_move` manda `MOVE_CURSOR`. Sem a segunda fila o
  driver funciona e o cursor fica com a CPU;
- reserva cerca de 29 KB (duas filas, pedidos e a imagem do cursor), que o
  TOP mostra;
- `-DVG_MEDIR` liga a medida (ciclos e pixels por apresentacao no log).

Medida (QEMU 1920x1080 com KVM, contador de ciclos em volta do fim do
quadro, instrumentacao temporaria fora do commit):

| Caminho | Ciclos por quadro |
|---|---|
| CPU: `gfx_copy_back()` para o LFB Bochs | ~400 milhoes |
| `VIRTGPU.DLL`: `present` | ~1,8 milhao |

O caminho por CPU e tao caro no QEMU porque copia 8 MB para o LFB, e no
boot BIOS o `kernel_entry.asm` mapeia o LFB em `0xE0000000`/`0xFD000000`
com `0x9B` (PWT|PCD): sem cache, cada escrita vai sozinha. Mapear como
write-combining aceleraria o caminho por CPU, mas e mudanca no caminho por
CPU e fica de fora daqui.

## Areas alteradas (etapa 3)

O WIN redesenha o quadro inteiro (`need_draw`) e nao guarda o que mudou.
Por isso as areas so vem de onde o kernel ja sabe exatamente o retangulo; o
resto continua sendo a tela inteira:

| De onde | Area | Como |
|---|---|---|
| Cursor movido fora de quadro (`cursor_draw`) | caixa velha + caixa nova | apresenta na hora |
| Arrasto de janela (`dc_present`) | o retangulo que ele ja calcula | apresenta na hora |
| Programa atualizando a propria janela (`gui_present` vindo de um programa) | a janela (titulo + conteudo) | `win_api_dirty = 2` vira `need_draw = 2`; o quadro inteiro e redesenhado na RAM, mas so a janela (mais as duas posicoes do cursor) vai para a placa |
| Qualquer outro pedido de redesenho (`need_draw = 1`, 52 lugares) | tela inteira | como antes |

Regras que mantem isso certo:

- qualquer `win_api_dirty = 1` ou `need_draw = 1` no mesmo quadro vence o 2:
  na duvida, vai a tela inteira;
- mais de 10 areas num quadro tambem vira a tela inteira; o driver junta
  mais de 12 numa so;
- com driver, `gfx_offscreen` fica 1 entre os quadros: o back buffer e a
  tela, e o que se desenha fora de quadro vai para ele. Sem isso o cursor e o
  arrasto escreviam no LFB, que o virtio nao mostra (o cursor ficava parado
  na tela ate o proximo quadro inteiro; defeito da etapa 2, corrigido aqui).

Sem driver nada disso roda: `gfx_dmg_add` so e chamado depois de
`if (gpu_drv)`, e o LFB continua sendo escrito como antes.

Medida (QEMU 1920x1080 com KVM, driver compilado com `-DVG_MEDIR`, media de
64 apresentacoes movendo o cursor, digitando no painel de IA e com alguns
quadros inteiros no meio):

| Etapa | Ciclos por apresentacao | Pixels por apresentacao |
|---|---|---|
| 2: tela inteira | ~1,7 milhao | 2.073.600 |
| 3: areas do kernel | ~213 mil a ~318 mil | ~112 mil a ~204 mil |

Antes disso foi tentado descobrir as areas **dentro do driver**, comparando
cada quadro com uma copia do anterior (8 MB a mais, blocos de 64x32). Mandou
40 vezes menos pixels, mas custou **mais** que a tela inteira (~1,95 a
2,4 milhoes de ciclos), porque comparar le 16 MB por quadro. Foi descartado.

## Cursor pela placa (etapa 4)

Com `GPU_CAP_CURSOR`, logo depois de o driver entrar (e de novo depois de
cada `resume`), o kernel monta a seta (`cur_row`, na escala da tela: 22x34 em
1920x1080) em ARGB e chama `cursor_set`. Se o driver aceitar, `gpu_hwcur`
vira 1 e:

- `cursor_redraw_at` e `cursor_draw` so mandam a posicao (`cursor_move`) e
  nao desenham nada no quadro; `cursor_saved` fica 0, entao nao ha o que
  restaurar;
- o arrasto (`dc_present`) monta a imagem sem a seta e move o cursor da placa;
- as areas da etapa 3 deixam de incluir as caixas do cursor: mover o mouse
  nao manda pixel nenhum, so um comando de 56 bytes.

Se `cursor_move` falhar, o driver e desligado como em qualquer erro, o quadro
volta pela CPU e o proximo quadro inteiro ja desenha a seta por software.

O `/win/printscreen` (e o `screendump` do QEMU) le o quadro, entao **nao
mostra o cursor** quando ele esta na placa.

## Ver no TOP (aba Video)

O `TOP.EXE` tem a aba **Video** (F3 ou Tab ate ela), que le `k_vram`
(`struct k_vram` em `so.h`; no kernel, `win_vram_info()` em `video/visual.c`):

- placa (Bochs VBE, GOP ou VGA), id PCI e endereco do framebuffer;
- driver: "nenhum: a CPU desenha tudo", "X (ativo)" ou
  "X (parado com o WIN ao fundo)";
- modo e estado: texto, WIN na tela ou WIN ao fundo;
- a VRAM num quadro, como o mapa da RAM: tela visivel, segunda pagina
  (pagina dupla Bochs), texto e fonte, livre;
- a RAM comum gasta por causa do video: back buffer, papel de parede,
  superficies das janelas e memoria do driver da placa.

`k_vram` so le o estado que ja existe; nao reprograma a placa. No GOP a placa
so informa o quadro visivel, entao o total da VRAM e o tamanho desse quadro.

O TOP roda numa tela de texto, entao ve o driver suspenso (WIN ao fundo),
com a memoria que ele reservou. Sem driver, a linha "Driver da placa" mostra
"sem driver: nada reservado", que e a prova visivel da regra principal.

## SODOS.CFG (placas e drivers)

Arquivo do computador, procurado em `D:\SODOS.CFG` e depois em
`U:\SODOS.CFG` (o notebook nao tem D:). A secao `[video]` tem uma entrada
por placa, cada uma comecando em `placa=`:

```
[video]
placa=1002:15E7
nome=AMD Radeon Graphics (Barcelo, Ryzen 7 5825U)
driver=AMDGOP.DLL
ativo=1
```

- `placa=` vendor:device do PCI (no Linux, `lspci -nn`);
- `nome=` aparece no TOP (aba Video) e no `GPU.LOG`;
- `driver=` DLL em `X:\SODOS\DRV`, na mesma unidade do `SODOS.CFG`;
- `ativo=0` deixa a placa com a CPU, mesmo que ela tenha driver.

Uma entrada do `SODOS.CFG` vence a tabela embutida (`VIRTGPU.DLL` para o
virtio). O kernel le o arquivo a cada abertura do WIN; o TOP o le uma vez.

## GPU.LOG

O notebook nao tem COM1. Sempre que ha driver para tentar, o kernel grava
`X:\LOG\GPU.LOG` (na unidade do `SODOS.CFG`, ou `D:\LOG`, junto do `SERIAL.LOG` e do `BOOT.LOG`; antes de 2026-11-17 era `X:\SODOS\GPU.LOG`) com a placa achada
(id, barramento:dispositivo:funcao, nome), o modo e o framebuffer, o caminho
do DLL, o que o driver escreveu e cada evento (ativo, suspenso, ativo de
novo, desativado e o motivo). O arquivo e regravado ao ativar, ao suspender
(Ctrl+F1, para poder le-lo numa tela do DOS), ao voltar e ao descarregar.
O buffer (8 KB) so existe enquanto ha driver para tentar.

## AMDGOP.DLL (notebook)

A placa do notebook e a Radeon integrada do Ryzen 7 5825U: **AMD Barcelo,
PCI 1002:15E7**, no barramento 4 (`04:00.0`), atras de uma ponte. Por isso a
busca PCI agora segue as pontes.

Ligar o motor da GPU (copia pela SDMA, desenho 2D) exige carregar firmware
pelo PSP/SMU, que e o grosso do amdgpu do Linux. O `AMDGOP.DLL` nao faz
isso: e um driver de exibicao sobre o framebuffer que o UEFI (GOP) ja deixou
na tela (mapeado com write-combining pelo `kernel_entry.asm`). O ganho e
copiar para a tela so as areas que o kernel diz que mudaram (etapa 3), em vez
do quadro inteiro. Sem cursor pela placa por enquanto: os registradores do
cursor do DCN sao o proximo passo possivel, mas um erro ali pode travar a
tela num hardware real.

O que ele escreve no `GPU.LOG`:

- id, classe/revisao e os seis BARs da placa;
- tela, endereco e pitch do framebuffer;
- no `selftest`: escreve e le de volta um padrao no framebuffer, e mede os
  ciclos de **uma copia do quadro inteiro** (o custo do caminho por CPU);
- no fim (Esc do WIN): quadros, ciclos por quadro e pixels por quadro, para
  comparar com a copia inteira.

Teste no QEMU (o QEMU nao emula a Radeon; o `SODOS.CFG` de teste aponta a
placa Bochs `1234:1111` para o `AMDGOP.DLL`):

| Caminho | Copia do quadro inteiro |
|---|---|
| BIOS, `-vga std` (LFB em `0xFD000000`, sem cache) | ~404 milhoes de ciclos (1920x1080) |
| UEFI (OVMF), `-vga none -device bochs-display`: GOP de verdade, como no notebook | ~2,1 milhoes de ciclos (1280x800) |

No caminho GOP (o do notebook) o driver entrou pelo `SODOS.CFG`, o TOP
mostrou o nome da placa e "(GOP (UEFI))", e Ctrl+F1/Ctrl+F7 suspendeu e
retomou o driver (antes so o caminho Bochs retomava o driver na volta).

Para testar no notebook:

1. `make grub` (kernel novo em `/boot/sodos/kernel.elf`; pede sudo);
2. no pendrive: `U:\SODOS.CFG`, `U:\SODOS\DRV\AMDGOP.DLL`, `U:\TOP.EXE`;
3. iniciar o SODOS, `win`, usar um pouco, Ctrl+F1 e `top` (aba Video);
4. sair do WIN com Esc e trazer o `U:\LOG\GPU.LOG`.

### Primeiro boot no notebook (2026-11-08)

O `GPU.LOG` do primeiro boot:

```
[gpu] placa 100215E7 em 00040000 (AMD Radeon Graphics (Barcelo, Ryzen 7 5825U)), SODOS.CFG em U:
[gpu] modo 00000780x000004B0 fb D0000000 pitch 00001E00
[gpu] -> U:\SODOS\DRV\AMDGOP.DLL
[amdgop] pci 100215E7 classe/rev 030000C1
[amdgop] BARs D000000C 00000000 E000000C 00000000 0000D001 FCC00000
[amdgop] copia do quadro inteiro: ciclos 002F34C4
[gpu] ativo: amdgop (Radeon, GOP)
[gpu] suspenso / ativo de novo  (10 vezes)
[gpu] desativado: fim do WIN
[amdgop] fim: quadros 00001934 ciclos/quadro 0005D829 pixels/quadro 0003BFFB
```

O que ele diz:

- a busca pelas pontes achou a placa em `04:00.0`, e o `SODOS.CFG` do U:
  escolheu o driver;
- tela de 1920x1200; o framebuffer GOP (`0xD0000000`) e o BAR0 da placa
  (abertura da VRAM, 64 bits, prefetch); BAR2 `0xE0000000` (doorbells),
  BAR4 portas `0xD000`, BAR5 `0xFCC00000` (registradores, onde ficaria o
  cursor do DCN);
- **copia do quadro inteiro: 3.093.700 ciclos** (~1,5 ms a 2 GHz, ~6 GB/s
  com write-combining), que e o custo de cada quadro pelo caminho por CPU;
- **com o driver: 383.017 ciclos por quadro** (~0,19 ms) e 245.755 pixels
  por quadro (10,7% da tela), na media de 6.452 quadros, contando os quadros
  inteiros: **8,1 vezes menos** que o caminho por CPU;
- 10 idas e voltas com Ctrl+F1/Ctrl+F7 sem erro, e saida limpa no Esc.

### Cursor pelo DCN (AMDCUR.DLL)

A Barcelo usa o DCN 2.1 (o mesmo da Renoir no Linux, `CHIP_RENOIR`).
Registradores no BAR5 (`0xFCC00000`), em `(0x34C0 + registrador) * 4`,
offsets de `dcn_2_1_0_offset.h` do Linux 6.12.

**Etapa A (so leitura, 2026-11-08).** O `AMDGOP.DLL` le e registra o estado
que o firmware deixou:

```
[amdgop] dcn fb_base 0000F400 fb_top 0000F41F fb_offset 00000810
[amdgop] pipe 00000000 hubp_cntl 000E0000 surf 000000F4:00000000 pitch 00000780
[amdgop]   cur_ctl 01000000 cur_addr 00000000:00000000 size 00000000 ...
[amdgop]   cnvc_cur 00000084
[amdgop] pipe 00000001..3 surf 00000000:00000000 pitch 00000000
```

- o DCN ve a VRAM em `0xF4_0000_0000`..`0xF4_1FFF_FFFF` (512 MB);
- o **pipe 0** mostra a tela: superficie em `0xF4_0000_0000` (o inicio da
  VRAM, que a CPU ve em `0xD0000000`), pitch 1920; os pipes 1-3 estao vazios;
- o cursor esta desligado em todos os pipes.

**Etapa B (`AMDCUR.DLL`, `-DAMD_CURSOR`).** O mesmo fonte do `AMDGOP.DLL`
com `GPU_CAP_CURSOR`, seguindo `hubp2_cursor_set_attributes`,
`hubp1_cursor_set_position` e `dpp2_set_cursor_attributes`:

- so usa o pipe cuja superficie nao e 0 e cujo pitch e a largura da tela;
  sem ele, recusa o cursor e a CPU continua desenhando (testado no QEMU);
- imagem 64x64 ARGB pre-multiplicado 16 MB adiante do inicio da VRAM (CPU
  `fb + 0x01000000`, GPU `superficie + 0x01000000`, conferido dentro de
  `fb_base`..`fb_top`); os 16 KB que estavam la sao guardados e devolvidos
  no fim;
- HUBP: endereco, `CURSOR_SIZE` 64x64, `CURSOR_CONTROL` modo 2, pitch 64,
  8 linhas por bloco; `CURSOR_SETTINGS` ajuste 3; DPP: `CUR0_MODE` 2; liga
  os dois; `cursor_move` so escreve `CURSOR_POSITION`;
- suspender (Ctrl+F1) e sair desligam o cursor;
- escolhido pelo `SODOS.CFG` (`driver=AMDCUR.DLL`); voltar para
  `driver=AMDGOP.DLL` tira o cursor da placa sem mexer em mais nada.

**Primeiro boot com o `AMDCUR.DLL` (2026-11-08): o cursor apareceu e segue
o mouse.** O `GPU.LOG`:

```
[amdcur] pipe 00000000 imagem gpu 000000F4:01000000 cur_ctl 03000201 cnvc 000100A5 size 00400040
[gpu] cursor pela placa: amdcur (Radeon, GOP, cursor DCN)
[gpu] desativado: fim do WIN
[amdgop] fim: quadros 00000004 ...
```

- `cur_ctl 03000201`: ligado, modo 2, pitch 64, 8 linhas por bloco, como
  programado; `cnvc 000100A5`: ligado, modo 2 (o bit 16 e o
  `CUR0_UPDATE_PENDING`, a troca acontece no proximo quadro da tela);
- so 4 quadros na sessao inteira: mover o mouse ja nao gera quadro nenhum
  (so escreve `CURSOR_POSITION`), e os 4 sao os redesenhos completos;
- falta exercitar Ctrl+F1/Ctrl+F7 com o cursor da placa (nao houve nesta
  sessao).

## Radeon RX 570 (desktop, Polaris 10)

A placa do desktop e uma **Radeon RX 570 (Polaris 10, GCN 4 / GFX8),
PCI 1002:67DF**, no barramento 1 (`01:00.0`). O mesmo PC tem a Radeon
integrada do Ryzen 7000 (`1002:164E`, RDNA2), sem uso no SODOS. O
`D:\SODOS.CFG` aponta a RX 570 para o `AMDGOP.DLL`:

```
[video]
placa=1002:67DF
nome=AMD Radeon RX 570 (Polaris)
driver=AMDGOP.DLL
ativo=1
```

### Primeiro boot com o AMDGOP.DLL (2026-11-17)

```
[gpu] placa 100267DF em 00010000 (AMD Radeon RX 570 (Polaris)), SODOS.CFG em D:
[gpu] modo 00000780x00000438 fb 00000000 pitch 00001E00
[amdgop] pci 100267DF classe/rev 030000EF
[amdgop] BARs 0000000C 000000FB 0000000C 000000FC 0000F001 F6F00000
[amdgop] copia do quadro inteiro: ciclos 004F6376
[gpu] ativo: amdgop (Radeon, GOP)
```

- tela 1920x1080; o `fb 00000000` era o log cortando o endereco em 32 bits:
  o framebuffer e o **BAR0 de 64 bits em `0xFB_0000_0000`** (acima de 4 GB,
  BAR grande/Resizable BAR); BAR2 `0xFC_0000_0000` (doorbells), BAR4 portas
  `0xF000`, **BAR5 `0xF6F00000` (registradores, 32 bits)**;
- **copia do quadro inteiro: 5.203.830 ciclos** (o caminho por CPU);
- o log termina em "ativo": a sessao nao saiu do WIN com Esc, entao nao ha
  a linha `fim` com ciclos por quadro. Falta essa medida.

Desde esta data o `gpu.c` e o `AMDGOP.DLL` escrevem o `fb` com 64 bits
(`000000FB:00000000`).

### Por que a RX 570 e o melhor alvo de 3D

Comparada a Barcelo (notebook) e a integrada RDNA2 deste PC:

- **firmware sem PSP**: no VI (Polaris) o amdgpu carrega os microcodigos
  pelo SMU7 (`AMDGPU_FW_LOAD_SMU`), nao pelo PSP fechado das placas novas;
- **codigo de referencia estavel**: `gmc_v8_0.c`, `gfx_v8_0.c`,
  `sdma_v3_0.c`, `dce_v11_0.c`, `smu7_*` no Linux; registradores em
  `asic_reg/{dce/dce_11_2,gmc/gmc_8_1,gca/gfx_8_0,bif/bif_5_0}_d.h`;
- **Mesa**: radeonsi (GL) e RADV (Vulkan) suportam GFX8.

### Etapa A: diagnostico so leitura (`AMDGOP.DLL`, 2026-11-17)

Com `pci_id == 1002:67DF`, o `a_init` chama `polaris_diag()`, que **so le**
o BAR5 (registrador `r` em `BAR5 + r * 4`, sem segmentos) e registra:

- `CONFIG_MEMSIZE` (VRAM em MB). Se der 0 ou `FFFFFFFF`, para ai (BAR5
  errado ou placa desligada) e nao le mais nada;
- BAR0 de 64 bits e `fb - BAR0` (onde o GOP pos a tela dentro da VRAM);
- MC: `MC_VM_FB_LOCATION` (base/topo da VRAM vista pela GPU, em 16 MB),
  `FB_OFFSET`, AGP, abertura de sistema, `L1_TLB`, `VM_CONTEXT0_CNTL`,
  `MC_ARB_RAMCFG`, `MC_SEQ_MISC0` (bits 31:28 = tipo de memoria, 5 = GDDR5);
- GFX: `GRBM_STATUS`/`2`, `CP_ME_CNTL` (bits 24/26/28 = CE/PFP/ME parados),
  `CP_MEC_CNTL`, `CP_RB0_CNTL`, `RLC_CNTL`, `GB_ADDR_CONFIG`; SMC:
  `SMC_MESSAGE_0`/`SMC_RESP_0`;
- DCE 11.2, os 6 CRTCs (passo `0, 0x200, 0x400, 0x2600, 0x2800, 0x2A00`):
  `CRTC_CONTROL`, `GRPH_ENABLE`/`CONTROL`, superficie, pitch, fim X/Y,
  posicao do feixe, e os `CUR_*` (controle, superficie, tamanho, posicao,
  hot spot);
- o **CRTC da tela** (CRTC e GRPH ligados, superficie != 0, pitch = largura)
  e se a superficie cai dentro da VRAM do MC.

Para testar: iniciar o SODOS, `win`, usar um pouco, **sair com Esc** (para
a linha `fim`) e trazer o `D:\LOG\GPU.LOG` (no Windows: `E:\LOG\GPU.LOG`).

**Primeiro boot com a etapa A (2026-11-17):**

```
[amdgop] polaris vram MB 00001000 bar0 000000FB:00000000 fb-bar0 00000000
[amdgop] mc fb_location F4FFF400 fb_offset 00000000 agp_bot 00000000 agp_top 00000000 agp_base 00000000
[amdgop] mc sys_low 00000000 sys_high 00000000 l1_tlb 00000503 vm_ctx0 00FFFED8 ramcfg 00006062 seq_misc0 506091F2
[amdgop] gfx grbm 00003028 grbm2 00000008 cp_me 15000000 cp_mec 50000000 rb0 00418000 rlc 00000000 gb_addr 22011003
[amdgop] smc msg 00000000 resp 00000001
[amdgop] crtc 00000000 ctl 80410311 grph_en 00000001 grph_ctl 0A10800A surf 000000F4:00000000 pitch 00000780 fim 00000780x00000438 pos 077B0085
[amdgop]   cur_ctl 04000010 cur_surf 00000000:00000000 size 00000000 pos 00000000 hot 00000000
[amdgop] crtc 00000001 ctl 80400310 grph_en 00000001 grph_ctl 0A10800A surf 000000F4:00000000 pitch 00000780 ...
[amdgop] crtc 00000002..5 ctl 80400310 surf 00000000:00000000 pitch 00000000
[amdgop] crtc da tela 00000000 surf-mc_base 00000000 (dentro da VRAM do MC)
[amdgop] copia do quadro inteiro: ciclos 004F81F0
```

Os offsets conferem: o `GB_ADDR_CONFIG` lido (`22011003`) e o valor
"golden" da Polaris 10 no `gfx_v8_0.c`, e o CRTC 0 mostra o feixe andando
(`pos`). O que a placa diz:

- **VRAM: 4096 MB GDDR5** (`seq_misc0` bits 31:28 = 5). A CPU ve a VRAM no
  BAR0 em `0xFB_0000_0000`, e a tela comeca no inicio dela (`fb-bar0 0`);
- **a GPU ve a VRAM em `0xF4_0000_0000`..`0xF4_FFFF_FFFF`** (`fb_location`
  base `F400`, topo `F4FF`), a mesma base da Barcelo. AGP e abertura de
  sistema zeradas e `VM_CONTEXT0_CNTL` bit 0 = 0: **nao ha GART nem VM**
  ligados (etapa D comeca do zero);
- **motores parados**: `GRBM_STATUS` bit 31 (GUI_ACTIVE) = 0; `CP_ME_CNTL`
  `15000000` = CE, PFP e ME em halt; `CP_MEC_CNTL` `50000000` = MEC1 e MEC2
  em halt; `RLC_CNTL` 0 = RLC desligado. Nenhum microcodigo de GFX rodando:
  as etapas E e F carregam tudo;
- **SMC**: `SMC_RESP_0` = 1 (`PPSMC_Result_OK` da ultima mensagem). Indica
  um firmware do SMU respondendo, mas so a etapa C confirma;
- **tela no CRTC 0**: ligado (`CRTC_MASTER_EN`), superficie
  `0xF4_0000_0000` (inicio da VRAM), pitch 1920, 1920x1080;
  `GRPH_CONTROL` `0A10800A` = 32 bpp, ARGB8888, `ARRAY_LINEAR_ALIGNED`
  (linear: por isso a CPU escreve direto). O CRTC 1 tem a mesma superficie,
  mas esta desligado (sobra do GOP); os CRTCs 2 a 5 estao vazios;
- **cursor desligado** em todos os CRTCs (`CUR_CONTROL` bit 0 = 0,
  endereco 0): a etapa B parte de um estado limpo;
- copia do quadro inteiro: 5.210.608 ciclos (igual ao boot anterior).

Faltou de novo a linha `fim` (ciclos por quadro): o log termina em "ativo
de novo" (uma ida e volta com Ctrl+F1), entao a sessao nao saiu do WIN com
Esc.

### Etapa B: cursor pelo DCE 11.2 (`AMDCUR.DLL`, 2026-11-17)

O `AMDCUR.DLL` (o mesmo fonte com `-DAMD_CURSOR`) ganhou um caminho Polaris,
escolhido por `pci_id == 1002:67DF`. Ele segue `dce_v11_0_lock_cursor`,
`show_cursor`, `hide_cursor` e `cursor_move_locked` do amdgpu (Linux 6.12),
sempre no CRTC da tela achado na etapa A:

- toda escrita fica entre `CUR_UPDATE.CURSOR_UPDATE_LOCK` = 1 e 0 (a placa
  aplica tudo junto no proximo quadro);
- imagem 64x64 ARGB pre-multiplicado 16 MB adiante do inicio da VRAM, como
  na Barcelo: CPU `fb + 0x01000000` (`0xFB_0100_0000`), GPU
  `0xF4_0100_0000`, conferido dentro de `MC_VM_FB_LOCATION`; os 16 KB que
  estavam la sao guardados e devolvidos no fim;
- `CUR_SURFACE_ADDRESS(_HIGH)`, `CUR_SIZE` = 63/63 (64x64: a placa le com
  pitch 64), `CUR_CONTROL`: `CURSOR_EN` 1, `CURSOR_MODE` 2 (24.8
  pre-multiplicado), sem `2X_MAGNIFY`; os outros campos ficam como o GOP
  deixou;
- diferenca do DCN: no DCE o `CUR_POSITION` e o **canto de cima** da imagem
  (a placa nao subtrai o hot spot). O driver faz `ponta - hot`, e o que passa
  da borda esquerda/de cima vai para `CUR_HOT_SPOT` (recorte), como o Linux;
- `cursor_move` so escreve `CUR_POSITION`/`CUR_HOT_SPOT` (com o lock);
  suspender (Ctrl+F1) e sair desligam `CURSOR_EN`;
- sem CRTC da tela ou com imagem invalida, recusa e a CPU continua
  desenhando a seta.

No log, a ativacao escreve `[amdcur] polaris crtc ... cur_ctl ... size ...
update ...`. Esperado: `cur_ctl` com o bit 0 e `0x200` ligados, `size
003F003F`, e `update` sem o bit 16 (lock solto).

Instalado em `E:` (2026-11-17): `D:\SODOS\DRV\AMDCUR.DLL` (o anterior em
`AMDCUR.OLD`) e `D:\SODOS.CFG` com `driver=AMDCUR.DLL` (o anterior em
`D:\SODOS.BAK`). **Para desfazer**, basta voltar para `driver=AMDGOP.DLL`.

Teste: entrar no `win`, mover o mouse (a seta deve seguir sem rastro e sem
redesenhar a tela), Ctrl+F1 e Ctrl+F7 (a seta some e volta), sair com Esc e
trazer o `E:\LOG\GPU.LOG`.

**Primeiro boot com o cursor (2026-11-17): a seta da placa apareceu e segue
o mouse, mas com metade da altura e um bloco de ruido logo abaixo dela.**
Causa: o DCE 11 le a imagem do cursor com **pitch fixo de 128 pixels** (o
tamanho maximo), nao com a largura do `CUR_SIZE`. Com a imagem em 64x64, a
linha `r` da tela vinha da linha `2r` da imagem (seta achatada) e as linhas
32..63 liam os 16 KB seguintes da VRAM, fora da imagem (o ruido). O Linux
nunca ve isso porque sempre usa 128x128 (`max_cursor_width`).

Correcao: no caminho Polaris a imagem passou a 128x128 (64 KB, guardados e
devolvidos como antes) e `CUR_SIZE` = 127/127, igual ao amdgpu. A Barcelo
(DCN, com pitch programavel) continua em 64x64. O nome no TOP virou "amdcur
(Radeon, GOP, cursor da placa)".

O `GPU.LOG` desse boot nao tem a linha `[amdcur] polaris ...`: o log so e
regravado ao suspender, voltar e sair, e a sessao nao passou por nenhum
deles depois de o cursor ligar.

**Segundo boot, imagem 128x128 (2026-11-17):**

```
[gpu] ativo: amdcur (Radeon, GOP, cursor da placa)
[amdcur] polaris crtc 00000000 imagem gpu 000000F4:01000000 cur_ctl 04000211 size 007F007F update 00000000
[gpu] cursor pela placa: amdcur (Radeon, GOP, cursor da placa)
[gpu] desativado: fim do WIN
[amdgop] fim: quadros 000003B9 ciclos/quadro 00378695 pixels/quadro 0016899E (quadro inteiro: ciclos 004F6A70 pixels 001FA400)
```

- **na tela: a seta tem o tamanho certo e o ruido sumiu** (conferido pelo
  usuario); a correcao do pitch 128 esta confirmada;
- `cur_ctl 04000211`: `CURSOR_EN`, `CURSOR_MODE` 2, o resto como o GOP
  deixou; `size 007F007F` (128x128); `update 00000000` (lock solto, nada
  pendente): o cursor foi aceito pelo kernel ("cursor pela placa");
- saida limpa com Esc ("desativado: fim do WIN"), com a imagem devolvida;
- 953 quadros, **3.638.933 ciclos por quadro** contra 5.204.592 da copia
  inteira (1,4 vez menos) e 1.476.766 pixels por quadro (71% da tela). Na
  Barcelo foram 10,7% da tela: aqui a sessao redesenhou areas grandes (a
  pagina do C8X no Firefox), entao o ganho das areas e pequeno. Mover o
  mouse ja nao entra nessa conta: so escreve `CUR_POSITION`;
- falta exercitar Ctrl+F1/Ctrl+F7 com o cursor da placa (nao houve nesta
  sessao).

### Etapa C: estado do SMC (`AMDGOP.DLL`, 2026-11-17)

O SMC e o processador do SMU7 (energia, clocks, tensoes). No VI o amdgpu
carrega os microcodigos de SDMA e GFX **pelo SMU** (`AMDGPU_FW_LOAD_SMU`),
entao o que fazer nas etapas E e F depende de o firmware do SMU ja estar
rodando ou nao. O `polaris_smc_diag()` (dentro da etapa A) le pela porta
indireta, como `vi_smc_rreg`: endereco em `SMC_IND_INDEX_11` (`0x1AC`),
valor em `SMC_IND_DATA_11` (`0x1AD`), offsets de `smu_7_1_3_d.h`. A unica
escrita e o indice, devolvido ao valor de antes no fim.

| Endereco SMC | Registrador | O que diz |
|---|---|---|
| `0x80000000` | `SMC_SYSCON_RESET_CNTL` | bit 0 `rst_reg`: SMC em reset |
| `0x80000004` | `SMC_SYSCON_CLOCK_CNTL_0` | bit 0 `ck_disable`: clock do SMC parado |
| `0x80000370` | `SMC_PC_C` | contador de programa do SMC |
| `0xE0003088` | `SMU_STATUS` | bit 0 `SMU_DONE` |
| `0xE00030A4` | `SMU_FIRMWARE` | bit 16 `SMU_MODE` (modo protegido), bit 17 `SMU_SEL` |
| `0x3F000` | `FIRMWARE_FLAGS` | bit 0 `INTERRUPTS_ENABLED` (o firmware terminou de iniciar) |
| `0x3F010` | `FEATURE_STATUS` | bit 13 `VOLTAGE_CONTROLLER_ON` (`polaris10_is_dpm_running`) |
| `0x20000` | `SMU7_Firmware_Header` | versao, tamanho, flags, entrada e tamanho do codigo |

O log termina com o veredito de `smu7_is_smc_ram_running`: **RODANDO** se
`ck_disable` = 0 e `SMC_PC_C >= 0x20100`, senao **PARADO**, e se o SMU esta
em modo protegido. Se estiver rodando, o `polaris10_start_smu` do Linux nao
recarrega o firmware: as etapas seguintes so mandam mensagens ao SMU. Se
estiver parado, a proxima etapa e carregar o `polaris10_smc.bin` (modo
protegido ou nao, conforme `SMU_MODE`).

**Primeiro boot com a etapa C (2026-11-17):**

```
[amdgop] smc msg 00000000 resp 00000001
[amdgop] smc rst 00000000 ck 01000000 pc 00002B4C smu_status 00000000 smu_fw 00030006
[amdgop] smc fw_flags 8791F2F6 feature_status 6DFE4798 hdr versao 55B3F277 tam CFDEA551 flags 7FEBFFB5 entrada F118A16C codigo 08483B6E
[amdgop] smc PARADO (sem firmware rodando), modo protegido
```

- **O SMC esta ligado, mas so na ROM de boot**: fora de reset (`rst` 0) e
  com clock (`ck_disable` = 0), mas o contador de programa esta em
  `0x2B4C`, abaixo de `0x20100` (onde fica o firmware na RAM do SMC). E o
  estado que o amdgpu chama de "SMC RAM not running": o GOP/VBIOS nao
  carregou o firmware do SMU;
- a RAM do SMC so tem lixo: `FIRMWARE_FLAGS`, `FEATURE_STATUS` e o
  cabecalho em `0x20000` sao valores aleatorios (memoria nunca escrita).
  Sem firmware, o `SMC_RESP_0` = 1 da etapa A nao quer dizer nada;
- **`SMU_FIRMWARE` = `00030006`**: `SMU_MODE` (bit 16) = 1, **modo
  protegido**, e `SMU_SEL` (bit 17) = 1 (chave de producao). O Linux iria
  por `polaris10_start_smu_in_protection_mode`: o firmware tem de ser a
  imagem **assinada** da AMD, e o SMC confere a assinatura;
- qual imagem: a RX 570 e `1002:67DF` revisao `EF`, que o amdgpu marca como
  "kicker" (`ASICID_IS_P20` em `amdgpu.h`), entao o arquivo e
  **`amdgpu/polaris10_k_smc.bin`** do linux-firmware (`amdgpu_cgs.c`);
- consequencia para as etapas seguintes: no VI os microcodigos de SDMA e GFX
  sao carregados **pelo SMU** (`AMDGPU_FW_LOAD_SMU`), entao antes de ligar
  qualquer motor o SODOS tem de carregar e iniciar esse firmware. Isso vira
  a etapa C2 (abaixo).

Nessa sessao tambem: 469 quadros, 2.563.120 ciclos por quadro contra
5.204.493 da copia inteira (2,0 vezes menos, 50% da tela por quadro), e o
cursor da placa ligou de novo sem ruido.

### Etapa C2: firmware do SMU (`AMDSMC.DLL`, 2026-11-17)

O `AMDSMC.DLL` e o mesmo `amdgop.c` com `-DAMD_SMC` (que inclui o
`AMD_CURSOR`): tela, areas, cursor do DCE e, no `init`, a carga do firmware
do SMU. Segue `polaris10_start_smu_in_protection_mode`,
`smu7_upload_smu_firmware_image` e `smu7_send_msg_to_smc_offset` (Linux
6.12, `pm/powerplay/smumgr`):

1. se o SMC ja roda firmware (`ck_disable` 0 e `SMC_PC_C >= 0x20100`, por
   exemplo numa segunda entrada no WIN), so registra e nao mexe em nada;
2. exige `SMU_FIRMWARE` com modo protegido (bit 16) e chave de producao
   (bit 17); outro caso usaria outra imagem e outra sequencia, entao nao
   carrega;
3. le `\SODOS\FW\P10KSMC.BIN` (o `amdgpu/polaris10_k_smc.bin` do
   linux-firmware, em `firmware/amdgpu/` com a licenca) e confere o
   `smc_firmware_header_v1_0`: tamanho total, versao 1, ucode multiplo de
   4 dentro do arquivo, ate 128 KB, inicio em `0x20000`;
4. reset do SMC (`SYSCON_RESET_CNTL.rst_reg` 1), ucode na RAM do SMC a partir
   de `0x20000` por `SMC_IND_INDEX_11`/`DATA_11` com `AUTO_INCREMENT_IND_11`
   (`SMC_IND_ACCESS_CNTL` bit 11), auto-incremento desligado no fim;
5. `SMU_STATUS` 0, clock ligado, reset solto; espera `RCU_UC_EVENTS`
   `INTERRUPTS_ENABLED` (a ROM de boot pronta);
6. mensagem `PPSMC_MSG_Test` (`0x100`) com argumento `0x20000`
   (`SMC_MSG_ARG_0` `0xA4`, `SMC_RESP_0`, `SMC_MESSAGE_0`): a ROM confere a
   assinatura e responde em `SMU_STATUS` (`SMU_DONE`, `SMU_PASS`);
7. com `SMU_PASS`, zera `FIRMWARE_FLAGS`, pulsa o reset e espera
   `FIRMWARE_FLAGS.INTERRUPTS_ENABLED` (o firmware iniciou).

Toda espera tem limite (1 a 2 s pelo `k->ms` e um teto de voltas), o indice
do SMC volta ao valor de antes, e qualquer falha so vai para o log: a tela e
o cursor nao dependem do SMU. Codigos no log: `-1` nao tentou (arquivo,
modo, kernel antigo), `-2` a ROM nao ligou as interrupcoes, `-3` sem
`SMU_DONE`, `-4` assinatura recusada (`SMU_PASS` 0), `-5` aceito mas nao
iniciou.

**Leitura de arquivo pelos drivers.** A `gpu_kapi` ganhou, no fim, `ext`
(`GPU_KAPI_EXT`, "FWRD"), `fw_size` e `fw_read`, que leem
`X:\SODOS\FW\<nome>` (X = unidade do `SODOS.CFG`, ou D:; nome sem pastas).
O `GPU_ABI` continua 1: os DLLs antigos nao olham os campos novos, e um DLL
novo num kernel antigo ve `ext` diferente e passa sem eles. O
`make disk-files`/`make sendpc` copia o firmware para `\SODOS\FW\P10KSMC.BIN`
(e a licenca em `LICAMDGP.TXT`).

**Testes sem a placa (2026-11-17):**

- `make test-amdsmc`: o `amdgop.c` com `AMD_SMC` contra um SMC simulado no
  host (`drivers/amdgop/smcsim.c`): porta indireta com auto-incremento, RAM
  do SMC, reset/clock, a ROM que liga as interrupcoes, confere a imagem na
  mensagem Test e inicia o firmware no reset seguinte. 9 cenarios, 0
  falhas: carga normal (RAM = ucode, Test com `0x20000`, `FIRMWARE_FLAGS`,
  SMC rodando, cursor ainda funciona), firmware ja rodando (nenhuma
  escrita), imagem adulterada (`-4`, nao fica rodando), sem arquivo,
  modo nao protegido, ROM travada (`-2` dentro do limite), cabecalho
  invalido, `gpu_kapi` antiga, indice devolvido;
- QEMU, `-vga std` com o `SODOS.CFG` apontando a Bochs (`1234:1111`) para o
  `AMDSMC.DLL`: o kernel leu os 130.244 bytes pelo `fw_read`, o cabecalho
  conferiu, "placa nao e a RX 570: nada carregado", driver ativo, cursor da
  placa recusado (sem DCE), desktop normal;
- QEMU, `-device virtio-vga`: o `VIRTGPU.DLL` (sem mudanca) continua ativo
  com a `gpu_kapi` nova, cursor pela placa.

O que a simulacao nao cobre: o comportamento real da ROM do SMC (a
assinatura, os tempos) e o efeito do firmware rodando sobre clocks e
ventoinha. Isso so a placa responde: o `D:\SODOS.CFG` do desktop passa a
`driver=AMDSMC.DLL` (o anterior em `D:\SODOS.BK2`). Para voltar, basta
`driver=AMDCUR.DLL`. O firmware fica carregado ate desligar o PC (um
reinicio a quente para o Windows encontra o SMC ja rodando, como depois do
amdgpu do Linux).

**Primeiro boot com o `AMDSMC.DLL` (2026-11-18): o firmware do SMU
carregou e iniciou na RX 570.** Do `\LOG\SERIAL.LOG` (a primeira entrada no
WIN; o `GPU.LOG` so guarda a ultima):

```
[amdgop] smc rst 00000000 ck 01000000 pc 000028DC smu_status 00000000 smu_fw 00030006
[amdgop] smc fw_flags 00000001 feature_status 0001E083 hdr versao 02171113 tam 000000B4 flags 00000000 entrada 00020100 codigo 0001D2B8
[amdgop] smc PARADO (sem firmware rodando), modo protegido
[amdsmc] P10KSMC.BIN bytes 0001FCC4 cabecalho 00000024 v00000001 ucode_versao 00171100 ucode 0001FBC4 em 00000100 inicio 00020000
[amdsmc] firmware do SMU RODANDO: versao 00171100 pc 00024220 flags 00000001 resp_test 00000001
[gpu] ativo: amdsmc (Radeon, GOP, cursor, SMU)
[gpu] cursor pela placa: amdsmc (Radeon, GOP, cursor, SMU)
```

E a segunda entrada no WIN, no mesmo boot (`GPU.LOG`):

```
[amdgop] smc msg 00000100 resp 00000001
[amdgop] smc rst 40000000 ck 01000000 pc 0002421C smu_status 00000003 smu_fw 0003001E
[amdgop] smc fw_flags 00000001 feature_status 00000000 hdr versao 02171100 tam 000000B4 flags 00000000 entrada 00020100 codigo 0001D250
[amdgop] smc RODANDO (firmware do SMU na RAM do SMC), modo protegido
[amdsmc] firmware do SMU ja roda: pc 0002055C flags 00000001 (nada a carregar)
```

- antes da carga, o SMC estava na ROM (`pc 28DC`), mas a RAM dele ainda
  tinha o firmware que o **driver do Windows** carregou na sessao anterior
  (cabecalho com versao `02171113`, entrada `0x20100`): o reinicio a quente
  reinicia o SMC, mas nao apaga a RAM. Por isso o diagnostico decide por
  `SMC_PC_C`, nao pelo conteudo da RAM;
- a ROM aceitou a assinatura (`resp_test 1`, `SMU_STATUS` = 3:
  `SMU_DONE` e `SMU_PASS`) e o firmware iniciou (`FIRMWARE_FLAGS` bit 0,
  `pc 0x24220`, dentro do firmware). O cabecalho na RAM passou a ser o do
  linux-firmware (`02171100`, codigo `0x1D250`);
- na segunda entrada o driver so conferiu e nao mexeu em nada;
  `FEATURE_STATUS` = 0: nenhum recurso de energia (DPM) ligado, como
  esperado: o Linux so liga o DPM depois de mandar as tabelas ao SMU;
- a tela e o cursor da placa seguiram normais; nessa sessao 96 quadros,
  1.169.736 ciclos por quadro (4,4 vezes menos que a copia inteira).

### Etapa E: motor de copia SDMA0 (`AMDSDMA.DLL`, 2026-11-18)

**Ordem trocada com a D.** Para ligar a SDMA nao e preciso GART: o anel, o
rptr, o fence, a tabela do SMU e o microcodigo ficam todos na **VRAM**, que a
CPU escreve pela abertura do BAR0 e a placa le pelo endereco do MC
(`0xF4_xxxx_xxxx`, VMID 0, VM desligada). A GART (etapa D) so e preciso
quando a SDMA tiver de ler a RAM do PC (o back buffer do kernel), para o
`present` pela placa.

O `AMDSDMA.DLL` e o `amdgop.c` com `-DAMD_SDMA` (que inclui `AMD_SMC` e
`AMD_CURSOR`). Depois de o firmware do SMU estar rodando (etapa C2):

1. **microcodigo pelo SMU**, como `smu7_request_smu_load_fw` (o Linux 6.12
   nao tem mais carga direta para a Polaris): le `\SODOS\FW\P10SDMA.BIN`
   (`amdgpu/polaris10_sdma.bin`), poe o ucode na VRAM (32 MB + 4 KB), monta
   uma `SMU_DRAMData_TOC` com uma `SMU_Entry` (id 1 = SDMA0, versao, endereco
   MC, tamanho), manda `SMU_DRAM_ADDR_HI/LO` (`0x252/0x253`, 800 KB de VRAM
   para o SMU), `DRV_DRAM_ADDR_HI/LO` (`0x250/0x251`, a TOC) e
   `LoadUcodes` (`0x254`) com a mascara `0x2`, e espera o bit em
   `SMU74_SoftRegisters.UcodeLoadStatus` (ponteiro em `0x20030` do cabecalho
   do firmware do SMU; no `polaris10_k_smc.bin` os SoftRegisters ficam em
   `0x3FA24`, o campo em `+108`);
2. **anel** (`sdma_v3_0_gfx_resume`, so a SDMA0): golden settings da
   Polaris 10, F32 em HALT, anel de 4 KB (`RB_SIZE` 10), rptr com writeback,
   sem doorbell e sem polling de wptr (o wptr vai pelo registrador,
   `GFX_RB_WPTR` em bytes), `RB_ENABLE`, `IB_ENABLE`, F32 solto,
   `AUTO_CTXSW`;
3. **testes**, cada um terminado por um `FENCE` na VRAM que a CPU espera
   (com `sfence` + `HDP_MEM_COHERENCY_FLUSH_CNTL` antes de a placa ler e
   `HDP_DEBUG0` antes de a CPU ler): escrita de uma palavra
   (`sdma_v3_0_ring_test_ring`), `CONST_FILL` de 4 MB (2 pacotes de 2 MB),
   `COPY` linear de 4 MB (4 pacotes de 1 MB, limite do Linux `0x1FFFFF`),
   conferidos pela CPU uma pagina por vez; e, para comparar, a CPU
   preenchendo os mesmos 4 MB.

Mapa da VRAM usado (todo dentro dos 64 MB que o kernel mapeia; a tela GOP
ocupa os primeiros 8 MB e o cursor fica em 16 MB):

| Deslocamento | O que |
|---|---|
| `0x02000000` | TOC do SMU |
| `0x02001000` | ucode da SDMA0 |
| `0x02100000` | 800 KB do SMU (`SMU_DRAM_ADDR`) |
| `0x02200000` | anel (4 KB) |
| `0x02201000` | rptr; `+0x80` teste de escrita; `+0xC0` fence |
| `0x02400000`, `0x02800000` | areas A e B dos testes (4 MB cada) |

Falhas so vao para o log e deixam a SDMA em HALT; a tela e o cursor nao
dependem dela. Codigos (`sd_res`): `-1` nao tentou (tela fora da VRAM do MC,
SoftRegisters invalido, sem arquivo), `-3` o SMU nao carregou
(`UcodeLoadStatus`), `-11` sem fence no teste de escrita, `-12` palavra
errada, `-13`/`-14` preenchimento, `-15`/`-16` copia. No fim do WIN
(`fini`) a SDMA volta ao HALT.

**Testes sem a placa (2026-11-18):**

- `make test-amdsmc` agora roda tambem o `smcsim.c` com `-DAMD_SDMA`: o
  SMU simulado responde `0xFE` sem firmware, guarda os enderecos das
  mensagens, le a TOC da VRAM simulada e confere o ucode contra o arquivo; a
  SDMA simulada executa o anel (WRITE, FENCE, CONST_FILL, COPY, NOP) a cada
  escrita do wptr e escreve o rptr. Cenarios novos, 0 falhas: tudo ligado
  (TOC e SMU_DRAM nos enderecos certos, `UcodeLoadStatus`, `RB_BASE` e
  tamanho, F32 solto, rptr = wptr, os tres testes certos, `fini` em HALT),
  SMU que nao carrega (`-3`, anel desligado), SDMA que nao anda (`-11`
  dentro do limite, deixada em HALT) e sem `P10SDMA.BIN` (`-1`). A
  simulacao com o firmware real achou um erro: o limite que eu tinha posto
  para os SoftRegisters (`<= 0x3F000`) recusaria o `0x3FA24` de verdade;
- QEMU, Bochs apontada para o `AMDSDMA.DLL`: o kernel leu os dois firmwares,
  "placa nao e a RX 570: SDMA nao liga", driver ativo, desktop normal.

O que a simulacao nao cobre: se o SMU aceita uma TOC so com a SDMA0 (o
Linux pede os sete microcodigos de uma vez), os tempos reais e o formato do
`CONST_FILL` (o Linux manda so o OP no cabecalho; se o padrao de 32 bits
nao vier certo, o log mostra a primeira palavra lida). O `D:\SODOS.CFG` do
desktop passa a `driver=AMDSDMA.DLL` (o anterior em `D:\SODOS.BK3`); para
voltar, `driver=AMDSMC.DLL`.

**Primeiro boot com o `AMDSDMA.DLL` (2026-11-18): a SDMA0 anda na RX
570.** Do `\LOG\SERIAL.LOG`:

```
[amdsmc] firmware do SMU RODANDO: versao 00171100 pc 00020550 flags 00000001 resp_test 00000001
[amdsdma] P10SDMA.BIN bytes 00003194 v00000001 ucode_versao 0000003A ucode 00003094 em 00000100
[amdsdma] soft_regs 0003FA24 status antes 00000000 toc 000000F4:02000000
[amdsdma] ucode da SDMA0 carregado pelo SMU: versao 0000003A bytes 00003094 status 00000002
[amdsdma] anel 000000F4:02200000 rb_cntl 00001015 f32 00000000 status 46DEE557
[amdsdma] escrita pela SDMA ok
[amdsdma] preenchimento de 4 MB: ciclos 002A02BD paginas erradas 00000400 (primeira palavra 3C3C3C3C)
[amdsdma] testes falharam: 00000004 (SDMA parada)
```

- o SMU aceitou a TOC so com a SDMA0 e carregou o microcodigo
  (`UcodeLoadStatus` = 2): a duvida principal da etapa ficou respondida;
- o anel ligou (`RB_SIZE` 10, rptr com writeback, F32 solto) e a SDMA
  **executou** a escrita e os fences: o motor de copia funciona;
- o preenchimento rodou nos 4 MB (todas as 1.024 paginas escritas, em
  2.753.213 ciclos), mas com `3C3C3C3C`: o `CONST_FILL` so com o OP no
  cabecalho (como o Linux manda) tem `fillsize` 0, e a RX 570 repete o
  **byte** baixo do dado. O Linux nao percebe porque so usa o fill para
  zerar. Corrigido: o cabecalho leva `fillsize` 2 (palavra de 32 bits), e o
  `smcsim.c` passou a imitar a placa (`fillsize` 0 = byte), entao este erro
  agora quebra a simulacao;
- a copia nao rodou nesse boot (os testes param no primeiro erro).

So o `AMDSDMA.DLL` mudou (copiado direto para `E:\SODOS\DRV`, sem kernel
novo). Falta o boot que confirme preenchimento e copia.

**Segundo boot, `fillsize` 2 (2026-11-18): SDMA0 funcionando na RX 570.**

```
[amdsdma] escrita pela SDMA ok
[amdsdma] preenchimento de 4 MB: ciclos 0029E4D0 paginas erradas 00000000 (primeira palavra 5AA5C33C)
[amdsdma] copia de 4 MB: ciclos 002A2137 paginas erradas 00000000
[amdsdma] (CPU preenchendo os mesmos 4 MB: ciclos 00270380)
[amdsdma] SDMA0 FUNCIONANDO
...
[amdsdma] SDMA0 parada (fim)
```

- escrita, preenchimento de 4 MB (padrao de 32 bits certo) e copia de 4 MB
  VRAM -> VRAM conferidos pagina por pagina, sem erro; a SDMA parou no Esc;
- tempos medidos de ponta a ponta (do pacote ao fence visto pela CPU):
  preenchimento 2.745.552 ciclos, copia 2.761.015, a CPU preenchendo pela
  abertura 2.556.800. A placa ainda nao e mais rapida que a CPU nisso: os
  dois tempos da SDMA sao quase iguais, sinal de que o custo nao e a
  memoria (a copia le e escreve o dobro). Suspeitos: o clock da SDMA/memoria
  (o firmware do SMU roda sem DPM, provavelmente no nivel mais baixo) e a
  espera pela CPU (cada volta invalida o HDP e le a VRAM pelo PCIe). A
  vantagem que ja existe: enquanto a SDMA trabalha a CPU fica livre;
- quadros dessa sessao: 2.334, 4.111.419 ciclos por quadro (80% da tela,
  com o Firefox aberto).

### Etapa D, primeira parte: a SDMA le a RAM e copia a tela (`AMDSDMA.DLL`, 2026-11-18)

**Sem GART, de proposito.** O MC esta como o GOP deixou (etapa A):
`VM_CONTEXT0` desligado e `MC_VM_MX_L1_TLB_CNTL.SYSTEM_ACCESS_MODE` = 0
("PA only"). Nesse modo um endereco fora de `MC_VM_FB_LOCATION` deveria ir
direto para a RAM pelo endereco fisico. O back buffer do kernel fica em
`0x100000` (`GFX_BACK_PHYS`, fisico = virtual, abaixo de 4 GB) e o `k->alloc`
tambem devolve fisico = virtual. Se a placa ler a RAM assim, o `present` pela
placa nao precisa de tabela de paginas; a GART (a segunda parte da D) so
volta a ser preciso se esse modo nao funcionar ou para memoria acima do que o
modo alcanca.

Depois de a SDMA0 passar nos testes da etapa E, o `sd_teste_ram()`:

1. enche 64 KB de RAM (`k->alloc`) com um padrao e copia para a VRAM (COPY
   linear, endereco = fisico da RAM); a CPU confere;
2. zera a area B pela SDMA e copia um retangulo 64x32 de uma imagem
   256x64 na RAM para (100,10) de uma imagem de 1920 de largura na VRAM com
   **COPY sub-janela** (`sub_op` 4, `elementsize` 2 = 4 bytes, pitch e
   tamanhos - 1, como o `cik_sdma` do Mesa para GFX8); a CPU confere o
   retangulo e que em volta dele continua zero.

**So leitura da RAM:** nenhum pacote manda a placa escrever na RAM.

Se passar, o `present` vira a placa (`sd_present`): uma sub-janela por area
alterada (back buffer na RAM -> framebuffer na VRAM, pitch de cada lado), um
fence no fim e espera de ate 200 ms; acima de 40 areas, um quadro inteiro.
Qualquer falha (fence que nao vem) para a SDMA, desliga o caminho e a CPU
copia o quadro, como antes. Se o teste da RAM falhar, a SDMA continua ligada
mas o `present` segue pela CPU. No fim, o log diz quantos quadros a SDMA
copiou (`[amdsdma] quadros pela SDMA ... falhas ...`).

**Testes sem a placa (2026-11-18):** o `smcsim.c` (com `AMD_SDMA`) trata o
endereco fora da VRAM como ponteiro do host (modo PA) e executa a sub-janela.
Cenarios novos, 0 falhas: teste da RAM passa e liga o present; duas areas
(uma passando da borda, recortada) copiadas da RAM para a tela e nada fora
delas; quadro inteiro; a SDMA para no meio da sessao e a CPU copia o quadro
(caminho desligado, SDMA em HALT); placa que le lixo da RAM (o present fica
na CPU, a SDMA continua ligada). QEMU (Bochs com `AMDSDMA.DLL`): desktop
normal.

O que so a placa responde: se o modo PA realmente leva a SDMA a RAM (com o
endereco fisico e coerente com a cache da CPU) e se a sub-janela tem o
formato certo na Polaris. O teste para antes de usar; nada escreve na RAM.

### Etapas da RX 570

| # | Etapa | Estado |
|---|---|---|
| A | Diagnostico so leitura: VRAM, MC, motores, DCE (`polaris_diag`) | feito, rodou na placa (2026-11-17) |
| B | Cursor pelo DCE 11.2 (`CUR_CONTROL`, `CUR_SURFACE_ADDRESS`, `CUR_SIZE`, `CUR_POSITION`, `CUR_UPDATE` com lock), imagem 64x64 na VRAM: `AMDCUR.DLL` passa a ter caminho Polaris | feito, rodou na placa (128x128); falta Ctrl+F1/F7 |
| C | Estado do SMC: leitura indireta (`SMC_IND_INDEX_11`/`DATA_11`) de `SMC_SYSCON_CLOCK_CNTL_0` e `SMC_PC_C`, para saber se o firmware do SMU esta rodando | feito: SMC so na ROM, firmware a carregar, modo protegido |
| C2 | Carregar o `polaris10_k_smc.bin` (assinado) na RAM do SMC em modo protegido, como `polaris10_start_smu_in_protection_mode` + `smu7_upload_smu_firmware_image`, e esperar `SMU_DONE`/`SMU_PASS` | feito: firmware do SMU rodando na placa (2026-11-18) |
| D | A SDMA lendo a RAM do PC para o `present` pela placa: primeiro pelo modo PA (sem GART), testado antes de usar (`AMDSDMA.DLL`); GART (GMC 8, VM contexto 0) so se o modo PA nao servir | modo PA feito, testado em simulacao e QEMU; falta rodar na placa |
| E | SDMA 3.0: microcodigo `polaris10_sdma.bin` pelo SMU, anel e fence na VRAM, escrita, preenchimento e copia pela placa (`AMDSDMA.DLL`) | feito: SDMA0 funcionando na placa (escrita, preenchimento e copia de 4 MB, 2026-11-18) |
| F | GFX 8: microcodigos PFP/ME/CE/MEC/RLC, anel GFX, IH (interrupcoes), fences | em aberto |
| G | Interface estilo DRM no kernel (BOs, mapear no processo, submeter, esperar) e Mesa radeonsi/RADV com um winsys do SODOS | em aberto |

As etapas D a G escrevem na placa e podem travar a maquina: cada uma entra
como DLL separado ou com `-D`, como o `AMDCUR.DLL`, e o `AMDGOP.DLL` continua
sendo o caminho seguro.

## Configuracao (`D:\SODOS\VIDEO.CFG`)

```
gpu=0              nunca carrega driver (diagnostico, driver que se comporta mal)
gpudrv=GPUTEST.DLL forca este driver de D:\SODOS\DRV, com ou sem placa na tabela
```

`gpudrv=` existe para testar o carregador; uso normal nao precisa dele.

## Drivers

| Placa (vendor:device) | DLL | Estado |
|---|---|---|
| `1AF4:1050` virtio-gpu | `VIRTGPU.DLL` | funciona no QEMU (`-device virtio-vga`): 2D, tela inteira por quadro |
| `1002:15E7` AMD Barcelo (pelo `SODOS.CFG`) | `AMDGOP.DLL` | funciona no notebook (1920x1200, GOP): 8,1x menos ciclos por quadro |
| `1002:67DF` Radeon RX 570 (pelo `SODOS.CFG`) | `AMDGOP.DLL` | funciona no desktop (1920x1080, GOP acima de 4 GB); diagnostico Polaris (etapa A) rodou: tela no CRTC 0, motores parados |
| `1002:67DF` Radeon RX 570 (pelo `SODOS.CFG`) | `AMDSMC.DLL` | funciona no desktop: tela, cursor da placa e firmware do SMU carregado (etapa C2) |
| `1002:67DF` Radeon RX 570 (pelo `SODOS.CFG`) | `AMDSDMA.DLL` | funciona no desktop: tela, cursor da placa, firmware do SMU e SDMA0 (escrita, preenchimento e copia na VRAM) |
| nenhuma (so `gpudrv=`) | `GPUTEST.DLL` | teste: copia o back buffer para o LFB pela CPU, reserva 64 KB para aparecer no TOP |
| nenhuma (so `gpudrv=`) | `GPUFAIL.DLL` | o mesmo, mas `present` falha no 20o quadro |

Fontes em `drivers/<nome>/`; `make -C c_compiler drivers` gera
`c_compiler/out/DRV/*.DLL` e `make disk-files` copia para `D:\SODOS\DRV`.

## Etapas

| # | Etapa | Estado |
|---|---|---|
| 0 | Este documento | feito |
| 1 | `video/gpu.h` + `video/gpu.c`: tabela PCI, carga, `selftest`, suspender/voltar, desativacao segura, `gpu=0`/`gpudrv=`; `link64 /dll /export:`; `GPUTEST`/`GPUFAIL` | feito |
| 2 | `VIRTGPU.DLL`: virtio PCI moderno, fila de controle, `present` da tela inteira | feito |
| 3 | `present` so das areas alteradas (`GPU_CAP_DAMAGE`), areas vindas do kernel | feito |
| 4 | Cursor por hardware (`GPU_CAP_CURSOR`) | feito |
| 5 | Operacoes na placa (`FILL`/`BLIT`), so se medido e registrado na tabela de custo | em aberto |

Fora do escopo por enquanto: virtio-gpu 3D (virgl), VMware SVGA, amdgpu
(o notebook segue no framebuffer GOP pelo caminho atual). Excecao: a RX 570
do desktop, com etapas proprias na secao "Radeon RX 570".

O redesenho so das areas alteradas **no caminho por CPU** (sem driver) e
outra mudanca, separada desta: ela mexeria no caminho atual e por isso so
entra se for pedida.

## Testar

Testes feitos no QEMU (KVM, 1920x1080), entrando no WIN, indo a uma tela do
DOS com Ctrl+F1, abrindo o TOP na aba Video e voltando com Ctrl+F7:

| Cenario | Log serial (`[gpu]`) | Resultado |
|---|---|---|
| `-vga std`, sem chave | nenhuma linha | igual a antes |
| `gpudrv=GPUTEST.DLL` | ativo, suspenso, ativo de novo | desktop normal; TOP: driver parado, 64 KB |
| `gpudrv=GPUFAIL.DLL` | ativo, `desativado: present falhou` | desktop segue pela CPU; nao tenta de novo no Ctrl+F7 |
| `-device virtio-vga`, sem `VIRTGPU.DLL` | `1AF41050 -> ...VIRTGPU.DLL`, `nao carregou` | desktop pelo Bochs VBE, como antes |
| `-device virtio-vga` com `VIRTGPU.DLL` | ativo, suspenso, ativo de novo; `fim do WIN` no Esc | desktop pela placa; TOP: "virtio-gpu (parado com o WIN ao fundo)", 6 KB; texto volta no Ctrl+F1 e no Esc; reabre no Ctrl+F7 |
| virtio x `-vga std`, mouse PS/2: mover o cursor, digitar no painel de IA, arrastar o Gerenciador | (etapa 3) | as duas telas iguais (so muda onde o cursor parou e o relogio); sem rastro do cursor; a janela arrastada fica no lugar |
| virtio com cursor pela placa, `-trace virtio_gpu_update_cursor` | `cursor pela placa`; de novo depois do Ctrl+F7 | o trace segue o caminho do mouse (0,0 -> ~400,240 -> ~720,320 -> barra de titulo -> arrasto ate ~630,194); o quadro nao tem a seta; o arrasto funciona; com `-vga std` a seta continua no quadro |
| `gpudrv=` + `gpu=0` | nenhuma linha | igual a antes |

Para repetir, ponha a chave no `VIDEO.CFG` do `disk.img` (com `mcopy`) e:

```
make run                        # sem placa na tabela: tudo pela CPU
make run-virtio                 # virtio-gpu: VIRTGPU.DLL entra sozinho
```

## Arquivos

- `video/gpu.h` — contrato com o driver e o que `visual.c` chama.
- `video/gpu.c` — tabela PCI, carregador, `kapi`, suspender/voltar, desligar.
- `video/visual.c` — `gfx_gpu_try()`, o teste em `gfx_frame_end`, `gfx_for_gpu`
  em `gfx_buf_init`, chaves `gpu` no `win_load_cfg`, `k_vram`; areas
  alteradas (`gfx_dmg_add`, `gfx_gpu_frame`, `cursor_draw`, `dc_present`).
- `drivers/gputest/gputest.c` — `GPUTEST.DLL` e `GPUFAIL.DLL`.
- `drivers/virtgpu/virtgpu.c` — `VIRTGPU.DLL`.
- `drivers/amdgop/amdgop.c` — `AMDGOP.DLL` (notebook).
- `c_compiler/link/link64.c` — `/dll` e `/export:`.
- `docs/VIDEO.md` — os modelos de renderizacao por CPU (retro/avancado),
  que continuam valendo com ou sem driver.
