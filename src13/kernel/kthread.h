/* kthread.h - escalonador SMP: filas por CPU, threads de programas, futex e
 * o lock grande do kernel (BKL).
 *
 * Modelo (parecido com o Linux 2.4/2.6 do tempo do BKL):
 * - cada CPU tem a sua fila de prontas (FIFO, round-robin) com lock proprio;
 * - o "mundo" (shell, WIN, Firefox, drivers: tudo que existia antes) e uma
 *   tarefa so, presa a CPU0, com prioridade sobre as threads. Ele nunca e
 *   preemptado pelo escalonador; so cede a CPU quando espera (join/futex) ou
 *   quando muda de CPU (smp run);
 * - threads criadas por programas (k_thread_create) rodam em qualquer CPU da
 *   afinidade, preemptadas pelo timer local (LAPIC nos APs, PIT na CPU0);
 * - o kernel nao e reentrante: toda chamada k_* passa por um thunk que pega o
 *   BKL. O mundo segura o BKL enquanto roda codigo do kernel e o solta quando
 *   executa codigo de programa; uma thread o solta ao bloquear.
 */
#pragma once
#include <stdint.h>

#define KT_CPU_MAX 8
#define KT_MAX     64

/* Uma vez no kmain, antes do sti: CPU0, o mundo, GS base, thunks. */
void kt_early_init(void);
/* Chamado pelo smp.c: a CPU 'idx' (AP) entra no escalonador. Nao volta. */
void kt_ap_enter(int idx);
/* A CPU0 ja tem LAPIC/timer: libera o balanceamento entre CPUs. */
void kt_set_online(int idx);
int kt_online_count(void);

/* Ticks: LAPIC (APs) e PIT (CPU0, a cada 1 ms). IRQs desligadas, EOI feito. */
void kt_tick_lapic(int ms);
void kt_tick_pit(void);
/* IPI de reschedule (vetor 0xF3) e de TLB (0xF4). */
void kt_ipi_resched(void);
void kt_ipi_tlb(void);

/* 1 se a CPU corrente roda uma thread de programa (nao o mundo). */
int kt_in_thread(void);
/* 1 se o mundo esta rodando na CPU corrente. */
int kt_world_here(void);
/* Indice da CPU corrente (0 antes do SMP). */
int kt_cpu_index(void);
/* pid do programa dono da thread corrente. */
uint32_t kt_cur_pid(void);
/* CPU0 na tarefa ociosa (para a estatistica do TOP). */
int kt_cpu_idle(int idx);
void kt_cpu_times(int idx, uint32_t* busy_ms, uint32_t* idle_ms);
int kt_cpu_threads(int idx);

/* BKL. Os thunks dos k_* chamam enter/leave. */
void kt_bkl_enter(void);
uint64_t kt_bkl_leave(void);
/* Mundo: solta o BKL inteiro (antes de entrar num programa) e devolve a
 * profundidade; resume o pega de novo com essa profundidade. */
int kt_bkl_suspend(void);
void kt_bkl_resume(int depth);
/* Mundo: profundidade atual (struct mod_cpu guarda por tarefa do WIN). */
int kt_bkl_depth(void);
/* Ponto ocioso do mundo: deixa entrar as threads que esperam o BKL. */
void kt_bkl_breathe(void);
void kt_bkl_set_depth(int depth);
/* IRQ0 no meio do codigo de um programa: tenta pegar o BKL para o mundo. */
int kt_bkl_try_world(void);
void kt_bkl_drop_world(void);

/* Threads de programas. */
int kt_thread_create(int (*fn)(void*), void* arg, uint32_t stack_size, uint32_t mask);
int kt_thread_join(int tid, int* code);
int kt_thread_detach(int tid);
void kt_thread_exit(int code) __attribute__((noreturn));
int kt_thread_self(void);
int kt_thread_affinity(int tid, uint32_t mask);
void kt_thread_yield(void);
int kt_thread_tls(uint64_t fs);
/* Fim do programa pid: mata e libera as threads dele (mundo, com BKL). */
void kt_release_pid(uint32_t pid);
/* sched_yield de dentro de uma thread: solta o BKL e cede a CPU. */
void kt_yield_in_thread(void);

/* Futex: espera enquanto *addr == val. 0 = acordou, -11 = valor ja mudou,
 * -110 = estourou timeout_ms (< 0 = sem limite). */
int kt_futex_wait(volatile int* addr, int val, int timeout_ms);
int kt_futex_wake(volatile int* addr, int n);

/* smp run: o mundo passa para outra CPU (e volta com cpu 0). */
void kt_world_migrate(int cpu);

/* Falta de CPU numa thread (codigo do programa): encerra so a thread. */
void kt_thread_fault_exit(void) __attribute__((noreturn));
/* Excecao fatal: as outras CPUs param (NMI). */
void kt_stop_others(void);
int kt_stopping(void);
/* Contadores e estado das filas na serial (comando smp). */
void kt_dump(void);
/* Tabela de paginas mudou: as outras CPUs recarregam o CR3. */
void kt_tlb_shootdown(void);
