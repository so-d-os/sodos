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
| Ctrl+F1..F5 (WIN ao fundo) | `suspend()`: o driver devolve a tela ao modo texto e guarda o que reservou. Sem `suspend`, e descarregado |
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
`X:\SODOS\GPU.LOG` (na unidade do `SODOS.CFG`, ou D:) com a placa achada
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
4. sair do WIN com Esc e trazer o `U:\SODOS\GPU.LOG`.

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

Estado: aguardando o primeiro boot no notebook.

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
(o notebook segue no framebuffer GOP pelo caminho atual).

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
