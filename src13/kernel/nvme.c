/* nvme.c - Particao SODOSHD num SSD NVMe, por polling (ver nvme.h).
 *
 * Bem simples: uma fila de admin e uma fila de I/O com 16 entradas, um
 * comando por vez, sem interrupcoes (quem chama ja esta com cli, como no
 * ATA). Os dados passam por um buffer de 64 KB alinhado a 4 KB, entao o
 * buffer de quem chama pode estar em qualquer endereco.
 *
 * Seguranca: o SSD tem o Linux nas outras particoes. Todo acesso passa por
 * nvme_io(), que soma o inicio da particao e recusa o que passa do fim dela. */
#include "nvme.h"
#include "mm.h"
#include "serial.h"

extern int pci_mmio_map(uint64_t phys, uint64_t len);

#define QN 16u                 /* entradas por fila */
#define BOUNCE (64u * 1024u)   /* maior transferencia por comando */

/* Registradores do controlador */
#define R_CAP  0x00
#define R_CC   0x14
#define R_CSTS 0x1C
#define R_AQA  0x24
#define R_ASQ  0x28
#define R_ACQ  0x30

struct nv {
  volatile uint8_t* bar;
  uint32_t dstrd;              /* passo dos doorbells em bytes */
  uint8_t* asq; uint8_t* acq;  /* admin */
  uint8_t* isq; uint8_t* icq;  /* I/O (fila 1) */
  uint32_t asq_t, acq_h, acq_p;
  uint32_t isq_t, icq_h, icq_p;
  uint16_t cid;
  uint8_t* buf;                /* BOUNCE bytes, alinhado a 4 KB */
  uint64_t* prpl;              /* lista de PRP (uma pagina) */
  uint8_t* idb;                /* 4 KB para IDENTIFY e setores avulsos */
  uint32_t max_sec;            /* setores por comando */
  uint64_t ns_secs;
  uint64_t part_lba;
  uint32_t part_secs;
  char name[41];
  int state;                   /* 0 nunca tentou, 1 pronto, <0 falhou */
};

static struct nv nv;

static inline uint32_t inl_(uint16_t port) {
  uint32_t v;
  __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}
static inline void outl_(uint16_t port, uint32_t v) {
  __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}
static uint32_t pci_rd(int bus, int dev, int fun, int off) {
  outl_(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
               ((uint32_t)fun << 8) | ((uint32_t)off & 0xFCu));
  return inl_(0xCFC);
}
static void pci_wr(int bus, int dev, int fun, int off, uint32_t v) {
  outl_(0xCF8, 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
               ((uint32_t)fun << 8) | ((uint32_t)off & 0xFCu));
  outl_(0xCFC, v);
}

static inline uint32_t r32(uint32_t off) { return *(volatile uint32_t*)(nv.bar + off); }
static inline void w32(uint32_t off, uint32_t v) { *(volatile uint32_t*)(nv.bar + off) = v; }
static uint64_t r64(uint32_t off) { return r32(off) | ((uint64_t)r32(off + 4) << 32); }
static void w64(uint32_t off, uint64_t v) { w32(off, (uint32_t)v); w32(off + 4, (uint32_t)(v >> 32)); }

static uint64_t pa(const void* p) { return (uint64_t)(uintptr_t)p; }

static void zero(void* p, uint32_t n) {
  uint8_t* b = (uint8_t*)p;
  while (n--) *b++ = 0;
}
static void copy(void* d, const void* s, uint32_t n) {
  __asm__ volatile ("cld; rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
}

static void* amem(uint32_t n) {
  uint8_t* raw = (uint8_t*)kmalloc(n + 4096u);
  uint8_t* a;
  if (!raw) return 0;
  a = (uint8_t*)(uintptr_t)((pa(raw) + 4095u) & ~4095ull);
  zero(a, n);
  return a;
}

/* Log na COM1 e na tela: o PC real nao tem serial, e e no boot que o D:
 * pode falhar (assim o motivo aparece sem cabo). */
extern void puts_k(const char* s);
static void nlog(const char* s) {
  serial_puts(s);
  puts_k(s);
}

static void log_hex(const char* s, uint32_t v) {
  static const char hx[] = "0123456789ABCDEF";
  char b[10];
  int i;
  for (i = 0; i < 8; i++) b[i] = hx[(v >> (28 - 4 * i)) & 0xFu];
  b[8] = '\n';
  b[9] = 0;
  nlog(s);
  nlog(b);
}

/* Espera sem relogio (quem chama esta com cli): ~voltas de pause. */
static int wait_rdy(uint32_t want, uint32_t ms) {
  uint64_t n = (uint64_t)ms * 20000u;
  while ((r32(R_CSTS) & 1u) != want) {
    if (r32(R_CSTS) == 0xFFFFFFFFu || !n--) return -1;
    __asm__ volatile ("pause");
  }
  return 0;
}

/* Poe o comando c (64 bytes) na fila e espera a resposta. 0 = ok. */
static int submit(int io, uint32_t* c) {
  uint8_t* sq = io ? nv.isq : nv.asq;
  uint8_t* cq = io ? nv.icq : nv.acq;
  uint32_t* tail = io ? &nv.isq_t : &nv.asq_t;
  uint32_t* head = io ? &nv.icq_h : &nv.acq_h;
  uint32_t* ph = io ? &nv.icq_p : &nv.acq_p;
  uint32_t q = io ? 1u : 0u;
  volatile uint32_t* e;
  uint64_t spins = 20000ull * 10000u;  /* ~10 s */
  uint16_t cid = ++nv.cid;
  uint32_t st;
  c[0] = (c[0] & 0xFFFFu) | ((uint32_t)cid << 16);
  copy(sq + *tail * 64u, c, 64u);
  *tail = (*tail + 1u) % QN;
  __asm__ volatile ("mfence" ::: "memory");
  w32(0x1000u + (2u * q) * nv.dstrd, *tail);
  e = (volatile uint32_t*)(cq + *head * 16u);
  while (((e[3] >> 16) & 1u) != *ph) {
    if (!spins--) {
      log_hex("[nvme] sem resposta, opc=", c[0] & 0xFFu);
      return -1;
    }
    __asm__ volatile ("pause");
  }
  st = (e[3] >> 17) & 0x7FFFu;
  *head = (*head + 1u) % QN;
  if (*head == 0) *ph ^= 1u;
  w32(0x1000u + (2u * q + 1u) * nv.dstrd, *head);
  if (st) {
    log_hex("[nvme] erro, status=", st);
    return -2;
  }
  return 0;
}

static void cmd_clear(uint32_t* c) { zero(c, 64u); }

/* PRP1/PRP2 para bytes no buffer de 64 KB. */
static void set_prp(uint32_t* c, uint8_t* b, uint32_t bytes) {
  uint64_t p1 = pa(b), p2 = 0;
  uint32_t pages = (bytes + 4095u) / 4096u, i;
  if (pages == 2) p2 = p1 + 4096u;
  else if (pages > 2) {
    for (i = 1; i < pages; i++) nv.prpl[i - 1] = p1 + i * 4096ull;
    p2 = pa(nv.prpl);
  }
  c[6] = (uint32_t)p1; c[7] = (uint32_t)(p1 >> 32);
  c[8] = (uint32_t)p2; c[9] = (uint32_t)(p2 >> 32);
}

static int identify(uint32_t nsid, uint32_t cns) {
  uint32_t c[16];
  cmd_clear(c);
  c[0] = 0x06;
  c[1] = nsid;
  set_prp(c, nv.idb, 4096u);
  c[10] = cns;
  return submit(0, c);
}

/* Le/grava n setores do namespace 1 (lba absoluto) via buffer. */
static int raw_io(int write, uint64_t lba, uint32_t n) {
  uint32_t c[16];
  cmd_clear(c);
  c[0] = write ? 0x01 : 0x02;
  c[1] = 1;
  set_prp(c, nv.buf, n * 512u);
  c[10] = (uint32_t)lba;
  c[11] = (uint32_t)(lba >> 32);
  c[12] = n - 1u;
  return submit(1, c);
}

static int flush(void) {
  uint32_t c[16];
  cmd_clear(c);
  c[0] = 0x00;
  c[1] = 1;
  return submit(1, c);
}

static int ctrl_up(int bus, int dev, int fun) {
  uint32_t bar0 = pci_rd(bus, dev, fun, 0x10);
  uint32_t bar1 = ((bar0 & 6u) == 4u) ? pci_rd(bus, dev, fun, 0x14) : 0;
  uint64_t base = ((uint64_t)bar1 << 32) | (bar0 & ~0xFu);
  uint32_t cmd = pci_rd(bus, dev, fun, 0x04), mdts, i;
  uint64_t cap;
  uint32_t c[16];
  if (!base || (bar0 & 1u)) return -1;
  /* memoria + bus master, INTx desligado (so polling) */
  pci_wr(bus, dev, fun, 0x04, (cmd & 0xFFFFu) | 0x6u | 0x400u);
  if (pci_mmio_map(base, 0x4000u) != 0) {
    log_hex("[nvme] BAR fora do alcance, alto=", bar1);
    return -1;
  }
  nv.bar = (volatile uint8_t*)(uintptr_t)base;
  cap = r64(R_CAP);
  if ((cap >> 48) & 0xFu) {
    nlog("[nvme] controlador nao aceita paginas de 4 KB\n");
    return -1;
  }
  nv.dstrd = 4u << ((cap >> 32) & 0xFu);
  /* Para o controlador (o firmware pode ter deixado ligado) */
  if (r32(R_CC) & 1u) {
    w32(R_CC, r32(R_CC) & ~1u);
  }
  if (wait_rdy(0, 500u * (uint32_t)(((cap >> 24) & 0xFFu) + 1u)) != 0) {
    nlog("[nvme] controlador nao parou\n");
    return -1;
  }
  nv.asq = (uint8_t*)amem(4096u);
  nv.acq = (uint8_t*)amem(4096u);
  nv.isq = (uint8_t*)amem(4096u);
  nv.icq = (uint8_t*)amem(4096u);
  nv.buf = (uint8_t*)amem(BOUNCE);
  nv.prpl = (uint64_t*)amem(4096u);
  nv.idb = (uint8_t*)amem(4096u);
  if (!nv.asq || !nv.acq || !nv.isq || !nv.icq || !nv.buf || !nv.prpl || !nv.idb) return -1;
  nv.asq_t = nv.acq_h = nv.isq_t = nv.icq_h = 0;
  nv.acq_p = nv.icq_p = 1;
  w32(R_AQA, ((QN - 1u) << 16) | (QN - 1u));
  w64(R_ASQ, pa(nv.asq));
  w64(R_ACQ, pa(nv.acq));
  /* IOCQES=4 (16 B), IOSQES=6 (64 B), MPS=0 (4 KB), CSS=NVM, EN */
  w32(R_CC, (4u << 20) | (6u << 16) | 1u);
  if (wait_rdy(1, 500u * (uint32_t)(((cap >> 24) & 0xFFu) + 1u)) != 0) {
    log_hex("[nvme] controlador nao ligou, CSTS=", r32(R_CSTS));
    return -1;
  }
  /* Controlador: modelo e MDTS */
  if (identify(0, 1) != 0) return -1;
  for (i = 0; i < 40; i++) nv.name[i] = (char)nv.idb[24 + i];
  nv.name[40] = 0;
  for (i = 40; i > 0 && (nv.name[i - 1] == ' ' || nv.name[i - 1] == 0); i--) nv.name[i - 1] = 0;
  mdts = nv.idb[77];
  nv.max_sec = BOUNCE / 512u;
  if (mdts && mdts < 4 && ((4096u << mdts) / 512u) < nv.max_sec) nv.max_sec = (4096u << mdts) / 512u;
  /* Namespace 1: tamanho e setor de 512 bytes */
  if (identify(1, 0) != 0) return -1;
  nv.ns_secs = *(uint64_t*)nv.idb;
  {
    uint32_t f = nv.idb[26] & 0xFu;
    uint32_t lbads = nv.idb[128 + f * 4 + 2];
    if (lbads != 9) {
      log_hex("[nvme] setor nao e de 512 bytes, LBADS=", lbads);
      return -1;
    }
  }
  /* Fila de I/O 1 */
  cmd_clear(c);
  c[0] = 0x05;
  c[6] = (uint32_t)pa(nv.icq); c[7] = (uint32_t)(pa(nv.icq) >> 32);
  c[10] = ((QN - 1u) << 16) | 1u;
  c[11] = 1u;  /* contigua, sem interrupcao */
  if (submit(0, c) != 0) return -1;
  cmd_clear(c);
  c[0] = 0x01;
  c[6] = (uint32_t)pa(nv.isq); c[7] = (uint32_t)(pa(nv.isq) >> 32);
  c[10] = ((QN - 1u) << 16) | 1u;
  c[11] = (1u << 16) | 1u;
  if (submit(0, c) != 0) return -1;
  return 0;
}

static int is_label(const uint8_t* b) {
  static const char want[] = "SODOSHD    ";
  int i, ok32 = 1, ok16 = 1;
  if (b[510] != 0x55 || b[511] != 0xAA) return 0;
  for (i = 0; i < 11; i++) {
    if (b[71 + i] != (uint8_t)want[i]) ok32 = 0;
    if (b[43 + i] != (uint8_t)want[i]) ok16 = 0;
  }
  return ok32 || ok16;
}

/* O Windows grava o rotulo so na entrada de volume do diretorio raiz; no
 * setor de boot (BS_VolLab) fica "NO NAME" (o E: deste PC e assim). O GRUB e
 * o Windows leem o do raiz, entao aqui tambem. FAT32 com setor de 512 bytes:
 * le ate 8 setores do primeiro cluster do raiz. nv.buf tem o setor de boot. */
static int root_label(uint64_t first) {
  static const char want[] = "SODOSHD    ";
  uint8_t bs[512];
  uint32_t spc, rsvd, nf, fsz, rc, s, k, n;
  uint64_t lba;
  copy(bs, nv.buf, 512u);
  if (bs[510] != 0x55 || bs[511] != 0xAA) return 0;
  if (bs[11] != 0 || bs[12] != 2) return 0;
  if (bs[82] != 'F' || bs[83] != 'A' || bs[84] != 'T' || bs[85] != '3' || bs[86] != '2') return 0;
  spc = bs[13];
  rsvd = (uint32_t)bs[14] | ((uint32_t)bs[15] << 8);
  nf = bs[16];
  fsz = *(const uint32_t*)(bs + 36);
  rc = *(const uint32_t*)(bs + 44);
  if (!spc || !nf || !fsz || rc < 2) return 0;
  lba = first + rsvd + (uint64_t)nf * fsz + (uint64_t)(rc - 2u) * spc;
  n = spc > 8u ? 8u : spc;
  for (s = 0; s < n; s++) {
    if (raw_io(0, lba + s, 1) != 0) return 0;
    for (k = 0; k < 512u; k += 32u) {
      const uint8_t* e = nv.buf + k;
      int i, ok = 1;
      if (e[0] == 0) return 0;                       /* fim do diretorio */
      if (e[0] == 0xE5 || e[11] == 0x0F || !(e[11] & 0x08)) continue;
      for (i = 0; i < 11; i++)
        if (e[i] != (uint8_t)want[i]) ok = 0;
      return ok;                                     /* uma so entrada de rotulo */
    }
  }
  return 0;
}

/* Procura na GPT a particao basic data com FAT rotulado SODOSHD. */
static int find_part(void) {
  static const uint8_t basic[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
  uint64_t ent_lba;
  uint32_t n_ent, ent_sz, i, j, per;
  if (raw_io(0, 1, 1) != 0) return -1;
  if (nv.buf[0] != 'E' || nv.buf[1] != 'F' || nv.buf[2] != 'I' || nv.buf[3] != ' ' ||
      nv.buf[4] != 'P' || nv.buf[5] != 'A' || nv.buf[6] != 'R' || nv.buf[7] != 'T') {
    nlog("[nvme] SSD sem GPT\n");
    return -1;
  }
  ent_lba = *(uint64_t*)(nv.buf + 72);
  n_ent = *(uint32_t*)(nv.buf + 80);
  ent_sz = *(uint32_t*)(nv.buf + 84);
  if (ent_sz < 128 || ent_sz > 512 || (512 % ent_sz) || n_ent > 1024) return -1;
  per = 512u / ent_sz;
  for (i = 0; i < (n_ent + per - 1) / per; i++) {
    uint8_t sec[512];
    if (raw_io(0, ent_lba + i, 1) != 0) return -1;
    copy(sec, nv.buf, 512u);
    for (j = 0; j < per; j++) {
      const uint8_t* e = sec + j * ent_sz;
      uint64_t first = *(const uint64_t*)(e + 32), last = *(const uint64_t*)(e + 40);
      int k, same = 1;
      for (k = 0; k < 16; k++)
        if (e[k] != basic[k]) same = 0;
      if (!same || last < first || last >= nv.ns_secs) continue;
      if (last - first + 1u > 0xFFFFFFFFull) continue;
      if (raw_io(0, first, 1) != 0) return -1;
      if (!is_label(nv.buf) && !root_label(first)) continue;
      nv.part_lba = first;
      nv.part_secs = (uint32_t)(last - first + 1u);
      return 0;
    }
  }
  nlog("[nvme] nenhuma particao FAT com rotulo SODOSHD\n");
  return -1;
}

int nvme_init(void) {
  int bus, dev, fun;
  if (nv.state == 1) return 0;
  if (nv.state < 0) return nv.state;
  nv.state = -1;
  if (!mm_ready()) return -1;
  for (bus = 0; bus < 256; bus++) {
    for (dev = 0; dev < 32; dev++) {
      int nf = 1;
      for (fun = 0; fun < nf; fun++) {
        uint32_t id = pci_rd(bus, dev, fun, 0);
        if (id == 0xFFFFFFFFu || id == 0) continue;
        if (fun == 0 && (pci_rd(bus, dev, 0, 0x0C) & 0x00800000u)) nf = 8;
        if ((pci_rd(bus, dev, fun, 0x08) >> 8) != 0x010802u) continue;
        log_hex("[nvme] controlador ", (uint32_t)((bus << 8) | (dev << 3) | fun));
        if (ctrl_up(bus, dev, fun) != 0) continue;
        nlog("[nvme] ");
        nlog(nv.name);
        nlog("\n");
        if (find_part() != 0) continue;
        nlog("[nvme] ");
        nlog(nv.name);
        log_hex(": particao SODOSHD no setor ", (uint32_t)nv.part_lba);
        nv.state = 1;
        return 0;
      }
    }
  }
  nlog("[nvme] nada encontrado\n");
  return -1;
}

uint32_t nvme_sectors(void) { return nv.state == 1 ? nv.part_secs : 0; }
uint64_t nvme_part_lba(void) { return nv.part_lba; }
const char* nvme_name(void) { return nv.name; }

static int nvme_io(int write, uint32_t lba, uint32_t count, uint8_t* p) {
  if (nv.state != 1 || !p || count == 0) return -2;
  if (lba >= nv.part_secs || count > nv.part_secs - lba) return -1;
  while (count) {
    uint32_t k = count > nv.max_sec ? nv.max_sec : count;
    if (write) copy(nv.buf, p, k * 512u);
    if (raw_io(write, nv.part_lba + lba, k) != 0) return -3;
    if (!write) copy(p, nv.buf, k * 512u);
    p += k * 512u;
    lba += k;
    count -= k;
  }
  if (write && flush() != 0) return -3;
  return 0;
}

int nvme_read(uint32_t lba, uint32_t count, void* buf) {
  return nvme_io(0, lba, count, (uint8_t*)buf);
}

int nvme_write(uint32_t lba, uint32_t count, const void* buf) {
  return nvme_io(1, lba, count, (uint8_t*)(uintptr_t)buf);
}
