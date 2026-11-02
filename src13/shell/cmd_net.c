#include "shell.h"
#include "net.h"
#include "wifi.h"
#include "wpa.h"

static int usage(void) {
  puts_k("Uso: net [status]          placa e endereco atuais\n");
  puts_k("     net dhcp              pede endereco por DHCP\n");
  puts_k("     net wifi start [cfg]  conecta usando " WIFI_CFG_PATH "\n");
  puts_k("     net wifi stop | status | test | watch [s]\n");
  return 1;
}

static void put_ip(uint32_t ip) {
  char num[16];
  int i;
  for (i = 3; i >= 0; i--) {
    uint32_to_string((ip >> (i * 8)) & 0xFFu, num, (int)sizeof(num));
    puts_k(num);
    if (i) putc('.');
  }
}

static int do_dhcp(void) {
  uint32_t ip, mask, gw, dns;
  int rc;
  puts_k("DHCP...\n");
  rc = net_dhcp();
  if (rc == -1) {
    puts_k("net: sem placa de rede\n");
    return 1;
  }
  if (rc < 0) {
    puts_k("net: nenhum servidor DHCP respondeu\n");
    return 1;
  }
  net_get_addr(&ip, &mask, &gw, &dns);
  puts_k("ip ");
  put_ip(ip);
  puts_k("  mascara ");
  put_ip(mask);
  puts_k("  gateway ");
  put_ip(gw);
  puts_k("  dns ");
  put_ip(dns);
  puts_k("\n");
  return 0;
}

static int do_wifi(int argc, char** argv) {
  if (argc < 3 || name_eq(argv[2], "status")) {
    wifi_status();
    return 0;
  }
  if (name_eq(argv[2], "start")) return wifi_start(argc > 3 ? argv[3] : 0) == 0 ? 0 : 1;
  if (name_eq(argv[2], "stop")) {
    wifi_stop();
    return 0;
  }
  if (name_eq(argv[2], "watch")) {
    int secs = argc > 3 ? (int)str_to_uint32(argv[3], 10) : 40;
    return wifi_watch(secs > 0 ? secs : 40) > 0 ? 0 : 1;
  }
  if (name_eq(argv[2], "test")) {
    int rc = wpa_selftest();
    char num[16];
    if (!rc) {
      puts_k("WPA2: SHA1, HMAC, PRF, AES, unwrap, CCMP e PBKDF2 ok\n");
      return 0;
    }
    uint32_to_string((uint32_t)rc, num, (int)sizeof(num));
    puts_k("WPA2: falhou o teste ");
    puts_k(num);
    puts_k("\n");
    return 1;
  }
  return usage();
}

int cmd_net(int argc, char** argv) {
  if (argc < 2 || name_eq(argv[1], "status")) {
    wifi_status();
    return 0;
  }
  if (name_eq(argv[1], "dhcp")) return do_dhcp();
  if (name_eq(argv[1], "wifi")) return do_wifi(argc, argv);
  return usage();
}
