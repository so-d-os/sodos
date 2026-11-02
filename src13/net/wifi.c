/* WiFi: le WIFI.CFG, liga o RTL8852BE e prepara o WPA2. Ver wifi.h. */
#include "wifi.h"
#include "wpa.h"
#include "rtw89.h"
#include "net.h"
#include "shell.h"
#include "serial.h"
#include "drives.h"
#include "mm.h"

static struct wifi_cfg cfg;
static struct rtw_info chip;
static uint8_t pmk[32];
static int stage;  /* ultima etapa que deu certo */
static uint8_t* fw_buf;
static uint32_t fw_len;

static const char* stage_name[] = {
  "parado", "configuracao lida", "chaves WPA2 e firmware prontos", "placa encontrada",
  "firmware escolhido", "placa ligada (versao e efuse lidos)", "firmware rodando na placa"
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

int wifi_cfg_load(const char* path, struct wifi_cfg* c) {
  static char text[2048];
  int n = file_load(path, text, (int)sizeof(text));
  int i = 0;
  if (n < 0) return -1;
  {
    uint8_t* z = (uint8_t*)c;
    int k;
    for (k = 0; k < (int)sizeof(*c); k++) z[k] = 0;
  }
  c->dhcp = 1;
  while (i < n) {
    char* line = text + i;
    char* key;
    char* val;
    char* e;
    int len = 0;
    while (i + len < n && text[i + len] != '\n') len++;
    i += len + 1;
    line[len] = 0;
    if (len && line[len - 1] == '\r') line[--len] = 0;
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '#' || *line == ';' || !*line) continue;
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
  if (!c->ssid_len) return -2;
  {
    int pl = 0;
    while (c->psk[pl]) pl++;
    if (pl < 8 || pl > 63) return -2;
  }
  return 0;
}

/* ---------- etapas ---------- */

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
  rc = wifi_cfg_load(path, &cfg);
  if (rc == -1) {
    say("Nao consegui ler o arquivo de configuracao.\n");
    return -1;
  }
  if (rc == -2) {
    say("WIFI.CFG precisa de ssid= e psk= (8 a 63 caracteres).\n");
    return -1;
  }
  puts_k("  rede \"");
  puts_k(cfg.ssid);
  puts_k("\"");
  if (cfg.has_bssid) {
    puts_k(" bssid ");
    put_mac(cfg.bssid);
  }
  if (cfg.channel) {
    puts_k(" canal ");
    put_u((uint32_t)cfg.channel);
  }
  puts_k(cfg.dhcp ? " ip dhcp\n" : " ip fixo\n");
  stage = 1;

  rc = wpa_selftest();
  if (rc) {
    puts_k("  autoteste WPA2 falhou no teste ");
    put_u((uint32_t)rc);
    puts_k("\n");
    return -1;
  }
  wpa_pmk(cfg.psk, (const uint8_t*)cfg.ssid, cfg.ssid_len, pmk);
  puts_k("  chave WPA2 (PMK) calculada\n");
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

  /* Etapa 3 do driver: ligar BB/RF, procurar a rede, associar. */
  rtw_power_off();
  say("Proxima etapa do driver (ligar o radio e procurar a rede) ainda nao existe.\n");
  puts_k("  A placa foi desligada de novo.\n");
  return -2;
}

int wifi_start(const char* path) {
  int rc;
  rtw_log_clear();
  rc = start_steps(path);
  save_log();
  return rc;
}

void wifi_stop(void) {
  rtw_power_off();
  stage = 0;
  puts_k("WiFi desligado.\n");
}

void wifi_status(void) {
  puts_k("WiFi: ");
  puts_k(stage_name[stage]);
  puts_k("\n");
  if (stage >= 1) {
    puts_k("  rede \"");
    puts_k(cfg.ssid);
    puts_k("\"\n");
  }
  if (stage >= 5) {
    puts_k("  MAC ");
    put_mac(chip.mac);
    puts_k("\n");
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
  }
}
