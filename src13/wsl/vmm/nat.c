/* nat.c - NAT da VM do WSL (como o NAT do WSL2/slirp, mas em cima da placa
 * do SODOS, que pode ser WiFi: nada de ponte com outro MAC).
 *
 * A VM ve a rede 10.0.3.0/24: ela e 10.0.3.15, o "roteador" e 10.0.3.2 (o
 * WSL.EXE responde ARP por qualquer IP e o ping para 10.0.3.2). Cada fluxo
 * TCP/UDP de saida ganha uma porta do SODOS em 30000-39999 (eco ICMP: o id) e
 * vai com o IP do SODOS (k_net_tap_send). O kernel desvia as respostas para
 * essas portas (k_net_tap_recv) e aqui elas voltam para a VM.
 *
 * TCP para 10.0.3.2 e o proprio SODOS (como o IP do Windows visto do WSL2):
 * vai com destino ao IP do SODOS e o kernel entrega na propria pilha (o
 * loopback do net.c), entao a API HTTP (porta 8797) responde de dentro da
 * VM. A resposta volta com 10.0.3.2 de origem. Kernel antigo (sem loopback)
 * manda para a placa e a conexao so nao abre. */
#include <string.h>
#include "nat.h"
#include "../sodos_api.h"

const uint8_t nat_guest_mac[6] = {0x52, 0x54, 0x00, 0x57, 0x53, 0x4C};
static const uint8_t gw_mac[6] = {0x52, 0x54, 0x00, 0x57, 0x53, 0x02};

#define PORT_LO 30000
#define NSLOT 10000 /* 30000..39999 */

struct slot {
  uint8_t used, proto;
  uint8_t gw;        /* a VM pediu 10.0.3.2: e o proprio SODOS */
  uint16_t gport;    /* porta (ou id ICMP) da VM */
  uint32_t rip;      /* destino */
  uint16_t rport;
  uint32_t last_ms;
};
static struct slot slots[NSLOT];
static uint32_t our_ip, our_mask, our_gw, our_dns;
static int up;
static uint32_t next_slot;

/* respostas locais (ARP, ping no roteador) */
#define LQ 16
static uint8_t lq[LQ][1514];
static uint16_t lql[LQ];
static uint32_t lq_h, lq_t;

static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wbe16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}
static void wbe32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static uint32_t csum_add(uint32_t s, const uint8_t* p, int n) {
  int i;
  for (i = 0; i + 1 < n; i += 2) s += (uint32_t)((p[i] << 8) | p[i + 1]);
  if (n & 1) s += (uint32_t)(p[n - 1] << 8);
  return s;
}
static uint16_t csum_fold(uint32_t s) {
  while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
  return (uint16_t)~s;
}

static void ip_fix(uint8_t* ip) {
  int ihl = (ip[0] & 15) * 4;
  wbe16(ip + 10, 0);
  wbe16(ip + 10, csum_fold(csum_add(0, ip, ihl)));
}

/* checksum TCP/UDP com pseudo-cabecalho */
static void l4_fix(uint8_t* ip, int len) {
  int ihl = (ip[0] & 15) * 4, l = len - ihl;
  uint8_t* l4 = ip + ihl;
  uint32_t s;
  int off = ip[9] == 6 ? 16 : 6;
  if (l < off + 2) return;
  if (ip[9] == 17 && be16(l4 + 6) == 0) return; /* UDP sem checksum */
  wbe16(l4 + off, 0);
  s = csum_add(0, ip + 12, 8);
  s += ip[9];
  s += (uint32_t)l;
  s = csum_add(s, l4, l);
  {
    uint16_t c = csum_fold(s);
    if (ip[9] == 17 && c == 0) c = 0xFFFF;
    wbe16(l4 + off, c);
  }
}

static void icmp_fix(uint8_t* ip, int len) {
  int ihl = (ip[0] & 15) * 4;
  uint8_t* l4 = ip + ihl;
  wbe16(l4 + 2, 0);
  wbe16(l4 + 2, csum_fold(csum_add(0, l4, len - ihl)));
}

static void lq_put(const uint8_t* f, int n) {
  if (lq_h - lq_t >= LQ || n > 1514) return;
  memcpy(lq[lq_h % LQ], f, (size_t)n);
  lql[lq_h % LQ] = (uint16_t)n;
  lq_h++;
}

int nat_init(uint32_t* dns) {
  k_net_addr(&our_ip, &our_mask, &our_gw, &our_dns);
  if (!our_ip || k_net_tap_open(PORT_LO, PORT_LO + NSLOT - 1) != 0) return -1;
  memset(slots, 0, sizeof slots);
  up = 1;
  if (dns) *dns = our_dns;
  return 0;
}

void nat_close(void) {
  if (up) k_net_tap_close();
  up = 0;
}

static int slot_for(uint8_t proto, uint16_t gport, uint32_t rip, uint16_t rport) {
  uint32_t now = k_ms(), i, best = NSLOT, best_age = 0;
  uint32_t idle = proto == 6 ? 600000u : 120000u;
  for (i = 0; i < NSLOT; i++) {
    struct slot* s = &slots[i];
    if (s->used && s->proto == proto && s->gport == gport && s->rip == rip && s->rport == rport) {
      s->last_ms = now;
      return (int)i;
    }
  }
  /* livre, ou o mais velho que passou do tempo */
  for (i = 0; i < NSLOT; i++) {
    uint32_t k = (next_slot + i) % NSLOT;
    struct slot* s = &slots[k];
    if (!s->used) {
      best = k;
      break;
    }
    if (now - s->last_ms > idle && now - s->last_ms > best_age) {
      best = k;
      best_age = now - s->last_ms;
    }
  }
  if (best == NSLOT) return -1;
  next_slot = best + 1;
  slots[best].used = 1;
  slots[best].proto = proto;
  slots[best].gport = gport;
  slots[best].rip = rip;
  slots[best].rport = rport;
  slots[best].last_ms = now;
  return (int)best;
}

static void arp_reply(const uint8_t* f) {
  uint8_t r[42];
  uint32_t tip = be32(f + 38);
  if (be16(f + 20) != 1 || tip == NAT_GUEST_IP) return;
  memcpy(r, f + 6, 6);
  memcpy(r + 6, gw_mac, 6);
  wbe16(r + 12, 0x0806);
  wbe16(r + 14, 1);
  wbe16(r + 16, 0x0800);
  r[18] = 6;
  r[19] = 4;
  wbe16(r + 20, 2);
  memcpy(r + 22, gw_mac, 6);
  wbe32(r + 28, tip);
  memcpy(r + 32, f + 22, 6);
  memcpy(r + 38, f + 28, 4);
  lq_put(r, 42);
}

void nat_from_guest(const uint8_t* f, int len) {
  static uint8_t ip[1514];
  int iplen, ihl, k;
  uint32_t dst;
  if (len < 14) return;
  if (be16(f + 12) == 0x0806) {
    if (len >= 42) arp_reply(f);
    return;
  }
  if (be16(f + 12) != 0x0800) return; /* IPv6 etc.: nao */
  iplen = len - 14;
  if (iplen < 20 || iplen > 1500) return;
  memcpy(ip, f + 14, (size_t)iplen);
  ihl = (ip[0] & 15) * 4;
  if ((ip[0] >> 4) != 4 || ihl < 20 || ihl > iplen) return;
  if (be16(ip + 2) < iplen) iplen = be16(ip + 2);
  dst = be32(ip + 16);
  if (dst == NAT_GW_IP && ip[9] == 6 && up && iplen >= ihl + 20 && !(be16(ip + 6) & 0x3FFF)) {
    /* TCP para o roteador = o SODOS (API em 10.0.3.2:8797) */
    k = slot_for(6, be16(ip + ihl), NAT_GW_IP, be16(ip + ihl + 2));
    if (k < 0) return;
    slots[k].gw = 1;
    wbe32(ip + 12, our_ip);
    wbe32(ip + 16, our_ip);
    wbe16(ip + ihl, (uint16_t)(PORT_LO + k));
    ip_fix(ip);
    l4_fix(ip, iplen);
    k_net_tap_send(ip, iplen);
    return;
  }
  if (dst == NAT_GW_IP) {
    /* ping no roteador virtual */
    if (ip[9] == 1 && iplen >= ihl + 8 && ip[ihl] == 8) {
      uint8_t r[1514];
      memcpy(r, f + 6, 6);
      memcpy(r + 6, gw_mac, 6);
      wbe16(r + 12, 0x0800);
      memcpy(r + 14, ip, (size_t)iplen);
      wbe32(r + 14 + 12, NAT_GW_IP);
      wbe32(r + 14 + 16, be32(ip + 12));
      r[14 + ihl] = 0;
      icmp_fix(r + 14, iplen);
      ip_fix(r + 14);
      lq_put(r, 14 + iplen);
    }
    return;
  }
  if (!up || (dst & 0xF0000000u) == 0xE0000000u || dst == 0xFFFFFFFFu) return;
  if (be16(ip + 6) & 0x3FFF) return; /* fragmentos: nao */
  if (ip[9] == 6 || ip[9] == 17) {
    if (iplen < ihl + 8) return;
    k = slot_for(ip[9], be16(ip + ihl), dst, be16(ip + ihl + 2));
    if (k < 0) return;
    slots[k].gw = 0;
    wbe32(ip + 12, our_ip);
    wbe16(ip + ihl, (uint16_t)(PORT_LO + k));
    ip_fix(ip);
    l4_fix(ip, iplen);
  } else if (ip[9] == 1) {
    if (iplen < ihl + 8 || ip[ihl] != 8) return; /* so eco */
    k = slot_for(1, be16(ip + ihl + 4), dst, 0);
    if (k < 0) return;
    wbe32(ip + 12, our_ip);
    wbe16(ip + ihl + 4, (uint16_t)(PORT_LO + k));
    ip_fix(ip);
    icmp_fix(ip, iplen);
  } else {
    return;
  }
  k_net_tap_send(ip, iplen);
}

/* pacote IP do kernel -> quadro para a VM. 0 se nao e de ninguem */
static int from_tap(uint8_t* f, int n) {
  uint8_t* ip = f + 14;
  int ihl = (ip[0] & 15) * 4, k;
  uint8_t* l4 = ip + ihl;
  if (n < ihl + 8) return 0;
  if (be16(ip + 2) < n) n = be16(ip + 2);
  if (ip[9] == 6 || ip[9] == 17) {
    k = be16(l4 + 2) - PORT_LO;
    if (k < 0 || k >= NSLOT || !slots[k].used || slots[k].proto != ip[9]) return 0;
    slots[k].last_ms = k_ms();
    if (slots[k].gw) wbe32(ip + 12, NAT_GW_IP); /* veio do proprio SODOS */
    wbe32(ip + 16, NAT_GUEST_IP);
    wbe16(l4 + 2, slots[k].gport);
    ip_fix(ip);
    l4_fix(ip, n);
  } else if (ip[9] == 1 && l4[0] == 0) {
    k = be16(l4 + 4) - PORT_LO;
    if (k < 0 || k >= NSLOT || !slots[k].used || slots[k].proto != 1) return 0;
    wbe32(ip + 16, NAT_GUEST_IP);
    wbe16(l4 + 4, slots[k].gport);
    ip_fix(ip);
    icmp_fix(ip, n);
  } else if (ip[9] == 1 && (l4[0] == 3 || l4[0] == 11)) {
    /* erro ICMP: desfaz o NAT do pacote original que vem dentro */
    uint8_t* in = l4 + 8;
    int ih = (in[0] & 15) * 4;
    uint8_t* il4 = in + ih;
    if (n < ihl + 8 + ih + 8) return 0;
    if (in[9] == 6 || in[9] == 17) k = be16(il4) - PORT_LO;
    else if (in[9] == 1) k = be16(il4 + 4) - PORT_LO;
    else return 0;
    if (k < 0 || k >= NSLOT || !slots[k].used) return 0;
    wbe32(in + 12, NAT_GUEST_IP);
    if (in[9] == 1) wbe16(il4 + 4, slots[k].gport);
    else wbe16(il4, slots[k].gport);
    ip_fix(in);
    wbe32(ip + 16, NAT_GUEST_IP);
    ip_fix(ip);
    icmp_fix(ip, n);
  } else {
    return 0;
  }
  memcpy(f, nat_guest_mac, 6);
  memcpy(f + 6, gw_mac, 6);
  wbe16(f + 12, 0x0800);
  return 14 + n;
}

int nat_to_guest(uint8_t* f, int cap) {
  if (lq_t != lq_h) {
    int n = lql[lq_t % LQ];
    if (n > cap) n = cap;
    memcpy(f, lq[lq_t % LQ], (size_t)n);
    lq_t++;
    return n;
  }
  if (!up || cap < 1514) return 0;
  for (;;) {
    int n = k_net_tap_recv(f + 14, cap - 14), r;
    if (n <= 0) return 0;
    r = from_tap(f, n);
    if (r > 0) return r;
  }
}
