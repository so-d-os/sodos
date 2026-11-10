/* gpu.h - Driver de placa de video (docs/GPU.md).
 * A primeira metade e o contrato com o DLL do driver (compilado a parte,
 * gcc -m64 + link64 /dll). A segunda e o que visual.c chama.
 *
 * Regra: sem driver ativo, o kernel nao gasta nada alem de gpu_drv (um
 * ponteiro) e dos testes listados na tabela de custo de docs/GPU.md. */
#pragma once
#include <stdint.h>

#define GPU_ABI 1

/* O que o kernel oferece ao driver. Nada de libc nem k_*: so isto. */
struct gpu_kapi {
  int abi;
  void (*log)(const char* s);
  void (*log_hex)(uint32_t v);
  uint32_t (*pci_read32)(uint32_t bus, uint32_t dev, uint32_t fun, uint32_t off);
  void (*pci_write32)(uint32_t bus, uint32_t dev, uint32_t fun, uint32_t off, uint32_t v);
  /* memoria com endereco fisico = virtual; align potencia de 2 (0 = 16) */
  void* (*alloc)(uint32_t bytes, uint32_t align);
  void (*free)(void* p);
  uint32_t (*ms)(void);   /* relogio em ms, para timeouts */
};

/* Placa achada e quadro que o kernel ja usa. */
struct gpu_mode {
  uint32_t w, h;
  uint32_t pitch;          /* bytes por linha de back */
  void* back;              /* back buffer em RAM (gfx_back), 32 bpp */
  volatile uint8_t* fb;    /* framebuffer linear (LFB) */
  uint32_t fb_pitch;
  uint32_t pci_id;         /* vendor << 16 | device */
  uint32_t pci_bus, pci_dev, pci_fun;
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
  /* 0 = ok. Qualquer outro valor: o kernel chama fini e descarrega. */
  int (*init)(const struct gpu_kapi* k, const struct gpu_mode* m);
  int (*selftest)(void);
  /* rects = 0 apresenta a tela inteira */
  int (*present)(const struct gpu_rect* rects, int n);
  void (*fini)(void);
  /* Opcionais. O WIN foi para o fundo (Ctrl+F1..F12): a placa devolve a tela
   * ao modo texto mas guarda o que reservou. resume volta com o modo novo.
   * Sem elas o kernel usa fini e, na volta, carrega de novo. */
  int (*suspend)(void);
  int (*resume)(const struct gpu_mode* m);
  /* Opcionais, com GPU_CAP_CURSOR: cursor desenhado pela placa por cima do
   * quadro. argb: w*h palavras 0xAARRGGBB (w, h <= 64); hot = ponta da seta. */
  int (*cursor_set)(const uint32_t* argb, int w, int h, int hot_x, int hot_y);
  int (*cursor_move)(int x, int y);
};

/* Unico simbolo que o DLL exporta. */
typedef const struct gpu_drv* (*gpu_driver_fn)(void);
#define GPU_DRIVER_SYM "gpu_driver"

/* ---------- lado do kernel (gpu.c) ---------- */

/* Driver ativo; 0 = a CPU desenha tudo (o caminho de sempre). */
extern const struct gpu_drv* gpu_drv;

/* Uma linha do VIDEO.CFG que comeca com "gpu" (gpu=0, gpudrv=X.DLL).
 * line = 0 volta ao padrao (chamado no inicio da leitura do arquivo). */
void gpu_cfg_line(const char* line, int n);
/* 1 se vale tentar um driver: placa na tabela (ou gpudrv=) e gpu=0 ausente.
 * Roda so na troca de modo. */
int gpu_wanted(void);
/* Carrega, inicia e testa o driver. 0 = ativo. Falhou: tudo liberado. */
int gpu_start(const struct gpu_mode* m);
/* Desliga e descarrega o driver (fim do WIN, erro). Sem driver, nada. */
void gpu_stop(const char* why);
/* Fim do WIN: gpu_stop sem marcar falha. */
void gpu_close(void);
/* WIN para o fundo / volta. Sem driver, nada. gpu_resume: 0 = ativo de novo;
 * outro valor: o driver foi descarregado e o quadro volta a CPU. */
void gpu_suspend(void);
int gpu_resume(const struct gpu_mode* m);
/* 1 se ha driver carregado, parado com o WIN ao fundo. */
int gpu_suspended(void);
/* Apresenta o back buffer. rects = 0 (ou n = 0): a tela inteira; senao so
 * essas areas (ja recortadas na tela). 0 = ok; outro valor: o driver ja foi
 * desligado e quem chamou tem que mostrar o quadro pela CPU. */
int gpu_present(const struct gpu_rect* rects, int n);
/* 1 = a placa desenha o cursor: o kernel nao o poe no quadro e so manda a
 * posicao. Volta a 0 quando o driver para. */
extern int gpu_hwcur;
/* Entrega a imagem do cursor; liga gpu_hwcur se o driver aceitar. */
void gpu_cursor_set(const uint32_t* argb, int w, int h, int hot_x, int hot_y);
/* 0 = ok; outro valor: o driver caiu e o quadro volta a CPU. */
int gpu_cursor_move(int x, int y);
/* Placa de video no PCI (vendor << 16 | device), seguindo as pontes; 0 se
 * nao achou. Nome dela no SODOS.CFG (nome=), ou 0. */
uint32_t gpu_find_display(void);
const char* gpu_card_name(void);
/* Para o TOP (k_vram): nome do driver ativo e KB que ele reservou. */
const char* gpu_name(void);
uint32_t gpu_mem_kb(void);
