/* hang.h - Ctrl+Alt+Del: tela 1, HANG.LOG e reinicio.
 *
 * Fora da tela 1 (Ctrl+F1) o Ctrl+Alt+Del leva a ela. Se o sistema estava
 * parado (o mundo, CPU0, sem ler o teclado ha 1,5 s), antes tira uma foto de
 * todas as CPUs por NMI e escreve D:\LOG\HANG.LOG; se a tela 1 nao vier (ou
 * vier e nao responder) em 5 s, congela as CPUs, grava e reinicia. Na tela 1
 * o Ctrl+Alt+Del reinicia direto (gravando o HANG.LOG antes se ela estiver
 * parada). Um segundo Ctrl+Alt+Del no meio de tudo isso reinicia na hora.
 * Sem a thread (uma CPU so) e como era: reinicia na hora. */
#pragma once
#include <stdint.h>

/* kbd_watch (reboot.c), em qualquer contexto (IRQ1, guardiao, TECLA). */
void hang_ctrl_alt_del(void);
/* Sobe a thread do Ctrl+Alt+Del (guard_start, depois do SMP). */
void hang_start(void);
/* IRQ0 da CPU0 (irq.c): com hang_force ligado e o mundo preso, leva a tela
 * 1 para a frente de onde o mundo estiver (vt_rescue). */
extern volatile int hang_force;
void hang_tick(uint64_t rip);
/* SMP TRAVAR laco|cli|bkl [s]: trava de proposito, para testar. */
int hang_test(const char* mode, int delay_s);
