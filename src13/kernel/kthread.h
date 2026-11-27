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
#define KT_MAX     1024  /* o Firefox cria ~60-100 threads; o Claude (Electron), 6-8 processos com 30-40 cada */

/* Uma vez no kmain, antes do sti: CPU0, o mundo, GS base, thunks. */
void kt_early_init(void);
/* Chamado pelo smp.c: a CPU 'idx' (AP) entra no escalonador. Nao volta. */
void kt_ap_enter(int idx);
/* A CPU0 ja tem LAPIC/timer: libera o balanceamento entre CPUs. */
void kt_set_online(int idx);
int kt_online_count(void);
/* Limite termico: so as CPUs 0..n-1 rodam threads; as outras ficam em hlt.
 * kt_cpu_cap = CPUs liberadas agora. */
void kt_set_cpu_cap(int n);
int kt_cpu_cap(void);

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
void kt_world_idle(uint32_t ms); /* laco do WIN ocioso: empresta a CPU0 por ate ms */
void kt_world_kick(void);        /* RIT: entrada chegou, o mundo para de emprestar */
/* Tempo real (RIT): thread do kernel presa a cpu, na frente da fila; com 3+
 * CPUs a cpu fica reservada (as outras threads saem dela). */
int kt_thread_create_rt(int (*fn)(void*), void* arg, uint32_t stack_size, int cpu, int reserve);
uint32_t kt_rt_mask(void);
/* Boost do primeiro plano: threads do pid passam na frente por ms. */
void kt_boost_pid(uint32_t pid, uint32_t ms);
void* kt_cur_task(void);         /* dono de travas recursivas (xhci.c) */
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
/* kt_bkl_enter para codigo do kernel numa kthread (nao le o quadro do thunk). */
void kt_bkl_enter_kernel(void);
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
/* 1 se a thread existe e nao terminou. */
int kt_thread_alive(int tid);
/* tid de uma thread viva com a pilha em [lo,hi) (a atual pelo rsp de agora,
 * as paradas pelo rsp salvo); -1 = a atual sem tid; 0 = nenhuma. */
int kt_rsp_in(uint64_t lo, uint64_t hi, uint32_t* pid);
int kt_thread_affinity(int tid, uint32_t mask);
void kt_thread_yield(void);
int kt_thread_tls(uint64_t fs);
/* TEB do Windows da tarefa corrente = base do GS dela; 0 = nenhum. */
void kt_win_teb_set(uint64_t teb);
uint64_t kt_win_teb_get(void);
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

/* TROCA (kernel/troca.c): o isr_dispatch marca o RIP interrompido em volta
 * dos tiques e IPIs que podem trocar de tarefa; uma thread tirada da CPU ali
 * fica com ele ate voltar. kt_irq_rips lista os RIPs das que estao fora da
 * CPU agora (chamado com as outras CPUs paradas). */
void kt_irq_enter(uint64_t rip);
void kt_irq_leave(void);
int kt_irq_rips(uint64_t* out, int max);
