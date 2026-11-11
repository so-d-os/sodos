/* api [status] | api start | api stop | api restart
 *
 * O servico HTTP da API sobe no boot quando ja existe rede. Se a rede
 * apareceu depois (WiFi, DHCP, cabo), este comando sobe o servico sem
 * reiniciar o SO. Sem parametro mostra On/Off, endereco e porta. */
#include "shell.h"
#include "net.h"
#include "httpd.h"

#define API_PORT 8797
/* 10.0.2.15: endereco do SLIRP do QEMU, alcancado no host por 127.0.0.1. */
#define API_QEMU_IP 0x0A00020Fu

static void put_num(uint32_t n) {
  char num[16];
  uint32_to_string(n, num, (int)sizeof(num));
  puts_k(num);
}

static void put_ip(uint32_t ip) {
  int i;
  for (i = 3; i >= 0; i--) {
    put_num((ip >> (i * 8)) & 0xFFu);
    if (i) putc('.');
  }
}

static void put_url(uint32_t ip, int port) {
  puts_k("http://");
  put_ip(ip);
  putc(':');
  put_num((uint32_t)port);
  putc('/');
}

static void show_host_alias(uint32_t ip, int port) {
  if (ip != API_QEMU_IP) return;
  puts_k("  no host QEMU: http://127.0.0.1:");
  put_num((uint32_t)port);
  puts_k("/\n");
}

static void show_on(void) {
  int port = httpd_port();
  uint32_t ip, mask, gw, dns;
  puts_k("API: On\n");
  puts_k("  porta ");
  put_num((uint32_t)port);
  puts_k("\n");
  if (!net_ready()) {
    puts_k("  endereco: (sem link)\n");
    puts_k("  rede: sem link; o servico volta a responder quando a rede voltar\n");
    return;
  }
  net_get_addr(&ip, &mask, &gw, &dns);
  puts_k("  endereco: ");
  put_url(ip, port);
  puts_k("\n");
  show_host_alias(ip, port);
  puts_k("  comandos: GET /exec?cmd=...  |  GET /win/execute?cmd=...  |  GET /win/printscreen\n");
}

static void show_off(void) {
  uint32_t ip, mask, gw, dns;
  puts_k("API: Off\n");
  puts_k("  porta ");
  put_num(API_PORT);
  puts_k(" (parada)\n");
  if (!net_ready()) {
    puts_k("  endereco: (sem rede)\n");
    puts_k("  rede: sem link; use net wifi start (ou net dhcp) e depois api start\n");
    return;
  }
  net_get_addr(&ip, &mask, &gw, &dns);
  puts_k("  endereco quando ligar: ");
  put_url(ip, API_PORT);
  puts_k("\n");
  show_host_alias(ip, API_PORT);
  puts_k("  ligar: api start\n");
}

static int show_status(void) {
  if (httpd_port()) show_on();
  else show_off();
  return 0;
}

static int usage(void) {
  puts_k("Uso: api [status]   mostra On/Off, endereco e porta\n");
  puts_k("     api start     sobe o servico quando a rede estiver pronta\n");
  puts_k("     api stop      para o servico\n");
  puts_k("     api restart   para e sobe de novo\n");
  return 1;
}

int cmd_api(int argc, char** argv) {
  const char* sub = argc > 1 ? argv[1] : "status";
  if (name_eq(sub, "status")) return show_status();
  if (name_eq(sub, "start")) {
    if (httpd_port()) return show_status();
    if (!net_ready()) {
      puts_k("API: sem rede; ligue a rede primeiro (net wifi start ou net dhcp)\n");
      return 1;
    }
    httpd_start(API_PORT);
    if (!httpd_port()) {
      puts_k("API: nao subiu na porta ");
      put_num(API_PORT);
      puts_k("\n");
      return 1;
    }
    return show_status();
  }
  if (name_eq(sub, "stop")) {
    if (httpd_port()) httpd_stop();
    return show_status();
  }
  if (name_eq(sub, "restart")) {
    if (httpd_port()) httpd_stop();
    if (!net_ready()) {
      puts_k("API: sem rede; ligue a rede primeiro (net wifi start ou net dhcp)\n");
      return 1;
    }
    httpd_start(API_PORT);
    if (!httpd_port()) {
      puts_k("API: nao subiu na porta ");
      put_num(API_PORT);
      puts_k("\n");
      return 1;
    }
    puts_k("API: reiniciada\n");
    return show_status();
  }
  return usage();
}
