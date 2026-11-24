/* reboot.h - Reinicio da maquina, Ctrl+Alt+Del e Ctrl+C */
#pragma once
#include <stdint.h>

/* Reinicia o PC. Nao retorna. */
void sys_reboot(void) __attribute__((noreturn));

/* Recebe cada scancode (set 1) lido do teclado em 0x60, inclusive os
 * prefixos 0xE0 e as solturas. Ctrl+Alt+Del chama hang_ctrl_alt_del().
 * Devolve o que fazer com o byte na fila do teclado (ps2in.c): */
#define KBW_PASS    0   /* vai para a fila */
#define KBW_SWALLOW 1   /* some (o Del do Ctrl+Alt+Del) */
#define KBW_E0      2   /* prefixo E0: segura ate ver o byte seguinte */
int kbd_watch(uint8_t sc);

/* Ctrl+F1 visto (ps2in.c): o guardiao age se o mundo estiver parado. */
void kbd_emergency_note(void);
/* Thread do guardiao (CPUs 1+): teclado USB, Ctrl+Alt+Del e Ctrl+F1 mesmo
 * com o mundo travado. Chamado uma vez depois do smp_init. */
void guard_start(void);

/* 1 se houve Ctrl+C ou Ctrl+Break desde a ultima chamada (e limpa o aviso).
 * No WIN o aviso nao e registrado: la Ctrl+C e tecla comum. */
int kbd_break_take(void);

/* on = 1 enquanto um comando do prompt roda: ai Ctrl+C vale tambem no WIN. */
void kbd_break_arm(int on);

/* 1 enquanto um dos Ctrl esta apertado (ja vale para a tecla que
 * get_char acabou de devolver). */
int kbd_ctrl_down(void);
/* 1 enquanto um dos Alt esta apertado. */
int kbd_alt_down(void);

/* Teclado cru (k_kbd_raw, terminal do WSL): Ctrl+C nao para o programa e
 * Ctrl+letra chega como 1..26. Volta a 0 quando o programa termina. */
void kbd_raw_set(int on);
int kbd_raw(void);
