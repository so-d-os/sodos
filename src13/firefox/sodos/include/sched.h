/* sched.h - escalonamento: o SODOS nao tem prioridades reais. */
#ifndef _SCHED_H
#define _SCHED_H

#ifdef __cplusplus
extern "C" {
#endif

#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2

int sched_yield(void);
int sched_get_priority_min(int policy);
int sched_get_priority_max(int policy);

/* lote2 */
#include <stddef.h>
#include <sys/types.h>
struct sched_param {
  int sched_priority;
};
#define CPU_SETSIZE 1024
typedef struct {
  unsigned long bits[CPU_SETSIZE / (8 * sizeof(unsigned long))];
} cpu_set_t;
#define CPU_ZERO(s) __builtin_memset((s), 0, sizeof(cpu_set_t))
#define CPU_SET(c, s) ((s)->bits[(c) / (8 * sizeof(unsigned long))] |= 1UL << ((c) % (8 * sizeof(unsigned long))))
#define CPU_ISSET(c, s) (((s)->bits[(c) / (8 * sizeof(unsigned long))] >> ((c) % (8 * sizeof(unsigned long)))) & 1)
#define CPU_COUNT(s) 1
int sched_setaffinity(pid_t pid, size_t n, const cpu_set_t* set);
int sched_getaffinity(pid_t pid, size_t n, cpu_set_t* set);
int sched_getcpu(void);

#ifdef __cplusplus
}
#endif

#endif
