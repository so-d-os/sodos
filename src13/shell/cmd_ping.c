#include "shell.h"
#include "net.h"

static void put_u32(uint32_t n) {
  char num[16];
  uint32_to_string(n, num, (int)sizeof(num));
  puts_k(num);
}

static void put_ip(uint32_t ip) {
  int i;
  for (i = 3; i >= 0; i--) {
    put_u32((ip >> (i * 8)) & 0xFFu);
    if (i) putc('.');
  }
}

static int usage(void) {
  puts_k("Uso: ping <host|ip> [-n vezes] [-l bytes]\n");
  puts_k("Ex.: ping 10.0.2.2   ping example.com -n 2\n");
  return 1;
}

int cmd_ping(int argc, char** argv) {
  const char* host = 0;
  uint32_t count = 4;
  uint32_t size = 32;
  uint32_t ip;
  uint32_t sent = 0;
  uint32_t recv = 0;
  uint32_t rtt_min = 0xFFFFFFFFu;
  uint32_t rtt_max = 0;
  uint32_t rtt_sum = 0;
  uint32_t seq;
  int i;
  for (i = 1; i < argc; i++) {
    if (name_eq(argv[i], "-n") && i + 1 < argc) {
      count = str_to_uint32(argv[++i], 10);
    } else if (name_eq(argv[i], "-l") && i + 1 < argc) {
      size = str_to_uint32(argv[++i], 10);
    } else if (argv[i][0] == '-' || host) {
      return usage();
    } else {
      host = argv[i];
    }
  }
  if (!host || count == 0) return usage();
  if (size > 1472) size = 1472;
  if (!net_ready()) {
    puts_k("ping: sem placa de rede\n");
    return 1;
  }
  if (net_resolve(host, &ip) < 0) {
    puts_k("ping: nao foi possivel resolver ");
    puts_k(host);
    puts_k("\n");
    return 1;
  }
  puts_k("Disparando ");
  puts_k(host);
  puts_k(" [");
  put_ip(ip);
  puts_k("] com ");
  put_u32(size);
  puts_k(" bytes de dados:\n");
  for (seq = 1; seq <= count; seq++) {
    uint32_t rtt = 0;
    uint8_t ttl = 0;
    int rc = net_ping(ip, (uint16_t)seq, (int)size, 2000, &rtt, &ttl);
    sent++;
    if (rc == 0) {
      recv++;
      if (rtt < rtt_min) rtt_min = rtt;
      if (rtt > rtt_max) rtt_max = rtt;
      rtt_sum += rtt;
      puts_k("Resposta de ");
      put_ip(ip);
      puts_k(": bytes=");
      put_u32(size);
      puts_k(rtt == 0 ? " tempo<1ms" : " tempo=");
      if (rtt) {
        put_u32(rtt);
        puts_k("ms");
      }
      puts_k(" TTL=");
      put_u32(ttl);
      puts_k(" seq=");
      put_u32(seq);
      puts_k("\n");
    } else if (rc == -1) {
      puts_k("Falha ao enviar (ARP sem resposta).\n");
    } else {
      puts_k("Esgotado o tempo limite do pedido.\n");
    }
    if (seq < count) net_delay(1000);
  }
  puts_k("\nEstatisticas do ping para ");
  put_ip(ip);
  puts_k(":\n  Pacotes: enviados=");
  put_u32(sent);
  puts_k(", recebidos=");
  put_u32(recv);
  puts_k(", perdidos=");
  put_u32(sent - recv);
  puts_k(" (");
  put_u32((sent - recv) * 100 / sent);
  puts_k("% de perda)\n");
  if (recv) {
    puts_k("  Tempo: minimo=");
    put_u32(rtt_min);
    puts_k("ms, maximo=");
    put_u32(rtt_max);
    puts_k("ms, media=");
    put_u32(rtt_sum / recv);
    puts_k("ms");
    if (!net_has_clock()) puts_k(" (sem PIT, tempos aproximados)");
    puts_k("\n");
  }
  return recv ? 0 : 1;
}
