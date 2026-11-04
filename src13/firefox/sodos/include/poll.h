/* poll.h - poll(2) cooperativo do SODOS.
 *
 * Fds de soquete sao os handles de sodos_net (SODOS_SOCK_FD_BASE em diante).
 * Arquivos comuns nao entram no poll; use leitura direta. O poll devolve
 * POLLIN quando ha bytes no buffer TCP, POLLOUT/SODOS_POLLCONN sempre que a
 * conexao esta estabelecida e POLLHUP no fim do fluxo.
 */
#ifndef _POLL_H
#define _POLL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int nfds_t;

struct pollfd {
  int fd;
  short events;
  short revents;
};

#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
#define POLLRDNORM 0x0040
#define POLLWRNORM 0x0100

int poll(struct pollfd* fds, nfds_t nfds, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
