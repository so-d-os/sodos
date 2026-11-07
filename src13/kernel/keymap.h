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
/* Igual, para as teclas com prefixo E0 que sao caractere: o Enter e a / do
 * teclado numerico. As demais (setas, Home...) devolvem 0. */
int keymap_ext_char(uint8_t sc);
/* 1 se sc (sem E0) e tecla do teclado numerico com o NumLock desligado: vale
 * como a tecla E0 de mesmo codigo (seta, Home...). Ligado, keymap_char da o
 * numero. */
int keymap_kp_nav(uint8_t sc);
/* 1 enquanto um dos Shift esta apertado (Shift+setas marcam texto). */
int keymap_shift(void);
/* Tecla que sobrou de uma combinacao que nao existe (0 = nenhuma). */
int keymap_pending(void);
