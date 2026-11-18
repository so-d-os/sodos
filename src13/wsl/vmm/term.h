/* term.h - Terminal ANSI/VT do console do WSL (ver term.c). */
#ifndef WSL_TERM_H
#define WSL_TERM_H
#include <stdint.h>

/* Resposta do terminal para o Linux (posicao do cursor, identificacao). */
typedef void (*term_reply_fn)(void* arg, const char* s);

/* 1 = tela de celulas (cores); 0 = texto puro por k_putc. */
int term_init(term_reply_fn fn, void* arg);
void term_size(int* cols, int* rows);
void term_write(const uint8_t* s, int n);
void term_flush(void);
void term_end(void);

#endif
