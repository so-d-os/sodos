/* smpt.c - teste das threads SMP (k_thread_* e k_futex_*).
 *
 *   SMPT [threads] [limite]
 *
 * 1. conta os primos abaixo de 'limite' (padrao 10000000) com 1 thread;
 * 2. a mesma conta dividida em N threads (padrao: numero de CPUs);
 * 3. N threads incrementam um contador com um mutex de futex e chamam
 *    k_malloc/k_free (BKL) ao mesmo tempo.
 * Mostra em que CPUs cada thread rodou, o tempo de cada fase e o ganho.
 * O resultado tambem vai para a serial (k_dbg) com o prefixo [smpt].
 * A soma em double (0,5 * primo) confere que o estado SSE sobrevive a
 * preempcao: o valor e exato e tem que bater com o da fase 1. */
#include <so.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXT 16
#define HAMMER 200000L

struct job {
  int idx, step;
  unsigned limit;
  unsigned count;
  double sum;
  unsigned cpus;     /* bit por CPU em que a thread foi vista */
  unsigned moves;
};

static void dbg(const char* s) { k_dbg(s, (int)strlen(s)); }

static void say(const char* fmt, ...);
#include <stdarg.h>
static void say(const char* fmt, ...) {
  char b[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  fputs(b, stdout);
  dbg("[smpt] ");
  dbg(b);
}

static int is_prime(unsigned n) {
  unsigned d;
  if (n < 2) return 0;
  if (n % 2 == 0) return n == 2;
  for (d = 3; d * d <= n; d += 2)
    if (n % d == 0) return 0;
  return 1;
}

static int worker(void* a) {
  struct job* j = (struct job*)a;
  unsigned n, c = 0, k = 0;
  double s = 0.0;
  int last = -1;
  /* blocos de 4096 numeros intercalados entre as threads */
  for (n = 4096u * (unsigned)j->idx; n < j->limit;
       n = ((n & 4095u) == 4095u) ? n + 1u + 4096u * (unsigned)(j->step - 1) : n + 1u) {
    if (is_prime(n)) {
      c++;
      s += 0.5 * (double)n;
    }
    if ((++k & 1023u) == 0) {
      int cpu = k_thread_cpu();
      if (cpu >= 0 && cpu < 32) j->cpus |= 1u << cpu;
      if (cpu != last) {
        if (last >= 0) j->moves++;
        last = cpu;
      }
    }
  }
  j->count = c;
  j->sum = s;
  return (int)c;
}

/* ---- mutex de futex (Drepper: 0 livre, 1 travado, 2 travado com espera) ---- */
static volatile int mtx;
static volatile long shared;

static void lock(void) {
  int c = 0;
  if (__atomic_compare_exchange_n(&mtx, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
  if (c != 2) c = __atomic_exchange_n(&mtx, 2, __ATOMIC_ACQUIRE);
  while (c != 0) {
    k_futex_wait(&mtx, 2, -1);
    c = __atomic_exchange_n(&mtx, 2, __ATOMIC_ACQUIRE);
  }
}

static void unlock(void) {
  if (__atomic_fetch_sub(&mtx, 1, __ATOMIC_RELEASE) != 1) {
    mtx = 0;
    k_futex_wake(&mtx, 1);
  }
}

static int hammer(void* a) {
  struct job* j = (struct job*)a;
  int i;
  for (i = 0; i < HAMMER; i++) {
    lock();
    shared = shared + 1;     /* sem atomico: so o mutex protege */
    unlock();
    if ((i & 255) == 0) {
      void* p = k_malloc(64 + (unsigned)i % 512u);
      if (!p) return -1;
      memset(p, j->idx, 64);
      k_free(p);
      j->cpus |= 1u << (k_thread_cpu() & 31);
    }
  }
  return 0;
}

static unsigned run_phase(int n, unsigned limit, struct job* jobs, unsigned* count, double* sum) {
  int tid[MAXT];
  unsigned t0, dt;
  int i;
  t0 = k_ms();
  for (i = 0; i < n; i++) {
    memset(&jobs[i], 0, sizeof(jobs[i]));
    jobs[i].idx = i;
    jobs[i].step = n;
    jobs[i].limit = limit;
    tid[i] = k_thread_create(worker, &jobs[i], 0, 0);
    if (tid[i] <= 0) {
      say("k_thread_create falhou: %d\n", tid[i]);
      return 0;
    }
  }
  *count = 0;
  *sum = 0.0;
  for (i = 0; i < n; i++) {
    int code = 0;
    k_thread_join(tid[i], &code);
    *count += jobs[i].count;
    *sum += jobs[i].sum;
  }
  dt = k_ms() - t0;
  return dt ? dt : 1;
}

static void cpu_list(unsigned m, char* out) {
  int i, k = 0;
  out[0] = 0;
  for (i = 0; i < 32; i++)
    if (m & (1u << i)) k += sprintf(out + k, "%s%d", k ? "," : "", i);
}

/* SMPT kill: threads que nunca terminam; o programa sai sem join e o
 * kernel tem que mata-las antes de liberar o heap.
 * SMPT fault: uma thread executa ud2; so ela morre. */
static volatile int spin_go;
static int spinner(void* a) {
  volatile unsigned long x = 0;
  (void)a;
  for (;;) {
    x++;
    if ((x & 0xFFFFF) == 0) {
      void* p = k_malloc(128);
      k_free(p);
    }
    if ((x & 0x3FFFFF) == 0) k_futex_wait(&spin_go, 0, 1);
  }
  return 0;
}
static int faulty(void* a) {
  (void)a;
  __asm__ volatile ("ud2");
  return 7;
}

int main(int argc, char** argv) {
  static struct job jobs[MAXT];
  if (argc > 1 && strcmp(argv[1], "kill") == 0) {
    int i, n = k_cpu_count() * 2;
    for (i = 0; i < n && i < MAXT; i++) k_thread_create(spinner, 0, 0, 0);
    k_futex_wait(&spin_go, 0, 300);
    say("SMPT kill: %d threads rodando; saindo sem join\n", n);
    return 0;
  }
  if (argc > 1 && strcmp(argv[1], "fault") == 0) {
    int code = 0, t = k_thread_create(faulty, 0, 0, 0);
    int rc = k_thread_join(t, &code);
    say("SMPT fault: join=%d codigo=%d (esperado -1); main segue viva\n", rc, code);
    return code == -1 ? 0 : 1;
  }
  int ncpu = k_cpu_count();
  int n = argc > 1 ? atoi(argv[1]) : ncpu;
  unsigned limit = argc > 2 ? (unsigned)atoi(argv[2]) : 10000000u;
  unsigned c1, cn, t1, tn, all = 0;
  double s1, sn;
  int i, ok = 1, tid[MAXT];
  char cl[64];

  if (n < 1) n = 1;
  if (n > MAXT) n = MAXT;
  if (ncpu < 1) ncpu = 1;
  say("SMPT: %d CPUs, %d threads, primos abaixo de %u\n", ncpu, n, limit);

  t1 = run_phase(1, limit, jobs, &c1, &s1);
  cpu_list(jobs[0].cpus, cl);
  say("1 thread : %u primos em %u ms (CPUs %s)\n", c1, t1, cl);

  tn = run_phase(n, limit, jobs, &cn, &sn);
  for (i = 0; i < n; i++) {
    cpu_list(jobs[i].cpus, cl);
    say("  thread %d: %u primos, CPUs %s, trocas de CPU %u\n", i, jobs[i].count, cl, jobs[i].moves);
    all |= jobs[i].cpus;
  }
  cpu_list(all, cl);
  say("%d threads: %u primos em %u ms (CPUs usadas: %s)\n", n, cn, tn, cl);
  if (cn != c1 || sn != s1) {
    say("ERRO: resultado diferente (%u/%u, soma %s)\n", cn, c1, sn == s1 ? "igual" : "DIFERENTE");
    ok = 0;
  }
  say("ganho: %u.%02ux\n", t1 / tn, (t1 * 100u / tn) % 100u);

  /* fase 3: mutex de futex + BKL */
  shared = 0;
  mtx = 0;
  t1 = k_ms();
  for (i = 0; i < n; i++) {
    memset(&jobs[i], 0, sizeof(jobs[i]));
    jobs[i].idx = i;
    tid[i] = k_thread_create(hammer, &jobs[i], 64 * 1024, 0);
  }
  all = 0;
  for (i = 0; i < n; i++) {
    int code = 0;
    if (tid[i] > 0) k_thread_join(tid[i], &code);
    if (code != 0) ok = 0;
    all |= jobs[i].cpus;
  }
  cpu_list(all, cl);
  say("mutex: contador %ld de %ld em %u ms (CPUs %s)\n", (long)shared, (long)n * HAMMER,
      k_ms() - t1, cl);
  if (shared != (long)n * HAMMER) ok = 0;

  say(ok ? "SMPT OK\n" : "SMPT FALHOU\n");
  return ok ? 0 : 1;
}
