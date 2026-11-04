/* t_nspr.c - teste do NSPR 4.35 rodando sobre a libsodos (PRTEST.EXE).
 *
 * Cobre o que o Gecko usa primeiro: init, tempo, locks, condicao, threads,
 * TLS, arquivos e formatacao. Soquetes ficam para o M4 (os stubs devolvem
 * ENOSYS de proposito).
 */
#include <stdio.h>
#include <string.h>

#include "prinit.h"
#include "prthread.h"
#include "prlock.h"
#include "prcvar.h"
#include "prtime.h"
#include "prinrval.h"
#include "prio.h"
#include "prerror.h"
#include "prmem.h"
#include "prprf.h"

static int passed;
static int failed;

static void ok(const char* what) {
  printf("PASS %s\n", what);
  fflush(stdout);
  passed++;
}

static void fail(const char* what, int line) {
  printf("FAIL %s (linha %d, PR erro %d)\n", what, line, PR_GetError());
  fflush(stdout);
  failed++;
}

#define CHECK(cond, what)      \
  do {                         \
    if (cond) ok(what);        \
    else fail(what, __LINE__); \
  } while (0)

static PRLock* lock;
static PRLock* counter_lock;
static int counter;
static PRUintn tls_index;

static void PR_CALLBACK worker(void* arg) {
  int id = (int)(PRWord)arg;
  int i;
  PR_SetThreadPrivate(tls_index, (void*)(PRWord)(id + 1));
  for (i = 0; i < 200; i++) {
    PR_Lock(counter_lock);
    counter++;
    PR_Unlock(counter_lock);
  }
  if ((int)(PRWord)PR_GetThreadPrivate(tls_index) != id + 1) counter = -1000000;
}

static PRLock* cv_lock;
static PRCondVar* cv;
static PRInt32 produced;
static PRInt32 consumed;

static void PR_CALLBACK producer(void* arg) {
  int i;
  (void)arg;
  for (i = 0; i < 10; i++) {
    PR_Lock(cv_lock);
    produced++;
    PR_NotifyCondVar(cv);
    PR_Unlock(cv_lock);
    PR_Sleep(PR_MillisecondsToInterval(1));
  }
}

static void PR_CALLBACK consumer(void* arg) {
  (void)arg;
  PR_Lock(cv_lock);
  while (consumed < 10) {
    while (produced <= consumed) PR_WaitCondVar(cv, PR_INTERVAL_NO_TIMEOUT);
    consumed++;
  }
  PR_Unlock(cv_lock);
}

static void test_time(void) {
  PRTime a = PR_Now();
  PRIntervalTime t0 = PR_IntervalNow();
  PR_Sleep(PR_MillisecondsToInterval(25));
  CHECK(PR_Now() - a >= 20000, "NSPR: PR_Now e PR_Sleep");
  CHECK(PR_IntervalToMilliseconds(PR_IntervalNow() - t0) >= 20,
        "NSPR: intervalo monotonico");
}

static void test_locks(void) {
  PRLock* l = PR_NewLock();
  CHECK(l != NULL, "NSPR: PR_NewLock");
  PR_Lock(l);
  PR_Unlock(l);
  PR_DestroyLock(l);
  ok("NSPR: lock/unlock");
}

static void test_threads(void) {
  PRThread* t[4];
  int i;
  PRStatus st;

  counter = 0;
  counter_lock = PR_NewLock();
  PR_NewThreadPrivateIndex(&tls_index, NULL);
  for (i = 0; i < 4; i++) {
    t[i] = PR_CreateThread(PR_USER_THREAD, worker, (void*)(PRWord)i,
                           PR_PRIORITY_NORMAL, PR_GLOBAL_THREAD,
                           PR_JOINABLE_THREAD, 0);
    if (!t[i]) break;
  }
  CHECK(i == 4, "NSPR: cria 4 threads");
  for (i = 0; i < 4 && t[i]; i++) {
    st = PR_JoinThread(t[i]);
    if (st != PR_SUCCESS) break;
  }
  CHECK(i == 4 && counter == 800, "NSPR: join + mutex + TLS");
  PR_DestroyLock(counter_lock);
}

static void test_cond(void) {
  PRThread* p;
  PRThread* c;

  cv_lock = PR_NewLock();
  cv = PR_NewCondVar(cv_lock);
  produced = consumed = 0;
  c = PR_CreateThread(PR_USER_THREAD, consumer, NULL, PR_PRIORITY_NORMAL,
                      PR_GLOBAL_THREAD, PR_JOINABLE_THREAD, 0);
  p = PR_CreateThread(PR_USER_THREAD, producer, NULL, PR_PRIORITY_NORMAL,
                      PR_GLOBAL_THREAD, PR_JOINABLE_THREAD, 0);
  PR_JoinThread(p);
  PR_JoinThread(c);
  CHECK(produced == 10 && consumed == 10, "NSPR: condvar produtor/consumidor");

  {
    PRIntervalTime start = PR_IntervalNow();
    PRStatus r;
    PR_Lock(cv_lock);
    r = PR_WaitCondVar(cv, PR_MillisecondsToInterval(30));
    PR_Unlock(cv_lock);
    /* O NSPR nao reporta timeout como falha: PR_SUCCESS e o tempo decorrido
     * e o que prova que o timedwait acordou sozinho. */
    CHECK(r == PR_SUCCESS &&
              PR_IntervalToMilliseconds(PR_IntervalNow() - start) >= 20,
          "NSPR: condvar com timeout");
  }
  PR_DestroyCondVar(cv);
  PR_DestroyLock(cv_lock);
}

static void test_file(void) {
  const char* path = "PRTEST.TXT";
  const char* msg = "NSPR no SODOS\nsegunda linha\n";
  char buf[64];
  PRFileDesc* fd;
  PRInt32 n;
  PRFileInfo info;

  fd = PR_Open(path, PR_WRONLY | PR_CREATE_FILE | PR_TRUNCATE, 0644);
  CHECK(fd != NULL, "NSPR: PR_Open para escrita");
  if (!fd) return;
  n = PR_Write(fd, msg, (PRInt32)strlen(msg));
  CHECK(n == (PRInt32)strlen(msg), "NSPR: PR_Write");
  PR_Close(fd);

  CHECK(PR_GetFileInfo(path, &info) == PR_SUCCESS && info.size == strlen(msg),
        "NSPR: PR_GetFileInfo");

  fd = PR_Open(path, PR_RDONLY, 0);
  CHECK(fd != NULL, "NSPR: PR_Open para leitura");
  if (!fd) return;
  memset(buf, 0, sizeof(buf));
  n = PR_Read(fd, buf, (PRInt32)sizeof(buf) - 1);
  PR_Close(fd);
  CHECK(n == (PRInt32)strlen(msg) && strcmp(buf, msg) == 0,
        "NSPR: PR_Read devolve o conteudo");
  PR_Delete(path);
}

static void test_misc(void) {
  char buf[64];
  void* p = PR_Malloc(128);
  CHECK(p != NULL, "NSPR: PR_Malloc");
  PR_Free(p);
  PR_snprintf(buf, sizeof(buf), "%d %s %llx", 42, "ok", (PRUint64)0xbeef);
  CHECK(strcmp(buf, "42 ok beef") == 0, "NSPR: PR_snprintf");
  CHECK(PR_GetCurrentThread() != NULL, "NSPR: PR_GetCurrentThread");
}

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  printf("PRTEST - NSPR %s sobre libsodos\n", PR_GetVersion());
  fflush(stdout);
  printf("chamando PR_Init...\n");
  fflush(stdout);
  PR_Init(PR_USER_THREAD, PR_PRIORITY_NORMAL, 0);
  printf("PR_Init ok\n");
  fflush(stdout);
  lock = PR_NewLock();
  (void)lock;

  test_time();
  test_locks();
  test_threads();
  test_cond();
  test_file();
  test_misc();

  PR_DestroyLock(lock);
  printf("\nPRTEST: %d ok, %d falhas\n", passed, failed);
  PR_Cleanup();
  return failed ? 1 : 0;
}
