/* sockt.c - teste dos soquetes k_sock_* do kernel (upload, download, DNS).
 *
 * SOCKT [ip] [porta] [KB]  (padrao 10.0.2.2 9555 1024)
 * Manda 4 bytes com o tamanho e KB*1024 bytes (padrao i*7+3); o servidor
 * (tools/sockt_srv.py) responde a soma e devolve o mesmo tanto de bytes.
 * Mostra o tempo de cada sentido e o de duas consultas DNS (a segunda deve
 * vir do cache do kernel).
 */
#include <so.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned parse_ip(const char* s) {
  unsigned ip = 0, part = 0;
  int dots = 0;
  for (; *s; s++) {
    if (*s == '.') {
      ip = (ip << 8) | part;
      part = 0;
      dots++;
    } else {
      part = part * 10 + (unsigned)(*s - '0');
    }
  }
  return dots == 3 ? (ip << 8) | part : 0;
}

static unsigned char buf[16384];

static int wait_bits(int h, int bits, unsigned limit_ms) {
  unsigned t0 = k_ms();
  for (;;) {
    int m = k_sock_poll(h);
    if (m & 4) return -1;
    if (m & bits) return m;
    if (k_ms() - t0 > limit_ms) return -2;
    k_yield();
  }
}

int main(int argc, char** argv) {
  unsigned ip = parse_ip(argc > 1 ? argv[1] : "10.0.2.2");
  int port = argc > 2 ? atoi(argv[2]) : 9555;
  int kb = argc > 3 ? atoi(argv[3]) : 1024;
  unsigned total = (unsigned)kb * 1024u, sent = 0, got = 0, sum = 0, t0, t1, i;
  unsigned dip = 0;
  int h, rc;
  unsigned char hdr[4];

  t0 = k_ms();
  rc = k_dns("example.com", &dip);
  t1 = k_ms();
  printf("dns 1: rc=%d ip=%u.%u.%u.%u %u ms\n", rc, dip >> 24, (dip >> 16) & 255,
         (dip >> 8) & 255, dip & 255, t1 - t0);
  t0 = k_ms();
  rc = k_dns("example.com", &dip);
  t1 = k_ms();
  printf("dns 2: rc=%d %u ms\n", rc, t1 - t0);

  h = k_sock_connect(ip, port);
  if (h <= 0) {
    printf("connect falhou %d\n", h);
    return 1;
  }
  if (wait_bits(h, 8, 5000) < 0) {
    printf("sem conexao (err %d)\n", k_sock_err(h));
    return 1;
  }
  hdr[0] = (unsigned char)(total >> 24);
  hdr[1] = (unsigned char)(total >> 16);
  hdr[2] = (unsigned char)(total >> 8);
  hdr[3] = (unsigned char)total;
  while (k_sock_send(h, hdr, 4) != 4) k_yield();
  t0 = k_ms();
  while (sent < total) {
    int n = total - sent > sizeof(buf) ? (int)sizeof(buf) : (int)(total - sent);
    int k;
    for (k = 0; k < n; k++) buf[k] = (unsigned char)((sent + k) * 7 + 3);
    k = k_sock_send(h, buf, n);
    if (k == -2) {
      printf("send erro %d em %u\n", k_sock_err(h), sent);
      return 1;
    }
    if (k > 0) sent += (unsigned)k;
    else if (wait_bits(h, 2, 20000) < 0) {
      printf("send parado em %u\n", sent);
      return 1;
    }
  }
  /* resposta: "S <soma>\n" e depois total bytes */
  {
    char line[64];
    int ln = 0;
    unsigned want_sum = 0;
    for (i = 0; i < total; i++) want_sum += (unsigned char)(i * 7 + 3);
    while (ln < (int)sizeof(line) - 1) {
      int k = k_sock_recv(h, line + ln, 1, 0);
      if (k == 1) {
        if (line[ln] == '\n') break;
        ln++;
      } else if (k == -1) {
        if (wait_bits(h, 1, 30000) < 0) {
          printf("sem resposta\n");
          return 1;
        }
      } else {
        printf("recv erro %d\n", k);
        return 1;
      }
    }
    line[ln] = 0;
    t1 = k_ms();
    printf("upload %u KB: %u ms (%u KB/s) servidor: %s esperado %u\n", (unsigned)kb, t1 - t0,
           t1 > t0 ? (unsigned)kb * 1000u / (t1 - t0) : 0, line, want_sum);
  }
  t0 = k_ms();
  while (got < total) {
    int k = k_sock_recv(h, buf, sizeof(buf), 0);
    if (k > 0) {
      int j;
      for (j = 0; j < k; j++) sum += buf[j];
      got += (unsigned)k;
    } else if (k == -1) {
      if (wait_bits(h, 1, 20000) < 0) break;
    } else {
      break;
    }
  }
  t1 = k_ms();
  printf("download %u de %u bytes: %u ms soma %u\n", got, total, t1 - t0, sum);
  k_sock_close(h);
  return got == total ? 0 : 1;
}
