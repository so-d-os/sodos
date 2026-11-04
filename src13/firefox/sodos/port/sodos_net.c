/* sodos_net.c - soquetes do port sobre a unica conexao TCP do kernel.
 *
 * O kernel (net/net.c, api_connect/api_send/api_recv) mantem UM TCB e uma
 * conexao por vez. Aqui o handle e sempre SODOS_SOCK_FD_BASE e o acesso e
 * serializado: abrir de novo fecha a anterior. Multiplas conexoes, poll de
 * verdade e TLS ficam para o M1 (tabela de TCB no kernel + NSS).
 */
#include <so.h>
#include <errno.h>

#include "sodos.h"
#include "sodos_internal.h"

#ifndef EINTR
#define EINTR 4
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED 111
#endif

static int net_fd = -1;

int sodos_net_connect(const char* host, int port) {
  int rc;
  if (!host || port <= 0 || port > 65535) {
    errno = EINVAL;
    return -1;
  }
  if (net_fd >= 0) sodos_net_close();
  rc = k_tcp_connect(host, port);
  if (rc != 0) {
    errno = (rc == -3) ? EINTR : ECONNREFUSED;
    return rc;   /* -1 erro, -2 sem rede/dns, -3 interrompido */
  }
  net_fd = SODOS_SOCK_FD_BASE;
  return net_fd;
}

int sodos_net_send(const void* buf, int len) {
  int sent = 0;
  if (net_fd < 0 || !buf || len < 0) return -1;
  while (sent < len) {
    int n = k_tcp_send((const char*)buf + sent, len - sent);
    if (n == -3) return -3;
    if (n <= 0) return -1;
    sent += n;
  }
  return sent;
}

int sodos_net_recv(void* buf, int cap) {
  if (net_fd < 0 || !buf || cap < 2) return -2;
  return k_tcp_recv(buf, cap);
}

int sodos_net_pending(void) {
  if (net_fd < 0) return -1;
  return k_net_pending();
}

int sodos_net_connected(void) { return net_fd >= 0; }

void sodos_net_close(void) {
  if (net_fd >= 0) k_tcp_close();
  net_fd = -1;
}
