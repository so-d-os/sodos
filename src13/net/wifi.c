/* WiFi: le WIFI.CFG, liga o RTL8852BE e prepara o WPA2. Ver wifi.h. */
#include "wifi.h"
#include "wpa.h"
#include "rtw89.h"
#include "net.h"
#include "shell.h"
#include "serial.h"
#include "drives.h"
#include "mm.h"

static struct wifi_cfg cfg;                /* a rede escolhida */
static struct wifi_cfg nets[WIFI_CFG_MAX];  /* todas as redes do WIFI.CFG */
static int nets_n;
static struct rtw_info chip;
static uint8_t pmk[32];
static int stage;  /* ultima etapa que deu certo */
static uint8_t* fw_buf;
static uint32_t fw_len;
static int scan_on;   /* net wifi scan: a lista nao e "do WIFI.CFG" */

static const char* stage_name[] = {
  "parado", "configuracao lida", "autoteste WPA2 e firmware prontos", "placa encontrada",
  "firmware escolhido", "placa ligada (versao e efuse lidos)", "firmware rodando na placa",
  "MAC pronta (firmware responde)", "radio ligado no canal", "rede encontrada (beacons) e PMK",
  "autenticado no AP", "associado", "WPA2 pronto", "conectado"
};

static void say(const char* s) {
  puts_k(s);
  rtw_log("[wifi] ");
  rtw_log(s);
}

/* Grava o log da tentativa no pendrive (o notebook nao tem COM1). */
static void save_log(void) {
  int n;
  const char* t = rtw_log_text(&n);
  if (file_save(WIFI_LOG_PATH, t, n) == 0) puts_k("  log gravado em " WIFI_LOG_PATH "\n");
}

static void put_u(uint32_t n) {
  char b[16];
  uint32_to_string(n, b, (int)sizeof(b));
  puts_k(b);
}

static void put_hex8(uint8_t v) {
  const char* h = "0123456789ABCDEF";
  putc(h[v >> 4]);
  putc(h[v & 15]);
}

static void put_mac(const uint8_t* m) {
  int i;
  for (i = 0; i < 6; i++) {
    put_hex8(m[i]);
    if (i != 5) putc(':');
  }
}

static void put_ip(uint32_t ip) {
  int i;
  for (i = 3; i >= 0; i--) {
    put_u((ip >> (i * 8)) & 0xFFu);
    if (i) putc('.');
  }
}

/* ---------- WIFI.CFG ---------- */

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int parse_mac(const char* s, uint8_t* m) {
  int i;
  for (i = 0; i < 6; i++) {
    int a = hexv(s[0]), b = hexv(s[1]);
    if (a < 0 || b < 0) return -1;
    m[i] = (uint8_t)(a * 16 + b);
    s += 2;
    if (i != 5) {
      if (*s != ':' && *s != '-') return -1;
      s++;
    }
  }
  return 0;
}

static int parse_ipv4(const char* s, uint32_t* out) {
  uint32_t ip = 0;
  int part;
  for (part = 0; part < 4; part++) {
    uint32_t v = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s++ - '0'); digits++; }
    if (!digits || v > 255) return -1;
    ip = (ip << 8) | v;
    if (part != 3 && *s++ != '.') return -1;
  }
  *out = ip;
  return 0;
}

static int key_is(const char* a, const char* b) { return name_eq(a, b); }

static void copy_str(char* d, const char* s, int max) {
  int i = 0;
  while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
  d[i] = 0;
}

static void cfg_clear(struct wifi_cfg* c) {
  uint8_t* z = (uint8_t*)c;
  int k;
  for (k = 0; k < (int)sizeof(*c); k++) z[k] = 0;
  c->dhcp = 1;
}

static int cfg_valid(const struct wifi_cfg* c) {
  int pl = 0;
  while (c->psk[pl]) pl++;
  return c->ssid_len && pl >= 8 && pl <= 63;
}

int wifi_cfg_load(const char* path, struct wifi_cfg* list, int max) {
  static char text[4096];
  int n = file_load(path, text, (int)sizeof(text));
  int i = 0, count = 0;
  struct wifi_cfg* c = list;
  if (n < 0) return -1;
  if (max <= 0) return -2;
  cfg_clear(c);
  for (;;) {
    char* line = text + i;
    char* key;
    char* val;
    char* e;
    int len = 0, next = 0;
    if (i >= n) {
      next = 1;  /* fim do arquivo fecha a ultima rede */
      line = 0;
    } else {
      while (i + len < n && text[i + len] != '\n') len++;
      i += len + 1;
      line[len] = 0;
      if (len && line[len - 1] == '\r') line[--len] = 0;
      while (*line == ' ' || *line == '\t') line++;
      if (*line == '#' || *line == ';' || !*line) continue;
      /* [nome] ou um segundo ssid= comecam outra rede */
      if (*line == '[') next = 1;
      else if (c->ssid_len) {
        key = line;
        while (*key && *key != '=' && *key != ' ' && *key != '\t') key++;
        {
          char sv = *key;
          *key = 0;
          next = key_is(line, "ssid");
          *key = sv;
        }
      }
    }
    if (next && (c->ssid_len || !line)) {
      if (cfg_valid(c)) {
        count++;
        if (count == max) break;
        c = list + count;
      }
      cfg_clear(c);
    }
    if (!line) break;
    if (*line == '[') continue;
    key = line;
    val = line;
    while (*val && *val != '=') val++;
    if (!*val) continue;
    *val++ = 0;
    /* tira espacos do fim da chave; o valor fica como esta (o SSID pode ter espacos) */
    for (e = val - 2; e >= key && (*e == ' ' || *e == '\t'); e--) *e = 0;
    if (key_is(key, "ssid")) {
      copy_str(c->ssid, val, (int)sizeof(c->ssid));
      c->ssid_len = 0;
      while (c->ssid[c->ssid_len]) c->ssid_len++;
    } else if (key_is(key, "psk")) {
      copy_str(c->psk, val, (int)sizeof(c->psk));
    } else if (key_is(key, "bssid")) {
      c->has_bssid = parse_mac(val, c->bssid) == 0;
    } else if (key_is(key, "channel")) {
      c->channel = (int)str_to_uint32(val, 10);
    } else if (key_is(key, "ip")) {
      if (key_is(val, "dhcp")) c->dhcp = 1;
      else if (parse_ipv4(val, &c->ip) == 0) c->dhcp = 0;
    } else if (key_is(key, "mask")) {
      parse_ipv4(val, &c->mask);
    } else if (key_is(key, "gateway") || key_is(key, "gw")) {
      parse_ipv4(val, &c->gw);
    } else if (key_is(key, "dns")) {
      parse_ipv4(val, &c->dns);
    }
  }
  return count ? count : -2;
}

/* ---------- escuta ---------- */

struct seen {
  uint8_t bssid[6];
  char ssid[33];
  uint32_t count;
};

static void log_mac(const uint8_t* m) {
  int i;
  for (i = 0; i < 6; i++) {
    char b[3];
    b[0] = "0123456789ABCDEF"[m[i] >> 4];
    b[1] = "0123456789ABCDEF"[m[i] & 15];
    b[2] = 0;
    rtw_log(b);
    if (i != 5) rtw_log(":");
  }
}

static uint8_t ap_bssid[6];  /* BSSID da rede do WIFI.CFG, achado na escuta */

static int mac_same(const uint8_t* a, const uint8_t* b) {
  int i;
  for (i = 0; i < 6; i++)
    if (a[i] != b[i]) return 0;
  return 1;
}

/* A rede do WIFI.CFG que o beacon anuncia (pelo BSSID ou pelo SSID), ou -1. */
static int net_match(const uint8_t* bssid, const char* ssid, int ch) {
  int j;
  for (j = 0; j < nets_n; j++) {
    const struct wifi_cfg* c = &nets[j];
    if ((c->channel ? c->channel : WIFI_DEF_CHANNEL) != ch) continue;
    if ((c->has_bssid && mac_same(bssid, c->bssid)) || name_eq(ssid, c->ssid)) return j;
  }
  return -1;
}

/* Escuta 'ms' no canal ch e lista os beacons. Devolve o indice (em nets) da
 * primeira rede do WIFI.CFG que apareceu, -1 se nenhuma, -2 se nada chegou. */
static int listen_beacons(uint32_t ms, int ch) {
  static struct seen list[16];
  struct rtw_rx rx;
  int n = 0, i, found = -1;
  uint32_t frames = 0, ppdu = 0, other = 0;
  uint32_t t0 = net_ms(), spins = 0;
  puts_k("  escutando beacons...\n");
  for (;;) {
    while (rtw_rx_poll(&rx)) {
      const uint8_t* f = rx.data;
      if (rx.type == 1) { ppdu++; continue; }
      if (rx.type != 0) { other++; continue; }
      frames++;
      if (rx.len < 38 || (f[0] != 0x80 && f[0] != 0x50)) continue;
      for (i = 0; i < n; i++)
        if (mac_same(list[i].bssid, f + 16)) break;
      if (i == n && n < 16) {
        int k;
        for (k = 0; k < 6; k++) list[n].bssid[k] = f[16 + k];
        list[n].ssid[0] = 0;
        list[n].count = 0;
        /* SSID: primeiro IE depois de timestamp, intervalo e capacidades */
        if (f[36] == 0 && f[37] <= 32 && 38u + f[37] <= rx.len) {
          for (k = 0; k < f[37]; k++)
            list[n].ssid[k] = (char)(f[38 + k] >= 32 && f[38 + k] < 127 ? f[38 + k] : '?');
          list[n].ssid[k] = 0;
        }
        n++;
      }
      if (i < 16) list[i].count++;
    }
    if (net_has_clock() ? (net_ms() - t0) >= ms : ++spins > ms * 2000u) break;
  }
  rtw_log("[wifi] quadros=");
  rtw_log_hex(frames);
  rtw_log(" ppdu=");
  rtw_log_hex(ppdu);
  rtw_log(" outros=");
  rtw_log_hex(other);
  rtw_log("\n");
  for (i = 0; i < n; i++) {
    int j = net_match(list[i].bssid, list[i].ssid, ch);
    int match = j >= 0;
    /* vale a ordem do arquivo: a primeira rede listada que esta no ar */
    if (match && (found < 0 || j < found)) {
      int k;
      found = j;
      for (k = 0; k < 6; k++) ap_bssid[k] = list[i].bssid[k];
    }
    puts_k(match ? "  * " : "    ");
    put_mac(list[i].bssid);
    puts_k("  \"");
    puts_k(list[i].ssid);
    puts_k("\"  beacons ");
    put_u(list[i].count);
    puts_k("\n");
    rtw_log(match ? "[wifi] * " : "[wifi]   ");
    log_mac(list[i].bssid);
    rtw_log(" \"");
    rtw_log(list[i].ssid);
    rtw_log("\" ");
    rtw_log_hex(list[i].count);
    rtw_log("\n");
  }
  if (!frames) {
    puts_k("  nenhum quadro recebido do ar\n");
    return -2;
  }
  if (found < 0 && !scan_on) puts_k("  nenhuma rede do WIFI.CFG neste canal\n");
  return found;
}

/* Autenticacao aberta (802.11 seq 1) e espera a resposta do AP (seq 2).
 * 0 = AP aceitou, >0 = status de recusa, -1 = sem resposta. */
static int authenticate(void) {
  uint8_t f[30];
  struct rtw_rx rx;
  int tries, k;
  uint32_t acks = 0;
  for (k = 0; k < 30; k++) f[k] = 0;
  f[0] = 0xB0;                                  /* gerencia, autenticacao */
  for (k = 0; k < 6; k++) {
    f[4 + k] = ap_bssid[k];
    f[10 + k] = chip.mac[k];
    f[16 + k] = ap_bssid[k];
  }
  f[26] = 1;                                    /* seq 1 (algoritmo 0 = aberto) */
  for (tries = 0; tries < 8; tries++) {
    uint32_t t0, spins = 0;
    rtw_log("[wifi] auth: enviando tentativa ");
    rtw_log_hex((uint32_t)tries + 1u);
    rtw_log("\n");
    f[22] = (uint8_t)(tries << 4);
    if (rtw_tx_mgmt(f, sizeof(f)) != 0) {
      rtw_log("[wifi] auth: TX recusado: ");
      rtw_log(rtw_error());
      rtw_log("\n");
      return -1;
    }
    t0 = net_ms();
    for (;;) {
      while (rtw_rx_poll(&rx)) {
        const uint8_t* r = rx.data;
        if (rx.ring == 1) {
          /* relatorio de liberacao do TX: status 0 ok, 1 tentativas, 2 tempo, 3 macid */
          uint32_t o;
          for (o = 0; o + 4 <= rx.len && o < 32; o += 4) {
            uint32_t d = r[o] | (r[o + 1] << 8) | (r[o + 2] << 16) | ((uint32_t)r[o + 3] << 24);
            rtw_log("[wifi] auth: RPP ");
            rtw_log_hex(d);
            rtw_log(" status=");
            rtw_log_hex((d >> 13) & 7u);
            rtw_log("\n");
          }
          continue;
        }
        if (rx.type == 10 && rx.len >= 8) {
          rtw_log("[wifi] auth: C2H ");
          rtw_log_hex(r[0] | (r[1] << 8) | (r[2] << 16) | ((uint32_t)r[3] << 24));
          rtw_log("\n");
          continue;
        }
        if (rx.type != 0 || rx.len < 30) continue;
        if (r[0] == 0xD4 && mac_same(r + 4, chip.mac)) acks++;  /* ACK para nos */
        if (r[0] != 0xB0 || !mac_same(r + 4, chip.mac) || !mac_same(r + 10, ap_bssid)) continue;
        if (r[26] != 2) continue;
        rtw_log("[wifi] auth: resposta do AP, status ");
        rtw_log_hex((uint32_t)(r[28] | (r[29] << 8)));
        rtw_log("\n");
        return r[28] | (r[29] << 8);
      }
      if (net_has_clock() ? (net_ms() - t0) >= 300u : ++spins > 600000u) break;
    }
    rtw_log("[wifi] auth: TX lido pela placa=");
    rtw_log_hex((uint32_t)rtw_tx_mgmt_done());
    rtw_log(" ACKs vistos=");
    rtw_log_hex(acks);
    rtw_log("\n");
  }
  return -1;
}

/* Registradores da placa no pendrive, para comparar com o Linux. */
static void save_regs(void) {
  uint8_t* buf = (uint8_t*)umalloc_owned(RTW_DUMP_SIZE, 0);
  if (!buf) return;
  rtw_dump_regs(buf);
  if (file_save(WIFI_REGS_PATH, (const char*)buf, (int)RTW_DUMP_SIZE) == 0)
    puts_k("  registradores gravados em " WIFI_REGS_PATH "\n");
  ufree(buf);
}

/* ---------- associacao, WPA2 e dados ---------- */

static struct wpa_sm sm;
static uint16_t tx_seq;
static int keys_ready;   /* PTK instalada: dados saem e entram cifrados (CCMP) */
static int connected;
static uint8_t txf[2400];
static uint8_t rxf[2400];
static const uint8_t llc_snap[6] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};
static int bg;            /* o vigia reconectando: sem tela e sem REGS.BIN */
static int ap_heard;      /* chegou quadro do AP desde o ultimo tick */
static uint32_t noack_run;   /* envios seguidos sem ACK (RPP) */
static uint32_t txfail_run;  /* envios seguidos que a placa recusou */

static int wait_over(uint32_t t0, uint32_t* spins, uint32_t ms) {
  return net_has_clock() ? (net_ms() - t0) >= ms : ++(*spins) > ms * 2000u;
}

/* Dados para o AP (ToDS). payload ja comeca com o LLC/SNAP. Cifra com o TK
 * depois do handshake. */
static int send_data(const uint8_t* da, const uint8_t* payload, int len) {
  const int hl = 24;
  int k;
  if (hl + 8 + len + 8 > (int)sizeof(txf)) return -1;
  for (k = 0; k < hl; k++) txf[k] = 0;
  txf[0] = 0x08;
  txf[1] = 0x01;
  for (k = 0; k < 6; k++) {
    txf[4 + k] = ap_bssid[k];
    txf[10 + k] = chip.mac[k];
    txf[16 + k] = da[k];
  }
  txf[22] = (uint8_t)(tx_seq << 4);
  txf[23] = (uint8_t)(tx_seq >> 4);
  tx_seq = (uint16_t)((tx_seq + 1u) & 0xFFFu);
  if (!keys_ready) {
    for (k = 0; k < len; k++) txf[hl + k] = payload[k];
    return rtw_tx_data(txf, (uint32_t)(hl + len));
  }
  for (k = 0; k < len; k++) txf[hl + 8 + k] = payload[k];
  wpa_ccmp_encrypt(sm.ptk + 32, sm.tx_pn++, 0, txf, hl, len);
  return rtw_tx_data(txf, (uint32_t)(hl + 8 + len + 8));
}

static int send_eapol(const uint8_t* body, int n) {
  static uint8_t b[600];
  int k;
  if (n + 8 > (int)sizeof(b)) return -1;
  for (k = 0; k < 6; k++) b[k] = llc_snap[k];
  b[6] = 0x88;
  b[7] = 0x8E;
  for (k = 0; k < n; k++) b[8 + k] = body[k];
  return send_data(ap_bssid, b, n + 8);
}

/* Dados do AP depois de conectar: quantos vieram, quantos eram so para nos,
 * quantos nao decifraram e quantos viraram Ethernet. Saem no WIFI.LOG. */
static uint32_t rx_data, rx_ucast, rx_bad, rx_eth;
/* Tudo que chegou no RXQ, beacons do AP, envios e relatorios de TX (RPQ). */
static uint32_t rx_all, rx_bcn, tx_n, tx_fail, rpp_ok, rpp_noack;

/* Um quadro 802.11 recebido. 1 = Ethernet em eth (*elen), 2 = tratado aqui
 * (EAPOL, desassociacao), 0 = ignorado. */
static int rx_frame(const uint8_t* f, uint32_t len, uint8_t* eth, int cap, int* elen) {
  int hl, plen, k;
  const uint8_t* p;
  if (len < 28) return 0;
  len -= 4;  /* FCS */
  if ((f[0] & 0x0C) == 0x00 && (f[0] == 0xC0 || f[0] == 0xA0) && mac_same(f + 4, chip.mac) &&
      mac_same(f + 10, ap_bssid)) {
    rtw_log(f[0] == 0xC0 ? "[wifi] deauth do AP, motivo " : "[wifi] disassoc do AP, motivo ");
    rtw_log_hex((uint32_t)(f[24] | (f[25] << 8)));
    rtw_log("\n");
    connected = 0;
    return 2;
  }
  if ((f[0] & 0x0C) != 0x08 || (f[0] & 0x40)) return 0;  /* so dados com conteudo */
  if ((f[1] & 3) != 2 || !mac_same(f + 10, ap_bssid)) return 0;  /* FromDS, do nosso AP */
  if (!(f[4] & 1) && !mac_same(f + 4, chip.mac)) return 0;
  if (mac_same(f + 16, chip.mac)) return 0;  /* nosso broadcast voltando */
  rx_data++;
  if (!(f[4] & 1)) rx_ucast++;
  hl = 24;
  if (f[0] & 0x80) {
    hl += 2;
    if (f[1] & 0x80) hl += 4;
  }
  if ((int)len <= hl || len > sizeof(rxf)) return 0;
  for (k = 0; k < (int)len; k++) rxf[k] = f[k];
  if (rxf[1] & 0x40) {
    const uint8_t* key;
    if (!keys_ready) return 0;
    plen = (int)len - hl - 16;
    if (plen < 8) return 0;
    key = (rxf[4] & 1) ? sm.gtk : sm.ptk + 32;
    if (wpa_ccmp_decrypt(key, rxf, hl, plen, 0) != 0) {
      rx_bad++;
      return 0;
    }
    p = rxf + hl + 8;
  } else {
    plen = (int)len - hl;
    p = rxf + hl;
  }
  if (plen < 8 || p[0] != 0xAA || p[1] != 0xAA || p[2] != 0x03) return 0;
  if (p[6] == 0x88 && p[7] == 0x8E) {
    static uint8_t out[600];
    const uint8_t* e = p + 8;
    int r = wpa_eapol_rx(&sm, e, plen - 8, out, sizeof(out));
    rtw_log("[wifi] EAPOL ");
    rtw_log_hex(plen >= 8 + 17 ? (uint32_t)((e[0] << 24) | (e[1] << 16) | (e[4] << 8) | e[5]) : 0);
    rtw_log(" info ");
    rtw_log_hex(plen >= 8 + 17 ? (uint32_t)((e[5] << 8) | e[6]) : 0);
    rtw_log(" replay ");
    rtw_log_hex(plen >= 8 + 17 ? (uint32_t)e[16] : 0);
    rtw_log(" len ");
    rtw_log_hex((uint32_t)(plen - 8));
    rtw_log(" seq ");
    rtw_log_hex((uint32_t)((rxf[22] | (rxf[23] << 8)) >> 4));
    rtw_log(r < 0 ? " recusado\n" : (r > 0 ? " respondido\n" : "\n"));
    if (r > 0) send_eapol(out, r);
    if (sm.state >= WPA_PTK_DONE) keys_ready = 1;  /* msg 4/4 ja saiu em claro */
    return 2;
  }
  if (14 + plen - 8 > cap) return 0;
  for (k = 0; k < 6; k++) {
    eth[k] = rxf[4 + k];        /* destino = A1 */
    eth[6 + k] = rxf[16 + k];   /* origem = A3 */
  }
  eth[12] = p[6];
  eth[13] = p[7];
  for (k = 8; k < plen; k++) eth[14 + k - 8] = p[k];
  *elen = 14 + plen - 8;
  rx_eth++;
  return 1;
}

static int link_tx(const uint8_t* fr, int len) {
  static uint8_t b[1600];
  int k;
  if (!connected || len < 14 || len - 14 + 8 > (int)sizeof(b)) return -1;
  for (k = 0; k < 6; k++) b[k] = llc_snap[k];
  b[6] = fr[12];
  b[7] = fr[13];
  for (k = 14; k < len; k++) b[8 + k - 14] = fr[k];
  tx_n++;
  if (send_data(fr, b, len - 14 + 8) == 0) {
    txfail_run = 0;
    return 0;
  }
  tx_fail++;
  txfail_run++;
  return -1;
}

static int link_rx(uint8_t* out, int cap) {
  struct rtw_rx rx;
  int n;
  while (rtw_rx_poll(&rx)) {
    if (rx.ring == 1) {
      uint32_t o;
      for (o = 0; o + 4 <= rx.len && o < 32; o += 4) {
        if ((rx.data[o + 1] >> 5) & 7u) {
          rpp_noack++;
          noack_run++;
        } else {
          rpp_ok++;
          noack_run = 0;
        }
      }
      continue;
    }
    if (rx.ring != 0 || rx.type != 0) continue;
    rx_all++;
    if (rx.len >= 16 && mac_same(rx.data + 10, ap_bssid)) ap_heard = 1;
    if (rx.len >= 24 && rx.data[0] == 0x80 && mac_same(rx.data + 16, ap_bssid)) rx_bcn++;
    if (rx_frame(rx.data, rx.len, out, cap, &n) == 1) return n;
  }
  return 0;
}

static void wifi_tick(void);
static const struct net_link wifi_link = {"WiFi", chip.mac, link_tx, link_rx, wifi_tick};

/* Pedido de associacao (802.11a: SSID, taxas, RSN IE). 0 = aceito, AID em *aid. */
static const uint8_t rates[10] = {0x01, 0x08, 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C};

static int associate(uint32_t* aid) {
  uint8_t f[128];
  struct rtw_rx rx;
  int n = 0, k, tries;
  for (k = 0; k < 24; k++) f[k] = 0;
  for (k = 0; k < 6; k++) {
    f[4 + k] = ap_bssid[k];
    f[10 + k] = chip.mac[k];
    f[16 + k] = ap_bssid[k];
  }
  n = 24;
  f[n++] = 0x11;  /* ESS + privacidade */
  f[n++] = 0x00;
  f[n++] = 10;    /* listen interval */
  f[n++] = 0;
  f[n++] = 0;     /* SSID */
  f[n++] = (uint8_t)cfg.ssid_len;
  for (k = 0; k < cfg.ssid_len; k++) f[n++] = (uint8_t)cfg.ssid[k];
  for (k = 0; k < (int)sizeof(rates); k++) f[n++] = rates[k];
  for (k = 0; k < (int)sizeof(wpa_rsn_ie); k++) f[n++] = wpa_rsn_ie[k];
  for (tries = 0; tries < 8; tries++) {
    uint32_t t0 = net_ms(), spins = 0;
    rtw_log("[wifi] assoc: enviando tentativa ");
    rtw_log_hex((uint32_t)tries + 1u);
    rtw_log("\n");
    if (rtw_tx_mgmt(f, (uint32_t)n) != 0) return -1;
    while (!wait_over(t0, &spins, 300)) {
      while (rtw_rx_poll(&rx)) {
        const uint8_t* r = rx.data;
        if (rx.ring != 0 || rx.type != 0 || rx.len < 30) continue;
        if (r[0] != 0x10 || !mac_same(r + 4, chip.mac) || !mac_same(r + 10, ap_bssid)) continue;
        rtw_log("[wifi] assoc: resposta, status ");
        rtw_log_hex((uint32_t)(r[26] | (r[27] << 8)));
        rtw_log(" AID ");
        rtw_log_hex((uint32_t)((r[28] | (r[29] << 8)) & 0x3FFF));
        rtw_log("\n");
        *aid = (uint32_t)((r[28] | (r[29] << 8)) & 0x3FFF);
        return r[26] | (r[27] << 8);
      }
    }
  }
  return -1;
}

/* Espera o handshake de 4 vias (o AP comeca com a msg 1/4). */
static int handshake(uint32_t ms) {
  struct rtw_rx rx;
  uint32_t t0 = net_ms(), spins = 0;
  uint8_t eth[64];
  int n;
  while (!wait_over(t0, &spins, ms)) {
    while (rtw_rx_poll(&rx)) {
      if (rx.ring == 1) {
        uint32_t o;
        for (o = 0; o + 4 <= rx.len && o < 32; o += 4) {
          const uint8_t* d = rx.data + o;
          uint32_t v = d[0] | (d[1] << 8) | (d[2] << 16) | ((uint32_t)d[3] << 24);
          rtw_log("[wifi] TX RPP ");
          rtw_log_hex(v);
          rtw_log(((v >> 13) & 7u) ? " sem ACK\n" : " ok\n");
        }
        continue;
      }
      if (rx.ring != 0 || rx.type != 0) continue;
      rx_frame(rx.data, rx.len, eth, (int)sizeof(eth), &n);
      if (!connected) return -2;
      if (keys_ready && sm.state == WPA_DONE) return 0;
    }
  }
  return keys_ready ? 0 : -1;
}

/* ---------- etapas ---------- */

static int chip_up(void);
static void watch_start(void);
static int w_dhcp;           /* DHCP pendente (vigia tenta a cada 15 s) */
static uint32_t w_dhcp_at;   /* ultima tentativa */
static int w_dhcp_fails;

static int load_firmware(void) {
  int sz = file_size(WIFI_FW_PATH);
  uint32_t off = 0;
  if (sz <= 0) {
    say("Firmware " WIFI_FW_PATH " nao encontrado.\n");
    puts_k("  (copie /lib/firmware/rtw89/rtw8852b_fw-1.bin do Linux para la)\n");
    return -1;
  }
  if (!fw_buf || fw_len < (uint32_t)sz) {
    if (fw_buf) ufree(fw_buf);
    fw_buf = (uint8_t*)umalloc_owned((uint32_t)sz, 0);
    if (!fw_buf) {
      say("Sem memoria para o firmware.\n");
      return -1;
    }
  }
  while (off < (uint32_t)sz) {
    int want = (uint32_t)sz - off > 65536u ? 65536 : (int)((uint32_t)sz - off);
    int got = file_read_at(WIFI_FW_PATH, off, (char*)fw_buf + off, want);
    if (got <= 0) {
      say("Erro lendo o firmware.\n");
      return -1;
    }
    off += (uint32_t)got;
  }
  fw_len = (uint32_t)sz;
  return 0;
}

static int start_steps(const char* path) {
  int rc;
  stage = 0;
  if (!path) path = WIFI_CFG_PATH;

  puts_k("WiFi: lendo ");
  puts_k(path);
  puts_k("\n");
  rc = wifi_cfg_load(path, nets, WIFI_CFG_MAX);
  if (rc == -1) {
    say("Nao consegui ler o arquivo de configuracao.\n");
    return -1;
  }
  if (rc == -2) {
    say("WIFI.CFG precisa de ssid= e psk= (8 a 63 caracteres).\n");
    return -1;
  }
  nets_n = rc;
  for (rc = 0; rc < nets_n; rc++) {
    const struct wifi_cfg* c = &nets[rc];
    puts_k("  rede ");
    put_u((uint32_t)rc + 1u);
    puts_k(": \"");
    puts_k(c->ssid);
    puts_k("\"");
    if (c->has_bssid) {
      puts_k(" bssid ");
      put_mac(c->bssid);
    }
    if (c->channel) {
      puts_k(" canal ");
      put_u((uint32_t)c->channel);
    }
    puts_k(c->dhcp ? " ip dhcp\n" : " ip fixo\n");
  }
  cfg = nets[0];
  stage = 1;

  rc = wpa_selftest();
  if (rc) {
    puts_k("  autoteste WPA2 falhou no teste ");
    put_u((uint32_t)rc);
    puts_k("\n");
    return -1;
  }
  stage = 2;

  if (load_firmware() != 0) return -1;
  puts_k("  firmware ");
  put_u(fw_len / 1024u);
  puts_k(" KB, versao ");
  put_u(fw_buf[4]);
  putc('.');
  put_u(fw_buf[5]);
  putc('.');
  put_u(fw_buf[6]);
  putc('.');
  put_u(fw_buf[7]);
  puts_k("\n");

  if (chip_up() != 0) return -1;
  watch_start();
  net_attach(&wifi_link);
  if (cfg.dhcp) {
    uint32_t ip, mask, gw, dns;
    puts_k("  DHCP...\n");
    if (net_dhcp() != 0) {
      puts_k("  o DHCP nao respondeu (a conexao WiFi continua; o vigia tenta de novo)\n");
      w_dhcp = 1;
      w_dhcp_at = net_ms();
      return -1;
    }
    net_get_addr(&ip, &mask, &gw, &dns);
    puts_k("  conectado: ip ");
    put_ip(ip);
    puts_k("  gateway ");
    put_ip(gw);
    puts_k("  dns ");
    put_ip(dns);
    puts_k("\n");
    rtw_log("[wifi] DHCP ok, ip ");
    rtw_log_hex(ip);
    rtw_log(" gateway ");
    rtw_log_hex(gw);
    rtw_log("\n");
  }
  stage = 13;
  return 0;
}

/* Da placa ate o WPA2 pronto (etapas 3 a 12). Tambem reinicia a placa
 * depois de uma queda (vigia, com bg = 1). Em falha a placa fica desligada. */
static int chip_up(void) {
  int rc;
  if (rtw_probe(&chip) != 0) {
    puts_k("  ");
    say(rtw_error());
    puts_k("\n");
    return -1;
  }
  puts_k("  placa RTL8852BE no PCI ");
  put_u((uint32_t)chip.bus);
  putc(':');
  put_u((uint32_t)chip.dev);
  putc('.');
  put_u((uint32_t)chip.fun);
  puts_k("\n");
  stage = 3;

  rc = rtw_power_on(&chip);
  if (rc != 0) {
    puts_k("  ligar a placa falhou: ");
    puts_k(rtw_error());
    puts_k("\n");
    return -1;
  }
  if (rtw_fw_pick(&chip, fw_buf, fw_len) != 0) {
    puts_k("  firmware: ");
    puts_k(rtw_error());
    puts_k("\n");
    rtw_power_off();
    return -1;
  }
  puts_k("  firmware escolhido: ");
  put_u(chip.fw_size / 1024u);
  puts_k(" KB para o cv ");
  put_u((uint32_t)chip.fw_cv);
  puts_k(chip.fw_type == 5 ? " (normal CE)\n" : " (normal)\n");
  stage = 4;

  puts_k("  placa ligada: chip versao ");
  putc((char)('A' + chip.cv));
  puts_k(", MAC ");
  put_mac(chip.mac);
  puts_k(", RFE ");
  put_u((uint32_t)chip.rfe);
  puts_k(chip.efuse_ok ? "\n" : " (efuse sem autoload)\n");
  stage = 5;

  puts_k("  enviando o firmware para a placa...\n");
  if (rtw_fw_load(&chip, fw_buf) != 0) {
    puts_k("  envio do firmware falhou: ");
    puts_k(rtw_error());
    puts_k("\n");
    rtw_power_off();
    return -1;
  }
  puts_k("  firmware rodando na placa\n");
  stage = 6;

  if (rtw_mac_init() != 0) {
    puts_k("  inicializar a MAC falhou: ");
    puts_k(rtw_error());
    puts_k("\n");
    rtw_power_off();
    return -1;
  }
  puts_k("  MAC pronta, firmware respondeu pelo RX\n");
  stage = 7;

  /* escuta cada canal das redes do WIFI.CFG, na ordem do arquivo */
  {
    int j, sel = -1, heard = 0;
    for (j = 0; j < nets_n && sel < 0; j++) {
      int ch = nets[j].channel ? nets[j].channel : WIFI_DEF_CHANNEL;
      int k;
      for (k = 0; k < j; k++)
        if ((nets[k].channel ? nets[k].channel : WIFI_DEF_CHANNEL) == ch) break;
      if (k < j) continue;  /* canal ja escutado */
      puts_k("  ligando o radio no canal ");
      put_u((uint32_t)ch);
      puts_k("...\n");
      if (rtw_radio_init(ch) != 0) {
        puts_k("  ligar o radio falhou: ");
        puts_k(rtw_error());
        puts_k("\n");
        rtw_power_off();
        return -1;
      }
      stage = 8;
      rc = listen_beacons(bg ? 1500 : 3000, ch);
      if (rc != -2) heard = 1;
      if (rc >= 0) sel = rc;
    }
    if (sel < 0) {
      puts_k(heard ? "  nenhuma rede do WIFI.CFG apareceu\n" : "");
      rtw_power_off();
      return -1;
    }
    cfg = nets[sel];
    puts_k("  usando a rede \"");
    puts_k(cfg.ssid);
    puts_k("\" (a senha dela no WIFI.CFG)\n");
    rtw_log("[wifi] rede escolhida: \"");
    rtw_log(cfg.ssid);
    rtw_log("\"\n");
  }
  wpa_pmk(cfg.psk, (const uint8_t*)cfg.ssid, cfg.ssid_len, pmk);
  puts_k("  chave WPA2 (PMK) calculada\n");
  stage = 9;
  if (!cfg.has_bssid) {
    int k;
    for (k = 0; k < 6; k++) cfg.bssid[k] = ap_bssid[k];
  }
  if (rtw_sta_init(chip.mac, ap_bssid) != 0) {
    puts_k("  preparar a estacao falhou: ");
    puts_k(rtw_error());
    puts_k("\n");
    rtw_power_off();
    return -1;
  }
  puts_k("  autenticando no AP...\n");
  rc = authenticate();
  if (rc < 0) {
    puts_k("  o AP nao respondeu a autenticacao\n");
    if (!bg) save_regs();
    rtw_power_off();
    return -1;
  }
  if (rc > 0) {
    puts_k("  o AP recusou a autenticacao, status ");
    put_u((uint32_t)rc);
    puts_k("\n");
    rtw_power_off();
    return -1;
  }
  puts_k("  autenticado (o AP respondeu: a placa transmite)\n");
  stage = 10;

  {
    uint32_t aid = 0;
    puts_k("  associando...\n");
    rc = associate(&aid);
    if (rc != 0) {
      puts_k(rc < 0 ? "  o AP nao respondeu a associacao\n" : "  o AP recusou a associacao, status ");
      if (rc > 0) {
        put_u((uint32_t)rc);
        puts_k("\n");
      }
      rtw_power_off();
      return -1;
    }
    puts_k("  associado, AID ");
    put_u(aid);
    puts_k("\n");
    stage = 11;
    if (rtw_sta_connect(chip.mac, ap_bssid, aid) != 0) {
      puts_k("  falhou: ");
      puts_k(rtw_error());
      puts_k("\n");
    }
  }
  wpa_sm_init(&sm, pmk, ap_bssid, chip.mac);
  keys_ready = 0;
  tx_seq = 0;
  connected = 1;
  puts_k("  handshake WPA2...\n");
  rc = handshake(5000);
  if (rc != 0) {
    puts_k(rc == -2 ? "  o AP desconectou durante o handshake (senha errada?)\n"
                    : "  o handshake WPA2 nao terminou (senha errada?)\n");
    connected = 0;
    rtw_power_off();
    return -1;
  }
  puts_k("  WPA2 pronto (chaves instaladas)\n");
  stage = 12;
  return 0;
}

/* ---------- vigia do link e reconexao ----------
 * Como o mac80211 e o wpa_supplicant: a queda vem do deauth/disassoc do AP,
 * de beacons que param ou de envios seguidos sem ACK (ai primeiro sonda o
 * AP com probe requests; so sem resposta e queda) ou da fila de TX travada.
 * Depois tenta de novo com espera crescente: associar outra vez no mesmo AP
 * e, se isso nao basta (ou a placa travou), desligar e religar a placa
 * escutando as redes do WIFI.CFG. Volta pedindo o mesmo IP ao DHCP. */

enum { W_OFF, W_UP, W_PROBE, W_DOWN };
static const char* const w_name[] = {"desligado", "conectado", "sondando o AP", "reconectando"};
static const uint32_t w_wait[] = {1000, 2000, 5000, 10000, 20000, 30000};

static int w_state;
static int w_busy;
static uint32_t w_now;       /* ms desta volta */
static uint32_t w_heard;     /* ultimo quadro do AP */
static uint32_t w_bcn0;      /* rx_bcn ao conectar: os beacons chegam depois? */
static uint32_t w_probe_at;
static int w_probes;
static uint32_t w_next;      /* proxima tentativa (W_DOWN) */
static uint32_t w_tries;     /* tentativas desde a queda */
static int w_full;           /* a proxima tentativa reinicia a placa */
static int w_silent;         /* tentativas com o AP calado */
static int w_fails;          /* reassociacoes com o AP no ar que falharam */
static uint32_t n_drops, n_reconn, n_restarts;
static const char* w_cause = "";

static void quiet_out(char c) { (void)c; }

static int beacons_seen(void) { return rx_bcn != w_bcn0; }

static void watch_start(void) {
  w_now = net_ms();
  w_heard = w_now;
  w_bcn0 = rx_bcn;
  ap_heard = 0;
  noack_run = 0;
  txfail_run = 0;
  w_dhcp = 0;
  w_dhcp_fails = 0;
  w_state = W_UP;
}

/* Probe request so para o AP (SSID e taxas): a resposta prova que ele ouve. */
static void send_probe(void) {
  uint8_t f[24 + 2 + 32 + sizeof(rates)];
  int n, k;
  for (k = 0; k < 24; k++) f[k] = 0;
  f[0] = 0x40;
  for (k = 0; k < 6; k++) {
    f[4 + k] = ap_bssid[k];
    f[10 + k] = chip.mac[k];
    f[16 + k] = ap_bssid[k];
  }
  n = 24;
  f[n++] = 0;
  f[n++] = (uint8_t)cfg.ssid_len;
  for (k = 0; k < cfg.ssid_len; k++) f[n++] = (uint8_t)cfg.ssid[k];
  for (k = 0; k < (int)sizeof(rates); k++) f[n++] = rates[k];
  rtw_tx_mgmt(f, (uint32_t)n);
}

/* 1 se chega algum quadro do AP em ate ms. */
static int ap_on_air(uint32_t ms) {
  struct rtw_rx rx;
  uint32_t t0 = net_ms(), spins = 0;
  while (!wait_over(t0, &spins, ms))
    while (rtw_rx_poll(&rx))
      if (rx.ring == 0 && rx.type == 0 && rx.len >= 16 && mac_same(rx.data + 10, ap_bssid))
        return 1;
  return 0;
}

static void log_counters(void) {
  rtw_log(" | rx ");
  rtw_log_hex(rx_all);
  rtw_log(" beacons ");
  rtw_log_hex(rx_bcn);
  rtw_log(" tx ");
  rtw_log_hex(tx_n);
  rtw_log(" sem ACK ");
  rtw_log_hex(rpp_noack);
  rtw_log(" recusados ");
  rtw_log_hex(tx_fail);
  rtw_log("\n");
}

static void link_lost(const char* why, int hw) {
  uint32_t halt = rtw_fw_halted();
  int n;
  rtw_log_text(&n);
  if (n > 20000) rtw_log_clear();  /* o log tem 32 KB: fica a queda nova */
  n_drops++;
  w_cause = why;
  rtw_log("[vigia] queda: ");
  rtw_log(why);
  log_counters();
  rtw_log_stall();
  if (halt) {
    /* O firmware parou o DMA (o RX morto do boot 13): associar de novo nao
     * adianta, so religando a placa. */
    rtw_log("[vigia] firmware em halt ");
    rtw_log_hex(halt);
    rtw_log(", reiniciando a placa\n");
    w_cause = "o firmware da placa parou (halt)";
    hw = 1;
  }
  connected = 0;
  keys_ready = 0;
  w_state = W_DOWN;
  w_tries = 0;
  w_full = hw;
  w_silent = 0;
  w_fails = 0;
  w_next = w_now;  /* a primeira tentativa e logo */
}

/* Associa de novo no mesmo AP, sem religar a placa. */
static int reassociate(void) {
  uint32_t aid = 0;
  if (rtw_sta_disconnect(chip.mac, ap_bssid) != 0) {
    rtw_log("[vigia] o firmware nao respondeu ao NO_LINK\n");
    w_full = 1;
    return -1;
  }
  if (authenticate() != 0) return -1;
  if (associate(&aid) != 0) return -1;
  if (rtw_sta_connect(chip.mac, ap_bssid, aid) != 0) {
    w_full = 1;
    return -1;
  }
  wpa_sm_init(&sm, pmk, ap_bssid, chip.mac);
  keys_ready = 0;
  tx_seq = 0;
  connected = 1;
  if (handshake(5000) != 0) {
    connected = 0;
    return -1;
  }
  return 0;
}

/* O mesmo IP de antes; se o servidor recusar, um novo. Sem resposta, o link
 * fica de pe e o DHCP tenta de novo a cada 15 s; da segunda vez sem resposta
 * pede do zero (o reinicio pode ter escolhido outra rede do WIFI.CFG). */
static void dhcp_again(void) {
  int rc;
  if (!cfg.dhcp) return;
  rc = w_dhcp_fails >= 1 ? -1 : net_dhcp_reboot();
  if (rc == -1 || rc == -3) rc = net_dhcp();
  w_dhcp = rc != 0;
  w_dhcp_fails = w_dhcp ? w_dhcp_fails + 1 : 0;
  w_dhcp_at = net_ms();
  rtw_log(w_dhcp ? "[vigia] DHCP sem resposta, tenta depois\n" : "[vigia] DHCP ok\n");
}

static void reconnect(void) {
  void (*prev)(char) = shell_get_out();
  int rc, n;
  w_tries++;
  rtw_log("[vigia] tentativa ");
  rtw_log_hex(w_tries);
  rtw_log(w_full ? ": reiniciando a placa\n" : ": associando de novo\n");
  shell_set_out(quiet_out);
  bg = 1;
  if (w_full) {
    n_restarts++;
    connected = 0;
    rtw_power_off();
    rc = chip_up();
    w_full = rc != 0;  /* falhou: a placa ficou desligada, so religando */
    w_silent = 0;
    w_fails = 0;
  } else if (beacons_seen() && !ap_on_air(600)) {
    rtw_log("[vigia] o AP nao aparece\n");
    rc = -1;
    /* Calado pode ser o RX da placa que morreu sem halt. */
    if (rtw_fw_halted() || ++w_silent >= 3) w_full = 1;
  } else {
    rc = reassociate();
    if (rc != 0 && ++w_fails >= 2) w_full = 1;
  }
  if (rc == 0) {
    watch_start();
    dhcp_again();
    n_reconn++;
    rtw_log("[vigia] reconectado\n");
  }
  bg = 0;
  shell_set_out(prev);
  if (rc != 0) {
    n = (int)w_tries - 1;
    if (n >= (int)(sizeof(w_wait) / sizeof(w_wait[0]))) n = (int)(sizeof(w_wait) / sizeof(w_wait[0])) - 1;
    w_next = net_ms() + w_wait[n];
  }
  {
    const char* t = rtw_log_text(&n);
    file_save(WIFI_LOG_PATH, t, n);
  }
}

/* Roda a cada net_poll (net.c), fora das esperas da pilha. */
static void wifi_tick(void) {
  if (w_state == W_OFF || w_busy) return;
  w_busy = 1;
  w_now = net_ms();
  if (ap_heard) {
    ap_heard = 0;
    w_heard = w_now;
    if (w_state == W_PROBE) {
      rtw_log("[vigia] o AP respondeu\n");
      w_state = W_UP;
    }
  }
  if ((w_state == W_UP || w_state == W_PROBE) && !connected) {
    link_lost("deauth/disassoc do AP", 0);
  } else if ((w_state == W_UP || w_state == W_PROBE) && txfail_run >= 10) {
    link_lost("a fila de TX da placa nao anda", 1);
  } else if ((w_state == W_UP || w_state == W_PROBE) && rtw_fw_halted()) {
    link_lost("o firmware da placa parou (halt)", 1);
  } else if (w_state == W_UP) {
    /* So com beacons chegando depois de conectar (senao o filtro da placa
     * os esconde e o silencio nao quer dizer nada). */
    if (net_has_clock() && beacons_seen() && (w_now - w_heard > 4000u || noack_run >= 16)) {
      rtw_log(noack_run >= 16 ? "[vigia] envios sem ACK, sondando o AP\n"
                              : "[vigia] sem beacons, sondando o AP\n");
      noack_run = 0;
      w_probes = 0;
      w_state = W_PROBE;
    } else if (w_dhcp && w_now - w_dhcp_at >= 15000u) {
      dhcp_again();
    }
  }
  if (w_state == W_PROBE) {
    if (w_probes < 3 && (!w_probes || w_now - w_probe_at >= 500u)) {
      send_probe();
      w_probes++;
      w_probe_at = w_now;
    } else if (w_probes >= 3 && w_now - w_probe_at >= 1000u) {
      link_lost("o AP parou de responder (sem beacons nem probe response)", 0);
    }
  }
  if (w_state == W_DOWN && (int32_t)(w_now - w_next) >= 0) reconnect();
  w_busy = 0;
}

int wifi_drop(int restart) {
  if (w_state == W_OFF) {
    puts_k("WiFi nao conectado.\n");
    return -1;
  }
  w_now = net_ms();
  link_lost(restart ? "teste: net wifi drop placa" : "teste: net wifi drop", restart);
  puts_k(restart ? "Queda simulada: o vigia vai reiniciar a placa.\n"
                 : "Queda simulada: o vigia vai associar de novo.\n");
  return 0;
}

void wifi_note(const char* what, uint32_t v) {
  uint32_t a, b, c;
  int n;
  const char* t;
  rtw_log(connected ? "[net] " : "[net] (AP desconectou) ");
  rtw_log(what);
  rtw_log_hex(v);
  rtw_log(" | rx dados ");
  rtw_log_hex(rx_data);
  rtw_log(" unicast ");
  rtw_log_hex(rx_ucast);
  rtw_log(" sem decifrar ");
  rtw_log_hex(rx_bad);
  rtw_log(" ethernet ");
  rtw_log_hex(rx_eth);
  rtw_log("\n      rx total ");
  rtw_log_hex(rx_all);
  rtw_log(" beacons ");
  rtw_log_hex(rx_bcn);
  rtw_log(" | tx ");
  rtw_log_hex(tx_n);
  rtw_log(" recusados ");
  rtw_log_hex(tx_fail);
  rtw_log(" ok ");
  rtw_log_hex(rpp_ok);
  rtw_log(" sem ACK ");
  rtw_log_hex(rpp_noack);
  rtw_ring_state(&a, &b, &c);
  rtw_log(" | RXQ ");
  rtw_log_hex(a);
  rtw_log(" RPQ ");
  rtw_log_hex(b);
  rtw_log(" ACH0 ");
  rtw_log_hex(c);
  rtw_log("\n");
  rtw_log_stall();
  if (tx_fail) {
    rtw_log("      ultimo erro: ");
    rtw_log(rtw_error());
    rtw_log("\n");
  }
  t = rtw_log_text(&n);
  file_save(WIFI_LOG_PATH, t, n);  /* sem mensagem: pode estar no desktop */
}

/* net wifi watch: pinga o gateway a cada 2 s por 'secs' segundos e anota
 * os contadores, para ver quando o RX morre sem o navegador no meio. */
int wifi_watch(int secs) {
  uint32_t ip, mask, gw, dns, t0, a, b, c;
  uint16_t seq = 1;
  int ok = 0;
  if (!connected) {
    puts_k("WiFi nao conectado.\n");
    return -1;
  }
  net_get_addr(&ip, &mask, &gw, &dns);
  rtw_log("[watch] inicio\n");
  rtw_log_stall();
  t0 = net_ms();
  while (net_ms() - t0 < (uint32_t)secs * 1000u) {
    uint32_t rtt = 0;
    int rc = net_ping(gw, seq++, 32, 1000, &rtt, 0);
    if (rc == 0) ok++;
    rtw_ring_state(&a, &b, &c);
    rtw_log("[watch] ms ");
    rtw_log_hex(net_ms() - t0);
    rtw_log(rc == 0 ? " ping ok " : " ping falhou ");
    rtw_log_hex(rtt);
    rtw_log(" rx ");
    rtw_log_hex(rx_all);
    rtw_log(" bcn ");
    rtw_log_hex(rx_bcn);
    rtw_log(" tx ");
    rtw_log_hex(tx_n);
    rtw_log(" rpp ");
    rtw_log_hex(rpp_ok);
    rtw_log(" RXQ ");
    rtw_log_hex(a);
    rtw_log(" ptcl ");
    rtw_log_hex(rtw_reg(0xC6C4u));
    rtw_log(" hisr00 ");
    rtw_log_hex(rtw_reg(0x10B4u));
    rtw_log("\n");
    puts_k(rc == 0 ? "." : "x");
    net_delay(1000);
  }
  puts_k("\n");
  rtw_log("[watch] fim\n");
  rtw_log_stall();
  save_log();
  return ok;
}

int wifi_start(const char* path) {
  int rc;
  w_state = W_OFF;
  rtw_log_clear();
  rc = start_steps(path);
  save_log();
  return rc;
}

/* ---------- scan: procura redes no ar sem conectar ---------- */

/* 5 GHz no Brasil: UNII-1, UNII-2A, UNII-2C e UNII-3. */
static const uint8_t canais_5g[] = {
  36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
  132, 136, 140, 144, 149, 153, 157, 161, 165
};

/* Le a lista de canais do argumento: 0/"" ou "auto" = todo o 5 GHz, "2g" =
 * 1..13, senao numeros separados por virgula. Devolve quantos canais. */
static int scan_canais(const char* arg, int* ch, int max) {
  int n = 0, i;
  if (arg && *arg && !name_eq(arg, "auto")) {
    if (name_eq(arg, "2g")) {
      for (i = 1; i <= 13 && n < max; i++) ch[n++] = i;
      return n;
    }
    while (*arg && n < max) {
      int v = 0;
      while (*arg >= '0' && *arg <= '9') v = v * 10 + (*arg++ - '0');
      if (v >= 1 && v <= 196) ch[n++] = v;
      if (*arg == ',' || *arg == ' ') arg++;
      else if (*arg) break;
    }
    return n;
  }
  for (i = 0; i < (int)sizeof(canais_5g) && n < max; i++) ch[n++] = canais_5g[i];
  return n;
}

/* Varre os canais e lista os beacons na tela e no WIFI.LOG, sem autenticar.
 * O log sai so com esta varredura (rtw_log_clear), entao o WIFI.EXE le a
 * lista fresca. 0 = ok; <0 = placa, canais ou WiFi ja conectado. */
int wifi_scan(const char* arg) {
  int ch[32];
  int n = scan_canais(arg, ch, 32);
  int i;
  if (n <= 0) {
    puts_k("net wifi scan: canais invalidos (ex.: 36,52 ou auto)\n");
    return -1;
  }
  if (connected) {
    puts_k("net wifi scan: o WiFi esta conectado; use net wifi stop antes\n");
    return -1;
  }
  if (w_state != W_OFF) wifi_stop();

  /* marca com "*" no log as redes do cfg que aparecerem no ar */
  nets_n = wifi_cfg_load(WIFI_CFG_PATH, nets, WIFI_CFG_MAX);
  if (nets_n < 0) nets_n = 0;

  puts_k("WiFi: procurando em ");
  put_u((uint32_t)n);
  puts_k(" canal(is); a lista sai no fim\n");
  rtw_log_clear();
  if (load_firmware() != 0) {
    save_log();
    return -1;
  }
  if (chip_up() != 0) {
    save_log();
    return -1;
  }
  scan_on = 1;
  for (i = 0; i < n; i++) {
    puts_k("  canal ");
    put_u((uint32_t)ch[i]);
    puts_k("\n");
    if (rtw_radio_init(ch[i]) != 0) {
      puts_k("  ligar o radio falhou: ");
      puts_k(rtw_error());
      puts_k("\n");
      continue;
    }
    listen_beacons(250, ch[i]);
  }
  scan_on = 0;
  rtw_power_off();
  stage = 0;
  save_log();
  puts_k("  varredura gravada em " WIFI_LOG_PATH "\n");
  return 0;
}

void wifi_stop(void) {
  if (connected || w_state != W_OFF) net_attach(0);
  w_state = W_OFF;
  connected = 0;
  keys_ready = 0;
  rtw_power_off();
  stage = 0;
  puts_k("WiFi desligado.\n");
}

void wifi_status(void) {
  puts_k("WiFi: ");
  puts_k(stage_name[stage]);
  puts_k("\n");
  if (stage >= 9) {
    puts_k("  rede \"");
    puts_k(cfg.ssid);
    puts_k("\"\n");
  } else if (stage >= 1) {
    puts_k("  ");
    put_u((uint32_t)nets_n);
    puts_k(" rede(s) no WIFI.CFG\n");
  }
  if (stage >= 5) {
    puts_k("  MAC ");
    put_mac(chip.mac);
    puts_k("\n");
  }
  if (w_state != W_OFF || n_drops) {
    puts_k("  vigia: ");
    puts_k(w_name[w_state]);
    if (w_state == W_DOWN) {
      uint32_t now = net_ms();
      puts_k(", tentativa ");
      put_u(w_tries + 1u);
      puts_k(" em ");
      put_u((int32_t)(w_next - now) > 0 ? (w_next - now) / 1000u : 0u);
      puts_k(" s");
    }
    if (w_state != W_OFF && !beacons_seen()) puts_k(" (sem beacons visiveis: so deauth e TX)");
    puts_k("\n  quedas ");
    put_u(n_drops);
    puts_k(", reconexoes ");
    put_u(n_reconn);
    puts_k(", placa reiniciada ");
    put_u(n_restarts);
    puts_k("x\n");
    if (n_drops) {
      puts_k("  ultima queda: ");
      puts_k(w_cause);
      puts_k("\n");
    }
  }
  {
    uint32_t ip, mask, gw, dns;
    const char* l = net_link_name();
    net_get_addr(&ip, &mask, &gw, &dns);
    puts_k("Rede: ");
    puts_k(l ? l : "sem placa");
    if (l) {
      puts_k("  ip ");
      put_ip(ip);
      puts_k("  gateway ");
      put_ip(gw);
      puts_k("  dns ");
      put_ip(dns);
    }
    puts_k("\n");
    if (l && net_dhcp_renew_in()) {
      puts_k("  DHCP: renova o aluguel em ");
      put_u(net_dhcp_renew_in());
      puts_k(" s\n");
    }
  }
}
