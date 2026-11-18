/* t_epoll.c - EPOLLTST.EXE: o shim epoll/timerfd/eventfd (dart/port/sodos_epoll.c)
 * no mesmo uso que o eventhandler_linux.cc do Dart faz dele.
 *
 *   x86_64-sodos-gcc64 t_epoll.c ../build/sodos_epoll.o -o EPOLLTST.EXE
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

static int ok, bad;

#define CHECK(c, ...)                         \
  do {                                        \
    if (c) {                                  \
      ok++;                                   \
      printf("ok   " __VA_ARGS__);            \
    } else {                                  \
      bad++;                                  \
      printf("FAIL " __VA_ARGS__);            \
      printf("     (%s:%d errno=%d)\n", __FILE__, __LINE__, errno); \
    }                                         \
  } while (0)

static uint64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int wake_fd;
static void* waker(void* arg) {
  char c = 'x';
  (void)arg;
  usleep(50 * 1000);
  write(wake_fd, &c, 1);
  return NULL;
}

int main(void) {
  int ep, pfd[2], tfd, efd, n;
  struct epoll_event ev, out[8];
  struct itimerspec its;
  uint64_t v, t0, dt;
  pthread_t th;
  char c;

  ep = epoll_create1(EPOLL_CLOEXEC);
  CHECK(ep >= 0, "epoll_create1 = %d\n", ep);
  CHECK(pipe2(pfd, O_CLOEXEC) == 0, "pipe2 %d %d\n", pfd[0], pfd[1]);

  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN;
  ev.data.u64 = 111;
  CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &ev) == 0, "ADD pipe\n");
  CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &ev) < 0 && errno == EEXIST, "ADD de novo = EEXIST\n");
  CHECK(epoll_wait(ep, out, 8, 0) == 0, "nada pronto com timeout 0\n");

  /* acordar por outra thread (o interrupt_fd do Dart) */
  wake_fd = pfd[1];
  pthread_create(&th, NULL, waker, NULL);
  t0 = mono_ms();
  n = epoll_wait(ep, out, 8, 2000);
  dt = mono_ms() - t0;
  CHECK(n == 1 && out[0].data.u64 == 111 && (out[0].events & EPOLLIN),
        "pipe acorda epoll_wait (n=%d, %u ms)\n", n, (unsigned)dt);
  pthread_join(th, NULL);
  CHECK(epoll_wait(ep, out, 8, 0) == 1, "nivel: continua pronto sem ler\n");
  read(pfd[0], &c, 1);
  CHECK(epoll_wait(ep, out, 8, 0) == 0, "lido: nada pronto\n");

  /* borda */
  ev.events = EPOLLIN | EPOLLET;
  CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, pfd[0], &ev) == 0, "MOD para EPOLLET\n");
  write(pfd[1], "ab", 2);
  CHECK(epoll_wait(ep, out, 8, 0) == 1, "EPOLLET: primeira borda\n");
  CHECK(epoll_wait(ep, out, 8, 0) == 0, "EPOLLET: sem nova borda\n");
  read(pfd[0], &c, 1);
  read(pfd[0], &c, 1);
  CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, pfd[0], NULL) == 0, "DEL pipe\n");

  /* timerfd absoluto em CLOCK_MONOTONIC, como o Dart arma o proximo timer */
  tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
  CHECK(tfd >= 0, "timerfd_create = %d\n", tfd);
  ev.events = EPOLLIN;
  ev.data.u64 = 222;
  CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev) == 0, "ADD timerfd\n");
  {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    memset(&its, 0, sizeof(its));
    its.it_value.tv_sec = now.tv_sec;
    its.it_value.tv_nsec = now.tv_nsec + 100 * 1000000L;
    if (its.it_value.tv_nsec >= 1000000000L) {
      its.it_value.tv_sec++;
      its.it_value.tv_nsec -= 1000000000L;
    }
  }
  CHECK(timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL) == 0, "timerfd_settime +100 ms\n");
  t0 = mono_ms();
  n = epoll_wait(ep, out, 8, 2000);
  dt = mono_ms() - t0;
  CHECK(n == 1 && out[0].data.u64 == 222 && dt >= 80 && dt < 1000,
        "timerfd dispara (n=%d, %u ms)\n", n, (unsigned)dt);
  CHECK(read(tfd, &v, 8) == 8 && v == 1, "read timerfd = 1 expiracao\n");
  CHECK(epoll_wait(ep, out, 8, 0) == 0, "timerfd desarmado depois do read\n");
  memset(&its, 0, sizeof(its));
  CHECK(timerfd_settime(tfd, 0, &its, NULL) == 0, "timerfd desarmar\n");

  /* timeout sem nada */
  t0 = mono_ms();
  n = epoll_wait(ep, out, 8, 60);
  dt = mono_ms() - t0;
  CHECK(n == 0 && dt >= 50, "timeout de 60 ms (%u ms)\n", (unsigned)dt);

  /* eventfd */
  efd = eventfd(0, EFD_NONBLOCK);
  CHECK(efd >= 0, "eventfd = %d\n", efd);
  CHECK(read(efd, &v, 8) < 0 && errno == EAGAIN, "eventfd vazio = EAGAIN\n");
  v = 3;
  write(efd, &v, 8);
  v = 4;
  write(efd, &v, 8);
  v = 0;
  CHECK(read(efd, &v, 8) == 8 && v == 7, "eventfd soma = %llu\n", (unsigned long long)v);

  /* arquivo comum nao entra (EPERM, como no Linux) */
  {
    int f = open("D:\\EPOLLTST.TMP", O_CREAT | O_RDWR, 0644);
    if (f >= 0) {
      CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, f, &ev) < 0 && errno == EPERM, "arquivo = EPERM\n");
      close(f);
      unlink("D:\\EPOLLTST.TMP");
    }
  }

  CHECK(close(tfd) == 0 && close(efd) == 0 && close(ep) == 0, "close\n");
  CHECK(close(ep) < 0 && errno == EBADF, "close duplo = EBADF\n");
  printf("EPOLLTST: %d ok, %d falhas\n", ok, bad);
  return bad ? 1 : 0;
}
