/* vt.h - Telas virtuais: Ctrl+F1..F5 sao cinco prompts do DOS em modo
 * texto, cada um com a sua tela, cursor, unidade e programa. Ctrl+F7 e o WIN.
 *
 * Cada tela e uma tarefa cooperativa com pilha propria (ctx_switch). So a
 * tela visivel le o teclado. As outras andam quando a visivel espera tecla:
 * um programa ao fundo escreve na copia da tela dele e para de novo quando
 * pede tecla, ou a cada 256 caracteres escritos. O WIN fica parado enquanto
 * nao esta na tela. */
#pragma once
#include <stdint.h>

#define VT_TEXT 5            /* Ctrl+F1..F5 */
#define VT_WIN  VT_TEXT      /* indice da tarefa do WIN (Ctrl+F7) */
#define VT_TASKS (VT_TEXT + 1)

/* Estado do console do kernel.c que cada tela guarda. */
struct con_state {
  int row, col;
  void (*out)(char c);
  int (*exec)(const char* path, int argc, char** argv);
  void (*flush)(void);  /* shell_set_flush */
  int pipe_on;
};
void con_state_save(struct con_state* s);
void con_state_load(const struct con_state* s);

/* Liga as telas; a tarefa do boot vira a tela 1. */
void vt_init(void);
/* 1 se quem esta rodando nao e a tela visivel (nao le teclado nem mexe no
 * hardware da tela). */
int vt_bg(void);
/* Scancode (make, sem E0) com Ctrl apertado: indice da tela, ou -1. */
int vt_hotkey(uint8_t sc);
/* Mostra a tela i e passa a ela o teclado. Volta quando esta tela for
 * mostrada de novo (ou, numa tela de texto, quando ela ganhar a vez ao fundo). */
void vt_select(int i);
/* A tela ao fundo cede a vez. wait=1: espera tecla e so volta na tela. */
void vt_yield(int wait);
/* A tela visivel, sem nada a fazer, da uma vez a uma tela ao fundo. */
void vt_idle(void);
/* Saida de caractere: ao fundo, cede a vez de tempos em tempos. */
void vt_out_tick(void);
/* Comando WIN: abre o WIN na tarefa propria ou volta a ele.
 * Devolve 1 se o WIN continua aberto quando esta tela volta. */
int vt_win(void);
