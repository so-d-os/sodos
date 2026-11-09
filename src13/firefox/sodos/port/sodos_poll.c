/* sodos_poll.c - poll(2) cooperativo.
 *
 * Entram o handle de sodos_net, pipes e o console (0-2); arquivos dao POLLNVAL (o POSIXX nao
 * compartilha a tabela). O poll cede a CPU em fatias curtas, entao outras
 * threads e o desktop continuam rodando enquanto ele espera.
 */
#include <errno.h>
#include <stdint.h>

#include "poll.h"
#include "sodos.h"

#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
extern int sodos_pipe_is(int fd);
extern short sodos_pipe_poll(int fd, short events);
#else
#define sodos_pipe_is(fd) 0
#define sodos_pipe_poll(fd, events) 0
#endif

extern int __so_stdin_raw;           /* c_compiler/lib/misc.c */
extern int sodos_stdin_ready(void);   /* libs/posix-lite/posix.c */

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
      } else if (fd <= 2 && !sodos_pipe_is(fd)) {
        /* Console: le sempre; fd 0 so tem dado em modo raw (termios). */
        if ((fds[i].events & POLLOUT) && fd > 0) ev |= POLLOUT;
        if ((fds[i].events & POLLIN) && fd == 0 && __so_stdin_raw && sodos_stdin_ready() > 0)
          ev |= POLLIN;
      } else if (sodos_pipe_is(fd)) {
        ev = sodos_pipe_poll(fd, fds[i].events);
      } else {
        ev = POLLNVAL;
      }

      fds[i].revents = ev;
      if (ev) ready++;
    }

    if (ready) return ready;
    if (timeout_ms == 0) {
      /* Threads cooperativas: um laco com poll(0) nao pode prender as demais. */
      sodos_yield();
      return 0;
    }

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
