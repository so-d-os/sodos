/*
 * hda.c - AUDIO.DLL: driver HD Audio (Intel High Definition Audio). Contrato
 * em kernel/audio.h, uso em docs/AUDIO.md.
 *
 * Acha os controladores HDA no PCI (classe 04:03), reseta, conversa com os
 * codecs pela CORB/RIRB (ou pelos comandos imediatos, se a CORB nao anda) e
 * fica com o primeiro codec que tem saida analogica: alto-falante, fone ou
 * linha. Liga o caminho DAC -> (mixer/seletor) -> pino de cada saida (os DACs
 * ouvem o mesmo fluxo) e toca um fluxo de saida em DMA num buffer circular,
 * em polling, sem interrupcao. Com fone ligado (presence detect) o
 * alto-falante cala.
 *
 * Primeiro os controladores que nao sao de placa de video: o HDMI da AMD/ATI
 * (1002) e da NVIDIA (10DE) so e tentado se nao houver outro, e tem so pinos
 * digitais (e o amdgop liga a tela em modo DVI, sem audio).
 *
 * Particularidades seguidas do Linux (sound/pci/hda/hda_intel.c):
 *  - AMD/ATI: snoop ligado no byte 0x42 do PCI (senao a placa le a RAM sem
 *    ver o cache da CPU); o CORBRP da AMD volta a 0 sozinho depois do reset.
 *  - Intel: TCSEL (byte 0x44) = 0.
 *  - Nenhuma entrada da BDL cruza 4 KB (algumas AMD/ATI exigem): o buffer e
 *    alinhado em 4 KB e cada entrada tem 4 KB.
 *
 * Roda no contexto do kernel: gcc -m64 -mno-sse -mno-red-zone, sem libc.
 * service() roda no IRQ0: nada de esperas longas ali.
 */
#include <stdint.h>
#include "audio.h"

/* ---------- registradores do controlador ---------- */
#define GCAP 0x00
#define VMIN 0x02
#define VMAJ 0x03
#define GCTL 0x08
#define STATESTS 0x0E
#define INTCTL 0x20
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP 0x48
#define CORBRP 0x4A
#define CORBCTL 0x4C
#define CORBSIZE 0x4E
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP 0x58
#define RINTCNT 0x5A
#define RIRBCTL 0x5C
#define RIRBSTS 0x5D
#define RIRBSIZE 0x5E
#define ICOI 0x60
#define ICII 0x64
#define ICIS 0x68
#define SD_BASE 0x80
#define SD_LEN 0x20
/* descritor de fluxo */
#define SD_CTL 0x00
#define SD_CTL2 0x02
#define SD_STS 0x03
#define SD_LPIB 0x04
#define SD_CBL 0x08
#define SD_LVI 0x0C
#define SD_FIFOS 0x10
#define SD_FMT 0x12
#define SD_BDPL 0x18
#define SD_BDPU 0x1C

/* ---------- verbos e parametros do codec ---------- */
#define V_GET_PARAM 0xF00u
#define V_GET_CONN_LIST 0xF02u
#define V_GET_POWER 0xF05u
#define V_GET_PIN_SENSE 0xF09u
#define V_GET_CONFIG 0xF1Cu
#define V_SET_CONN_SEL 0x701u
#define V_SET_POWER 0x705u
#define V_SET_STREAM 0x706u
#define V_SET_PIN_CTL 0x707u
#define V_SET_PIN_SENSE 0x709u
#define V_SET_EAPD 0x70Cu
#define V4_SET_FMT 0x2u
#define V4_SET_AMP 0x3u

#define P_VENDOR 0x00u
#define P_REV 0x02u
#define P_NODES 0x04u
#define P_FG_TYPE 0x05u
#define P_WCAPS 0x09u
#define P_PCM 0x0Au
#define P_PINCAP 0x0Cu
#define P_AMP_IN 0x0Du
#define P_CONN_LEN 0x0Eu
#define P_AMP_OUT 0x12u

#define W_OUT 0u   /* DAC */
#define W_MIX 2u
#define W_SEL 3u
#define W_PIN 4u

#define WC_IN_AMP (1u << 1)
#define WC_OUT_AMP (1u << 2)
#define WC_AMP_OVR (1u << 3)
#define WC_CONN (1u << 8)
#define WC_DIGITAL (1u << 9)
#define WC_POWER (1u << 10)

#define PC_TRIGGER (1u << 1)
#define PC_PRESENCE (1u << 2)
#define PC_HP_DRIVE (1u << 3)
#define PC_OUT (1u << 4)
#define PC_EAPD (1u << 16)

/* default device do pino (config default, bits 23:20) */
#define DEV_LINE 0u
#define DEV_SPK 1u
#define DEV_HP 2u
#define DEV_OTHER 0xFu

/* ---------- tamanhos ---------- */
#define CTRL_MAX 6
#define NODE_MAX 96
#define OUT_MAX 4
#define HOPS 6
#define BDL_N 32
#define BDL_BYTES 4096u
#define RING (BDL_N * BDL_BYTES)     /* 128 KB: 680 ms a 48 kHz */
#define TAG 1u                       /* numero do fluxo no link */
#define ZMARGIN 512u                 /* nao zera tao perto da cabeca da placa */
#define IDLE_MS 1500u                /* sem nada para tocar: a placa para */
#define JACK_MS 500u
#define CMD_MS 20u

struct pctrl {
  uint8_t bus, dev, fun, tried;
  uint16_t ven, did;
  uint8_t video;                     /* HDMI de placa de video */
  uint8_t pad;
};

struct node {
  uint32_t caps, pincap, cfg, amp_out, amp_in;
  uint8_t nconn;
  uint8_t conn[16];
};

struct out {
  uint8_t pin, dev, n, vnid;         /* vnid: no do volume (0 = nenhum) */
  uint8_t path[HOPS];                /* path[0] = pino ... path[n-1] = DAC */
  uint8_t sel[HOPS];                 /* entrada escolhida em path[i] */
  uint8_t present, jack;             /* jack: o pino sabe se ha fone */
};

static const struct audio_host* k;

static struct {
  struct pctrl pc[CTRL_MAX];
  int npc, ci;                       /* ci: controlador em uso (-1 nenhum) */
  volatile uint8_t* mm;
  uint16_t gcap;
  int iss, oss;
  void* cmdmem;
  volatile uint32_t* corb;
  volatile uint32_t* rirb;
  uint32_t corb_n, rirb_n;
  uint32_t rirb_rp;
  int imm;                           /* comandos imediatos (sem CORB) */
  int cmd_err;
  /* codec */
  int cad, afg, wstart, wn;
  uint32_t vendor, rev, afg_pcm, afg_amp_out, afg_amp_in;
  struct node nd[NODE_MAX];
  struct out o[OUT_MAX];
  int no;
  uint32_t pcm;                      /* taxas que todos os DACs tem */
  /* fluxo */
  volatile uint8_t* sd;
  uint8_t* ring;
  volatile uint32_t* bdl;
  uint16_t fmt;
  uint32_t rate, bps, maxq, lead;
  int started, running;
  uint64_t wr, pl, zr;               /* bytes escritos, tocados, zerados */
  uint32_t last_lpib, last_ms, idle_since, jack_ms;
  int vol;
  int dumped;                        /* estado do codec ja foi para o log no start */
} s;

/* ---------- texto ---------- */
struct sb {
  char* p;
  int n, max;
};
static void sb_c(struct sb* b, char c) {
  if (b->n < b->max - 1) b->p[b->n++] = c;
}
static void sb_s(struct sb* b, const char* t) {
  while (t && *t) sb_c(b, *t++);
}
static void sb_d(struct sb* b, uint32_t v) {
  char t[12];
  int i = 0;
  do t[i++] = (char)('0' + v % 10u); while ((v /= 10u) && i < 11);
  while (i) sb_c(b, t[--i]);
}
static void sb_x(struct sb* b, uint32_t v, int digits) {
  int i;
  for (i = digits - 1; i >= 0; i--) sb_c(b, "0123456789ABCDEF"[(v >> (i * 4)) & 15u]);
}

static void logs(const char* t) { k->log(t); }
static void logx(uint32_t v) { k->log_hex(v); }

/* ---------- PCI ---------- */
static uint32_t pci(const struct pctrl* p, uint32_t off) { return k->pci_read32(p->bus, p->dev, p->fun, off); }
static void pciw(const struct pctrl* p, uint32_t off, uint32_t v) { k->pci_write32(p->bus, p->dev, p->fun, off, v); }

/* ---------- MMIO e tempo ---------- */
static void barreira(void) { __asm__ volatile ("lock; addl $0,(%%rsp)" : : : "memory"); }
static void io_delay(void) { __asm__ volatile ("outb %%al, $0x80" : : "a"((uint8_t)0)); }

static uint8_t r8(uint32_t o) { return *(volatile uint8_t*)(s.mm + o); }
static uint16_t r16(uint32_t o) { return *(volatile uint16_t*)(s.mm + o); }
static uint32_t r32(uint32_t o) { return *(volatile uint32_t*)(s.mm + o); }
static void w8(uint32_t o, uint8_t v) { *(volatile uint8_t*)(s.mm + o) = v; }
static void w16(uint32_t o, uint16_t v) { *(volatile uint16_t*)(s.mm + o) = v; }
static void w32(uint32_t o, uint32_t v) { *(volatile uint32_t*)(s.mm + o) = v; }

static uint32_t rdw(uint32_t o, int w) { return w == 1 ? r8(o) : w == 2 ? r16(o) : r32(o); }

/* O relogio pode nao andar (IRQ0): o contador de voltas tambem encerra. */
static void delay_ms(uint32_t ms) {
  uint32_t t0 = k->ms(), n = 0;
  while (k->ms() - t0 <= ms && n++ < ms * 2000u + 2000u) io_delay();
}

/* (registro & mask) == val ate ms. 0 ok, -1 tempo esgotado. */
static int waitr(uint32_t off, int w, uint32_t mask, uint32_t val, uint32_t ms) {
  uint32_t t0 = k->ms(), n = 0;
  for (;;) {
    if ((rdw(off, w) & mask) == val) return 0;
    if (k->ms() - t0 > ms || ++n > ms * 4000u + 4000u) return -1;
    io_delay();
  }
}

/* ---------- comandos ao codec ---------- */
static int corb_cmd(uint32_t v, uint32_t* resp) {
  uint32_t wp = ((r16(CORBWP) & 0xFFu) + 1u) % s.corb_n, t0, n = 0;
  s.corb[wp] = v;
  barreira();
  w16(CORBWP, (uint16_t)wp);
  t0 = k->ms();
  for (;;) {
    uint32_t hw = r16(RIRBWP) & 0xFFu;
    while (s.rirb_rp != hw) {
      uint32_t lo, ex;
      s.rirb_rp = (s.rirb_rp + 1u) % s.rirb_n;
      lo = s.rirb[s.rirb_rp * 2u];
      ex = s.rirb[s.rirb_rp * 2u + 1u];
      if (ex & 0x10u) continue;      /* resposta nao solicitada */
      w8(RIRBSTS, 0x05);
      *resp = lo;
      return 0;
    }
    if (k->ms() - t0 > CMD_MS || ++n > 200000u) return -1;
  }
}

static int imm_cmd(uint32_t v, uint32_t* resp) {
  if (waitr(ICIS, 2, 1u, 0u, CMD_MS)) return -1;
  w16(ICIS, (uint16_t)(r16(ICIS) | 2u));   /* limpa o "resposta valida" */
  w32(ICOI, v);
  w16(ICIS, (uint16_t)((r16(ICIS) & ~2u) | 1u));
  if (waitr(ICIS, 2, 2u, 2u, CMD_MS)) return -1;
  *resp = r32(ICII);
  return 0;
}

static int cmd(uint32_t v, uint32_t* resp) {
  int rc = s.imm ? imm_cmd(v, resp) : corb_cmd(v, resp);
  if (rc) {
    *resp = 0;
    s.cmd_err++;
  }
  return rc;
}

/* verbo de 12 bits com 8 de dado; devolve a resposta (0 se nao veio) */
static uint32_t verb(int nid, uint32_t v, uint32_t data) {
  uint32_t r;
  (void)cmd((uint32_t)s.cad << 28 | (uint32_t)nid << 20 | v << 8 | (data & 0xFFu), &r);
  return r;
}
/* verbo de 4 bits com 16 de dado */
static uint32_t verb4(int nid, uint32_t v, uint32_t data) {
  uint32_t r;
  (void)cmd((uint32_t)s.cad << 28 | (uint32_t)nid << 20 | v << 16 | (data & 0xFFFFu), &r);
  return r;
}
static uint32_t param(int nid, uint32_t p) { return verb(nid, V_GET_PARAM, p); }

/* ---------- controlador ---------- */
static void pci_scan(void) {
  uint32_t bus, dev, fun;
  s.npc = 0;
  for (bus = 0; bus < 256u; bus++)
    for (dev = 0; dev < 32u; dev++) {
      uint32_t nf = 1;
      for (fun = 0; fun < nf; fun++) {
        uint32_t id = k->pci_read32(bus, dev, fun, 0), cls;
        struct pctrl* p;
        if (id == 0xFFFFFFFFu || (id & 0xFFFFu) == 0xFFFFu || id == 0) continue;
        if (fun == 0 && (k->pci_read32(bus, dev, 0, 0x0C) & 0x00800000u)) nf = 8;
        cls = k->pci_read32(bus, dev, fun, 0x08) >> 16;
        if (cls != 0x0403u || s.npc >= CTRL_MAX) continue;
        p = &s.pc[s.npc++];
        p->bus = (uint8_t)bus;
        p->dev = (uint8_t)dev;
        p->fun = (uint8_t)fun;
        p->ven = (uint16_t)(id & 0xFFFFu);
        p->did = (uint16_t)(id >> 16);
        /* HDMI: NVIDIA, ou ATI ao lado da GPU (funcao 0 = video). No Ryzen
         * o HDA analogico (1022:15E3) tambem fica no dispositivo da GPU. */
        p->video = p->ven == 0x10DEu ||
                   (p->ven == 0x1002u && fun && (k->pci_read32(bus, dev, 0, 0x08) >> 24) == 0x03u);
        p->tried = 0;
      }
    }
}

/* PCI PM: D3 -> D0. */
static void pci_d0(const struct pctrl* p) {
  uint32_t ptr, guard = 0;
  if (!(pci(p, 0x04) & (1u << 20))) return;
  ptr = pci(p, 0x34) & 0xFCu;
  while (ptr && guard++ < 48u) {
    uint32_t h = pci(p, ptr);
    if ((h & 0xFFu) == 1u) {
      uint32_t pm = pci(p, ptr + 4u);
      if (pm & 3u) {
        pciw(p, ptr + 4u, pm & ~3u);
        delay_ms(10);
        logs("placa estava em D");
        logx(pm & 3u);
        logs(", agora D0\n");
      }
      return;
    }
    ptr = (h >> 8) & 0xFCu;
  }
}

static uint32_t size_pick(uint8_t cap, uint32_t* n) {
  if (cap & 0x40u) {
    *n = 256;
    return 2;
  }
  if (cap & 0x20u) {
    *n = 16;
    return 1;
  }
  *n = 2;
  return 0;
}

static int corb_init(void) {
  uint64_t a;
  uint32_t code;
  if (!s.cmdmem) s.cmdmem = k->alloc(4096, 128);
  if (!s.cmdmem) return -1;
  s.corb = (volatile uint32_t*)s.cmdmem;
  s.rirb = (volatile uint32_t*)((uint8_t*)s.cmdmem + 2048);
  w8(CORBCTL, 0);
  (void)waitr(CORBCTL, 1, 2u, 0u, 10);
  code = size_pick(r8(CORBSIZE), &s.corb_n);
  w8(CORBSIZE, (uint8_t)((r8(CORBSIZE) & ~3u) | code));
  a = (uint64_t)(uintptr_t)s.corb;
  w32(CORBLBASE, (uint32_t)a);
  w32(CORBUBASE, (uint32_t)(a >> 32));
  w16(CORBWP, 0);
  w16(CORBRP, 0x8000u);
  (void)waitr(CORBRP, 2, 0x8000u, 0x8000u, 5);   /* na AMD o bit volta a 0 sozinho */
  w16(CORBRP, 0);
  if (waitr(CORBRP, 2, 0x8000u, 0u, 5)) logs("CORBRP nao zerou\n");
  w8(RIRBCTL, 0);
  (void)waitr(RIRBCTL, 1, 2u, 0u, 10);
  code = size_pick(r8(RIRBSIZE), &s.rirb_n);
  w8(RIRBSIZE, (uint8_t)((r8(RIRBSIZE) & ~3u) | code));
  a = (uint64_t)(uintptr_t)s.rirb;
  w32(RIRBLBASE, (uint32_t)a);
  w32(RIRBUBASE, (uint32_t)(a >> 32));
  w16(RIRBWP, 0x8000u);
  w16(RINTCNT, 1);
  s.rirb_rp = 0;
  w8(RIRBSTS, 0x05);
  w8(RIRBCTL, 0x02);                 /* DMA da RIRB, sem interrupcao */
  w8(CORBCTL, 0x02);
  (void)waitr(CORBCTL, 1, 2u, 2u, 10);
  s.imm = 0;
  return 0;
}

static void corb_off(void) {
  w8(CORBCTL, 0);
  w8(RIRBCTL, 0);
  (void)waitr(CORBCTL, 1, 2u, 0u, 10);
  (void)waitr(RIRBCTL, 1, 2u, 0u, 10);
}

static void streams_off(void) {
  uint32_t i, nsd = ((s.gcap >> 12) & 15u) + ((s.gcap >> 8) & 15u) + ((s.gcap >> 3) & 31u);
  for (i = 0; i < nsd && i < 30u; i++) {
    uint32_t o = SD_BASE + i * SD_LEN + SD_CTL;
    w8(o, (uint8_t)(r8(o) & ~0x1Eu));        /* RUN e interrupcoes */
  }
}

/* Para tudo e deixa o controlador em reset (link parado). */
static void ctrl_down(void) {
  if (!s.mm) return;
  streams_off();
  corb_off();
  w32(GCTL, r32(GCTL) & ~1u);
  (void)waitr(GCTL, 4, 1u, 0u, 50);
  s.mm = 0;
}

static int ctrl_up(struct pctrl* p) {
  uint32_t bar0, bar1, c;
  uint64_t base;
  p->tried = 1;
  bar0 = pci(p, 0x10);
  bar1 = ((bar0 & 6u) == 4u) ? pci(p, 0x14) : 0;
  base = ((uint64_t)bar1 << 32) | (bar0 & ~0xFu);
  if (!base || (bar0 & 1u)) {
    logs("BAR0 invalido\n");
    return -1;
  }
  pci_d0(p);
  c = pci(p, 0x04);
  pciw(p, 0x04, (c & 0xFFFFu) | 0x0406u);    /* memoria, bus master, sem INTx */
  if (p->ven == 0x1022u || (p->ven == 0x1002u && !p->video)) {
    /* snoop (ATI_SB450_HDAUDIO_MISC_CNTR2, byte 0x42) */
    uint32_t v = pci(p, 0x40), b = (v >> 16) & 0xFFu;
    b = (b & ~7u) | 2u;
    pciw(p, 0x40, (v & ~0x00FF0000u) | b << 16);
  } else if (p->ven == 0x8086u) {
    pciw(p, 0x44, pci(p, 0x44) & ~7u);       /* TCSEL = 0 */
  }
  if (k->mmio_map(base, 0x4000u) != 0) {
    logs("BAR0 fora do alcance\n");
    return -1;
  }
  s.mm = (volatile uint8_t*)(uintptr_t)base;
  s.gcap = r16(GCAP);
  w32(INTCTL, 0);
  streams_off();
  corb_off();
  delay_ms(1);
  w32(GCTL, r32(GCTL) & ~1u);
  if (waitr(GCTL, 4, 1u, 0u, 100)) logs("CRST nao desceu\n");
  delay_ms(1);
  w32(GCTL, r32(GCTL) | 1u);
  if (waitr(GCTL, 4, 1u, 1u, 100)) {
    logs("controlador nao saiu do reset\n");
    s.mm = 0;
    return -1;
  }
  delay_ms(20);                      /* codecs se anunciam em STATESTS */
  s.gcap = r16(GCAP);
  s.iss = (int)((s.gcap >> 8) & 15u);
  s.oss = (int)((s.gcap >> 12) & 15u);
  return corb_init();
}

/* ---------- codec ---------- */
static uint32_t wtype(uint32_t caps) { return (caps >> 20) & 15u; }

static void conn_read(int nid) {
  struct node* d = &s.nd[nid];
  uint32_t len = param(nid, P_CONN_LEN), r = 0, i, cnt, per, bits, mask, range;
  int prev = -1;
  cnt = len & 0x7Fu;
  per = (len & 0x80u) ? 2u : 4u;
  bits = (len & 0x80u) ? 16u : 8u;
  mask = (len & 0x80u) ? 0x7FFFu : 0x7Fu;
  range = (len & 0x80u) ? 0x8000u : 0x80u;
  d->nconn = 0;
  for (i = 0; i < cnt && i < 64u; i++) {
    uint32_t e;
    if (i % per == 0) r = verb(nid, V_GET_CONN_LIST, i);
    e = (r >> ((i % per) * bits)) & ((1u << bits) - 1u);
    if ((e & range) && prev >= 0) {
      int x;
      for (x = prev + 1; x <= (int)(e & mask) && d->nconn < 16; x++) d->conn[d->nconn++] = (uint8_t)x;
    } else if (d->nconn < 16) {
      d->conn[d->nconn++] = (uint8_t)(e & mask);
    }
    prev = (int)(e & mask);
  }
}

static void node_read(int nid) {
  struct node* d = &s.nd[nid];
  d->caps = param(nid, P_WCAPS);
  if (wtype(d->caps) == W_PIN) {
    d->pincap = param(nid, P_PINCAP);
    d->cfg = verb(nid, V_GET_CONFIG, 0);
  }
  if (d->caps & WC_OUT_AMP) d->amp_out = (d->caps & WC_AMP_OVR) ? param(nid, P_AMP_OUT) : s.afg_amp_out;
  if (d->caps & WC_IN_AMP) d->amp_in = (d->caps & WC_AMP_OVR) ? param(nid, P_AMP_IN) : s.afg_amp_in;
  if (d->caps & WC_CONN) conn_read(nid);
}

static int in_codec(int nid) { return nid >= s.wstart && nid < s.wstart + s.wn; }

/* Do pino ate um DAC analogico, passando so por mixers e seletores.
 * Prefere ligar direto num DAC a dar a volta. */
static int path_find(struct out* o, int nid, int depth) {
  struct node* d;
  uint32_t t;
  int i, pass;
  if (!in_codec(nid) || depth >= HOPS) return 0;
  d = &s.nd[nid];
  t = wtype(d->caps);
  for (i = 0; i < depth; i++)
    if (o->path[i] == nid) return 0;
  o->path[depth] = (uint8_t)nid;
  if (t == W_OUT) {
    if (d->caps & WC_DIGITAL) return 0;     /* S/PDIF */
    o->n = (uint8_t)(depth + 1);
    return 1;
  }
  if (depth > 0 && t != W_MIX && t != W_SEL) return 0;
  for (pass = 0; pass < 2; pass++)
    for (i = 0; i < d->nconn; i++) {
      int c = d->conn[i], dac = in_codec(c) && wtype(s.nd[c].caps) == W_OUT;
      if (dac != (pass == 0)) continue;
      o->sel[depth] = (uint8_t)i;
      if (path_find(o, c, depth + 1)) return 1;
    }
  return 0;
}

/* Saidas na ordem alto-falante, fone, linha, "outro". */
static int find_outputs(void) {
  static const uint8_t order[4] = { DEV_SPK, DEV_HP, DEV_LINE, DEV_OTHER };
  int pass, nid;
  s.no = 0;
  for (pass = 0; pass < 4; pass++)
    for (nid = s.wstart; nid < s.wstart + s.wn && s.no < OUT_MAX; nid++) {
      struct node* d = &s.nd[nid];
      struct out* o = &s.o[s.no];
      uint32_t dev = (d->cfg >> 20) & 15u, i;
      if (wtype(d->caps) != W_PIN || !(d->pincap & PC_OUT) || (d->caps & WC_DIGITAL)) continue;
      if ((d->cfg >> 30) == 1u || dev != order[pass]) continue;   /* 1 = nada ligado */
      for (i = 0; i < sizeof(o->path); i++) o->path[i] = o->sel[i] = 0;
      o->pin = (uint8_t)nid;
      o->dev = (uint8_t)dev;
      o->vnid = 0;
      o->present = 0;
      o->jack = (d->pincap & PC_PRESENCE) && (d->cfg >> 30) != 2u;  /* 2 = fixo (interno) */
      if (!path_find(o, nid, 0)) continue;
      /* volume: o primeiro amplificador de saida com passos, a partir do DAC */
      for (i = o->n; i-- > 0;) {
        struct node* v = &s.nd[o->path[i]];
        if ((v->caps & WC_OUT_AMP) && ((v->amp_out >> 8) & 0x7Fu)) {
          o->vnid = o->path[i];
          break;
        }
      }
      s.no++;
    }
  return s.no;
}

static int codec_scan(int cad) {
  uint32_t sub, fg, nfg, i;
  int nid;
  s.cad = cad;
  s.cmd_err = 0;
  s.vendor = param(0, P_VENDOR);
  if (!s.vendor || s.vendor == 0xFFFFFFFFu) return 0;
  s.rev = param(0, P_REV);
  sub = param(0, P_NODES);
  fg = (sub >> 16) & 0xFFu;
  nfg = sub & 0xFFu;
  s.afg = 0;
  for (i = 0; i < nfg && i < 8u; i++)
    if ((param((int)(fg + i), P_FG_TYPE) & 0xFFu) == 1u) {
      s.afg = (int)(fg + i);
      break;
    }
  logs("codec ");
  logx((uint32_t)cad);
  logs(": ");
  logx(s.vendor);
  if (!s.afg) {
    logs(" sem grupo de audio\n");
    return 0;
  }
  verb(s.afg, V_SET_POWER, 0);       /* D0 */
  {
    uint32_t t0 = k->ms();
    while (((verb(s.afg, V_GET_POWER, 0) >> 4) & 15u) != 0 && k->ms() - t0 < 100u) delay_ms(1);
  }
  sub = param(s.afg, P_NODES);
  s.wstart = (int)((sub >> 16) & 0xFFu);
  s.wn = (int)(sub & 0xFFu);
  if (s.wstart >= NODE_MAX) s.wn = 0;
  else if (s.wstart + s.wn > NODE_MAX) s.wn = NODE_MAX - s.wstart;
  s.afg_pcm = param(s.afg, P_PCM);
  s.afg_amp_out = param(s.afg, P_AMP_OUT);
  s.afg_amp_in = param(s.afg, P_AMP_IN);
  for (nid = 0; nid < NODE_MAX; nid++) {
    struct node* d = &s.nd[nid];
    d->caps = d->pincap = d->cfg = d->amp_out = d->amp_in = 0;
    d->nconn = 0;
  }
  for (nid = s.wstart; nid < s.wstart + s.wn; nid++) node_read(nid);
  find_outputs();
  logs(", ");
  logx((uint32_t)s.wn);
  logs(" nos, saidas analogicas: ");
  logx((uint32_t)s.no);
  logs("\n");
  return s.no;
}

/* ---------- caminho, volume, fone ---------- */
static void amp_set(int nid, int output, int idx, int mute, uint32_t gain) {
  uint32_t p = (output ? 0x8000u : 0x4000u) | 0x3000u | ((uint32_t)(idx & 15) << 8) | (mute ? 0x80u : 0u) |
               (gain & 0x7Fu);
  verb4(nid, V4_SET_AMP, p);
}

/* 0 dB (offset); amplificadores so de atenuacao tem offset = passos. */
static uint32_t amp_0db(uint32_t caps) { return caps & 0x7Fu; }

static void vol_apply(void) {
  int i;
  for (i = 0; i < s.no; i++) {
    struct out* o = &s.o[i];
    uint32_t caps, steps, top, g;
    if (!o->vnid) continue;
    caps = s.nd[o->vnid].amp_out;
    steps = (caps >> 8) & 0x7Fu;
    top = amp_0db(caps) ? amp_0db(caps) : steps;   /* nunca acima de 0 dB */
    g = top * (uint32_t)s.vol / 100u;
    amp_set(o->vnid, 1, 0, s.vol == 0, g);
  }
}

static void path_on(struct out* o) {
  int i;
  for (i = 0; i < o->n; i++) {
    int nid = o->path[i];
    struct node* d = &s.nd[nid];
    uint32_t t = wtype(d->caps);
    if (d->caps & WC_POWER) verb(nid, V_SET_POWER, 0);
    if (i < o->n - 1) {
      if (t == W_MIX) {
        if (d->caps & WC_IN_AMP) {
          int x;
          for (x = 0; x < d->nconn && x < 16; x++)
            amp_set(nid, 0, x, x != o->sel[i], amp_0db(d->amp_in));
        }
      } else {
        if (d->nconn > 1) verb(nid, V_SET_CONN_SEL, o->sel[i]);
        if (t == W_SEL && (d->caps & WC_IN_AMP)) amp_set(nid, 0, o->sel[i], 0, amp_0db(d->amp_in));
      }
    }
    if ((d->caps & WC_OUT_AMP) && nid != o->vnid) amp_set(nid, 1, 0, 0, amp_0db(d->amp_out));
  }
  if (s.nd[o->pin].pincap & PC_EAPD) verb(o->pin, V_SET_EAPD, 0x02);   /* amplificador externo */
}

/* Fone ligado: o alto-falante cala. force = aplica mesmo sem mudanca. */
static void jack_apply(int force) {
  int i, hp = 0, changed = force;
  for (i = 0; i < s.no; i++) {
    struct out* o = &s.o[i];
    if (o->dev != DEV_HP || !o->jack) continue;
    {
      uint32_t p;
      if (s.nd[o->pin].pincap & PC_TRIGGER) verb(o->pin, V_SET_PIN_SENSE, 0);
      p = (verb(o->pin, V_GET_PIN_SENSE, 0) >> 31) & 1u;
      if (p != o->present) changed = 1;
      o->present = (uint8_t)p;
      if (p) hp = 1;
    }
  }
  if (!changed) return;
  for (i = 0; i < s.no; i++) {
    struct out* o = &s.o[i];
    uint32_t ctl = 0x40u;                          /* saida ligada */
    if (o->dev == DEV_HP && (s.nd[o->pin].pincap & PC_HP_DRIVE)) ctl |= 0x80u;
    if (hp && o->dev == DEV_SPK) ctl = 0;
    verb(o->pin, V_SET_PIN_CTL, ctl);
  }
}

/* ---------- diagnostico ---------- */
#define V_GET_CONN_SEL 0xF01u
#define V_GET_PIN_CTL 0xF07u
#define V_GET_EAPD 0xF0Cu
#define V_GET_GPIO_DATA 0xF15u
#define V_GET_GPIO_MASK 0xF16u
#define V_GET_GPIO_DIR 0xF17u
#define V_GET_SUBSYS 0xF20u
#define V4_GET_AMP 0xBu
#define P_GPIO 0x11u

static void line_out(struct sb* b) {
  b->p[b->n] = 0;
  logs(b->p);
  b->n = 0;
}

/* GET_AMP: out = saida/entrada, esquerda e direita do indice idx. */
static void sb_amp(struct sb* b, int nid, int out, int idx) {
  uint32_t l = verb4(nid, V4_GET_AMP, (out ? 0x8000u : 0) | 0x2000u | (uint32_t)idx);
  uint32_t r = verb4(nid, V4_GET_AMP, (out ? 0x8000u : 0) | (uint32_t)idx);
  sb_x(b, l & 0xFFu, 2);
  sb_c(b, '/');
  sb_x(b, r & 0xFFu, 2);
}

/* Todos os nos do codec na serial (e no log do AUDIO): para comparar com o
 * codec#0 do Linux quando o som nao sai. */
static void dump(const char* quando) {
  char t[200];
  struct sb b;
  int nid, i;
  const struct pctrl* p = &s.pc[s.ci < 0 ? 0 : s.ci];
  b.p = t;
  b.n = 0;
  b.max = (int)sizeof(t);
  sb_s(&b, "--- codec (");
  sb_s(&b, quando);
  sb_s(&b, ") pci subsys ");
  sb_x(&b, pci(p, 0x2C), 8);
  sb_s(&b, " codec subsys ");
  sb_x(&b, verb(s.afg, V_GET_SUBSYS, 0), 8);
  sb_s(&b, " gpio ");
  sb_x(&b, param(s.afg, P_GPIO), 8);
  sb_s(&b, " dados/mask/dir ");
  sb_x(&b, verb(s.afg, V_GET_GPIO_DATA, 0), 2);
  sb_c(&b, '/');
  sb_x(&b, verb(s.afg, V_GET_GPIO_MASK, 0), 2);
  sb_c(&b, '/');
  sb_x(&b, verb(s.afg, V_GET_GPIO_DIR, 0), 2);
  sb_s(&b, " afg pw ");
  sb_x(&b, verb(s.afg, V_GET_POWER, 0), 2);
  sb_c(&b, '\n');
  line_out(&b);
  for (nid = s.wstart; nid < s.wstart + s.wn; nid++) {
    struct node* d = &s.nd[nid];
    uint32_t t2 = wtype(d->caps);
    sb_s(&b, "no ");
    sb_x(&b, (uint32_t)nid, 2);
    sb_s(&b, " t");
    sb_x(&b, t2, 1);
    sb_s(&b, " caps ");
    sb_x(&b, d->caps, 6);
    if (d->caps & WC_POWER) {
      sb_s(&b, " pw ");
      sb_x(&b, verb(nid, V_GET_POWER, 0), 2);
    }
    if (t2 == W_PIN) {
      sb_s(&b, " pincap ");
      sb_x(&b, d->pincap, 8);
      sb_s(&b, " cfg ");
      sb_x(&b, d->cfg, 8);
      sb_s(&b, " ctl ");
      sb_x(&b, verb(nid, V_GET_PIN_CTL, 0), 2);
      if (d->pincap & PC_EAPD) {
        sb_s(&b, " eapd ");
        sb_x(&b, verb(nid, V_GET_EAPD, 0), 2);
      }
      if (d->pincap & PC_PRESENCE) {
        sb_s(&b, " sense ");
        sb_x(&b, verb(nid, V_GET_PIN_SENSE, 0), 8);
      }
    }
    if (t2 == W_OUT) {
      sb_s(&b, " fmt ");
      sb_x(&b, verb(nid, 0xA00u, 0) & 0xFFFFu, 4);   /* GET_CONVERTER_FORMAT */
      sb_s(&b, " fluxo ");
      sb_x(&b, verb(nid, 0xF06u, 0) & 0xFFu, 2);
    }
    if (d->caps & WC_OUT_AMP) {
      sb_s(&b, " aout ");
      sb_x(&b, d->amp_out, 8);
      sb_c(&b, '=');
      sb_amp(&b, nid, 1, 0);
    }
    if (d->caps & WC_IN_AMP) {
      sb_s(&b, " ain ");
      sb_x(&b, d->amp_in, 8);
      sb_c(&b, '=');
      for (i = 0; i < d->nconn && i < 4; i++) {
        if (i) sb_c(&b, ',');
        sb_amp(&b, nid, 0, i);
      }
    }
    if (d->nconn) {
      sb_s(&b, " conn");
      for (i = 0; i < d->nconn; i++) {
        sb_c(&b, ' ');
        sb_x(&b, d->conn[i], 2);
      }
      if (d->nconn > 1 && t2 != W_MIX) {
        sb_s(&b, " sel ");
        sb_x(&b, verb(nid, V_GET_CONN_SEL, 0) & 0xFFu, 2);
      }
    }
    sb_c(&b, '\n');
    line_out(&b);
  }
}

/* ---------- formato ---------- */
static const struct {
  uint32_t hz;
  uint16_t fmt;                      /* BASE, MULT, DIV */
  uint8_t bit;                       /* bit no parametro PCM */
} rates[] = {
  { 8000u, 5u << 8, 0 },
  { 11025u, 1u << 14 | 3u << 8, 1 },
  { 16000u, 2u << 8, 2 },
  { 22050u, 1u << 14 | 1u << 8, 3 },
  { 32000u, 1u << 11 | 2u << 8, 4 },
  { 44100u, 1u << 14, 5 },
  { 48000u, 0, 6 },
  { 88200u, 1u << 14 | 1u << 11, 7 },
  { 96000u, 1u << 11, 8 },
  { 176400u, 1u << 14 | 3u << 11, 9 },
  { 192000u, 3u << 11, 10 },
};
#define NRATES ((int)(sizeof(rates) / sizeof(rates[0])))

static int dac_used(int nid, int upto) {
  int i;
  for (i = 0; i < upto; i++)
    if (s.o[i].path[s.o[i].n - 1] == nid) return 1;
  return 0;
}

static void pcm_caps(void) {
  int i;
  s.pcm = 0xFFFFFFFFu;
  for (i = 0; i < s.no; i++) {
    int dac = s.o[i].path[s.o[i].n - 1];
    uint32_t p = param(dac, P_PCM);
    if (!p) p = s.afg_pcm;
    s.pcm &= p;
  }
  s.pcm |= 1u << 6;                  /* 48 kHz: o padrao exige */
}

/* ---------- fluxo ---------- */
static void sd_stop(void) {
  if (!s.sd) return;
  s.sd[SD_CTL] = (uint8_t)(s.sd[SD_CTL] & ~0x1Eu);
  {
    uint32_t t0 = k->ms(), n = 0;
    while ((s.sd[SD_CTL] & 2u) && k->ms() - t0 < 10u && n++ < 40000u) io_delay();
  }
}

/* Reset do descritor e registradores; nao liga. */
static void sd_program(void) {
  uint64_t a = (uint64_t)(uintptr_t)s.bdl;
  uint32_t t0, n = 0;
  sd_stop();
  s.sd[SD_CTL] = (uint8_t)(s.sd[SD_CTL] | 1u);
  t0 = k->ms();
  while (!(s.sd[SD_CTL] & 1u) && k->ms() - t0 < 10u && n++ < 40000u) io_delay();
  s.sd[SD_CTL] = (uint8_t)(s.sd[SD_CTL] & ~1u);
  t0 = k->ms();
  n = 0;
  while ((s.sd[SD_CTL] & 1u) && k->ms() - t0 < 10u && n++ < 40000u) io_delay();
  s.sd[SD_CTL2] = (uint8_t)(TAG << 4);
  *(volatile uint32_t*)(s.sd + SD_CBL) = RING;
  *(volatile uint16_t*)(s.sd + SD_LVI) = (uint16_t)(BDL_N - 1);
  *(volatile uint16_t*)(s.sd + SD_FMT) = s.fmt;
  *(volatile uint32_t*)(s.sd + SD_BDPL) = (uint32_t)a;
  *(volatile uint32_t*)(s.sd + SD_BDPU) = (uint32_t)(a >> 32);
  s.sd[SD_STS] = 0x1C;               /* limpa (escreve 1) */
}

static void ring_zero(uint64_t pos, uint64_t len) {
  uint32_t off, part, i;
  if (len > RING) len = RING;
  off = (uint32_t)(pos % RING);
  while (len) {
    part = RING - off;
    if (part > len) part = (uint32_t)len;
    for (i = 0; i < part; i++) s.ring[off + i] = 0;
    len -= part;
    off = 0;
  }
}

static void pos_update(void) {
  uint32_t lpib = *(volatile uint32_t*)(s.sd + SD_LPIB), now = k->ms(), d, ex;
  if (lpib >= RING) return;          /* leitura fora do buffer: ignora */
  d = (lpib + RING - s.last_lpib) % RING;
  /* parado mais de meia volta (IRQ0 sem rodar): as voltas pelo relogio */
  ex = (uint32_t)((uint64_t)(now - s.last_ms) * s.bps / 1000u);
  if (ex > RING / 2u + d) s.pl += (uint64_t)((ex - d + RING / 2u) / RING) * RING;
  s.pl += d;
  s.last_lpib = lpib;
  s.last_ms = now;
}

/* ---------- operacoes ---------- */
static void a_fini(void);

static int a_start(uint32_t rate) {
  int i, r = -1;
  for (i = 0; i < NRATES; i++)
    if (rates[i].hz == rate && (s.pcm & (1u << rates[i].bit))) r = i;
  if (r < 0 || !s.sd) return -1;
  sd_stop();
  s.running = 0;
  s.fmt = (uint16_t)(rates[r].fmt | 1u << 4 | 1u);   /* 16 bits, 2 canais */
  s.rate = rate;
  s.bps = rate * 4u;
  s.maxq = s.bps / 5u;                               /* 200 ms */
  if (s.maxq < 16384u) s.maxq = 16384u;
  if (s.maxq > RING - 16384u) s.maxq = RING - 16384u;
  s.maxq &= ~3u;
  for (i = 0; i < s.no; i++) {
    int dac = s.o[i].path[s.o[i].n - 1];
    path_on(&s.o[i]);
    if (!dac_used(dac, i)) {
      verb4(dac, V4_SET_FMT, s.fmt);
      verb(dac, V_SET_STREAM, TAG << 4);
    }
  }
  vol_apply();
  jack_apply(1);
  s.jack_ms = k->ms();
  if (!s.dumped) {
    s.dumped = 1;
    dump("primeiro som");
  }
  sd_program();
  s.lead = ((uint32_t)*(volatile uint16_t*)(s.sd + SD_FIFOS) + 1u + s.bps / 500u + 3u) & ~3u;
  if (s.lead > 8192u) s.lead = 8192u;
  s.started = 1;
  return 0;
}

static void ring_copy(const uint8_t* src, uint32_t n) {
  uint32_t off = (uint32_t)(s.wr % RING), part, i;
  while (n) {
    part = RING - off;
    if (part > n) part = n;
    for (i = 0; i < part; i++) s.ring[off + i] = src[i];
    src += part;
    n -= part;
    s.wr += part;
    off = 0;
  }
}

static int a_write(const void* buf, int bytes) {
  uint32_t n;
  if (!s.started || !buf || bytes < 0) return -1;
  n = (uint32_t)bytes & ~3u;
  if (!s.running) {
    /* comeca do zero: buffer limpo, posicao 0, enche e liga */
    ring_zero(0, RING);
    s.wr = s.pl = s.zr = 0;
    s.last_lpib = 0;
    if (n > s.maxq) n = s.maxq;
    ring_copy((const uint8_t*)buf, n);
    sd_program();
    barreira();
    s.last_ms = k->ms();
    s.idle_since = 0;
    s.sd[SD_CTL] = (uint8_t)(s.sd[SD_CTL] | 2u);   /* RUN */
    s.running = 1;
    return (int)n;
  }
  pos_update();
  if (s.wr < s.pl + s.lead) s.wr = (s.pl + s.lead + 3u) & ~(uint64_t)3u;   /* faltou som */
  if (s.wr >= s.pl + s.maxq) return 0;
  if (n > s.pl + s.maxq - s.wr) n = (uint32_t)(s.pl + s.maxq - s.wr) & ~3u;
  ring_copy((const uint8_t*)buf, n);
  barreira();
  s.idle_since = 0;
  return (int)n;
}

static int a_pending(void) {
  if (!s.running) return 0;
  pos_update();
  return s.wr > s.pl ? (int)(s.wr - s.pl) : 0;
}

static int a_service(void) {
  uint64_t start, end;
  uint32_t now;
  if (!s.running) return 0;
  pos_update();
  /* o que ja tocou vira silencio: se ninguem escrever, a placa toca zeros */
  end = s.pl > ZMARGIN ? s.pl - ZMARGIN : 0;
  start = s.zr;
  if (s.wr > RING && start < s.wr - RING) start = s.wr - RING;   /* nao apaga o que vai tocar */
  if (end > start) ring_zero(start, end - start);
  if (end > s.zr) s.zr = end;
  now = k->ms();
  if (s.pl >= s.wr) {
    if (!s.idle_since) {
      s.idle_since = now | 1u;
    } else if (now - s.idle_since > IDLE_MS) {
      sd_stop();
      s.running = 0;
      return 0;
    }
  } else {
    s.idle_since = 0;
  }
  if (now - s.jack_ms > JACK_MS && s.cmd_err < 4) {
    s.jack_ms = now;
    jack_apply(0);
  }
  return 1;
}

static void a_stop(void) {
  sd_stop();
  s.running = 0;
}

/* Diagnostico (HDAVERB.EXE): k_audio_volume(-2 - v) manda o verbo cru v
 * (no << 20 | verbo << 8 | dado, ou no << 20 | verbo4 << 16 | dado16) ao
 * codec em uso e devolve a resposta. -1 continua sendo "so le o volume". */
static int a_volume(int pct) {
  if (pct <= -2) {
    uint32_t v = (uint32_t)(-(pct + 2)), r;
    if (!s.mm || v > 0x0FFFFFFFu) return -1;
    if (cmd((uint32_t)s.cad << 28 | v, &r) != 0) return -1;
    return (int)r;
  }
  if (pct >= 0) {
    s.vol = pct > 100 ? 100 : pct;
    if (s.mm) vol_apply();
  }
  return s.vol;
}

static const char* vendor_name(uint32_t v) {
  switch (v >> 16) {
  case 0x10ECu: return "Realtek";
  case 0x14F1u: return "Conexant";
  case 0x111Du: return "IDT";
  case 0x8384u: return "SigmaTel";
  case 0x1106u: return "VIA";
  case 0x11D4u: return "Analog Devices";
  case 0x13F6u:
  case 0x434Du: return "C-Media";
  case 0x1AF4u: return "QEMU";
  case 0x8086u: return "Intel";
  case 0x1002u: return "AMD";
  case 0x10DEu: return "NVIDIA";
  case 0x1013u: return "Cirrus Logic";
  case 0x1057u: return "Motorola";
  default: return "codec";
  }
}

static const char* dev_name(uint32_t dev) {
  switch (dev) {
  case DEV_SPK: return "alto-falante";
  case DEV_HP: return "fone";
  case DEV_LINE: return "linha";
  default: return "saida";
  }
}

static void sb_pci(struct sb* b, const struct pctrl* p) {
  sb_x(b, p->ven, 4);
  sb_c(b, ':');
  sb_x(b, p->did, 4);
  sb_s(b, " em ");
  sb_x(b, p->bus, 2);
  sb_c(b, ':');
  sb_x(b, p->dev, 2);
  sb_c(b, '.');
  sb_x(b, p->fun, 1);
}

static int a_info(char* out, int max) {
  struct sb b;
  int i, j;
  b.p = out;
  b.n = 0;
  b.max = max;
  if (s.ci < 0 || !s.mm) {
    sb_s(&b, "Nenhuma placa em uso.\n");
    out[b.n] = 0;
    return b.n;
  }
  sb_s(&b, "Placa HDA ");
  sb_pci(&b, &s.pc[s.ci]);
  sb_s(&b, ", HDA ");
  sb_d(&b, r8(VMAJ));
  sb_c(&b, '.');
  sb_d(&b, r8(VMIN));
  sb_s(&b, s.imm ? ", comandos imediatos\n" : ", CORB/RIRB\n");
  sb_s(&b, "Codec ");
  sb_d(&b, (uint32_t)s.cad);
  sb_s(&b, ": ");
  sb_s(&b, vendor_name(s.vendor));
  sb_c(&b, ' ');
  sb_x(&b, s.vendor >> 16, 4);
  sb_c(&b, ':');
  sb_x(&b, s.vendor & 0xFFFFu, 4);
  sb_s(&b, ", grupo de audio no ");
  sb_x(&b, (uint32_t)s.afg, 2);
  sb_c(&b, '\n');
  for (i = 0; i < s.no; i++) {
    struct out* o = &s.o[i];
    sb_s(&b, "  ");
    sb_s(&b, dev_name(o->dev));
    sb_s(&b, " no ");
    for (j = 0; j < o->n; j++) {
      if (j) sb_s(&b, j == o->n - 1 ? " <- DAC " : " <- ");
      sb_x(&b, o->path[j], 2);
    }
    if (o->dev == DEV_HP && o->jack) sb_s(&b, o->present ? " (fone ligado)" : " (sem fone)");
    if (!o->vnid) sb_s(&b, " (sem volume na placa)");
    sb_c(&b, '\n');
  }
  sb_s(&b, "Taxas:");
  for (i = 0; i < NRATES; i++)
    if (s.pcm & (1u << rates[i].bit)) {
      sb_c(&b, ' ');
      sb_d(&b, rates[i].hz);
    }
  sb_s(&b, " Hz\nVolume ");
  sb_d(&b, (uint32_t)s.vol);
  sb_s(&b, "%\n");
  if (s.running) {
    uint32_t p = s.wr > s.pl ? (uint32_t)(s.wr - s.pl) : 0;
    sb_s(&b, "Tocando ");
    sb_d(&b, s.rate);
    sb_s(&b, " Hz, fila ");
    sb_d(&b, p * 1000u / s.bps);
    sb_s(&b, " ms\n");
  } else {
    sb_s(&b, "Parada (DMA desligado)\n");
  }
  if (s.cmd_err) {
    sb_s(&b, "Comandos ao codec sem resposta: ");
    sb_d(&b, (uint32_t)s.cmd_err);
    sb_c(&b, '\n');
  }
  for (i = 0; i < s.npc; i++) {
    if (i == s.ci) continue;
    sb_s(&b, "Outra placa HDA ");
    sb_pci(&b, &s.pc[i]);
    sb_s(&b, s.pc[i].video ? " (HDMI da placa de video)" : "");
    sb_s(&b, s.pc[i].tried ? ": sem saida analogica\n" : ": nao usada\n");
  }
  out[b.n] = 0;
  return b.n;
}

/* GPIOs do FCH da AMD (0xFED81500, um registrador de 32 bits por pino: bit 16
 * nivel lido, bit 22 valor de saida, bit 23 saida ligada), so para o log. No
 * notebook com Conexant 2782:12C2 o alto-falante so toca depois de um boot no
 * Linux (reboot quente); comparar estes valores num boot frio e num boot vindo
 * do Linux mostra se o amplificador e ligado por um GPIO do chipset. So le. */
#define FCH_GPIO 0xFED81500u
#define FCH_GPIO_N 256u

static void fch_gpio_dump(void) {
  volatile uint32_t* g = (volatile uint32_t*)(uintptr_t)FCH_GPIO;
  char t[120];
  struct sb b;
  uint32_t i;
  if (k->mmio_map(FCH_GPIO & ~0xFFFu, 0x1000u) != 0) {
    logs("gpio AMD fora do alcance\n");
    return;
  }
  b.p = t;
  b.n = 0;
  b.max = (int)sizeof(t);
  logs("--- gpio AMD (FED81500)\n");
  for (i = 0; i < FCH_GPIO_N; i++) {
    if (i % 8u == 0) {
      sb_s(&b, "gpio ");
      sb_x(&b, i, 2);
      sb_c(&b, ':');
    }
    sb_c(&b, ' ');
    sb_x(&b, g[i], 8);
    if (i % 8u == 7u) {
      sb_c(&b, '\n');
      line_out(&b);
    }
  }
}

/* ---------- ACP da AMD (Renoir/Cezanne, 1022:15E2 rev 01) ----------
 * O coprocessador de audio fica ao lado do HDA (04:00.5 no notebook, o HDA
 * em 04:00.6) e o bloco sempre ligado dele tem o AZ_DEVICE_STATE (AZ = o
 * controlador HDA), a configuracao dos pinos e o seletor de clock de audio.
 * Num boot frio o ACP chega sem energia (PGFSM) e com a memoria desligada no
 * PCI; o Linux, no boot, liga e reseta o ACP (snd_rn_pci_acp3x, rn_acp_init)
 * e ao desistir dele (sem microfone digital) so reseta, sem desligar: depois
 * de um boot no Linux o alto-falante tocava no SODOS ate a energia ser
 * cortada. Aqui: a mesma sequencia (init + deinit do Linux, sem interrupcao).
 * Deslocamentos: rn_chip_offset_byte.h menos ACP_PHY_BASE_ADDRESS. */
#define ACP_SOFT_RESET 0x1000u
#define ACP_CONTROL 0x1004u
#define ACP_PGFSM_CONTROL 0x101Cu
#define ACP_PGFSM_STATUS 0x1020u
#define ACP_CLKMUX_SEL 0x1024u
#define ACP_PME_EN 0x1400u
#define ACP_DEVICE_STATE 0x1404u
#define AZ_DEVICE_STATE 0x1408u
#define ACP_PIN_CONFIG 0x1440u

static volatile uint32_t* acp_mm;

static uint32_t acp_rd(uint32_t o) { return acp_mm[o / 4u]; }
static void acp_wr(uint32_t o, uint32_t v) { acp_mm[o / 4u] = v; }

static void acp_log(const char* quando) {
  char t[200];
  struct sb b;
  b.p = t;
  b.n = 0;
  b.max = (int)sizeof(t);
  sb_s(&b, "acp ");
  sb_s(&b, quando);
  sb_s(&b, ": pgfsm ");
  sb_x(&b, acp_rd(ACP_PGFSM_STATUS), 8);
  sb_s(&b, " ctl ");
  sb_x(&b, acp_rd(ACP_CONTROL), 8);
  sb_s(&b, " reset ");
  sb_x(&b, acp_rd(ACP_SOFT_RESET), 8);
  sb_s(&b, " clkmux ");
  sb_x(&b, acp_rd(ACP_CLKMUX_SEL), 8);
  sb_s(&b, " pme ");
  sb_x(&b, acp_rd(ACP_PME_EN), 8);
  sb_s(&b, " dev ");
  sb_x(&b, acp_rd(ACP_DEVICE_STATE), 8);
  sb_s(&b, " az ");
  sb_x(&b, acp_rd(AZ_DEVICE_STATE), 8);
  sb_s(&b, " pinos ");
  sb_x(&b, acp_rd(ACP_PIN_CONFIG), 8);
  sb_c(&b, '\n');
  line_out(&b);
}

/* (registro & mask) == val no ACP, ate ~ms. */
static int acp_wait(uint32_t off, uint32_t mask, uint32_t val, uint32_t ms) {
  uint32_t t0 = k->ms(), n = 0;
  while ((acp_rd(off) & mask) != val) {
    if (k->ms() - t0 > ms || ++n > ms * 4000u + 4000u) return -1;
    io_delay();
  }
  return 0;
}

static void acp_reset(void) {
  acp_wr(ACP_SOFT_RESET, 1);
  if (acp_wait(ACP_SOFT_RESET, 0x00010001u, 0x00010001u, 10)) logs("acp: reset nao completou\n");
  acp_wr(ACP_SOFT_RESET, 0);
  if (acp_wait(ACP_SOFT_RESET, 0xFFFFFFFFu, 0, 10)) logs("acp: reset nao soltou\n");
}

/* Acha o ACP na mesma posicao PCI de um HDA da AMD (funcao 5) e liga. */
static void acp_on(void) {
  int c;
  for (c = 0; c < s.npc; c++) {
    struct pctrl a;
    uint32_t id, rev, bar, cmdr, st;
    if (s.pc[c].ven != 0x1022u) continue;
    a = s.pc[c];
    a.fun = 5;
    id = pci(&a, 0);
    rev = pci(&a, 0x08) & 0xFFu;
    if (id != 0x15E21022u || rev != 1u) continue;
    bar = pci(&a, 0x10);
    if (!(bar & ~0xFu) || (bar & 1u)) continue;
    cmdr = pci(&a, 0x04);
    pciw(&a, 0x04, (cmdr & 0xFFFFu) | 0x2u);         /* memoria (o Linux: 0000 -> 0002) */
    if (k->mmio_map(bar & ~0xFu, 0x2000u) != 0) {
      logs("acp: BAR fora do alcance\n");
      return;
    }
    acp_mm = (volatile uint32_t*)(uintptr_t)(bar & ~0xFu);
    logs("acp 1022:15E2 em ");
    logx((uint32_t)a.bus << 16 | (uint32_t)a.dev << 8 | a.fun);
    logs(" comando PCI ");
    logx(cmdr & 0xFFFFu);
    logs("\n");
    acp_log("antes");
    /* rn_acp_init: energia, ACP_CONTROL = 1, reset, clock = 3 */
    st = acp_rd(ACP_PGFSM_STATUS);
    if (st != 0) {
      if ((st & 3u) != 1u) acp_wr(ACP_PGFSM_CONTROL, 1);
      if (acp_wait(ACP_PGFSM_STATUS, 0xFFFFFFFFu, 0, 50)) logs("acp: energia nao ligou\n");
      acp_wr(ACP_PME_EN, 1);
    }
    acp_wr(ACP_CONTROL, 1);
    acp_reset();
    acp_wr(ACP_CLKMUX_SEL, 3);
    /* rn_acp_deinit sem desligar a energia (acp_power_gating = 0) */
    acp_reset();
    acp_wr(ACP_CONTROL, 0);
    acp_log("depois");
    return;
  }
}

/* Capability de energia PCI (PM, id 1): deslocamento ou 0. */
static uint32_t pm_cap(const struct pctrl* p) {
  uint32_t ptr, guard = 0;
  if (!(pci(p, 0x04) & (1u << 20))) return 0;
  ptr = pci(p, 0x34) & 0xFCu;
  while (ptr && guard++ < 48u) {
    uint32_t h = pci(p, ptr);
    if ((h & 0xFFu) == 1u) return ptr;
    ptr = (h >> 8) & 0xFCu;
  }
  return 0;
}

static void az_log(const char* quando, uint32_t pmcsr) {
  logs("hda pm ");
  logs(quando);
  logs(": pmcsr ");
  logx(pmcsr);
  if (acp_mm) {
    logs(" az ");
    logx(acp_rd(AZ_DEVICE_STATE));
    logs(" acp ");
    logx(acp_rd(ACP_DEVICE_STATE));
  }
  logs("\n");
}

/* Ciclo de energia PCI D0 -> D3hot -> D0 no HDA da AMD, antes do init. O
 * Linux poe o controlador em D3hot sempre que a placa fica parada e volta a
 * D0 ao tocar; depois de um boot no Linux (reboot quente) o alto-falante
 * tocava no SODOS, e num boot frio o controlador nunca saiu de D0. Com
 * No_Soft_Reset os BARs ficam; o init configura o resto. */
static void hda_pm_cycle(void) {
  int c;
  for (c = 0; c < s.npc; c++) {
    const struct pctrl* p = &s.pc[c];
    uint32_t ptr, pm;
    if (p->ven != 0x1022u || p->video) continue;
    ptr = pm_cap(p);
    if (!ptr) continue;
    pm = pci(p, ptr + 4u);
    az_log("antes", pm);
    pciw(p, ptr + 4u, (pm & ~0x8003u) | 3u);    /* D3hot (sem limpar PME_Status) */
    delay_ms(20);
    az_log("em D3", pci(p, ptr + 4u));
    pciw(p, ptr + 4u, pm & ~0x8003u);           /* D0 */
    delay_ms(20);
    az_log("de volta em D0", pci(p, ptr + 4u));
  }
}

/* ---------- fotografia para comparar boot frio x boot vindo do Linux ----------
 * No notebook Positivo VAIO (Conexant 2782:12C2) o alto-falante toca no SODOS
 * so depois de um reboot quente vindo do Linux. Codec (registradores padrao),
 * GPIOs de saida da AMD, ACP e ciclo PCI ja foram comparados sem diferenca;
 * aqui vai o resto para o log: memoria do EC, PCI das funcoes do HDA e da
 * ponte, registradores do HDA antes do reset e os do no 1C da Conexant. So le. */
static inline uint8_t inb(uint16_t port) {
  uint8_t v;
  __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}
static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); }

/* EC pelas portas do ACPI (_CRS do H_EC: 62 dados, 66 comando/estado). */
static int ec_wait(uint8_t mask, uint8_t want) {
  uint32_t n;
  for (n = 0; n < 20000u; n++) {
    if ((inb(0x66) & mask) == want) return 0;
    io_delay();
  }
  return -1;
}

static int ec_read(uint8_t addr, uint8_t* v) {
  if (inb(0x66) & 1u) (void)inb(0x62);             /* resposta velha na saida */
  if (ec_wait(2, 0)) return -1;
  outb(0x66, 0x80);                                 /* RD_EC */
  if (ec_wait(2, 0)) return -1;
  outb(0x62, addr);
  if (ec_wait(1, 1)) return -1;
  *v = inb(0x62);
  return 0;
}

static void hex_lines(const char* nome, const uint8_t* d, uint32_t n) {
  char t[120];
  struct sb b;
  uint32_t i;
  b.p = t;
  b.n = 0;
  b.max = (int)sizeof(t);
  for (i = 0; i < n; i++) {
    if (i % 16u == 0) {
      sb_s(&b, nome);
      sb_c(&b, ' ');
      sb_x(&b, i, 3);
      sb_c(&b, ':');
    }
    sb_c(&b, ' ');
    sb_x(&b, d[i], 2);
    if (i % 16u == 15u) {
      sb_c(&b, '\n');
      line_out(&b);
    }
  }
}

static void pci_cfg_dump(uint32_t bus, uint32_t dev, uint32_t fun) {
  uint8_t d[256];
  char nome[16];
  struct sb b;
  uint32_t i;
  if (k->pci_read32(bus, dev, fun, 0) == 0xFFFFFFFFu) return;
  for (i = 0; i < 256u; i += 4u) {
    uint32_t v = k->pci_read32(bus, dev, fun, i);
    d[i] = (uint8_t)v;
    d[i + 1u] = (uint8_t)(v >> 8);
    d[i + 2u] = (uint8_t)(v >> 16);
    d[i + 3u] = (uint8_t)(v >> 24);
  }
  b.p = nome;
  b.n = 0;
  b.max = (int)sizeof(nome);
  sb_s(&b, "pci ");
  sb_x(&b, bus, 2);
  sb_c(&b, ':');
  sb_x(&b, dev, 2);
  sb_c(&b, '.');
  sb_x(&b, fun, 1);
  nome[b.n] = 0;
  hex_lines(nome, d, 256u);
}

static void snapshot_pre(void) {
  uint8_t d[256];
  uint32_t i, bus, dev;
  int c;
  logs("--- fotografia (antes de mexer)\n");
  for (i = 0; i < 256u; i++)
    if (ec_read((uint8_t)i, &d[i])) {
      logs("ec: sem resposta\n");
      break;
    }
  if (i == 256u) hex_lines("ec", d, 256u);
  for (c = 0; c < s.npc; c++) {
    const struct pctrl* p = &s.pc[c];
    uint32_t f, bar;
    if (p->ven != 0x1022u || p->video) continue;
    /* a ponte cujo barramento secundario e o do HDA */
    for (dev = 0; dev < 32u; dev++)
      for (f = 0; f < 8u; f++) {
        uint32_t h = k->pci_read32(0, dev, f, 0x0C), bn = k->pci_read32(0, dev, f, 0x18);
        if (k->pci_read32(0, dev, f, 0) == 0xFFFFFFFFu || ((h >> 16) & 0x7Fu) != 1u) continue;
        if (((bn >> 8) & 0xFFu) == p->bus) pci_cfg_dump(0, dev, f);
      }
    bus = p->bus;
    for (f = 0; f < 8u; f++) pci_cfg_dump(bus, p->dev, f);
    /* registradores do HDA como o firmware (ou o Linux) deixou */
    bar = pci(p, 0x10);
    if ((bar & ~0xFu) && !(bar & 1u) && k->mmio_map(bar & ~0xFu, 0x4000u) == 0 && (pci(p, 0x04) & 2u)) {
      volatile uint8_t* m = (volatile uint8_t*)(uintptr_t)(bar & ~0xFu);
      for (i = 0; i < 256u; i++) d[i] = m[i];
      hex_lines("hda", d, 256u);
    }
  }
}

/* ---------- modo ACPI ----------
 * Fotografias de 25/11/2026: entre um boot frio (mudo) e um boot vindo do
 * Linux (toca), so a memoria do EC muda (fora temperatura e ventoinha): os
 * bytes 0x12 (00 -> FF), 0x2B e 0x2D (00 -> 03), que o ACPI nem nomeia. O EC
 * tem energia propria e so esquece quando a energia e cortada. Hipotese: o EC
 * solta o amplificador do alto-falante quando o sistema liga o modo ACPI
 * (FADT: SMI_CMD <- ACPI_ENABLE), o que o Linux faz no boot e o SODOS nunca
 * fez. A SCI (IRQ 9) continua mascarada no PIC do SODOS. */
static inline uint16_t inw(uint16_t port) {
  uint16_t v;
  __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}

static int sig_is(const volatile uint8_t* p, const char* s, int n) {
  int i;
  for (i = 0; i < n; i++)
    if (p[i] != (uint8_t)s[i]) return 0;
  return 1;
}

static const volatile uint8_t* rsdp_find(void) {
  /* o endereco que o Linux mostrou (efi: ACPI 2.0=0xcc744014); depois a area
   * legada de 0xE0000 a 0xFFFFF */
  static const uint64_t fixos[1] = { 0xCC744014ull };
  uint64_t a;
  int i;
  for (i = 0; i < 1; i++) {
    const volatile uint8_t* p = (const volatile uint8_t*)(uintptr_t)fixos[i];
    if (sig_is(p, "RSD PTR ", 8)) return p;
  }
  for (a = 0xE0000ull; a < 0x100000ull; a += 16u) {
    const volatile uint8_t* p = (const volatile uint8_t*)(uintptr_t)a;
    if (sig_is(p, "RSD PTR ", 8)) return p;
  }
  return 0;
}

static uint32_t rd32le(const volatile uint8_t* p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void ec_bytes_log(const char* quando) {
  static const uint8_t offs[3] = { 0x12, 0x2B, 0x2D };
  int i;
  logs("ec ");
  logs(quando);
  logs(":");
  for (i = 0; i < 3; i++) {
    uint8_t v = 0xEE;
    (void)ec_read(offs[i], &v);
    logs(" [");
    logx(offs[i]);
    logs("]=");
    logx(v);
  }
  logs("\n");
}

static void acpi_enable_test(void) {
  const volatile uint8_t* rsdp = rsdp_find();
  const volatile uint8_t* fadt = 0;
  uint32_t smi, pm1, n, i, t0;
  uint8_t en;
  if (!rsdp) {
    logs("acpi: RSDP nao achado\n");
    return;
  }
  if (rsdp[15] >= 2) {                               /* XSDT: entradas de 64 bits */
    const volatile uint8_t* x = (const volatile uint8_t*)(uintptr_t)(rd32le(rsdp + 24) | (uint64_t)rd32le(rsdp + 28) << 32);
    if (sig_is(x, "XSDT", 4)) {
      n = (rd32le(x + 4) - 36u) / 8u;
      for (i = 0; i < n && i < 64u && !fadt; i++) {
        const volatile uint8_t* t = (const volatile uint8_t*)(uintptr_t)(rd32le(x + 36 + i * 8) | (uint64_t)rd32le(x + 40 + i * 8) << 32);
        if (sig_is(t, "FACP", 4)) fadt = t;
      }
    }
  }
  if (!fadt) {                                       /* RSDT: entradas de 32 bits */
    const volatile uint8_t* r = (const volatile uint8_t*)(uintptr_t)rd32le(rsdp + 16);
    if (sig_is(r, "RSDT", 4)) {
      n = (rd32le(r + 4) - 36u) / 4u;
      for (i = 0; i < n && i < 64u && !fadt; i++) {
        const volatile uint8_t* t = (const volatile uint8_t*)(uintptr_t)rd32le(r + 36 + i * 4);
        if (sig_is(t, "FACP", 4)) fadt = t;
      }
    }
  }
  if (!fadt) {
    logs("acpi: FADT nao achada\n");
    return;
  }
  smi = rd32le(fadt + 48);
  en = fadt[52];
  pm1 = rd32le(fadt + 64);
  logs("acpi: SMI_CMD ");
  logx(smi);
  logs(" ACPI_ENABLE ");
  logx(en);
  logs(" PM1a_CNT ");
  logx(pm1);
  logs(pm1 && (inw((uint16_t)pm1) & 1u) ? " (ja em modo ACPI)\n" : " (modo legado)\n");
  ec_bytes_log("antes");
  if (!smi || !en || !pm1 || smi > 0xFFFFu || pm1 > 0xFFFFu) return;
  if (!(inw((uint16_t)pm1) & 1u)) {
    outb((uint16_t)smi, en);
    t0 = k->ms();
    while (!(inw((uint16_t)pm1) & 1u) && k->ms() - t0 < 3000u) delay_ms(10);
    logs((inw((uint16_t)pm1) & 1u) ? "acpi: modo ACPI ligado (SCI_EN)\n" : "acpi: SCI_EN nao subiu\n");
  }
  delay_ms(500);
  ec_bytes_log("depois");
}

/* Registradores do no 1C (vendor) da Conexant: o Linux le os do CX8070/SN6140
 * com GET_PROC_COEF (4 bits, 0xC) e o registrador no byte alto (0xCA0, 0xCB0). */
static void codec_vendor_dump(void) {
  uint8_t d[256];
  uint32_t r;
  int nid;
  static const uint8_t nos[3] = { 0x1C, 0x1B, 0x28 };
  for (nid = 0; nid < 3; nid++) {
    char nome[8] = "cx1C";
    nome[2] = "0123456789ABCDEF"[nos[nid] >> 4];
    nome[3] = "0123456789ABCDEF"[nos[nid] & 15u];
    for (r = 0; r < 256u; r++) {
      int err = s.cmd_err;
      d[r] = (uint8_t)verb4(nos[nid], 0xCu, r << 8);
      if (s.cmd_err != err) break;                  /* nao responde: pula o no */
    }
    if (r < 256u) {
      logs(nome);
      logs(": sem resposta\n");
      continue;
    }
    hex_lines(nome, d, 256u);
  }
}

/* Espera o estado de energia real (bits 7:4 do GET_POWER) do no chegar a st. */
static void power_wait(int nid, uint32_t st, uint32_t ms) {
  uint32_t t0 = k->ms();
  while (((verb(nid, V_GET_POWER, 0) >> 4) & 15u) != st && k->ms() - t0 < ms) delay_ms(1);
}

/* Reset do grupo de audio (verbo 7FF) e energia D3 -> D0 de verdade.
 * No notebook com Conexant SN6180 (14F1:20D1, subsistema 2782:12C2) o codec
 * chegava do boot ja em D0, todo configurado como o Linux faria, e mudo: o
 * SET_POWER D0 do init nao mudava nada. Com o reset seguido de D0 (o
 * D:\TEMP\SOMTEST4.BAT de 25/11/2026) o alto-falante passou a tocar. O
 * reset volta os widgets ao padrao; o init configura tudo de novo depois.
 * As configuracoes dos pinos e os parametros ja lidos nao mudam. */
static void codec_power_cycle(void) {
  uint32_t r;
  logs("reset do grupo de audio (7FF) e energia D3 -> D0\n");
  (void)cmd((uint32_t)s.cad << 28 | (uint32_t)s.afg << 20 | 0x7FFu << 8, &r);
  delay_ms(100);
  verb(s.afg, V_SET_POWER, 3);
  power_wait(s.afg, 3, 100);
  verb(s.afg, V_SET_POWER, 0);
  power_wait(s.afg, 0, 200);
  delay_ms(10);
}

static int try_ctrl(int i) {
  struct pctrl* p = &s.pc[i];
  uint32_t mask, r;
  int cad;
  logs("controlador ");
  logx((uint32_t)p->ven << 16 | p->did);
  logs(" em ");
  logx((uint32_t)p->bus << 16 | (uint32_t)p->dev << 8 | p->fun);
  logs("\n");
  if (ctrl_up(p) != 0) {
    ctrl_down();
    return 0;
  }
  mask = r16(STATESTS) & 0x7FFFu;
  w16(STATESTS, (uint16_t)mask);
  if (!mask) {
    logs("nenhum codec respondeu\n");
    ctrl_down();
    return 0;
  }
  /* a CORB anda? senao, comandos imediatos */
  for (cad = 0; cad < 15 && !(mask & (1u << cad)); cad++) {
  }
  s.cad = cad;
  if (corb_cmd((uint32_t)cad << 28 | V_GET_PARAM << 8 | P_VENDOR, &r) != 0) {
    logs("CORB/RIRB sem resposta: comandos imediatos\n");
    corb_off();
    s.imm = 1;
  }
  for (cad = 0; cad < 15; cad++)
    if ((mask & (1u << cad)) && codec_scan(cad) > 0) return 1;
  ctrl_down();
  return 0;
}

static int a_init(const struct audio_host* h) {
  uint8_t* z = (uint8_t*)&s;
  uint32_t i;
  int c, pass, found = 0;
  k = h;
  for (i = 0; i < sizeof(s); i++) z[i] = 0;
  s.ci = -1;
  s.vol = 90;
  pci_scan();
  if (!s.npc) {
    logs("nenhum controlador HD Audio no PCI\n");
    return -1;
  }
  snapshot_pre();
  acpi_enable_test();
  acp_on();   /* AMD: antes do HDA, como no boot do Linux */
  hda_pm_cycle();
  for (pass = 0; pass < 2 && !found; pass++)
    for (c = 0; c < s.npc && !found; c++) {
      if (s.pc[c].video != pass) continue;
      if (try_ctrl(c)) {
        found = 1;
        s.ci = c;
      }
    }
  if (!found) {
    logs("nenhuma saida analogica\n");
    return -1;
  }
  if (!s.oss) {
    logs("o controlador nao tem fluxo de saida\n");
    a_fini();
    return -1;
  }
  s.ring = (uint8_t*)k->alloc(RING, 4096);
  s.bdl = (volatile uint32_t*)k->alloc(BDL_N * 16u, 128);
  if (!s.ring || !s.bdl) {
    logs("sem memoria para o buffer\n");
    a_fini();
    return -1;
  }
  for (i = 0; i < BDL_N; i++) {
    uint64_t a = (uint64_t)(uintptr_t)(s.ring + i * BDL_BYTES);
    s.bdl[i * 4u] = (uint32_t)a;
    s.bdl[i * 4u + 1u] = (uint32_t)(a >> 32);
    s.bdl[i * 4u + 2u] = BDL_BYTES;
    s.bdl[i * 4u + 3u] = 0;          /* sem IOC: polling */
  }
  s.sd = s.mm + SD_BASE + (uint32_t)s.iss * SD_LEN;
  if (s.pc[s.ci].ven == 0x1022u) fch_gpio_dump();   /* antes de mexer no codec */
  if ((s.vendor >> 16) == 0x14F1u) codec_vendor_dump();
  codec_power_cycle();
  pcm_caps();
  for (c = 0; c < s.no; c++) path_on(&s.o[c]);
  vol_apply();
  jack_apply(1);
  dump("init");
  logs("pronto: ");
  logs(vendor_name(s.vendor));
  logs(", ");
  logx((uint32_t)s.no);
  logs(" saida(s)\n");
  return 0;
}

static void a_fini(void) {
  if (s.mm) {
    sd_stop();
    if (s.afg) verb(s.afg, V_SET_POWER, 3);   /* D3 */
    ctrl_down();
  }
  if (s.ring) k->free(s.ring);
  if (s.bdl) k->free((void*)s.bdl);
  if (s.cmdmem) k->free(s.cmdmem);
  s.ring = 0;
  s.bdl = 0;
  s.cmdmem = 0;
  s.sd = 0;
  s.started = s.running = 0;
}

static const struct audio_ops ops = {
  AUDIO_ABI, "HDA", a_init, a_fini, a_start, a_write, a_pending, a_service, a_stop, a_volume, a_info,
};

const struct audio_ops* audio_driver(void) { return &ops; }
