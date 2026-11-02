/* reboot.h - Reinicio da maquina, Ctrl+Alt+Del e Ctrl+C */
#pragma once
#include <stdint.h>

/* Reinicia o PC. Nao retorna. */
void sys_reboot(void) __attribute__((noreturn));

/* Recebe cada scancode (set 1) lido do teclado em 0x60, inclusive os
 * prefixos 0xE0 e as solturas. Ctrl+Alt+Del chama sys_reboot(). */
void kbd_watch(uint8_t sc);

/* 1 se houve Ctrl+C ou Ctrl+Break desde a ultima chamada (e limpa o aviso).
 * No WIN o aviso nao e registrado: la Ctrl+C e tecla comum. */
int kbd_break_take(void);
