/* smp.h - descoberta de CPUs e bring-up dos APs. */
#ifndef SMP_H
#define SMP_H

#include <stdint.h>

/* Le o ACPI (MADT), liga o LAPIC do BSP e acorda os APs.
 * Sem ACPI/LAPIC ou com qualquer falha, fica em 1 CPU (a BSP) e so registra. */
void smp_init(void);

/* Quantas CPUs foram encontradas no MADT (inclui a BSP). */
int smp_cpu_count(void);

/* Estado de uma CPU (0 = BSP) para o TOP. Mesmo layout de struct k_cpu_stat
 * em c_compiler/include/so.h. busy_ms/idle_ms: desde o boot. */
struct k_cpu_stat {
  unsigned id;       /* APIC id */
  unsigned index;    /* 0 = BSP */
  unsigned online;   /* 1 = a CPU subiu */
  unsigned state;    /* 0 = hlt/parada, 1 = ativa */
  unsigned busy_ms;  /* tempo trabalhando */
  unsigned idle_ms;  /* tempo parada (hlt) */
  unsigned pad[2];
};
int smp_cpu_stat(int i, struct k_cpu_stat* out);

/* Executa fn(arg) numa CPU (1..N-1) e espera terminar. 0 = ok.
 * A BSP fica em hlt enquanto a tarefa roda: quem chama tem a maquina so
 * para si (teclado, tela, rede), como se fosse a BSP. */
int smp_run(int cpu, void (*fn)(void*), void* arg);

/* Confirma o IPI no LAPIC (chamado pelo isr_dispatch). */
void smp_ipi_ack(void);

#endif
