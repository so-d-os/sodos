/* keymap.h - Layout do teclado: scancode (set 1) -> caractere CP850. */
#pragma once
#include <stdint.h>

#define KEYB_US 0
#define KEYB_BR 1   /* ABNT2, com teclas mortas ´ ` ~ ^ ¨ e AltGr */

void keymap_set(int layout);
int keymap_get(void);
const char* keymap_name(int layout);

/* Chamado por kbd_watch com cada tecla (ja sem o prefixo E0, em ext):
 * acompanha Shift, AltGr e Caps Lock para todos os leitores do teclado. */
void keymap_watch(int ext, uint8_t key, int up);

/* Caractere CP850 da tecla sc (make code sem E0), ou 0. Uma tecla morta
 * devolve 0 e combina com a proxima: ~ e a viram ã. Se nao combina, sai o
 * acento e a tecla fica em keymap_pending(). */
int keymap_char(uint8_t sc);
/* Tecla que sobrou de uma combinacao que nao existe (0 = nenhuma). */
int keymap_pending(void);
