/* Tarefas cooperativas. Nao ha preempcao: quem espera a rede ou o disco
 * chama sched_yield(). No desktop, isso devolve o turno ao WIN, que pode
 * atender outra janela.
 * SSE/x87 nao sao salvos na troca: ela so ocorre dentro de uma chamada
 * (XMM e caller-saved na ABI SysV) e o kernel e compilado -mno-sse. */
#pragma once

void sched_set_idle(void (*fn)(void));
void (*sched_get_idle(void))(void);
/* 1 dentro de sched_yield (idle ou tarefa rodando). */
int sched_in_yield(void);
void sched_yield(void);
/* 1 se um programa grafico cedeu o turno ao desktop. Definido em visual.c. */
int gui_coop_pause(void);
/* fn devolve 1 quando a tarefa acabou. No maximo uma por yield. */
int sched_post(int (*fn)(void*), void* arg);
/* Excecao fatal: sched_yield nao faz mais nada (ver isr.c). */
void sched_freeze(void);
