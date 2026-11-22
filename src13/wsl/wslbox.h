/* wslbox.h - Caixa de comandos da VM do WSL (SET WSL=1).
 *
 * O WSL.EXE em modo servico (WSL run --service, tela Ctrl+F10) publica esta
 * caixa no registro de nomes do kernel (k_svc_set "WSL"). Quem quer rodar um
 * comando no Linux (WSL exec, o BASH.EXE do Claude Code) pega uma vaga, poe
 * o comando e espera; a thread da CPU da VM (vmm/chan.c) manda o pedido pela
 * segunda serial (ttyS1) ao agente da imagem (/usr/local/sbin/sodos-agent) e
 * devolve stdout, stderr e o codigo de saida nos aneis da vaga.
 *
 * A memoria e k_malloc_shared (sem dono): sobrevive ao servico e e reusada
 * quando ele volta. Protocolo na serial (texto + bytes):
 *   SODOS -> Linux:  R <id> <flags> <cwdlen> <cmdlen>\n<cwd><cmd>   (rodar)
 *                    K <id>\n                                       (matar)
 *   Linux -> SODOS:  READY\n | O <id> <len>\n<bytes> | E <id> <len>\n<bytes>
 *                    | X <id> <codigo>\n
 * id = vaga.geracao (a geracao muda a cada pedido da vaga). */
#ifndef WSLBOX_H
#define WSLBOX_H
#include <stdint.h>

#define WSLB_MAGIC 0x584F4257u /* "WBOX" */
#define WSLB_VERSION 1
#define WSLB_SLOTS 8
#define WSLB_RING 65536u
#define WSLB_CWD 260
#define WSLB_CMD_MAX (256u * 1024u)

/* flags do pedido */
#define WSLB_LOGIN 1 /* bash -l (como o "bash -c -l" do Claude Code) */

enum { SL_FREE, SL_CLAIM, SL_POSTED, SL_SENT, SL_DONE };

struct wslb_ring {
  volatile uint32_t head, tail; /* head: quem escreve; tail: quem le */
  uint8_t buf[WSLB_RING];
};

struct wslb_slot {
  volatile int state;
  volatile int seq;       /* muda a cada novidade (o cliente espera com futex) */
  volatile uint32_t gen;  /* geracao do pedido (id = vaga.gen) */
  int flags;
  char cwd[WSLB_CWD];
  char* cmd;              /* k_malloc_shared do cliente; ele solta depois */
  uint32_t cmd_len;
  uint32_t fed;           /* servidor: bytes do pedido ja postos na serial */
  volatile int kill;      /* cliente pede para matar (Ctrl+C) */
  volatile int killed;    /* servidor ja mandou o K */
  volatile int exit_code;
  struct wslb_ring out, err;
};

struct wslbox {
  uint32_t magic, version;
  volatile int alive;     /* o servico esta rodando */
  volatile int ready;     /* o agente do Linux disse READY */
  volatile uint32_t beat; /* k_ms da ultima volta do servico */
  volatile int* wake;     /* futex da CPU da VM: o cliente acorda depois de postar */
  /* diagnostico (WSL status), escrito pela thread da VM */
  volatile uint32_t d_rx, d_tx;      /* bytes esperando: SODOS->Linux, Linux->SODOS */
  volatile uint32_t d_frames, d_irq; /* cabecalhos lidos, IRQ 3 ligada */
  volatile int d_feeding, d_ier;
  char d_last[64];                   /* ultimo cabecalho do agente */
  struct wslb_slot slot[WSLB_SLOTS];
};

static inline uint32_t wslb_used(const struct wslb_ring* r) { return r->head - r->tail; }
static inline uint32_t wslb_free(const struct wslb_ring* r) { return WSLB_RING - (r->head - r->tail); }

#endif
