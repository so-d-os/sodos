/* t_fboot.c - teste de fumaca da camada de plataforma do Firefox (FBOOT.EXE).
 *
 * Cobre o que o NSPR usa primeiro: init/log, tempo monotonico, mmap anonimo,
 * threads com pilha propria, mutex/cond/rwlock, TLS, semaforos e poll.
 * "FBOOT.EXE net" inclui um GET HTTP pela unica conexao do kernel.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>

#include "sodos.h"

static int passed;
static int failed;

static void ok(const char* what) {
  printf("PASS %s\n", what);
  fflush(stdout);
  passed++;
}

static void fail(const char* what, int line) {
  printf("FAIL %s (linha %d)\n", what, line);
  fflush(stdout);
  failed++;
}

#define CHECK(cond, what)      \
  do {                         \
    if (cond) ok(what);        \
    else fail(what, __LINE__); \
  } while (0)

/* ---------- threads ---------- */

#define WORKERS 6
#define ITERS 300

static pthread_mutex_t counter_mu = PTHREAD_MUTEX_INITIALIZER;
static int counter;
static int tls_bad;
static pthread_key_t tls_key;

static void* worker(void* arg) {
  int id = (int)(intptr_t)arg;
  int i;
  pthread_setspecific(tls_key, (void*)(intptr_t)(id + 1));
  for (i = 0; i < ITERS; i++) {
    pthread_mutex_lock(&counter_mu);
    counter++;
    pthread_mutex_unlock(&counter_mu);
    if ((i % 64) == 0) pthread_yield();
  }
  if ((int)(intptr_t)pthread_getspecific(tls_key) != id + 1) tls_bad = 1;
  return (void*)(intptr_t)(100 + id);
}

static pthread_mutex_t cond_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond_cv = PTHREAD_COND_INITIALIZER;
static int produced;
static int consumed;

static void* producer(void* arg) {
  int i;
  (void)arg;
  for (i = 0; i < 5; i++) {
    pthread_mutex_lock(&cond_mu);
    produced++;
    pthread_cond_signal(&cond_cv);
    pthread_mutex_unlock(&cond_mu);
    sodos_sleep_ms(1);
  }
  return NULL;
}

static void* consumer(void* arg) {
  (void)arg;
  pthread_mutex_lock(&cond_mu);
  while (consumed < 5) {
    while (produced <= consumed) pthread_cond_wait(&cond_cv, &cond_mu);
    consumed++;
  }
  pthread_mutex_unlock(&cond_mu);
  return NULL;
}

static void* ret42(void* arg) {
  (void)arg;
  return (void*)(intptr_t)42;
}

static volatile int detached_done;

static void* detached_worker(void* arg) {
  int id = (int)(intptr_t)arg;
  int i;
  for (i = 0; i < 500; i++) {
    pthread_mutex_lock(&counter_mu);
    counter++;
    pthread_mutex_unlock(&counter_mu);
  }
  (void)id;
  detached_done++;
  return NULL;
}

static void test_detached(void) {
  pthread_attr_t at;
  pthread_t t[4];
  int i;

  detached_done = 0;
  counter = 0;
  pthread_attr_init(&at);
  pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
  for (i = 0; i < 4; i++)
    pthread_create(&t[i], &at, detached_worker, (void*)(intptr_t)i);
  pthread_attr_destroy(&at);
  while (detached_done < 4) sodos_sleep_ms(1);
  CHECK(counter == 4 * 500, "threads: detached + reaping de pilha");
}

static void test_threads(void) {
  pthread_t t[WORKERS];
  void* rv[WORKERS];
  int i, bad = 0;

  counter = 0;
  tls_bad = 0;
  pthread_key_create(&tls_key, NULL);
  for (i = 0; i < WORKERS; i++)
    if (pthread_create(&t[i], NULL, worker, (void*)(intptr_t)i) != 0) bad = 1;
  if (!bad) {
    for (i = 0; i < WORKERS; i++) {
      rv[i] = NULL;
      if (pthread_join(t[i], &rv[i]) != 0) bad = 1;
    }
  }
  CHECK(!bad, "threads: create/join");
  CHECK(counter == WORKERS * ITERS, "threads: mutex protege o contador");
  CHECK(!tls_bad, "threads: TLS por thread");
  bad = 0;
  for (i = 0; i < WORKERS; i++)
    if ((int)(intptr_t)rv[i] != 100 + i) bad = 1;
  CHECK(!bad, "threads: valor de retorno do join");

  {
    pthread_t t;
    void* rv42 = NULL;
    pthread_create(&t, NULL, ret42, NULL);
    pthread_join(t, &rv42);
    CHECK((int)(intptr_t)rv42 == 42, "threads: retorno simples");
  }
}

static void test_cond(void) {
  pthread_t p, c;
  produced = consumed = 0;
  pthread_create(&c, NULL, consumer, NULL);
  pthread_create(&p, NULL, producer, NULL);
  pthread_join(p, NULL);
  pthread_join(c, NULL);
  CHECK(produced == 5 && consumed == 5, "cond: produtor/consumidor");

  {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
    struct timespec ts;
    int rc;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;
    ts.tv_nsec += 30000000L;
    if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&m);
    rc = pthread_cond_timedwait(&cv, &m, &ts);
    pthread_mutex_unlock(&m);
    CHECK(rc != 0, "cond: timedwait expira");
  }
}

static int once_count;
static void once_body(void) { once_count++; }

static void test_once_rwlock(void) {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;

  once_count = 0;
  pthread_once(&once, once_body);
  pthread_once(&once, once_body);
  CHECK(once_count == 1, "once: roda uma vez");

  pthread_rwlock_rdlock(&rw);
  pthread_rwlock_rdlock(&rw);
  pthread_rwlock_unlock(&rw);
  pthread_rwlock_unlock(&rw);
  pthread_rwlock_wrlock(&rw);
  pthread_rwlock_unlock(&rw);
  ok("rwlock: leitura compartilhada e escrita");
}

static void test_sem(void) {
  sem_t s;
  int v = -1;
  sem_init(&s, 0, 0);
  sem_post(&s);
  sem_wait(&s);
  sem_getvalue(&s, &v);
  sem_destroy(&s);
  CHECK(v == 0, "sem: post/wait");
}

/* ---------- tempo, memoria, poll ---------- */

static void test_time(void) {
  uint64_t a = sodos_now_ms();
  uint64_t b;
  sodos_sleep_ms(25);
  b = sodos_now_ms();
  CHECK(b - a >= 20 && b - a < 5000, "tempo: sleep de 25 ms");
  CHECK(sodos_wall_ms() > 1000000000000ULL, "tempo: relogio de parede");
}

static void test_mmap(void) {
  size_t n = 256 * 1024;
  unsigned char* p = (unsigned char*)mmap(NULL, n, PROT_READ | PROT_WRITE,
                                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int i, bad = 0;
  CHECK(p != MAP_FAILED, "mmap: bloco anonimo");
  if (p == MAP_FAILED) return;
  for (i = 0; i < (int)n; i += 4096)
    if (p[i] != 0) bad = 1;
  memset(p, 0xA5, n);
  if (p[n - 1] != 0xA5) bad = 1;
  CHECK(!bad, "mmap: memoria zerada e gravavel");
  CHECK(munmap(p, n) == 0, "mmap: munmap");
}

static void test_poll(void) {
  uint64_t a = sodos_now_ms();
  int rc = poll(NULL, 0, 30);
  uint64_t b = sodos_now_ms();
  CHECK(rc == 0 && b - a >= 25, "poll: timeout sem fds");
}

static void test_net(void) {
  char req[128];
  char buf[8192];
  int n, got = 0;
  int fd;

  fd = sodos_net_connect("info.cern.ch", 80);
  if (fd < 0) {
    printf("SKIP net: sem conexao (%d)\n", fd);
    return;
  }
  strcpy(req, "GET / HTTP/1.0\r\nHost: info.cern.ch\r\nConnection: close\r\n\r\n");
  if (sodos_net_send(req, (int)strlen(req)) < 0) {
    fail("net: send", __LINE__);
    sodos_net_close();
    return;
  }
  while (got < (int)sizeof(buf) - 1) {
    n = sodos_net_recv(buf + got, (int)sizeof(buf) - 1 - got);
    if (n == -3) continue;
    if (n <= 0) break;
    got += n;
  }
  buf[got] = 0;
  sodos_net_close();
  CHECK(got > 0 && strncmp(buf, "HTTP/", 5) == 0, "net: GET HTTP");
  if (got > 0)
    printf("     recebidos %d bytes: %.40s\n", got, buf);
}

int main(int argc, char** argv) {
  int net = argc > 1 && strcmp(argv[1], "net") == 0;

  sodos_init();
  printf("FBOOT - camada de plataforma do Firefox para o SODOS\n");
  printf("versao %s, log em %s\n", SODOS_VERSION, sodos_log_path());

  test_time();
  test_mmap();
  test_threads();
  test_detached();
  test_cond();
  test_once_rwlock();
  test_sem();
  test_poll();
  if (net) test_net();

  {
    uint32_t total = 0, freeb = 0;
    sodos_mem_info(&total, &freeb);
    printf("heap: %u KB livres de %u KB\n", freeb / 1024, total / 1024);
  }

  printf("\nFBOOT: %d ok, %d falhas\n", passed, failed);
  sodos_log(SODOS_LOG_INFO, "FBOOT: %d ok, %d falhas", passed, failed);
  sodos_log_flush();
  return failed ? 1 : 0;
}
