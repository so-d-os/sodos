/* Pilha minima: NE2000 em polling, ARP, IPv4, UDP (DNS A) e um cliente TCP.
 * Porta em 0x7C000 (magic NET1, connect/send/recv/close) para o curl.asm.
 * Endereco fixo do SLIRP: 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3. */
#include <stdint.h>
#include "net.h"
#include "httpd.h"
#include "sched.h"
#include "serial.h"
#include "shell.h"
#include "mm.h"

#define OUR_IP   0x0A00020Fu
#define OUR_MASK 0xFFFFFF00u
#define OUR_GW   0x0A000202u
#define OUR_DNS  0x0A000203u
#define GATE     0x0007C000u
#define NET_MAGIC 0x3154454Eu

#define TXPAGE 0x40
#define PSTART 0x46
#define PSTOP  0x80

#define ST_CLOSED    0
#define ST_SYN_SENT  1
#define ST_EST       2
#define ST_CLOSEWAIT 3

static inline void outb(uint16_t port, uint8_t v) {
  __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
  uint8_t v;
  __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}
static inline void outw(uint16_t port, uint16_t v) {
  __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
  uint16_t v;
  __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}
static inline void outl(uint16_t port, uint32_t v) {
  __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
  uint32_t v;
  __asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
  return v;
}

static void mem_copy(void* dst, const void* src, int n) {
  uint8_t* d = (uint8_t*)dst;
  const uint8_t* s = (const uint8_t*)src;
  int i;
  for (i = 0; i < n; i++) d[i] = s[i];
}

static void mem_set(void* dst, int v, int n) {
  uint8_t* d = (uint8_t*)dst;
  int i;
  for (i = 0; i < n; i++) d[i] = (uint8_t)v;
}

static int mac_eq(const uint8_t* a, const uint8_t* b) {
  int i;
  for (i = 0; i < 6; i++) if (a[i] != b[i]) return 0;
  return 1;
}

static int mac_bcast(const uint8_t* m) {
  int i;
  for (i = 0; i < 6; i++) if (m[i] != 0xFF) return 0;
  return 1;
}

static void wr16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static uint16_t rd16(const uint8_t* p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void wr32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static uint32_t rd32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

static uint32_t sum_bytes(uint32_t sum, const uint8_t* p, int n) {
  int i;
  for (i = 0; i + 1 < n; i += 2)
    sum += ((uint32_t)p[i] << 8) | p[i + 1];
  if (n & 1) sum += (uint32_t)p[n - 1] << 8;
  return sum;
}

static uint16_t fold(uint32_t sum) {
  while (sum >> 16)
    sum = (sum & 0xFFFFu) + (sum >> 16);
  return (uint16_t)sum;
}

static uint16_t ip_checksum(const uint8_t* h, int n) {
  return (uint16_t)~fold(sum_bytes(0, h, n));
}

static uint16_t nic_io;
static int nic_ok;
static int polling;

/* PIT canal 0 em modo 2, divisor 1193: um estouro por milissegundo. */
static uint32_t ms_now;
static uint16_t pit_prev;
static int pit_ok;

static uint16_t pit_read(void) {
  uint16_t lo;
  uint16_t hi;
  outb(0x43, 0x00);
  lo = inb(0x40);
  hi = inb(0x40);
  return (uint16_t)(lo | (hi << 8));
}

static void time_init(void) {
  int i;
  uint16_t a;
  outb(0x43, 0x34);
  outb(0x40, 0xA9);
  outb(0x40, 0x04);
  a = pit_read();
  pit_prev = a;
  for (i = 0; i < 200000; i++) {
    uint16_t b = pit_read();
    if (b != a) {
      pit_ok = 1;
      pit_prev = b;
      break;
    }
  }
}

static void time_poll(void) {
  uint16_t now;
  if (!pit_ok) return;
  now = pit_read();
  if (now > pit_prev) ms_now++;
  pit_prev = now;
}

/* 1 quando estourou o prazo. 2 se o usuario teclou ou clicou na janela.
 * Sem PIT, cai na contagem de voltas. */
static int wait_done(uint32_t* spins, uint32_t t0, uint32_t ms, uint32_t spin_max) {
  int done;
  extern int gui_input_pending(void);
  time_poll();
  if (pit_ok) done = (ms_now - t0) >= ms;
  else done = ++(*spins) >= spin_max;
  /* Cada volta entrega teclado e rato. A espera nao prende o desktop. */
  sched_yield();
  if (gui_input_pending()) return 2;
  return done;
}
static uint8_t our_mac[6];
static uint16_t ip_id = 1;

static uint8_t rxframe[1600];
static uint8_t txframe[1600];
static uint8_t wire[1600];

static uint32_t arp_ip;
static uint8_t arp_mac[6];
static int arp_valid;

static uint16_t dns_wait_id;
static uint16_t dns_wait_port;
static int dns_got;
static uint8_t dns_pkt[1400];
static int dns_pkt_len;

static uint16_t ping_id = 0x534F;
static uint16_t ping_seq;
static uint32_t ping_dip;
static int ping_got;
static uint8_t ping_ttl;
static uint8_t icmp_buf[1480];

static int tcp_state;
static int tcp_rst;
static int tcp_fin;
static uint16_t tcp_sport;
static uint16_t tcp_ephem = 49152;
static uint32_t tcp_isn = 0x20000000u;
static uint16_t tcp_dport;
static uint32_t tcp_dip;
static uint32_t tcp_snd_una;
static uint32_t tcp_snd_nxt;
static uint32_t tcp_rcv_nxt;
/* 4 KB no kernel; net_init troca por 32 KB do heap alto (registros TLS de 16 KB). */
static uint8_t tcp_rx_low[4096];
static uint8_t* tcp_rx = tcp_rx_low;
static int tcp_rx_cap = (int)sizeof(tcp_rx_low);
static int tcp_rx_len;
static uint8_t tcp_last[1000];
static int tcp_last_len;
static uint8_t tcp_last_flags;
static uint32_t tcp_last_seq;

static void nic_poll(void);
static int ip_send(uint32_t dst, uint8_t proto, const uint8_t* data, int len);

static uint32_t pci_r32(uint8_t dev, uint8_t fun, uint8_t off) {
  uint32_t addr = 0x80000000u | ((uint32_t)dev << 11) | ((uint32_t)fun << 8) |
                  (uint32_t)(off & 0xFCu);
  outl(0xCF8, addr);
  return inl(0xCFC);
}

static void pci_w32(uint8_t dev, uint8_t fun, uint8_t off, uint32_t val) {
  uint32_t addr = 0x80000000u | ((uint32_t)dev << 11) | ((uint32_t)fun << 8) |
                  (uint32_t)(off & 0xFCu);
  outl(0xCF8, addr);
  outl(0xCFC, val);
}

static void ne_out(uint16_t off, uint8_t v) { outb((uint16_t)(nic_io + off), v); }
static uint8_t ne_in(uint16_t off) { return inb((uint16_t)(nic_io + off)); }

static void ne_dma_write(uint16_t addr, const uint8_t* src, int len) {
  int n = (len + 1) & ~1;
  int i;
  ne_out(0x00, 0x22);
  ne_out(0x0A, (uint8_t)n);
  ne_out(0x0B, (uint8_t)(n >> 8));
  ne_out(0x08, (uint8_t)addr);
  ne_out(0x09, (uint8_t)(addr >> 8));
  ne_out(0x00, 0x12);
  for (i = 0; i < n; i += 2) {
    uint16_t w = src[i];
    if (i + 1 < len) w |= (uint16_t)src[i + 1] << 8;
    outw((uint16_t)(nic_io + 0x10), w);
  }
  ne_out(0x07, 0x40);
  ne_out(0x00, 0x22);
}

static void ne_dma_read(uint16_t addr, uint8_t* dst, int len) {
  int n = (len + 1) & ~1;
  int i;
  ne_out(0x00, 0x22);
  ne_out(0x0A, (uint8_t)n);
  ne_out(0x0B, (uint8_t)(n >> 8));
  ne_out(0x08, (uint8_t)addr);
  ne_out(0x09, (uint8_t)(addr >> 8));
  ne_out(0x00, 0x0A);
  for (i = 0; i < n; i += 2) {
    uint16_t w = inw((uint16_t)(nic_io + 0x10));
    if (i < len) dst[i] = (uint8_t)w;
    if (i + 1 < len) dst[i + 1] = (uint8_t)(w >> 8);
  }
  ne_out(0x07, 0x40);
  ne_out(0x00, 0x22);
}

static int ne_tx(const uint8_t* frame, int len) {
  int i;
  int n;
  int spins;
  if (!nic_ok || len < 14) return -1;
  if (len > 1514) len = 1514;
  n = len < 60 ? 60 : len;
  for (i = 0; i < len; i++) wire[i] = frame[i];
  for (; i < n; i++) wire[i] = 0;
  for (spins = 0; spins < 100000; spins++) {
    if ((ne_in(0x00) & 0x04) == 0) break;
  }
  ne_dma_write((uint16_t)(TXPAGE << 8), wire, n);
  ne_out(0x04, TXPAGE);
  ne_out(0x05, (uint8_t)n);
  ne_out(0x06, (uint8_t)(n >> 8));
  ne_out(0x00, 0x26);
  return 0;
}

static void arp_learn(uint32_t ip, const uint8_t* mac) {
  int i;
  if (ip == 0 || ip == 0xFFFFFFFFu) return;
  arp_ip = ip;
  for (i = 0; i < 6; i++) arp_mac[i] = mac[i];
  arp_valid = 1;
}

static void arp_send(uint16_t op, const uint8_t* dmac, uint32_t dip) {
  uint8_t* f = txframe;
  int i;
  mem_set(f, 0, 42);
  for (i = 0; i < 6; i++) f[i] = dmac[i];
  for (i = 0; i < 6; i++) f[6 + i] = our_mac[i];
  wr16(f + 12, 0x0806);
  wr16(f + 14, 1);
  wr16(f + 16, 0x0800);
  f[18] = 6;
  f[19] = 4;
  wr16(f + 20, op);
  for (i = 0; i < 6; i++) f[22 + i] = our_mac[i];
  wr32(f + 28, OUR_IP);
  for (i = 0; i < 6; i++) f[32 + i] = (op == 2) ? dmac[i] : 0;
  wr32(f + 38, dip);
  ne_tx(f, 42);
}

static int arp_lookup(uint32_t ip, uint8_t* mac) {
  int i;
  if (!arp_valid || arp_ip != ip) return -1;
  for (i = 0; i < 6; i++) mac[i] = arp_mac[i];
  return 0;
}

static int arp_resolve(uint32_t ip, uint8_t* mac) {
  static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  int tries;
  int spins;
  if (arp_lookup(ip, mac) == 0) return 0;
  for (tries = 0; tries < 4; tries++) {
    arp_send(1, bcast, ip);
    for (spins = 0; spins < 200000; spins++) {
      nic_poll();
      if (arp_lookup(ip, mac) == 0) return 0;
    }
  }
  return -1;
}

static void arp_rx(const uint8_t* f, int len) {
  uint16_t op;
  uint32_t sip;
  uint32_t tip;
  if (len < 42) return;
  if (rd16(f + 14) != 1 || rd16(f + 16) != 0x0800) return;
  if (f[18] != 6 || f[19] != 4) return;
  op = rd16(f + 20);
  sip = rd32(f + 28);
  tip = rd32(f + 38);
  arp_learn(sip, f + 22);
  if (op == 1 && tip == OUR_IP)
    arp_send(2, f + 22, sip);
}

static int ip_send(uint32_t dst, uint8_t proto, const uint8_t* data, int len) {
  uint8_t* f = txframe;
  uint8_t hopmac[6];
  uint32_t hop;
  int total;
  int i;
  if (len < 0 || len > 1480) return -1;
  hop = ((dst ^ OUR_IP) & OUR_MASK) ? OUR_GW : dst;
  if (arp_resolve(hop, hopmac) < 0) return -1;
  mem_set(f, 0, 14 + 20);
  for (i = 0; i < 6; i++) f[i] = hopmac[i];
  for (i = 0; i < 6; i++) f[6 + i] = our_mac[i];
  wr16(f + 12, 0x0800);
  total = 20 + len;
  f[14] = 0x45;
  wr16(f + 16, (uint16_t)total);
  wr16(f + 18, ip_id++);
  wr16(f + 20, 0x4000);
  f[22] = 64;
  f[23] = proto;
  wr32(f + 26, OUR_IP);
  wr32(f + 30, dst);
  wr16(f + 24, ip_checksum(f + 14, 20));
  for (i = 0; i < len; i++) f[34 + i] = data[i];
  return ne_tx(f, 14 + total);
}

static int dns_skip(const uint8_t* p, int n, int i) {
  int guard = 0;
  while (i < n && guard++ < 40) {
    uint8_t l = p[i];
    if (l == 0) return i + 1;
    if ((l & 0xC0) == 0xC0) return (i + 2 <= n) ? i + 2 : -1;
    if (i + 1 + l > n) return -1;
    i += 1 + l;
  }
  return -1;
}

/* Nome DNS (com ponteiros de compressao) em texto minusculo, sem ponto final. */
static int dns_expand(const uint8_t* p, int n, int i, char* out, int max) {
  int pos = 0;
  int guard = 0;
  int jumps = 0;
  int k;
  if (max < 2) return -1;
  while (guard++ < 64) {
    uint8_t l;
    if (i < 0 || i >= n) return -1;
    l = p[i];
    if (l == 0) {
      if (pos > 0 && out[pos - 1] == '.') pos--;
      out[pos] = 0;
      return 0;
    }
    if ((l & 0xC0) == 0xC0) {
      if (i + 1 >= n || ++jumps > 10) return -1;
      i = ((l & 0x3F) << 8) | p[i + 1];
      continue;
    }
    if ((l & 0xC0) != 0 || i + 1 + l > n || pos + l + 1 >= max) return -1;
    for (k = 0; k < l; k++) {
      char c = (char)p[i + 1 + k];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      out[pos++] = c;
    }
    out[pos++] = '.';
    i += 1 + l;
  }
  return -1;
}

static int dns_eq(const char* a, const char* b) {
  int i;
  for (i = 0;; i++) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
    if (ca != cb) return 0;
    if (ca == 0) return 1;
  }
}

/* Copia os A do nome pedido, seguindo CNAME no mesmo pacote.
 * Devolve a quantidade, 0 se a resposta nao tem endereco, -1 se o pacote e invalido,
 * -2 se o nome nao existe. */
static int dns_collect(const uint8_t* p, int len, const char* qname,
                       uint32_t* ips, int maxips, char* cname, int cname_max) {
  char cur[256];
  char owner[256];
  char target[256];
  int an, qd, q, i, off, nips, rcode, ci;
  if (len < 12 || (rd16(p + 2) & 0x8000) == 0) return -1;
  rcode = rd16(p + 2) & 0x000F;
  an = rd16(p + 6);
  if (rcode == 3 || (rcode != 0 && an == 0)) return -2;
  off = 12;
  qd = rd16(p + 4);
  for (q = 0; q < qd; q++) {
    off = dns_skip(p, len, off);
    if (off < 0 || off + 4 > len) return -1;
    off += 4;
  }
  ci = 0;
  while (qname[ci] && ci < (int)sizeof(cur) - 1) {
    char c = qname[ci];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    cur[ci++] = c;
  }
  cur[ci] = 0;
  if (cname && cname_max > 0) cname[0] = 0;
  nips = 0;
  for (i = 0; i < an; i++) {
    uint16_t typ;
    uint16_t rdlen;
    int name_at = off;
    off = dns_skip(p, len, off);
    if (off < 0 || off + 10 > len) break;
    typ = rd16(p + off);
    rdlen = rd16(p + off + 8);
    off += 10;
    if (off + rdlen > len) break;
    if (dns_expand(p, len, name_at, owner, (int)sizeof(owner)) == 0 && dns_eq(owner, cur)) {
      if (typ == 1 && rdlen == 4 && nips < maxips) {
        uint32_t ip = rd32(p + off);
        if (ip != 0) ips[nips++] = ip;
      } else if (typ == 5 && nips == 0 &&
                 dns_expand(p, len, off, target, (int)sizeof(target)) == 0 &&
                 target[0] && !dns_eq(target, cur)) {
        int k = 0;
        while (target[k] && k < (int)sizeof(cur) - 1) {
          cur[k] = target[k];
          k++;
        }
        cur[k] = 0;
        if (cname && cname_max > 1) {
          if (k > cname_max - 1) k = cname_max - 1;
          for (ci = 0; ci < k; ci++) cname[ci] = cur[ci];
          cname[k] = 0;
        }
      }
    }
    off += rdlen;
  }
  return nips;
}

static void udp_rx(uint32_t src, const uint8_t* p, int len) {
  uint16_t sport;
  uint16_t dport;
  uint16_t ulen;
  int n;
  (void)src;
  if (len < 8) return;
  sport = rd16(p);
  dport = rd16(p + 2);
  ulen = rd16(p + 4);
  if (ulen < 8 || ulen > len) return;
  if (dport != dns_wait_port || sport != 53) return;
  p += 8;
  n = ulen - 8;
  if (n < 12 || n > (int)sizeof(dns_pkt) || rd16(p) != dns_wait_id) return;
  if ((rd16(p + 2) & 0x8000) == 0) return;
  mem_copy(dns_pkt, p, n);
  dns_pkt_len = n;
  dns_got = 1;
}

static int parse_ip(const char* s, uint32_t* out) {
  uint32_t ip = 0;
  int part = 0;
  int acc = 0;
  int digs = 0;
  for (;;) {
    char c = *s++;
    if (c >= '0' && c <= '9') {
      acc = acc * 10 + (c - '0');
      if (acc > 255) return -1;
      digs = 1;
    } else if ((c == '.' || c == '\0') && digs) {
      ip = (ip << 8) | (uint32_t)acc;
      part++;
      acc = 0;
      digs = 0;
      if (c == '\0') break;
    } else {
      return -1;
    }
  }
  if (part != 4) return -1;
  *out = ip;
  return 0;
}

/* Ate maxips enderecos. Segue CNAME. EDNS pede resposta maior que 512 bytes. */
static int dns_lookup(const char* name, uint32_t* ips, int maxips, int depth) {
  uint8_t pkt[320];
  char cname[256];
  int off = 12;
  int tries;
  const char* s = name;
  uint32_t one = 0;
  if (!name || !name[0] || !ips || maxips < 1) return -1;
  if (depth > 4) return -1;
  if (parse_ip(name, &one) == 0) {
    ips[0] = one;
    return 1;
  }
  dns_wait_id = (uint16_t)(dns_wait_id + 1);
  if (dns_wait_id == 0) dns_wait_id = 1;
  dns_wait_port = 53000;
  mem_set(pkt, 0, 12);
  wr16(pkt, dns_wait_id);
  wr16(pkt + 2, 0x0100);
  wr16(pkt + 4, 1);
  wr16(pkt + 10, 1);
  while (*s) {
    const char* dot = s;
    int lab = 0;
    int i;
    while (*dot && *dot != '.') {
      dot++;
      lab++;
    }
    if (lab == 0 || lab > 63 || off + 1 + lab >= (int)sizeof(pkt) - 16) return -1;
    pkt[off++] = (uint8_t)lab;
    for (i = 0; i < lab; i++) pkt[off++] = s[i];
    s = *dot ? dot + 1 : dot;
  }
  pkt[off++] = 0;
  wr16(pkt + off, 1);
  wr16(pkt + off + 2, 1);
  off += 4;
  pkt[off++] = 0;
  wr16(pkt + off, 41);
  wr16(pkt + off + 2, 1232);
  wr32(pkt + off + 4, 0);
  wr16(pkt + off + 8, 0);
  off += 10;
  cname[0] = 0;
  for (tries = 0; tries < 4; tries++) {
    uint32_t spins = 0;
    uint32_t t0;
    uint8_t udp[8 + 320];
    int rc;
    dns_got = 0;
    dns_pkt_len = 0;
    wr16(udp, dns_wait_port);
    wr16(udp + 2, 53);
    wr16(udp + 4, (uint16_t)(8 + off));
    wr16(udp + 6, 0);
    mem_copy(udp + 8, pkt, off);
    if (ip_send(OUR_DNS, 17, udp, 8 + off) < 0) return -1;
    time_poll();
    t0 = ms_now;
    for (;;) {
      nic_poll();
      if (dns_got) break;
      {
        int w = wait_done(&spins, t0, 1500, 8000000u);
        if (w == 2) return -3;
        if (w) break;
      }
    }
    if (!dns_got) continue;
    rc = dns_collect(dns_pkt, dns_pkt_len, name, ips, maxips, cname, (int)sizeof(cname));
    if (rc > 0) return rc;
    if (rc < 0) return -1;
    if (cname[0]) return dns_lookup(cname, ips, maxips, depth + 1);
    return -1;
  }
  return -1;
}

static int dns_resolve(const char* name, uint32_t* out) {
  uint32_t ips[4];
  int n = dns_lookup(name, ips, 4, 0);
  if (n <= 0) return -1;
  *out = ips[0];
  return 0;
}

static int tcp_rx_append(const uint8_t* p, int n) {
  int i;
  if (n < 0 || tcp_rx_len + n > tcp_rx_cap) return -1;
  for (i = 0; i < n; i++) tcp_rx[tcp_rx_len + i] = p[i];
  tcp_rx_len += n;
  return 0;
}

static int tcp_csum_ok(uint32_t src, uint32_t dst, const uint8_t* seg, int len) {
  uint32_t sum = 0;
  sum += (src >> 16) & 0xFFFFu;
  sum += src & 0xFFFFu;
  sum += (dst >> 16) & 0xFFFFu;
  sum += dst & 0xFFFFu;
  sum += 6;
  sum += (uint32_t)len;
  sum = sum_bytes(sum, seg, len);
  return fold(sum) == 0xFFFFu || fold(sum) == 0;
}

static void tcp_raw(uint8_t flags, const uint8_t* data, int len, uint32_t seq, int advance) {
  uint8_t seg[24 + 1000];
  uint32_t sum;
  uint16_t csum;
  int i;
  int win;
  int hdr = 20;
  if (len < 0) len = 0;
  if (len > 1000) len = 1000;
  if ((flags & 0x02) && len == 0) hdr = 24;
  mem_set(seg, 0, hdr);
  wr16(seg, tcp_sport);
  wr16(seg + 2, tcp_dport);
  wr32(seg + 4, seq);
  wr32(seg + 8, tcp_rcv_nxt);
  seg[12] = (uint8_t)((hdr / 4) << 4);
  seg[13] = flags;
  win = tcp_rx_cap - tcp_rx_len;
  if (win > 16384) win = 16384;
  if (win < 0) win = 0;
  wr16(seg + 14, (uint16_t)win);
  if (hdr == 24) {
    seg[20] = 2;
    seg[21] = 4;
    wr16(seg + 22, 1460);
  }
  for (i = 0; i < len; i++) seg[hdr + i] = data[i];
  sum = 0;
  sum += (OUR_IP >> 16) & 0xFFFFu;
  sum += OUR_IP & 0xFFFFu;
  sum += (tcp_dip >> 16) & 0xFFFFu;
  sum += tcp_dip & 0xFFFFu;
  sum += 6;
  sum += (uint32_t)(hdr + len);
  sum = sum_bytes(sum, seg, hdr + len);
  csum = (uint16_t)~fold(sum);
  if (csum == 0) csum = 0xFFFF;
  wr16(seg + 16, csum);
  if (advance) {
    tcp_last_flags = flags;
    tcp_last_len = len;
    tcp_last_seq = seq;
    for (i = 0; i < len; i++) tcp_last[i] = data[i];
  }
  ip_send(tcp_dip, 6, seg, hdr + len);
  if (advance) {
    if (flags & 0x02) tcp_snd_nxt += 1;
    if (flags & 0x01) tcp_snd_nxt += 1;
    tcp_snd_nxt += (uint32_t)len;
  }
}

static void tcp_ack(void) {
  tcp_raw(0x10, 0, 0, tcp_snd_nxt, 0);
}

static void tcp_retransmit(void) {
  tcp_raw(tcp_last_flags, tcp_last, tcp_last_len, tcp_last_seq, 0);
}

static void tcp_input(uint32_t src, const uint8_t* seg, int len) {
  uint16_t sport;
  uint16_t dport;
  uint32_t seq;
  uint32_t ack;
  int doff;
  int flags;
  int plen;
  const uint8_t* data;
  if (tcp_state == ST_CLOSED || len < 20) return;
  if (!tcp_csum_ok(src, OUR_IP, seg, len)) return;
  sport = rd16(seg);
  dport = rd16(seg + 2);
  if (dport != tcp_sport || sport != tcp_dport || src != tcp_dip) return;
  seq = rd32(seg + 4);
  ack = rd32(seg + 8);
  doff = (seg[12] >> 4) * 4;
  if (doff < 20 || doff > len) return;
  flags = seg[13];
  data = seg + doff;
  plen = len - doff;
  if (flags & 0x04) {
    tcp_rst = 1;
    tcp_state = ST_CLOSED;
    return;
  }
  if (tcp_state == ST_SYN_SENT) {
    if ((flags & 0x12) == 0x12 && ack == tcp_snd_nxt) {
      tcp_snd_una = ack;
      tcp_rcv_nxt = seq + 1;
      tcp_state = ST_EST;
      if (plen > 0 && tcp_rx_append(data, plen) == 0)
        tcp_rcv_nxt += (uint32_t)plen;
      if (flags & 0x01) {
        tcp_rcv_nxt += 1;
        tcp_fin = 1;
        tcp_state = ST_CLOSEWAIT;
      }
      tcp_ack();
    }
    return;
  }
  if (flags & 0x10) {
    if ((int32_t)(ack - tcp_snd_una) >= 0 && (int32_t)(tcp_snd_nxt - ack) >= 0)
      tcp_snd_una = ack;
  }
  if (plen > 0 && seq == tcp_rcv_nxt) {
    if (tcp_rx_append(data, plen) == 0)
      tcp_rcv_nxt += (uint32_t)plen;
    else
      plen = 0;
    if ((flags & 0x01) && seq + (uint32_t)plen == tcp_rcv_nxt) {
      tcp_rcv_nxt += 1;
      tcp_fin = 1;
      if (tcp_state == ST_EST) tcp_state = ST_CLOSEWAIT;
    }
    tcp_ack();
  } else if (plen > 0) {
    tcp_ack();
  } else if ((flags & 0x01) && seq == tcp_rcv_nxt) {
    tcp_rcv_nxt += 1;
    tcp_fin = 1;
    if (tcp_state == ST_EST) tcp_state = ST_CLOSEWAIT;
    tcp_ack();
  }
}

static void icmp_rx(uint32_t src, uint8_t ttl, const uint8_t* p, int len) {
  if (len < 8 || len > (int)sizeof(icmp_buf)) return;
  if (fold(sum_bytes(0, p, len)) != 0xFFFFu) return;
  if (p[0] == 8 && p[1] == 0) {
    mem_copy(icmp_buf, p, len);
    icmp_buf[0] = 0;
    wr16(icmp_buf + 2, 0);
    wr16(icmp_buf + 2, ip_checksum(icmp_buf, len));
    ip_send(src, 1, icmp_buf, len);
  } else if (p[0] == 0 && p[1] == 0) {
    if (src == ping_dip && rd16(p + 4) == ping_id && rd16(p + 6) == ping_seq) {
      ping_ttl = ttl;
      ping_got = 1;
    }
  }
}

/* Segunda conexao TCP: escuta. O cliente do navegador continua em tcp_*. */
#define SV_OFF    0
#define SV_LISTEN 1
#define SV_SYN    2
#define SV_EST    3
#define SV_FIN    4

static int sv_state;
static unsigned sv_gen;
static uint16_t sv_port;
static uint16_t sv_rport;
static uint32_t sv_rip;
static uint32_t sv_snd_una, sv_snd_nxt, sv_rcv_nxt;
static uint8_t sv_rx[8192];
static int sv_rx_len;
static int sv_need_syn;
static int sv_need_ack;
static int sv_overflow;
static uint8_t sv_last[1100];
static int sv_last_len;
static uint8_t sv_last_flags;
static uint32_t sv_last_seq;
static uint32_t sv_tx_ms;
static int sv_busy;

static void sv_raw(uint8_t flags, const uint8_t* data, int len, uint32_t seq, int advance) {
  uint8_t seg[24 + 1000];
  uint32_t sum;
  uint16_t csum;
  int i;
  int hdr = 20;
  if (len < 0) len = 0;
  if (len > 1000) len = 1000;
  if ((flags & 0x02) && len == 0) hdr = 24;
  mem_set(seg, 0, hdr);
  wr16(seg, sv_port);
  wr16(seg + 2, sv_rport);
  wr32(seg + 4, seq);
  wr32(seg + 8, sv_rcv_nxt);
  seg[12] = (uint8_t)((hdr / 4) << 4);
  seg[13] = flags;
  wr16(seg + 14, 8192);
  if (hdr == 24) {
    seg[20] = 2;
    seg[21] = 4;
    wr16(seg + 22, 1460);
  }
  for (i = 0; i < len; i++) seg[hdr + i] = data[i];
  sum = 0;
  sum += (OUR_IP >> 16) & 0xFFFFu;
  sum += OUR_IP & 0xFFFFu;
  sum += (sv_rip >> 16) & 0xFFFFu;
  sum += sv_rip & 0xFFFFu;
  sum += 6;
  sum += (uint32_t)(hdr + len);
  sum = sum_bytes(sum, seg, hdr + len);
  csum = (uint16_t)~fold(sum);
  if (csum == 0) csum = 0xFFFF;
  wr16(seg + 16, csum);
  if (advance) {
    sv_last_flags = flags;
    sv_last_len = len;
    sv_last_seq = seq;
    for (i = 0; i < len; i++) sv_last[i] = data[i];
  }
  ip_send(sv_rip, 6, seg, hdr + len);
  time_poll();
  sv_tx_ms = ms_now;
  if (advance) {
    if (flags & 0x02) sv_snd_nxt += 1;
    if (flags & 0x01) sv_snd_nxt += 1;
    sv_snd_nxt += (uint32_t)len;
  }
}

static void sv_reset_listen(void) {
  sv_state = SV_LISTEN;
  sv_rx_len = 0;
  sv_need_syn = 0;
  sv_need_ack = 0;
  sv_overflow = 0;
}

static void srv_input(uint32_t src, const uint8_t* seg, int len) {
  uint16_t sport, dport;
  uint32_t seq, ack;
  int doff, flags, plen;
  const uint8_t* data;
  if (sv_state == SV_OFF || len < 20) return;
  if (!tcp_csum_ok(src, OUR_IP, seg, len)) return;
  sport = rd16(seg);
  dport = rd16(seg + 2);
  if (dport != sv_port) return;
  seq = rd32(seg + 4);
  ack = rd32(seg + 8);
  doff = (seg[12] >> 4) * 4;
  if (doff < 20 || doff > len) return;
  flags = seg[13];
  data = seg + doff;
  plen = len - doff;
  if ((flags & 0x02) && !(flags & 0x10)) {
    sv_rip = src;
    sv_rport = sport;
    sv_rcv_nxt = seq + 1;
    tcp_isn += 0x10000u + (ms_now & 0xFFFFu);
    if (tcp_isn < 0x1000u) tcp_isn = 0x20000000u;
    sv_snd_una = tcp_isn;
    sv_snd_nxt = tcp_isn;
    sv_rx_len = 0;
    sv_overflow = 0;
    sv_need_ack = 0;
    sv_need_syn = 1;
    sv_gen++;
    if (sv_gen == 0) sv_gen = 1;
    sv_state = SV_SYN;
    return;
  }
  if (sv_state == SV_LISTEN) return;
  if (src != sv_rip || sport != sv_rport) return;
  if (flags & 0x04) {
    sv_reset_listen();
    return;
  }
  if (flags & 0x10) {
    if ((int32_t)(ack - sv_snd_una) >= 0 && (int32_t)(sv_snd_nxt - ack) >= 0)
      sv_snd_una = ack;
    if (sv_state == SV_SYN && ack == sv_snd_nxt)
      sv_state = SV_EST;
    if (sv_state == SV_FIN && (int32_t)(sv_snd_una - sv_snd_nxt) >= 0)
      sv_reset_listen();
  }
  if (sv_state != SV_EST && sv_state != SV_FIN) return;
  if (plen > 0 && seq == sv_rcv_nxt) {
    int i;
    if (sv_rx_len + plen > (int)sizeof(sv_rx)) {
      sv_overflow = 1;
    } else {
      for (i = 0; i < plen; i++) sv_rx[sv_rx_len + i] = data[i];
      sv_rx_len += plen;
      sv_rcv_nxt += (uint32_t)plen;
      sv_need_ack = 1;
    }
  } else if (plen > 0) {
    sv_need_ack = 1;
  }
  if ((flags & 0x01) && seq + (uint32_t)(plen > 0 && seq == sv_rcv_nxt - (uint32_t)plen ? plen : 0) == sv_rcv_nxt) {
    sv_rcv_nxt += 1;
    sv_need_ack = 1;
  }
}

static void srv_pump(void) {
  if (sv_state == SV_OFF || sv_busy) return;
  sv_busy = 1;
  if (sv_overflow) {
    sv_raw(0x04, 0, 0, sv_snd_nxt, 0);
    sv_reset_listen();
    sv_busy = 0;
    return;
  }
  if (sv_need_syn && sv_state == SV_SYN) {
    sv_need_syn = 0;
    sv_raw(0x12, 0, 0, sv_snd_nxt, 1);
  }
  if (sv_need_ack && (sv_state == SV_EST || sv_state == SV_FIN || sv_state == SV_SYN)) {
    sv_need_ack = 0;
    if (sv_state == SV_EST)
      sv_raw(0x10, 0, 0, sv_snd_nxt, 0);
  }
  if ((sv_state == SV_SYN || sv_state == SV_FIN) &&
      (int32_t)(sv_snd_nxt - sv_snd_una) > 0 &&
      ms_now - sv_tx_ms > 400) {
    sv_raw(sv_last_flags, sv_last, sv_last_len, sv_last_seq, 0);
  }
  sv_busy = 0;
}

void net_srv_listen(int port) {
  if (port <= 0 || port > 65535) return;
  sv_port = (uint16_t)port;
  sv_reset_listen();
}

int net_srv_take(char* buf, int cap) {
  int i, hend, body, have, need;
  int clen = -1;
  if (!buf || cap < 2 || sv_state != SV_EST) return 0;
  hend = -1;
  for (i = 0; i + 3 < sv_rx_len; i++) {
    if (sv_rx[i] == '\r' && sv_rx[i + 1] == '\n' &&
        sv_rx[i + 2] == '\r' && sv_rx[i + 3] == '\n') {
      hend = i + 4;
      break;
    }
  }
  if (hend < 0) return 0;
  for (i = 0; i + 15 < hend; i++) {
    const char* k = "content-length:";
    int j = 0;
    while (k[j] && i + j < hend) {
      char c = (char)sv_rx[i + j];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (c != k[j]) break;
      j++;
    }
    if (k[j]) continue;
    while (i + j < hend && sv_rx[i + j] == ' ') j++;
    clen = 0;
    while (i + j < hend && sv_rx[i + j] >= '0' && sv_rx[i + j] <= '9') {
      clen = clen * 10 + sv_rx[i + j] - '0';
      j++;
    }
    break;
  }
  body = sv_rx_len - hend;
  need = clen < 0 ? 0 : clen;
  if (body < need) return 0;
  have = hend + (need > 0 ? need : body);
  if (clen < 0) have = hend;
  if (have >= cap) {
    sv_overflow = 1;
    return -1;
  }
  for (i = 0; i < have; i++) buf[i] = (char)sv_rx[i];
  buf[have] = 0;
  sv_rx_len = 0;
  return have;
}

static int sv_wait_una(uint32_t goal) {
  uint32_t t0;
  time_poll();
  t0 = ms_now;
  while ((int32_t)(sv_snd_una - goal) < 0) {
    if (sv_state == SV_LISTEN || sv_state == SV_OFF) return -1;
    nic_poll();
    time_poll();
    if (ms_now - t0 > 3000u) return -1;
  }
  return 0;
}

unsigned net_srv_gen(void) { return sv_gen; }

int net_srv_write(const void* data, int len, int fin) {
  const uint8_t* p = (const uint8_t*)data;
  int left;
  if (sv_state != SV_EST || len < 0) return -1;
  left = len;
  if (left == 0 && fin) {
    sv_raw(0x11, 0, 0, sv_snd_nxt, 1);
    if (sv_wait_una(sv_snd_nxt) < 0) {
      sv_reset_listen();
      return -1;
    }
  }
  while (left > 0) {
    int n = left > 1000 ? 1000 : left;
    int last = fin && n == left;
    uint32_t goal;
    sv_raw(last ? 0x19 : 0x18, p, n, sv_snd_nxt, 1);
    goal = sv_snd_nxt;
    p += n;
    left -= n;
    if (sv_wait_una(goal) < 0) {
      sv_reset_listen();
      return -1;
    }
  }
  if (fin) {
    sv_state = SV_FIN;
    sv_rx_len = 0;
  }
  return 0;
}

int net_srv_send(const void* data, int len) {
  return net_srv_write(data, len, 1);
}

static void ip_rx(const uint8_t* p, int len) {
  int ihl;
  int total;
  uint16_t got;
  uint8_t hdr[60];
  uint32_t src;
  uint32_t dst;
  int plen;
  if (len < 20) return;
  if ((p[0] >> 4) != 4) return;
  ihl = (p[0] & 0x0F) * 4;
  if (ihl < 20 || ihl > 60 || ihl > len) return;
  total = rd16(p + 2);
  if (total < ihl || total > len) return;
  if ((rd16(p + 6) & 0x3FFF) != 0) return;
  dst = rd32(p + 16);
  if (dst != OUR_IP) return;
  mem_copy(hdr, p, ihl);
  got = rd16(p + 10);
  hdr[10] = 0;
  hdr[11] = 0;
  if (ip_checksum(hdr, ihl) != got) return;
  src = rd32(p + 12);
  plen = total - ihl;
  if (p[9] == 17) udp_rx(src, p + ihl, plen);
  else if (p[9] == 6) {
    tcp_input(src, p + ihl, plen);
    srv_input(src, p + ihl, plen);
  }
  else if (p[9] == 1) icmp_rx(src, p[8], p + ihl, plen);
}

static void eth_rx(const uint8_t* f, int len) {
  uint16_t typ;
  if (len < 14) return;
  if (!mac_eq(f, our_mac) && !mac_bcast(f)) return;
  typ = rd16(f + 12);
  if (typ == 0x0806) arp_rx(f, len);
  else if (typ == 0x0800) ip_rx(f + 14, len - 14);
}

static int ne_rx_one(void) {
  uint8_t curr;
  uint8_t bnry;
  uint8_t page;
  uint8_t hdr[4];
  int count;
  int len;
  uint8_t next;
  uint8_t nb;
  ne_out(0x00, 0x62);
  curr = ne_in(0x07);
  ne_out(0x00, 0x22);
  bnry = ne_in(0x03);
  page = (uint8_t)(bnry + 1);
  if (page >= PSTOP) page = PSTART;
  if (page == curr) return 0;
  ne_dma_read((uint16_t)(page << 8), hdr, 4);
  count = hdr[2] | (hdr[3] << 8);
  next = hdr[1];
  if ((hdr[0] & 0x01) == 0 || count < 18 || count > 1518 ||
      next < PSTART || next >= PSTOP) {
    nb = (uint8_t)(curr - 1);
    if (nb < PSTART) nb = PSTOP - 1;
    ne_out(0x03, nb);
    return 0;
  }
  len = count - 4;
  if (len > (int)sizeof(rxframe)) len = (int)sizeof(rxframe);
  ne_dma_read((uint16_t)((page << 8) + 4), rxframe, len);
  nb = (uint8_t)(next - 1);
  if (next == PSTART) nb = PSTOP - 1;
  ne_out(0x03, nb);
  eth_rx(rxframe, len);
  return 1;
}

static void nic_poll(void) {
  int n = 0;
  time_poll();
  if (!nic_ok || polling) return;
  polling = 1;
  while (n++ < 8 && ne_rx_one()) {}
  polling = 0;
  srv_pump();
  httpd_poll();
}

void net_poll(void) { nic_poll(); }

static int ne_reset(uint16_t io) {
  int i;
  uint8_t prom[32];
  nic_io = io;
  outb((uint16_t)(io + 0x1F), inb((uint16_t)(io + 0x1F)));
  for (i = 0; i < 100000; i++) {
    if (inb((uint16_t)(io + 0x07)) & 0x80) break;
  }
  outb(io, 0x21);
  outb((uint16_t)(io + 0x0E), 0x49);
  outb((uint16_t)(io + 0x0A), 0);
  outb((uint16_t)(io + 0x0B), 0);
  outb((uint16_t)(io + 0x0C), 0x20);
  outb((uint16_t)(io + 0x0D), 0x02);
  outb((uint16_t)(io + 0x0A), 32);
  outb((uint16_t)(io + 0x0B), 0);
  outb((uint16_t)(io + 0x08), 0);
  outb((uint16_t)(io + 0x09), 0);
  outb(io, 0x0A);
  for (i = 0; i < 16; i++) {
    uint16_t w = inw((uint16_t)(io + 0x10));
    prom[i * 2] = (uint8_t)w;
    prom[i * 2 + 1] = (uint8_t)(w >> 8);
  }
  for (i = 0; i < 6; i++) our_mac[i] = prom[i * 2];
  outb(io, 0x21);
  outb((uint16_t)(io + 0x07), 0xFF);
  outb((uint16_t)(io + 0x01), PSTART);
  outb((uint16_t)(io + 0x02), PSTOP);
  outb((uint16_t)(io + 0x03), PSTART);
  outb((uint16_t)(io + 0x04), TXPAGE);
  outb((uint16_t)(io + 0x0F), 0x00);
  outb(io, 0x61);
  for (i = 0; i < 6; i++) outb((uint16_t)(io + 1 + i), our_mac[i]);
  for (i = 0; i < 8; i++) outb((uint16_t)(io + 8 + i), 0);
  outb((uint16_t)(io + 0x07), PSTART + 1);
  outb(io, 0x21);
  outb((uint16_t)(io + 0x0C), 0x04);
  outb((uint16_t)(io + 0x0D), 0x00);
  outb(io, 0x22);
  if (our_mac[0] == 0xFF && our_mac[1] == 0xFF) return -1;
  return 0;
}

static int nic_find(uint16_t* io_out) {
  uint8_t dev;
  uint8_t fun;
  for (dev = 0; dev < 32; dev++) {
    for (fun = 0; fun < 8; fun++) {
      uint32_t id = pci_r32(dev, fun, 0);
      uint32_t bar;
      uint32_t cmd;
      if (id == 0xFFFFFFFFu) {
        if (fun == 0) break;
        continue;
      }
      if (id != 0x802910ECu) {
        if (fun == 0) {
          uint8_t ht = (uint8_t)(pci_r32(dev, 0, 0x0C) >> 16);
          if ((ht & 0x80u) == 0) break;
        }
        continue;
      }
      bar = pci_r32(dev, fun, 0x10);
      if ((bar & 1u) == 0 || (bar & ~3u) == 0) {
        pci_w32(dev, fun, 0x10, 0x0000C101u);
        bar = pci_r32(dev, fun, 0x10);
      }
      cmd = pci_r32(dev, fun, 0x04);
      pci_w32(dev, fun, 0x04, (cmd & 0x0000FFFFu) | 1u);
      *io_out = (uint16_t)(bar & ~3u);
      return *io_out ? 0 : -1;
    }
  }
  return -1;
}

static void gate_set(uint32_t magic, uint32_t c, uint32_t s, uint32_t r, uint32_t cl) {
  volatile uint32_t* g = (volatile uint32_t*)GATE;
  g[0] = magic;
  g[1] = c;
  g[2] = s;
  g[3] = r;
  g[4] = cl;
}

static int api_connect(const char* host, int port);
static int api_send(const void* data, int len);
static int api_recv(void* data, int cap);
static void api_close(void);

static uint32_t fn_bits(void* fp) {
  uint32_t u = 0;
  mem_copy(&u, &fp, 4);
  return u;
}

static int tcp_wait_una(void) {
  uint32_t spins = 0;
  int tries = 0;
  uint32_t goal = tcp_snd_nxt;
  uint32_t t0;
  time_poll();
  t0 = ms_now;
  while (!tcp_rst && tcp_state != ST_CLOSED) {
    nic_poll();
    if ((int32_t)(tcp_snd_una - goal) >= 0) return 0;
    {
      int w = wait_done(&spins, t0, 500, 2500000u);
      if (w == 2) return -3;
      if (!w) continue;
    }
    spins = 0;
    time_poll();
    t0 = ms_now;
    if (++tries >= 8) return -1;
    tcp_retransmit();
  }
  return -1;
}

static uint16_t tcp_new_port(void) {
  uint16_t p = tcp_ephem++;
  if (tcp_ephem < 49152 || tcp_ephem > 64000) tcp_ephem = 49152;
  return p;
}

static uint32_t tcp_new_isn(void) {
  time_poll();
  tcp_isn += 0x10000u + (ms_now & 0xFFFFu);
  if (tcp_isn < 0x1000u) tcp_isn = 0x20000000u;
  return tcp_isn;
}

/* Porta e numero de sequencia novos a cada tentativa: o mesmo 4-tuple
 * em TIME_WAIT no servidor recusava o segundo acesso ao mesmo site. */
static int tcp_connect_ip(uint32_t ip, int port) {
  int attempt;
  for (attempt = 0; attempt < 3; attempt++) {
    int saw_rst;
    api_close();
    tcp_rst = 0;
    tcp_fin = 0;
    tcp_rx_len = 0;
    tcp_dip = ip;
    tcp_dport = (uint16_t)port;
    tcp_sport = tcp_new_port();
    tcp_snd_una = tcp_new_isn();
    tcp_snd_nxt = tcp_snd_una;
    tcp_rcv_nxt = 0;
    tcp_state = ST_SYN_SENT;
    tcp_raw(0x02, 0, 0, tcp_snd_nxt, 1);
    {
      int u = tcp_wait_una();
      if (u == -3) return -3;
      if (u == 0 && tcp_state != ST_CLOSED && tcp_state != ST_SYN_SENT)
        return 0;
    }
    saw_rst = tcp_rst;
    tcp_state = ST_CLOSED;
    if (!saw_rst) return -1;
  }
  return -1;
}

static int api_connect(const char* host, int port) {
  uint32_t ips[4];
  int n;
  int i;
  if (!nic_ok || !host || port <= 0 || port > 65535) return -1;
  n = dns_lookup(host, ips, 4, 0);
  if (n == -3) return -3;
  if (n <= 0) {
    serial_puts("[net] dns falhou\n");
    return -1;
  }
  for (i = 0; i < n; i++) {
    int c = tcp_connect_ip(ips[i], port);
    if (c == -3) return -3;
    if (c == 0) return 0;
  }
  tcp_state = ST_CLOSED;
  serial_puts("[net] tcp falhou\n");
  return -1;
}

static int api_send(const void* data, int len) {
  const uint8_t* p = (const uint8_t*)data;
  int left;
  if (!nic_ok || tcp_state != ST_EST || !data || len < 0) return -1;
  left = len;
  while (left > 0) {
    int n = left > 1000 ? 1000 : left;
    tcp_raw(0x18, p, n, tcp_snd_nxt, 1);
    {
      int u = tcp_wait_una();
      if (u == -3) return -3;
      if (u < 0) return -1;
    }
    p += n;
    left -= n;
  }
  return len;
}

static int api_recv(void* data, int cap) {
  uint8_t* dst = (uint8_t*)data;
  uint32_t spins = 0;
  uint32_t t0;
  extern int gui_input_pending(void);
  if (!nic_ok || !data || cap < 2) return -1;
  /* O clique ganha dos bytes ja recebidos: um download rapido nao esconde a tecla. */
  sched_yield();
  if (gui_input_pending()) return -3;
  time_poll();
  t0 = ms_now;
  for (;;) {
    int n;
    int i;
    nic_poll();
    if (gui_input_pending()) return -3;
    if (tcp_rst) return -1;
    n = tcp_rx_len;
    if (n > cap - 1) n = cap - 1;
    if (n > 0) {
      for (i = 0; i < n; i++) dst[i] = tcp_rx[i];
      dst[n] = 0;
      for (i = 0; i < tcp_rx_len - n; i++) tcp_rx[i] = tcp_rx[n + i];
      tcp_rx_len -= n;
      tcp_ack();
      return n;
    }
    if (tcp_fin || tcp_state == ST_CLOSED || tcp_state == ST_CLOSEWAIT) return 0;
    {
      int w = wait_done(&spins, t0, 8000, 40000000u);
      if (w == 2) return -3;
      if (w) return -1;
    }
  }
}

static void api_close(void) {
  if (tcp_state == ST_EST || tcp_state == ST_CLOSEWAIT) {
    tcp_raw(0x11, 0, 0, tcp_snd_nxt, 1);
  }
  tcp_state = ST_CLOSED;
  tcp_rx_len = 0;
}

void net_init(void) {
  uint16_t io = 0;
  volatile uint32_t* g = (volatile uint32_t*)GATE;
  int i;
  time_init();
  {
    uint8_t* big = (uint8_t*)umalloc_owned(32768u, 0);
    if (big) {
      tcp_rx = big;
      tcp_rx_cap = 32768;
    }
  }
  nic_ok = 0;
  arp_valid = 0;
  tcp_state = ST_CLOSED;
  for (i = 0; i < 5; i++) g[i] = 0;
  if (nic_find(&io) == 0 && ne_reset(io) == 0) {
    nic_ok = 1;
    gate_set(NET_MAGIC, fn_bits((void*)api_connect), fn_bits((void*)api_send),
             fn_bits((void*)api_recv), fn_bits((void*)api_close));
    puts_k("Rede IPv4 10.0.2.15. Comandos: ping, asm curl.asm\n");
    serial_puts("[net] 10.0.2.15 mac ");
    for (i = 0; i < 6; i++) {
      const char* hex = "0123456789ABCDEF";
      serial_putc(hex[our_mac[i] >> 4]);
      serial_putc(hex[our_mac[i] & 15]);
      if (i != 5) serial_putc(':');
    }
    serial_puts("\n");
  } else {
    puts_k("Rede: sem placa NE2000\n");
    serial_puts("[net] sem placa\n");
  }
}

int net_ready(void) {
  return nic_ok;
}

int net_tcp_connect(const char* host, int port) { return api_connect(host, port); }
int net_tcp_send(const void* data, int len) { return api_send(data, len); }
int net_tcp_recv(void* data, int cap) { return api_recv(data, cap); }
void net_tcp_close(void) { api_close(); }

uint32_t net_ms(void) {
  time_poll();
  return ms_now;
}

int net_has_clock(void) {
  return pit_ok;
}

int net_resolve(const char* host, uint32_t* ip) {
  if (!nic_ok || !host || !ip) return -1;
  return dns_resolve(host, ip);
}

int net_ping(uint32_t ip, uint16_t seq, int size, uint32_t timeout_ms,
             uint32_t* rtt_ms, uint8_t* ttl) {
  uint8_t* pkt = icmp_buf;
  uint32_t spins = 0;
  uint32_t t0;
  int len;
  int i;
  if (!nic_ok) return -1;
  if (size < 0) size = 0;
  if (size > (int)sizeof(icmp_buf) - 8) size = (int)sizeof(icmp_buf) - 8;
  len = 8 + size;
  pkt[0] = 8;
  pkt[1] = 0;
  wr16(pkt + 2, 0);
  wr16(pkt + 4, ping_id);
  wr16(pkt + 6, seq);
  for (i = 0; i < size; i++) pkt[8 + i] = (uint8_t)('a' + i % 23);
  wr16(pkt + 2, ip_checksum(pkt, len));
  ping_dip = ip;
  ping_seq = seq;
  ping_got = 0;
  time_poll();
  t0 = ms_now;
  if (ip_send(ip, 1, pkt, len) < 0) return -1;
  for (;;) {
    nic_poll();
    if (ping_got) {
      time_poll();
      if (rtt_ms) *rtt_ms = ms_now - t0;
      if (ttl) *ttl = ping_ttl;
      return 0;
    }
    if (wait_done(&spins, t0, timeout_ms, timeout_ms * 4000u)) return -2;
  }
}

void net_delay(uint32_t ms) {
  uint32_t spins = 0;
  uint32_t t0;
  time_poll();
  t0 = ms_now;
  for (;;) {
    nic_poll();
    if (wait_done(&spins, t0, ms, ms * 4000u)) return;
  }
}

void net_demo(void) {
  typedef int (*entry_fn)(void (*)(const char*));
  extern int asm_build(const char* path);
  int rc;
  if (!nic_ok) return;
  serial_puts("[net] curl.asm example.com\n");
  rc = asm_build("curl.asm");
  if (rc != 0) {
    serial_puts("[net] asm falhou\n");
    return;
  }
  rc = ((entry_fn)0x58000)(serial_puts);
  serial_puts("\n[net] curl retornou ");
  serial_hex32((uint32_t)rc);
  serial_puts("\n");
}
