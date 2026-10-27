/* Tarefas cooperativas. Nao ha preempcao: quem espera a rede ou o disco
 * chama sched_yield(). No desktop, isso devolve o turno ao WIN, que pode
 * atender outra janela. */
#pragma once

void sched_set_idle(void (*fn)(void));
void sched_yield(void);
/* 1 se um programa grafico cedeu o turno ao desktop. Definido em visual.c. */
int gui_coop_pause(void);
/* fn devolve 1 quando a tarefa acabou. No maximo uma por yield. */
int sched_post(int (*fn)(void*), void* arg);
