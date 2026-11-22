/* chan.c - Canal de comandos da VM do WSL: segunda serial (16550 em 0x2F8,
 * IRQ 3, ttyS1 no Linux) ligada a caixa wslbox (wslbox.h).
 *
 * Roda na thread da CPU da VM (dev_poll a cada saida), entao um comando do
 * BASH.EXE nao depende da tela Ctrl+F10 ganhar a vez. Da caixa para a serial
 * vai um pedido por vez (cabecalho + cwd + comando); da serial para a caixa,
 * as respostas do agente (O/E/X) vao para os aneis da vaga. Controle de
 * fluxo: com o anel de saida quase cheio o LSR nao diz "THR vazio" e o driver
 * 8250 do Linux espera, em vez de perder bytes. */
#include <stdio.h>
#include <string.h>
#include "vmm.h"
#include "../wslbox.h"
#include "../sodos_api.h"

#define CH_RING 65536u
#define CH_LOW 256u /* espaco minimo no anel de saida para aceitar bytes */

static struct {
  struct wslbox* box;
  uint8_t ier, lcr, mcr, scr, dll, dlm, fcr;
  int thre;
  uint8_t tx[CH_RING]; /* Linux -> SODOS */
  uint32_t txh, txt;
  uint8_t rx[CH_RING]; /* SODOS -> Linux */
  uint32_t rxh, rxt;
  /* leitura das respostas */
  char hdr[96];
  int hn;
  int dslot;           /* vaga do bloco de dados (-1 = descarta) */
  int dkind;           /* 'O' ou 'E' */
  uint32_t dleft;
  /* pedido sendo posto na serial */
  int feeding;
  char fhdr[96];
  int fhn, fhoff;
} ch; /* no .bss: os aneis somam 128 KB */

void chan_attach(struct wslbox* box) {
  ch.box = box;
  ch.thre = 1;
  ch.feeding = -1;
  ch.dslot = -1;
}

static uint32_t rx_free(void) { return CH_RING - (ch.rxh - ch.rxt); }
static uint32_t tx_free(void) { return CH_RING - (ch.txh - ch.txt); }

static void rx_put(const void* p, uint32_t n) {
  const uint8_t* b = (const uint8_t*)p;
  uint32_t i;
  for (i = 0; i < n; i++) ch.rx[(ch.rxh + i) % CH_RING] = b[i];
  ch.rxh += n;
}

static void irq_update(struct vm* vm) {
  int line = ((ch.ier & 1) && ch.rxh != ch.rxt) || ((ch.ier & 2) && ch.thre);
  if (!(ch.mcr & 8)) line = 0;
  if (line && ch.box) ch.box->d_irq++;
  pic_set_irq(vm, 3, line);
}

static void wake_slot(struct wslb_slot* s) {
  s->seq++;
  k_futex_wake(&s->seq, -1);
}

/* ---- SODOS -> Linux: um pedido por vez ---- */
static void feed(void) {
  struct wslbox* b = ch.box;
  int i;
  /* Ctrl+C do cliente: K id */
  for (i = 0; i < WSLB_SLOTS; i++) {
    struct wslb_slot* s = &b->slot[i];
    if (s->state == SL_SENT && s->kill && !s->killed && rx_free() > 64) {
      char k[48];
      int n = snprintf(k, sizeof k, "K %d.%u\n", i, s->gen);
      rx_put(k, (uint32_t)n);
      s->killed = 1;
    }
  }
  if (ch.feeding < 0) {
    if (!b->ready) return;
    for (i = 0; i < WSLB_SLOTS; i++) {
      struct wslb_slot* s = &b->slot[i];
      if (s->state == SL_POSTED) {
        uint32_t cl = (uint32_t)strlen(s->cwd);
        ch.fhn = snprintf(ch.fhdr, sizeof ch.fhdr, "R %d.%u %d %u %u\n", i, s->gen, s->flags, cl, s->cmd_len);
        ch.fhoff = 0;
        s->fed = 0;
        ch.feeding = i;
        break;
      }
    }
    if (ch.feeding < 0) return;
  }
  {
    struct wslb_slot* s = &b->slot[ch.feeding];
    uint32_t cl = (uint32_t)strlen(s->cwd), total = cl + s->cmd_len;
    while (ch.fhoff < ch.fhn && rx_free()) {
      rx_put(ch.fhdr + ch.fhoff, 1);
      ch.fhoff++;
    }
    while (ch.fhoff >= ch.fhn && s->fed < total && rx_free()) {
      uint32_t n = total - s->fed, f = rx_free();
      if (n > f) n = f;
      if (s->fed < cl) {
        if (n > cl - s->fed) n = cl - s->fed;
        rx_put(s->cwd + s->fed, n);
      } else {
        rx_put(s->cmd + (s->fed - cl), n);
      }
      s->fed += n;
    }
    if (ch.fhoff >= ch.fhn && s->fed >= total) {
      s->state = SL_SENT;
      ch.feeding = -1;
    }
  }
}

/* "S.G" -> vaga com essa geracao, ou -1 */
static int slot_of(const char* id) {
  int s = 0;
  uint32_t g = 0;
  while (*id >= '0' && *id <= '9') s = s * 10 + (*id++ - '0');
  if (*id++ != '.' || s < 0 || s >= WSLB_SLOTS) return -1;
  while (*id >= '0' && *id <= '9') g = g * 10u + (uint32_t)(*id++ - '0');
  if (ch.box->slot[s].gen != g || ch.box->slot[s].state != SL_SENT) return -1;
  return s;
}

static void header(void) {
  struct wslbox* b = ch.box;
  char kind = ch.hdr[0], id[32];
  long long v = 0;
  int i = 2, k = 0;
  ch.hdr[ch.hn] = 0;
  b->d_frames++;
  for (k = 0; k < (int)sizeof b->d_last - 1 && ch.hdr[k]; k++) b->d_last[k] = ch.hdr[k];
  b->d_last[k] = 0;
  k = 0;
  if (!strcmp(ch.hdr, "READY")) {
    /* agente (re)comecou: pedidos em voo morreram com o anterior */
    for (k = 0; k < WSLB_SLOTS; k++) {
      struct wslb_slot* s = &b->slot[k];
      if (s->state == SL_SENT || (s->state == SL_POSTED && ch.feeding == k)) {
        s->exit_code = 255;
        s->state = SL_DONE;
        wake_slot(s);
      }
    }
    ch.feeding = -1;
    b->ready = 1;
    return;
  }
  if ((kind != 'O' && kind != 'E' && kind != 'X') || ch.hdr[1] != ' ') return;
  while (ch.hdr[i] && ch.hdr[i] != ' ' && k < (int)sizeof id - 1) id[k++] = ch.hdr[i++];
  id[k] = 0;
  if (ch.hdr[i] == ' ') i++;
  {
    int neg = ch.hdr[i] == '-';
    if (neg) i++;
    while (ch.hdr[i] >= '0' && ch.hdr[i] <= '9') v = v * 10 + (ch.hdr[i++] - '0');
    if (neg) v = -v;
  }
  k = slot_of(id);
  if (kind == 'X') {
    if (k >= 0) {
      b->slot[k].exit_code = (int)v;
      b->slot[k].state = SL_DONE;
      wake_slot(&b->slot[k]);
    }
    return;
  }
  ch.dslot = k;
  ch.dkind = kind;
  ch.dleft = (uint32_t)v;
}

/* ---- Linux -> SODOS ---- */
static void parse(void) {
  int woke[WSLB_SLOTS] = {0};
  int i;
  while (ch.txt != ch.txh) {
    if (ch.dleft) {
      uint32_t n = ch.txh - ch.txt;
      if (n > ch.dleft) n = ch.dleft;
      if (ch.dslot >= 0) {
        struct wslb_slot* s = &ch.box->slot[ch.dslot];
        struct wslb_ring* r = ch.dkind == 'O' ? &s->out : &s->err;
        uint32_t f = wslb_free(r), j;
        if (n > f) n = f;
        if (!n) break; /* cliente atrasado: segura (o Linux espera) */
        for (j = 0; j < n; j++) r->buf[(r->head + j) % WSLB_RING] = ch.tx[(ch.txt + j) % CH_RING];
        __atomic_store_n(&r->head, r->head + n, __ATOMIC_RELEASE);
        woke[ch.dslot] = 1;
      }
      ch.txt += n;
      ch.dleft -= n;
      continue;
    }
    {
      uint8_t c = ch.tx[ch.txt % CH_RING];
      ch.txt++;
      if (c == '\n') {
        header();
        ch.hn = 0;
      } else if (c != '\r' && ch.hn < (int)sizeof ch.hdr - 1) {
        ch.hdr[ch.hn++] = (char)c;
      }
    }
  }
  for (i = 0; i < WSLB_SLOTS; i++)
    if (woke[i]) wake_slot(&ch.box->slot[i]);
}

/* thread da CPU da VM, a cada saida */
void chan_poll(struct vm* vm) {
  if (!ch.box) return;
  feed();
  parse();
  if (!ch.thre && tx_free() >= CH_LOW) ch.thre = 1;
  irq_update(vm);
  ch.box->d_rx = ch.rxh - ch.rxt;
  ch.box->d_tx = ch.txh - ch.txt;
  ch.box->d_feeding = ch.feeding;
  ch.box->d_ier = ch.ier | (ch.mcr << 8);
}

/* algo para a VM ja (a CPU parada deve rodar) */
int chan_pending(void) { return ch.box && ch.rxh != ch.rxt; }

/* ---- 16550 em 0x2F8 ---- */
uint8_t chan_uart_in(struct vm* vm, int r) {
  uint8_t v = 0;
  switch (r) {
  case 0:
    if (ch.lcr & 0x80) return ch.dll;
    if (ch.rxh != ch.rxt) v = ch.rx[ch.rxt++ % CH_RING];
    break;
  case 1:
    return (ch.lcr & 0x80) ? ch.dlm : ch.ier;
  case 2:
    if ((ch.ier & 1) && ch.rxh != ch.rxt) v = 0x04;
    else if ((ch.ier & 2) && ch.thre) {
      v = 0x02;
      ch.thre = 0;
    } else v = 0x01;
    if (ch.fcr & 1) v |= 0xC0;
    break;
  case 3: return ch.lcr;
  case 4: return ch.mcr;
  case 5:
    v = (uint8_t)((tx_free() >= CH_LOW ? 0x60 : 0x00) | (ch.rxh != ch.rxt ? 1 : 0));
    break;
  case 6: v = 0xB0; break;
  case 7: return ch.scr;
  }
  irq_update(vm);
  return v;
}

void chan_uart_out(struct vm* vm, int r, uint8_t v) {
  switch (r) {
  case 0:
    if (ch.lcr & 0x80) {
      ch.dll = v;
      return;
    }
    if (tx_free()) ch.tx[ch.txh++ % CH_RING] = v;
    ch.thre = tx_free() >= CH_LOW;
    break;
  case 1:
    if (ch.lcr & 0x80) {
      ch.dlm = v;
      return;
    }
    if ((v & 2) && !(ch.ier & 2)) ch.thre = tx_free() >= CH_LOW;
    ch.ier = v & 0x0F;
    break;
  case 2: ch.fcr = v; break;
  case 3: ch.lcr = v; break;
  case 4: ch.mcr = v & 0x1F; break;
  case 7: ch.scr = v; break;
  }
  irq_update(vm);
}
