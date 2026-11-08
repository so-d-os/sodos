# Driver de placa de video (aceleracao grafica)

Estado: **em projeto** (2026-11). Nada deste documento esta implementado
ainda; ele e o contrato que o codigo vai seguir. Quando uma etapa for feita,
a secao "Etapas" passa a dizer o que ja existe.

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

Esta e a escolha feita: um ponteiro testado uma vez por quadro. E o unico
custo fixo, e fica listado aqui para nao crescer sem ninguem ver.

| Item | Custo sem driver | Onde |
|---|---|---|
| `gpu_drv` (ponteiro para a tabela do driver) | 8 bytes de BSS | `video/gpu.c` |
| Teste `if (gpu_drv)` | 1 comparacao por quadro, em `gfx_frame_end` | `video/visual.c` |
| `gpu_probe()` no boot | compara vendor/device com uma tabela curta; aproveita a varredura PCI que o modo grafico ja faz (`pci_find_display_bar0`) | `video/gpu.c` |
| Teste em `gfx_buf_init()` (pular a pagina dupla se ha placa na tabela) | 1 comparacao por troca de modo, nunca por quadro | `video/visual.c` |
| Codigo do carregador (`gpu.c`) | algumas centenas de bytes no `kernel.bin` | `video/gpu.c` |

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
boot / entrar no WIN
  |
  gpu_probe(): vendor:device do PCI esta na tabela?
  |-- nao -> fim. gpu_drv = 0, nada carregado.
  |
  mod_dll_load("D:\SODOS\DRV\<nome>.DLL")
  |-- falhou (arquivo ausente, formato, import) -> log serial, fim.
  |
  drv->init(&gpu_kapi, &modo)        driver liga a placa, aloca o que precisa
  |-- erro -> drv->fini(), mod_dll_free(), fim.
  |
  drv->selftest()                    um quadro de teste vai ate a tela
  |-- erro -> drv->fini(), mod_dll_free(), fim.
  |
  gpu_drv = drv                      so aqui o kernel passa a usar o driver
```

Durante o uso, qualquer erro devolvido pelo driver (`present` falhou,
fila travou, timeout) faz o kernel:

1. zerar `gpu_drv` **antes** de qualquer outra coisa;
2. refazer o mesmo quadro pelo caminho por CPU (`gfx_copy_back`), para a
   tela nao ficar preta;
3. chamar `drv->fini()` e `mod_dll_free()`;
4. registrar o motivo no log serial (`[gpu] desativado: ...`).

Depois disso o sistema esta exatamente no estado "sem driver".

O DLL e carregado no contexto do kernel (`cur_pid == 0`), entao nao e
liberado quando um programa termina (`module.c` so libera DLLs ligadas a um
pid).

## Interface kernel <-> driver

Rascunho; os nomes podem mudar na etapa 1, mas a forma e esta.
`abi` permite ao kernel recusar um driver compilado para outra versao.

```c
/* video/gpu.h */
#define GPU_ABI 1

/* O que o kernel oferece ao driver. Nada de libc: so isto. */
struct gpu_kapi {
  int abi;
  void (*log)(const char* s);
  uint32_t (*pci_read32)(uint8_t bus, uint8_t dev, uint8_t fun, uint8_t off);
  void (*pci_write32)(uint8_t bus, uint8_t dev, uint8_t fun, uint8_t off, uint32_t v);
  void* (*alloc)(uint32_t bytes, uint32_t align);   /* memoria fisica = virtual */
  void (*free)(void* p);
  uint32_t (*ms)(void);                             /* relogio para timeouts */
};

/* Modo e superficie de trabalho que o kernel ja usa hoje. */
struct gpu_mode {
  uint32_t w, h;
  uint32_t pitch;      /* bytes por linha do back buffer */
  void* back;          /* back buffer em RAM (gfx_back) */
};

struct gpu_rect { int32_t x, y, w, h; };

#define GPU_CAP_PRESENT  1u   /* apresenta o back buffer (obrigatorio) */
#define GPU_CAP_DAMAGE   2u   /* aceita so as areas alteradas */
#define GPU_CAP_CURSOR   4u   /* cursor por hardware */
#define GPU_CAP_FILL     8u   /* reservado: preenchimento na placa */
#define GPU_CAP_BLIT    16u   /* reservado: copia na placa */

struct gpu_drv {
  int abi;
  const char* name;
  uint32_t caps;
  int (*init)(const struct gpu_kapi* k, const struct gpu_mode* m);
  int (*selftest)(void);
  /* 0 = ok. rects = NULL apresenta a tela inteira. */
  int (*present)(const struct gpu_rect* rects, int n);
  void (*fini)(void);
};

/* Unico simbolo que o DLL exporta. */
const struct gpu_drv* gpu_driver(void);
```

Regras para o DLL, porque ele roda no contexto do kernel:

- formato SOX2 (ABI LP64, a mesma do kernel), compilado com
  `-mno-sse -mno-red-zone` (o kernel nao salva SSE e as interrupcoes usam a
  mesma pilha);
- sem libc e sem `k_*` de programa: so o que vem em `gpu_kapi`;
- nao pode bloquear sem timeout. Toda espera usa `k->ms()` e devolve erro.

## Como o primeiro driver se encaixa no quadro atual

Hoje `gfx_buf_init()` escolhe entre dois jeitos de montar o quadro:

- **pagina dupla Bochs** (`gfx_dbuf`): desenha direto na outra metade da VRAM
  e troca o `Y_OFFSET`. E a preferida quando a VRAM tem altura virtual 2*h,
  o que acontece no QEMU com `-vga std`;
- **back buffer em RAM** (`gfx_back`, em `GFX_BACK_PHYS`): desenha na RAM e
  `gfx_copy_back()` copia o quadro inteiro para o framebuffer.

O driver usa o back buffer em RAM como memoria da placa, entao precisa dele.
Com driver ativo:

- `gfx_frame_begin` nao muda nem ganha teste: ja desenha em `gfx_back`;
- `gfx_frame_end` chama `gpu_drv->present(...)` em vez de `gfx_copy_back()`.

Ordem no boot do modo grafico:

1. `gpu_probe()` roda **antes** de `gfx_buf_init()`. Se a placa estiver na
   tabela, `gfx_buf_init()` pula a pagina dupla e monta o back buffer em RAM.
   Sem placa na tabela, `gfx_buf_init()` faz a mesma escolha de hoje.
2. Se a carga, o `init` ou o `selftest` falharem, `gfx_buf_init()` roda de
   novo e volta a escolha de hoje (pagina dupla, se houver).
3. Sem back buffer em RAM (`gfx_ram_ok()` falhou), o driver nao e carregado.

Esse pulo da pagina dupla e um teste a mais em `gfx_buf_init()`, que roda so
na troca de modo, nunca por quadro; esta na tabela de custo acima.

No **virtio-gpu 2D** (QEMU `-device virtio-vga`) isso vira:

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

## Ver no TOP (aba Video)

O `TOP.EXE` tem a aba **Video** (F3 ou Tab ate ela), que le `k_vram`
(`struct k_vram` em `so.h`; no kernel, `win_vram_info()` em `video/visual.c`):

- placa (Bochs VBE, GOP ou VGA), id PCI e endereco do framebuffer;
- driver da placa ativo, ou "nenhum: a CPU desenha tudo";
- modo e estado: texto, WIN na tela ou WIN ao fundo (atras de uma tela do
  DOS, Ctrl+F1, quando o modo grafico continua ocupando memoria);
- a VRAM num quadro, como o mapa da RAM: tela visivel, segunda pagina
  (pagina dupla Bochs), texto e fonte, livre;
- a RAM comum gasta por causa do video: back buffer, papel de parede,
  superficies das janelas e memoria do driver da placa.

`k_vram` so le o estado que ja existe; nao reprograma a placa. No GOP a placa
so informa o quadro visivel, entao o total da VRAM e o tamanho desse quadro.

Obrigacao do driver (etapa 1 em diante): quando ativo, o kernel preenche
`drv` com o nome dele e `gpu_kb` com a memoria que ele reservou (pedida por
`gpu_kapi.alloc`). Sem driver os dois ficam vazios, e a linha "Driver da
placa" do TOP mostra "sem driver: nada reservado", que e a prova visivel da
regra principal.


`D:\SODOS\VIDEO.CFG` ganha a chave:

```
gpu=0      nunca carrega driver (o padrao continua sendo tentar)
```

Serve para diagnostico e para maquinas onde o driver se comporta mal.

## Etapas

| # | Etapa | Estado |
|---|---|---|
| 0 | Este documento | feito |
| 1 | `video/gpu.h` + `video/gpu.c`: tabela PCI, carga, `selftest`, desativacao segura, `gpu=0`; os testes em `gfx_buf_init` e `gfx_frame_end` | a fazer |
| 2 | `VIRTGPU.DLL`: virtio PCI moderno, fila de controle, `present` da tela inteira | a fazer |
| 3 | `present` so das areas alteradas (`GPU_CAP_DAMAGE`) | a fazer |
| 4 | Cursor por hardware (`GPU_CAP_CURSOR`) | a fazer |
| 5 | Operacoes na placa (`FILL`/`BLIT`), so se medido e registrado na tabela de custo | em aberto |

Fora do escopo por enquanto: virtio-gpu 3D (virgl), VMware SVGA, amdgpu
(o notebook segue no framebuffer GOP pelo caminho atual).

O redesenho so das areas alteradas **no caminho por CPU** (sem driver) e
outra mudanca, separada desta: ela mexeria no caminho atual e por isso so
entra se for pedida.

## Testar

Planejado para a etapa 2:

```
make run-virtio    # QEMU com -device virtio-vga em vez de -vga std
make run           # sem a placa: tem que se comportar como antes
```

No log serial (`qemu_serial.log`):

```
[gpu] 1af4:1050 -> D:\SODOS\DRV\VIRTGPU.DLL
[gpu] virtio-gpu ativo 1920x1080
```

ou, sem a placa, nenhuma linha `[gpu]` alem de, no maximo,
`[gpu] nenhum driver`.

## Arquivos (planejados)

- `video/gpu.h`, `video/gpu.c` — interface, carregador e desativacao.
- `video/visual.c` — os testes de `gpu_drv` em `gfx_buf_init` e `gfx_frame_end`.
- `drivers/virtgpu/` — fonte do `VIRTGPU.DLL`.
- `docs/VIDEO.md` — os modelos de renderizacao por CPU (retro/avancado),
  que continuam valendo com ou sem driver.
