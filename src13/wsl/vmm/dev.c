/* dev.c - Dispositivos da VM do WSL: PIC 8259, PIT 8254, UART 16550, CMOS,
 * PCI tipo 1 (ponte + virtio-blk moderno) e o disco copy-on-write. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "vmm.h"
#include "../sodos_api.h"

#define PIT_HZ 1193182ull

struct pic {
  uint8_t irr, isr, imr, base, step, icw4, read_isr, aeoi, elcr, lines;
};

struct pit_ch {
  uint16_t reload;
  uint8_t mode, access, wflip, rflip, latched, lsb;
  uint16_t latch;
  int armed;         /* contando */
  int wait_gate;     /* carregado, esperando gate (canal 2) */
  uint64_t start;    /* TSC do inicio da contagem */
  uint64_t next;     /* canal 0: proximo IRQ0 */
};

struct uart {
  uint8_t ier, lcr, mcr, scr, dll, dlm, fcr;
  int thre;
};

struct vq {
  uint16_t size;
  uint16_t ready;
  uint64_t desc, avail, used;
  uint16_t last;
};

struct vblk {
  uint32_t dfsel, gfsel;
  uint32_t gfeat[2];
  uint8_t status, isr, gen;
  uint16_t qsel;
  struct vq q;
};

struct cow {
  uint64_t* keys; /* chunk + 1; 0 = vazio */
  uint8_t** vals;
  uint32_t cap, n;
  uint8_t* slab;
  uint32_t slab_left;
};

struct devs {
  struct pic pic[2];
  struct pit_ch pit[3];
  uint8_t p61;
  struct uart u;
  uint8_t cmos_idx;
  uint8_t cmos[128];
  uint32_t cf8;
  uint8_t cfg_host[256];
  uint8_t cfg_blk[256];
  struct vblk blk;
  struct cow cow;
  uint64_t ticks_tsc_num; /* tsc_hz */
  int warned_ports;
};

#define D(vm) ((struct devs*)(vm)->dev)
#define BAR_SIZE 0x4000u
#define BAR_DEFAULT 0xE0000000u

/* ================= PIC ================= */

static int pic_get(struct pic* p) {
  int i;
  for (i = 0; i < 8; i++) {
    uint8_t b = (uint8_t)(1u << i);
    if (p->isr & b) return -1;
    if ((p->irr & ~p->imr) & b) return i;
  }
  return -1;
}

int pic_pending_vector(struct vm* vm) {
  struct pic* m = &D(vm)->pic[0];
  struct pic* s = &D(vm)->pic[1];
  int i;
  for (i = 0; i < 8; i++) {
    uint8_t b = (uint8_t)(1u << i);
    if (m->isr & b) return -1;
    if (m->imr & b) continue;
    if (i == 2) {
      int k = pic_get(s);
      if (k >= 0) return s->base + k;
      continue;
    }
    if (m->irr & b) return m->base + i;
  }
  return -1;
}

void pic_ack(struct vm* vm, int vec) {
  struct pic* m = &D(vm)->pic[0];
  struct pic* s = &D(vm)->pic[1];
  if (vec >= m->base && vec < m->base + 8 && vec - m->base != 2) {
    uint8_t b = (uint8_t)(1u << (vec - m->base));
    if (!(m->elcr & b)) m->irr &= (uint8_t)~b;
    if (!m->aeoi) m->isr |= b;
  } else if (vec >= s->base && vec < s->base + 8) {
    uint8_t b = (uint8_t)(1u << (vec - s->base));
    if (!(s->elcr & b)) s->irr &= (uint8_t)~b;
    if (!s->aeoi) s->isr |= b;
    if (!m->aeoi) m->isr |= 4;
  }
}

void pic_set_irq(struct vm* vm, int irq, int level) {
  struct pic* p = &D(vm)->pic[irq >> 3];
  uint8_t b = (uint8_t)(1u << (irq & 7));
  if (level) {
    if (!(p->lines & b)) p->irr |= b; /* borda */
    p->lines |= b;
  } else {
    p->lines &= (uint8_t)~b;
    if (p->elcr & b) p->irr &= (uint8_t)~b;
  }
}

static void pic_eoi(struct pic* p, int irq) {
  if (irq < 0) {
    int i;
    for (i = 0; i < 8; i++)
      if (p->isr & (1u << i)) {
        irq = i;
        break;
      }
    if (irq < 0) return;
  }
  p->isr &= (uint8_t)~(1u << irq);
  /* nivel ainda alto: pede de novo */
  if ((p->elcr & (1u << irq)) && (p->lines & (1u << irq))) p->irr |= (uint8_t)(1u << irq);
}

static void pic_write(struct pic* p, int data, uint8_t v) {
  if (!data) {
    if (v & 0x10) { /* ICW1 */
      p->step = 2;
      p->icw4 = v & 1;
      p->imr = 0;
      p->isr = 0;
      p->irr = 0;
      p->read_isr = 0;
      p->aeoi = 0;
    } else if ((v & 0x18) == 0x08) { /* OCW3 */
      if (v & 2) p->read_isr = v & 1;
    } else { /* OCW2 */
      int cmd = v >> 5;
      if (cmd == 1 || cmd == 5) pic_eoi(p, -1);
      else if (cmd == 3 || cmd == 7) pic_eoi(p, v & 7);
    }
  } else {
    switch (p->step) {
    case 2: p->base = v & 0xF8; p->step = 3; break;
    case 3: p->step = p->icw4 ? 4 : 0; break;
    case 4: p->aeoi = (v >> 1) & 1; p->step = 0; break;
    default: p->imr = v;
    }
  }
}

/* ================= PIT ================= */

static uint64_t tsc_to_ticks(struct vm* vm, uint64_t d) {
  return (uint64_t)((double)d * (double)PIT_HZ / (double)vm->tsc_hz);
}
static uint64_t ticks_to_tsc(struct vm* vm, uint64_t t) {
  return (uint64_t)((double)t * (double)vm->tsc_hz / (double)PIT_HZ);
}

static uint32_t pit_n(struct pit_ch* c) { return c->reload ? c->reload : 65536u; }

static uint16_t pit_count(struct vm* vm, struct pit_ch* c, uint64_t now) {
  uint64_t el;
  uint32_t n = pit_n(c);
  if (!c->armed) return c->reload;
  el = tsc_to_ticks(vm, now - c->start);
  if (c->mode == 2 || c->mode == 3) return (uint16_t)(n - (el % n));
  return (uint16_t)(n - el);
}

static int pit_out(struct vm* vm, struct pit_ch* c, uint64_t now) {
  uint64_t el;
  uint32_t n = pit_n(c);
  if (!c->armed) return c->mode != 0;
  el = tsc_to_ticks(vm, now - c->start);
  switch (c->mode) {
  case 0: return el >= n;
  case 2: return (el % n) != n - 1;
  case 3: return (el % n) < n / 2;
  case 4: return el != n;
  default: return 1;
  }
}

static void pit_load(struct vm* vm, int ch, uint64_t now) {
  struct pit_ch* c = &D(vm)->pit[ch];
  if (ch == 2 && !(D(vm)->p61 & 1)) {
    c->armed = 0;
    c->wait_gate = 1;
    return;
  }
  c->armed = 1;
  c->wait_gate = 0;
  c->start = now;
  if (ch == 0) c->next = now + ticks_to_tsc(vm, pit_n(c));
}

static uint8_t pit_read(struct vm* vm, int ch, uint64_t now) {
  struct pit_ch* c = &D(vm)->pit[ch];
  uint16_t v;
  int hi;
  if (c->latched) {
    v = c->latch;
  } else {
    v = pit_count(vm, c, now);
  }
  switch (c->access) {
  case 1: hi = 0; c->latched = 0; break;
  case 2: hi = 1; c->latched = 0; break;
  default:
    hi = c->rflip;
    c->rflip ^= 1;
    if (hi) c->latched = 0;
  }
  return hi ? (uint8_t)(v >> 8) : (uint8_t)v;
}

static void pit_write(struct vm* vm, int port, uint8_t v, uint64_t now) {
  struct devs* d = D(vm);
  if (port == 3) {
    int ch = v >> 6;
    struct pit_ch* c;
    if (ch == 3) { /* read-back: so latch de contagem */
      int i;
      if (!(v & 0x20))
        for (i = 0; i < 3; i++)
          if (v & (2 << i)) {
            d->pit[i].latch = pit_count(vm, &d->pit[i], now);
            d->pit[i].latched = 1;
            d->pit[i].rflip = 0;
          }
      return;
    }
    c = &d->pit[ch];
    if (((v >> 4) & 3) == 0) {
      c->latch = pit_count(vm, c, now);
      c->latched = 1;
      c->rflip = 0;
      return;
    }
    c->access = (v >> 4) & 3;
    c->mode = (v >> 1) & 7;
    if (c->mode > 5) c->mode -= 4;
    c->wflip = c->rflip = 0;
    c->latched = 0;
    c->armed = 0;
    return;
  } else {
    struct pit_ch* c = &d->pit[port];
    switch (c->access) {
    case 1: c->reload = v; pit_load(vm, port, now); break;
    case 2: c->reload = (uint16_t)(v << 8); pit_load(vm, port, now); break;
    default:
      if (!c->wflip) {
        c->lsb = v;
        c->wflip = 1;
      } else {
        c->reload = (uint16_t)(c->lsb | (v << 8));
        c->wflip = 0;
        pit_load(vm, port, now);
      }
    }
  }
}

/* ================= UART ================= */

static int rx_avail(struct vm* vm) { return vm->rx_head != vm->rx_tail; }

static void uart_update(struct vm* vm) {
  struct uart* u = &D(vm)->u;
  int line = ((u->ier & 1) && rx_avail(vm)) || ((u->ier & 2) && u->thre);
  if (!(u->mcr & 8)) line = 0; /* OUT2 liga o IRQ no PC */
  pic_set_irq(vm, 4, line);
}

static void tx_put(struct vm* vm, uint8_t c) {
  uint32_t h = vm->tx_head;
  if (h - vm->tx_tail >= UART_RING) return;
  vm->tx[h % UART_RING] = c;
  vm->tx_head = h + 1;
}

static uint8_t uart_in(struct vm* vm, int r) {
  struct uart* u = &D(vm)->u;
  uint8_t v = 0;
  switch (r) {
  case 0:
    if (u->lcr & 0x80) return u->dll;
    if (rx_avail(vm)) {
      v = vm->rx[vm->rx_tail % sizeof vm->rx];
      vm->rx_tail++;
    }
    break;
  case 1:
    return (u->lcr & 0x80) ? u->dlm : u->ier;
  case 2:
    if ((u->ier & 1) && rx_avail(vm)) v = 0x04;
    else if ((u->ier & 2) && u->thre) {
      v = 0x02;
      u->thre = 0;
    } else v = 0x01;
    if (u->fcr & 1) v |= 0xC0;
    break;
  case 3: return u->lcr;
  case 4: return u->mcr;
  case 5: v = 0x60 | (rx_avail(vm) ? 1 : 0); break;
  case 6:
    if (u->mcr & 0x10)
      v = (uint8_t)(((u->mcr & 1) << 5) | ((u->mcr & 2) << 3) | ((u->mcr & 4) << 4) |
                    ((u->mcr & 8) << 4));
    else v = 0xB0;
    break;
  case 7: return u->scr;
  }
  uart_update(vm);
  return v;
}

static void uart_out(struct vm* vm, int r, uint8_t v) {
  struct uart* u = &D(vm)->u;
  switch (r) {
  case 0:
    if (u->lcr & 0x80) {
      u->dll = v;
      return;
    }
    if (u->mcr & 0x10) {
      uint32_t h = vm->rx_head;
      if (h - vm->rx_tail < sizeof vm->rx) {
        vm->rx[h % sizeof vm->rx] = v;
        vm->rx_head = h + 1;
      }
    } else {
      tx_put(vm, v);
    }
    u->thre = 1;
    break;
  case 1:
    if (u->lcr & 0x80) {
      u->dlm = v;
      return;
    }
    if ((v & 2) && !(u->ier & 2)) u->thre = 1;
    u->ier = v & 0x0F;
    break;
  case 2: u->fcr = v; break;
  case 3: u->lcr = v; break;
  case 4: u->mcr = v & 0x1F; break;
  case 7: u->scr = v; break;
  }
  uart_update(vm);
}

/* ================= CMOS ================= */

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static uint8_t cmos_read(struct vm* vm, uint8_t r) {
  struct devs* d = D(vm);
  if (r <= 9 || r == 0x32) {
    time_t t = time(0);
    long long days, secs;
    int y, m, dd;
    if (t < 0) t = 0;
    days = (long long)t / 86400;
    secs = (long long)t % 86400;
    {
      /* dias desde 1970 -> data civil (Howard Hinnant) */
      long long z = days + 719468, era = z / 146097, doe = z - era * 146097;
      long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
      dd = (int)(doy - (153 * mp + 2) / 5 + 1);
      m = (int)(mp < 10 ? mp + 3 : mp - 9);
      y = (int)(yoe + era * 400 + (m <= 2));
    }
    switch (r) {
    case 0: return bcd((int)(secs % 60));
    case 2: return bcd((int)(secs / 60 % 60));
    case 4: return bcd((int)(secs / 3600));
    case 6: return bcd((int)((days + 4) % 7 + 1));
    case 7: return bcd(dd);
    case 8: return bcd(m);
    case 9: return bcd(y % 100);
    case 0x32: return bcd(y / 100);
    default: return 0;
    }
  }
  switch (r) {
  case 0x0A: return 0x26;
  case 0x0B: return 0x02;
  case 0x0C: return 0;
  case 0x0D: return 0x80;
  }
  return d->cmos[r & 127];
}

/* ================= disco copy-on-write ================= */

static uint8_t** cow_slot(struct vm* vm, uint64_t chunk, int create) {
  struct cow* c = &D(vm)->cow;
  uint32_t h = (uint32_t)((chunk * 0x9E3779B97F4A7C15ull) >> 32) & (c->cap - 1);
  while (c->keys[h]) {
    if (c->keys[h] == chunk + 1) return &c->vals[h];
    h = (h + 1) & (c->cap - 1);
  }
  if (!create || c->n >= vm->cow_max_chunks) return 0;
  if (c->slab_left == 0) {
    c->slab = (uint8_t*)k_hmalloc(2u << 20, 4096);
    if (!c->slab) return 0;
    c->slab_left = (2u << 20) / 4096;
  }
  c->keys[h] = chunk + 1;
  c->vals[h] = c->slab;
  c->slab += 4096;
  c->slab_left--;
  c->n++;
  return &c->vals[h];
}

static int blk_read(struct vm* vm, uint64_t off, uint8_t* buf, uint32_t len) {
  while (len) {
    uint64_t ch = off >> 12;
    uint32_t in = (uint32_t)(off & 4095), take = 4096 - in;
    uint8_t** s;
    if (take > len) take = len;
    s = D(vm)->cow.n ? cow_slot(vm, ch, 0) : 0;
    if (s) {
      memcpy(buf, *s + in, take);
    } else {
      /* junta os pedacos seguintes que nao estao na camada */
      uint32_t run = take;
      while (run < len && run < (1u << 20)) {
        uint32_t nt = len - run > 4096 ? 4096 : len - run;
        if (D(vm)->cow.n && cow_slot(vm, (off + run) >> 12, 0)) break;
        run += nt;
      }
      if (wio_read(&vm->disk, off, buf, run)) return -1;
      take = run;
    }
    buf += take;
    off += take;
    len -= take;
  }
  return 0;
}

static int blk_write(struct vm* vm, uint64_t off, const uint8_t* buf, uint32_t len) {
  while (len) {
    uint64_t ch = off >> 12;
    uint32_t in = (uint32_t)(off & 4095), take = 4096 - in;
    uint8_t** s;
    if (take > len) take = len;
    s = cow_slot(vm, ch, 0);
    if (!s) {
      s = cow_slot(vm, ch, 1);
      if (!s) {
        static int warned;
        if (!warned++) vm_log(vm, "\nWSL: camada de escrita cheia (-w MB); escritas falham\n");
        return -1;
      }
      if (take != 4096 && wio_read(&vm->disk, ch << 12, *s, 4096)) return -1;
    }
    memcpy(*s + in, buf, take);
    buf += take;
    off += take;
    len -= take;
  }
  return 0;
}

/* ================= virtio-blk ================= */

#define VIRTQ_SIZE 256
#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2

struct seg {
  uint8_t* p;
  uint32_t len;
  int w;
};

static void vblk_reset(struct vm* vm) {
  struct vblk* b = &D(vm)->blk;
  memset(b, 0, sizeof *b);
  b->q.size = VIRTQ_SIZE;
  pic_set_irq(vm, 5, 0);
}

static void vblk_process(struct vm* vm) {
  struct vblk* b = &D(vm)->blk;
  struct vq* q = &b->q;
  uint8_t *desc, *avail, *used;
  uint16_t idx;
  int did = 0;
  if (!q->ready || !q->size) return;
  desc = gpa_ptr(vm, q->desc, 16ull * q->size);
  avail = gpa_ptr(vm, q->avail, 6 + 2ull * q->size);
  used = gpa_ptr(vm, q->used, 6 + 8ull * q->size);
  if (!desc || !avail || !used) return;
  idx = *(volatile uint16_t*)(avail + 2);
  while (q->last != idx) {
    static struct seg sg[VIRTQ_SIZE];
    uint16_t head = rd16(avail + 4 + 2 * (q->last % q->size)), i = head;
    int n = 0, k, guard = 0;
    uint8_t hdr[16];
    uint32_t hgot = 0, written = 0;
    uint8_t* status = 0;
    uint8_t st = 0;
    uint32_t type;
    uint64_t sector, pos;
    for (;;) {
      const uint8_t* e = desc + 16u * (i % q->size);
      uint64_t a = rd64(e);
      uint32_t l = rd32(e + 8);
      uint16_t f = rd16(e + 12);
      uint8_t* p = gpa_ptr(vm, a, l);
      if (!p || n >= VIRTQ_SIZE || ++guard > VIRTQ_SIZE) {
        st = 1;
        break;
      }
      sg[n].p = p;
      sg[n].len = l;
      sg[n].w = (f & VRING_DESC_F_WRITE) != 0;
      n++;
      if (!(f & VRING_DESC_F_NEXT)) break;
      i = rd16(e + 14);
    }
    /* cabecalho: primeiros 16 bytes legiveis */
    for (k = 0; k < n && hgot < 16 && !sg[k].w; k++) {
      uint32_t t = sg[k].len < 16 - hgot ? sg[k].len : 16 - hgot;
      memcpy(hdr + hgot, sg[k].p, t);
      hgot += t;
      sg[k].p += t;
      sg[k].len -= t;
    }
    /* status: ultimo byte gravavel */
    if (n > 0 && sg[n - 1].w && sg[n - 1].len >= 1) {
      status = sg[n - 1].p + sg[n - 1].len - 1;
      sg[n - 1].len -= 1;
    }
    if (hgot < 16 || !status) st = 1;
    type = rd32(hdr);
    sector = rd64(hdr + 8);
    pos = sector * 512;
    if (!st) {
      switch (type) {
      case 0: /* IN */
        for (k = 0; k < n && !st; k++) {
          if (!sg[k].w || !sg[k].len) continue;
          if (pos + sg[k].len > vm->disk_bytes || blk_read(vm, pos, sg[k].p, sg[k].len)) st = 1;
          pos += sg[k].len;
          written += sg[k].len;
        }
        break;
      case 1: /* OUT */
        for (k = 0; k < n && !st; k++) {
          if (sg[k].w || !sg[k].len) continue;
          if (pos + sg[k].len > vm->disk_bytes || blk_write(vm, pos, sg[k].p, sg[k].len)) st = 1;
          pos += sg[k].len;
        }
        break;
      case 4: /* FLUSH: a camada esta em RAM */
        break;
      case 8: /* GET_ID */
        for (k = 0; k < n; k++)
          if (sg[k].w && sg[k].len) {
            static const char id[20] = "SODOS-WSL-VHDX";
            uint32_t t = sg[k].len < 20 ? sg[k].len : 20;
            memcpy(sg[k].p, id, t);
            written += t;
            break;
          }
        break;
      default:
        st = 2;
      }
    }
    if (status) {
      *status = st;
      written += 1;
    }
    {
      uint16_t u = *(volatile uint16_t*)(used + 2);
      uint8_t* ue = used + 4 + 8 * (u % q->size);
      uint32_t id32 = head;
      memcpy(ue, &id32, 4);
      memcpy(ue + 4, &written, 4);
      __asm__ volatile("" ::: "memory");
      *(volatile uint16_t*)(used + 2) = (uint16_t)(u + 1);
    }
    q->last++;
    did = 1;
    idx = *(volatile uint16_t*)(avail + 2);
  }
  if (did) {
    b->isr |= 1;
    pic_set_irq(vm, 5, 1);
  }
}

/* common cfg do virtio 1.x, montado em 64 bytes */
static void common_image(struct vm* vm, uint8_t* c) {
  struct vblk* b = &D(vm)->blk;
  uint32_t df = 0;
  memset(c, 0, 64);
  if (b->dfsel == 0) df = 1u << 9;      /* FLUSH */
  else if (b->dfsel == 1) df = 1u << 0; /* VERSION_1 (bit 32) */
  memcpy(c + 0x00, &b->dfsel, 4);
  memcpy(c + 0x04, &df, 4);
  memcpy(c + 0x08, &b->gfsel, 4);
  if (b->gfsel < 2) memcpy(c + 0x0C, &b->gfeat[b->gfsel], 4);
  c[0x10] = 0xFF;
  c[0x11] = 0xFF; /* msix_config = NO_VECTOR */
  c[0x12] = 1;    /* num_queues */
  c[0x14] = b->status;
  c[0x15] = b->gen;
  memcpy(c + 0x16, &b->qsel, 2);
  if (b->qsel == 0) {
    memcpy(c + 0x18, &b->q.size, 2);
    c[0x1A] = 0xFF;
    c[0x1B] = 0xFF;
    memcpy(c + 0x1C, &b->q.ready, 2);
    memcpy(c + 0x20, &b->q.desc, 8);
    memcpy(c + 0x28, &b->q.avail, 8);
    memcpy(c + 0x30, &b->q.used, 8);
  }
}

static void common_write(struct vm* vm, uint32_t off, int size, uint64_t v) {
  struct vblk* b = &D(vm)->blk;
  uint8_t c[64];
  if (off + (uint32_t)size > 64) return;
  common_image(vm, c);
  memcpy(c + off, &v, (size_t)size);
  if (off < 4) memcpy(&b->dfsel, c, 4);
  else if (off >= 8 && off < 12) memcpy(&b->gfsel, c + 8, 4);
  else if (off >= 0x0C && off < 0x10) {
    if (b->gfsel < 2) memcpy(&b->gfeat[b->gfsel], c + 0x0C, 4);
  } else if (off == 0x14) {
    b->status = c[0x14];
    if (b->status == 0) vblk_reset(vm);
  } else if (off >= 0x16 && off < 0x18) memcpy(&b->qsel, c + 0x16, 2);
  else if (b->qsel == 0) {
    if (off >= 0x18 && off < 0x1A) {
      uint16_t s;
      memcpy(&s, c + 0x18, 2);
      if (s && s <= VIRTQ_SIZE && !(s & (s - 1))) b->q.size = s;
    } else if (off >= 0x1C && off < 0x1E) {
      memcpy(&b->q.ready, c + 0x1C, 2);
      if (b->q.ready) b->q.last = 0;
    } else if (off >= 0x20 && off < 0x28) memcpy(&b->q.desc, c + 0x20, 8);
    else if (off >= 0x28 && off < 0x30) memcpy(&b->q.avail, c + 0x28, 8);
    else if (off >= 0x30 && off < 0x38) memcpy(&b->q.used, c + 0x30, 8);
  }
}

static uint32_t bar0(struct vm* vm) { return rd32(D(vm)->cfg_blk + 0x10) & ~0xFu; }

int dev_mmio(struct vm* vm, uint64_t gpa, int size, int write, uint64_t* val) {
  struct vblk* b = &D(vm)->blk;
  uint64_t base = bar0(vm);
  if (base && gpa >= base && gpa + (uint64_t)size <= base + BAR_SIZE) {
    uint32_t off = (uint32_t)(gpa - base);
    if (off < 0x1000) {
      if (write) common_write(vm, off, size, *val);
      else {
        uint8_t c[64];
        *val = 0;
        common_image(vm, c);
        if (off + (uint32_t)size <= 64) memcpy(val, c + off, (size_t)size);
      }
    } else if (off < 0x2000) {
      if (!write) {
        *val = b->isr;
        b->isr = 0;
        pic_set_irq(vm, 5, 0);
      }
    } else if (off < 0x3000) {
      uint8_t c[64];
      uint64_t cap = vm->disk_bytes / 512;
      uint32_t o = off - 0x2000;
      memset(c, 0, sizeof c);
      memcpy(c, &cap, 8);
      if (!write) {
        *val = 0;
        if (o + (uint32_t)size <= 64) memcpy(val, c + o, (size_t)size);
      }
    } else {
      if (write) vblk_process(vm);
      else *val = 0;
    }
    return 1;
  }
  /* nada mapeado: le tudo 1 */
  if (!write) *val = ~0ull;
  {
    static int warned;
    if (vm->verbose && warned++ < 8)
      vm_log(vm, "\nWSL: MMIO sem dispositivo em %llx (%s)\n", (unsigned long long)gpa,
             write ? "escrita" : "leitura");
  }
  return 1;
}

/* ================= PCI ================= */

static void cfg16(uint8_t* c, int o, uint16_t v) { memcpy(c + o, &v, 2); }
static void cfg32(uint8_t* c, int o, uint32_t v) { memcpy(c + o, &v, 4); }

static void vcap(uint8_t* c, int at, int next, int len, int type, uint32_t off, uint32_t ln) {
  c[at] = 0x09;
  c[at + 1] = (uint8_t)next;
  c[at + 2] = (uint8_t)len;
  c[at + 3] = (uint8_t)type;
  c[at + 4] = 0; /* BAR 0 */
  cfg32(c, at + 8, off);
  cfg32(c, at + 12, ln);
}

static void pci_init(struct devs* d) {
  uint8_t* h = d->cfg_host;
  uint8_t* c = d->cfg_blk;
  cfg16(h, 0, 0x8086);
  cfg16(h, 2, 0x1237);
  cfg16(h, 4, 0x0006);
  h[0x0B] = 0x06; /* ponte host */
  cfg16(c, 0, 0x1AF4);
  cfg16(c, 2, 0x1042); /* virtio-blk moderno */
  cfg16(c, 4, 0x0000);
  cfg16(c, 6, 0x0010); /* lista de capacidades */
  c[0x08] = 1;         /* revisao */
  c[0x0A] = 0x80;
  c[0x0B] = 0x01; /* armazenamento, outro */
  cfg32(c, 0x10, BAR_DEFAULT);
  cfg16(c, 0x2C, 0x1AF4);
  cfg16(c, 0x2E, 0x0002);
  c[0x34] = 0x40;
  c[0x3C] = 5; /* IRQ 5 */
  c[0x3D] = 1; /* INTA */
  vcap(c, 0x40, 0x50, 16, 1, 0x0000, 0x1000); /* common */
  vcap(c, 0x50, 0x64, 20, 2, 0x3000, 0x1000); /* notify */
  cfg32(c, 0x60, 0);                          /* notify_off_multiplier */
  vcap(c, 0x64, 0x74, 16, 3, 0x1000, 0x1000); /* isr */
  vcap(c, 0x74, 0x00, 16, 4, 0x2000, 0x1000); /* device */
}

static uint8_t* pci_dev(struct devs* d) {
  uint32_t a = d->cf8;
  if (!(a & 0x80000000u) || ((a >> 16) & 0xFF) != 0 || ((a >> 8) & 7) != 0) return 0;
  switch ((a >> 11) & 31) {
  case 0: return d->cfg_host;
  case 1: return d->cfg_blk;
  }
  return 0;
}

static uint32_t pci_cfg_read(struct devs* d, int off, int size) {
  uint8_t* c = pci_dev(d);
  uint32_t v = 0;
  int reg = (int)(d->cf8 & 0xFC) + off;
  if (!c) return 0xFFFFFFFFu;
  if (reg + size > 256) return 0xFFFFFFFFu;
  memcpy(&v, c + reg, (size_t)size);
  return v;
}

static void pci_cfg_write(struct devs* d, int off, int size, uint32_t v) {
  uint8_t* c = pci_dev(d);
  int reg = (int)(d->cf8 & 0xFC) + off, i;
  if (!c || c == d->cfg_host) return;
  for (i = 0; i < size; i++) {
    int r = reg + i;
    uint8_t b = (uint8_t)(v >> (8 * i));
    if (r == 4) c[4] = b & 0x07;
    else if (r == 5) c[5] = b & 0x05;
    else if (r >= 0x10 && r < 0x14) {
      c[r] = b;
      if (r == 0x13) { /* BAR0 completo: alinha no tamanho */
        uint32_t bar = rd32(c + 0x10) & ~(BAR_SIZE - 1);
        cfg32(c, 0x10, bar);
      }
    } else if (r == 0x3C) c[r] = b;
  }
}

/* ================= portas ================= */

uint32_t dev_in(struct vm* vm, uint16_t port, int size) {
  struct devs* d = D(vm);
  uint64_t now = rdtsc_();
  if (port >= 0x3F8 && port <= 0x3FF) return uart_in(vm, port - 0x3F8);
  if (port == 0xCF8 && size == 4) return d->cf8;
  if (port >= 0xCFC && port <= 0xCFF) return pci_cfg_read(d, port - 0xCFC, size);
  switch (port) {
  case 0x20: case 0xA0: {
    struct pic* p = &d->pic[port == 0xA0];
    return p->read_isr ? p->isr : p->irr;
  }
  case 0x21: case 0xA1: return d->pic[port == 0xA1].imr;
  case 0x4D0: return d->pic[0].elcr;
  case 0x4D1: return d->pic[1].elcr;
  case 0x40: case 0x41: case 0x42: return pit_read(vm, port - 0x40, now);
  case 0x61: {
    static uint8_t tog;
    tog ^= 0x10;
    return (uint32_t)((d->p61 & 3) | tog | (pit_out(vm, &d->pit[2], now) ? 0x20 : 0));
  }
  case 0x71: return cmos_read(vm, d->cmos_idx);
  case 0x70: return d->cmos_idx;
  }
  if (vm->verbose && d->warned_ports++ < 16)
    vm_log(vm, "\nWSL: in %x sem dispositivo\n", port);
  return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu;
}

void dev_out(struct vm* vm, uint16_t port, int size, uint32_t v) {
  struct devs* d = D(vm);
  uint64_t now = rdtsc_();
  if (port >= 0x3F8 && port <= 0x3FF) {
    uart_out(vm, port - 0x3F8, (uint8_t)v);
    return;
  }
  if (port == 0xCF8 && size == 4) {
    d->cf8 = v;
    return;
  }
  if (port >= 0xCFC && port <= 0xCFF) {
    pci_cfg_write(d, port - 0xCFC, size, v);
    return;
  }
  switch (port) {
  case 0x20: case 0x21: pic_write(&d->pic[0], port & 1, (uint8_t)v); return;
  case 0xA0: case 0xA1: pic_write(&d->pic[1], port & 1, (uint8_t)v); return;
  case 0x4D0: d->pic[0].elcr = (uint8_t)v & 0xF8; return;
  case 0x4D1: d->pic[1].elcr = (uint8_t)v & 0xDE; return;
  case 0x40: case 0x41: case 0x42: case 0x43: pit_write(vm, port - 0x40, (uint8_t)v, now); return;
  case 0x61: {
    int rising = (v & 1) && !(d->p61 & 1);
    d->p61 = (uint8_t)(v & 3);
    if (rising && (d->pit[2].wait_gate || d->pit[2].armed)) {
      d->pit[2].armed = 1;
      d->pit[2].wait_gate = 0;
      d->pit[2].start = now;
    }
    return;
  }
  case 0x70: d->cmos_idx = (uint8_t)(v & 0x7F); return;
  case 0x71: d->cmos[d->cmos_idx] = (uint8_t)v; return;
  case 0x80: return; /* atraso de I/O */
  case 0x64:
    if ((v & 0xFF) == 0xFE) {
      snprintf(vm->stop_why, sizeof vm->stop_why, "a VM pediu reboot (i8042)");
      vm->stop = 1;
    }
    return;
  case 0xCF9:
    if (v & 4) {
      snprintf(vm->stop_why, sizeof vm->stop_why, "a VM pediu reboot (0xCF9)");
      vm->stop = 1;
    }
    return;
  case 0x604: case 0xB004: case 0x4004: /* desligar (QEMU/Bochs) */
    if (v & 0x2000) {
      snprintf(vm->stop_why, sizeof vm->stop_why, "a VM desligou");
      vm->stop = 1;
    }
    return;
  }
  if (vm->verbose && d->warned_ports++ < 16)
    vm_log(vm, "\nWSL: out %x <- %x sem dispositivo\n", port, v);
}

void dev_poll(struct vm* vm, uint64_t now) {
  struct devs* d = D(vm);
  struct pit_ch* c = &d->pit[0];
  if (c->armed && now >= c->next) {
    pic_set_irq(vm, 0, 1);
    pic_set_irq(vm, 0, 0);
    if (c->mode == 2 || c->mode == 3) {
      uint64_t per = ticks_to_tsc(vm, pit_n(c));
      c->next += per;
      if (c->next < now) c->next = now + per; /* atrasou muito: nao acumula */
    } else {
      c->armed = 0;
      c->next = 0;
    }
  }
  uart_update(vm);
}

uint64_t dev_next_event(struct vm* vm) {
  struct pit_ch* c = &D(vm)->pit[0];
  if (rx_avail(vm)) return 0;
  return c->armed ? c->next : ~0ull;
}

void dev_init(struct vm* vm) {
  struct devs* d = (struct devs*)k_hmalloc(sizeof(struct devs), 4096);
  uint32_t cap = 1024;
  if (!d) return;
  memset(d, 0, sizeof *d);
  vm->dev = d;
  d->pic[0].imr = d->pic[1].imr = 0xFF;
  d->pic[0].base = 0x08;
  d->pic[1].base = 0x70;
  d->u.thre = 1;
  pci_init(d);
  vblk_reset(vm);
  while (cap < vm->cow_max_chunks * 2u) cap <<= 1;
  d->cow.cap = cap;
  d->cow.keys = (uint64_t*)k_hmalloc((uint64_t)cap * 8, 4096);
  d->cow.vals = (uint8_t**)k_hmalloc((uint64_t)cap * 8, 4096);
  if (!d->cow.keys || !d->cow.vals) {
    vm->dev = 0;
    return;
  }
  memset(d->cow.keys, 0, (size_t)cap * 8);
}
