/* reboot.h - Reinicio da maquina e Ctrl+Alt+Del */
#pragma once
#include <stdint.h>

/* Reinicia o PC. Nao retorna. */
void sys_reboot(void) __attribute__((noreturn));

/* Recebe cada scancode (set 1) lido do teclado em 0x60, inclusive os
 * prefixos 0xE0 e as solturas. Ctrl+Alt+Del chama sys_reboot(). */
void kbd_watch(uint8_t sc);
