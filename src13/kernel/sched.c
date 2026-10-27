#include "sched.h"

#define SCHED_JOBS 8

static void (*idle_fn)(void);
static int (*jobs[SCHED_JOBS])(void*);
static void* job_arg[SCHED_JOBS];
static int in_yield;

void sched_set_idle(void (*fn)(void)) {
  idle_fn = fn;
}

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
  if (in_yield) return;
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
}
