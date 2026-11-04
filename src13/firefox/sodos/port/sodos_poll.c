/* sodos_poll.c - poll(2) cooperativo.
 *
 * So o handle de sodos_net entra; fds de arquivo dao POLLNVAL (o POSIXX nao
 * compartilha a tabela). O poll cede a CPU em fatias curtas, entao outras
 * threads e o desktop continuam rodando enquanto ele espera.
 */
#include <errno.h>
#include <stdint.h>

#include "poll.h"
#include "sodos.h"

int poll(struct pollfd* fds, nfds_t nfds, int timeout_ms) {
  uint64_t start = sodos_now_ms();

  for (;;) {
    nfds_t i;
    int ready = 0;

    for (i = 0; i < nfds; i++) {
      int fd = fds[i].fd;
      short ev = 0;

      fds[i].revents = 0;
      if (fd < 0) continue;

      if (fd == SODOS_SOCK_FD_BASE) {
        if (!sodos_net_connected()) {
          ev = POLLERR | POLLHUP;
        } else {
          int pending = sodos_net_pending();
          if (pending > 0) ev |= POLLIN;
          else if (pending < 0) ev |= POLLHUP;
          if (fds[i].events & POLLOUT) ev |= POLLOUT;
        }
      } else {
        ev = POLLNVAL;
      }

      fds[i].revents = ev;
      if (ev) ready++;
    }

    if (ready) return ready;
    if (timeout_ms == 0) return 0;

    {
      uint64_t elapsed = sodos_now_ms() - start;
      unsigned slice = 4;
      if (timeout_ms > 0) {
        if (elapsed >= (uint64_t)timeout_ms) return 0;
        if ((uint64_t)timeout_ms - elapsed < slice)
          slice = (unsigned)((uint64_t)timeout_ms - elapsed);
      }
      sodos_sleep_ms(slice ? slice : 1);
    }
  }
}
