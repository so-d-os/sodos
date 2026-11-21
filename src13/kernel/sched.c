#include "sched.h"
#include "kthread.h"
int fs_held_by_me(void); /* kexports.c */
int shell_key_polling(void); /* kernel.c */

#define SCHED_JOBS 8

static void (*idle_fn)(void);
static int (*jobs[SCHED_JOBS])(void*);
static void* job_arg[SCHED_JOBS];
static int in_yield;
static int frozen;

/* Excecao: o CRASH.LOG grava com o FAT, que cede a vez a cada poucos setores.
 * Depois disto ninguem mais troca de tarefa. */
void sched_freeze(void) { frozen = 1; }
int sched_frozen(void) { return frozen; }

void sched_set_idle(void (*fn)(void)) {
  idle_fn = fn;
}

void (*sched_get_idle(void))(void) { return idle_fn; }

int sched_in_yield(void) { return in_yield; }

int sched_post(int (*fn)(void*), void* arg) {
  int i;
  if (!fn) return -1;
  for (i = 0; i < SCHED_JOBS; i++) {
    if (jobs[i]) continue;
    jobs[i] = fn;
    job_arg[i] = arg;
    return i;
  }
  return -1;
}

void sched_yield(void) {
  int i;
  if (frozen) return;
  /* Thread de programa (kthread.c): solta o BKL e cede a CPU dela. O WIN e
   * as tarefas do sched_post sao do mundo. */
  if (kt_in_thread()) {
    kt_yield_in_thread();
    return;
  }
  if (in_yield) return;
  kt_bkl_breathe();
  /* Tarefa do WIN no meio de uma operacao de disco (dona da trava do FS):
   * nao cede a vez. O laco do WIN, retomado aqui, podia pedir um arquivo e
   * esperar a trava de uma tarefa que so ele faria andar (travava o mouse e
   * a tela por 20 s). As threads esperam no futex. */
  if (fs_held_by_me()) return;
  /* Antes de in_yield: outro programa, retomado daqui, tambem precisa ceder. */
  if (gui_coop_pause()) return;
  in_yield = 1;
  if (idle_fn) idle_fn();
  for (i = 0; i < SCHED_JOBS; i++) {
    if (!jobs[i]) continue;
    if (jobs[i](job_arg[i])) jobs[i] = 0;
    break;
  }
  in_yield = 0;
  /* Programa do console esperando tecla em laco (k_trygetc vazio + k_yield):
   * empresta a CPU0 ate o proximo tick em vez de girar. */
  if (shell_key_polling()) kt_world_idle(1);
}
