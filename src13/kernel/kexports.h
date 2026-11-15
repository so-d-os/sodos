/* kexports.h - Tabela de funcoes do kernel para programas (import "KERNEL"). */
#pragma once
#include <stdint.h>

void* kexport_find(const char* name);

/* Relogio de parede para o desktop: data AAAAMMDD e hora HHMMSS (UTC).
 * 0/-1 quando o RTC nao e confiavel. */
int rtc_date(void);
int rtc_time(void);

/* Enquanto um .EXE roda dentro de uma janela do WIN, a saida de texto dele
 * vai para 'hook' em vez da tela de texto. 0 volta ao console. */
void kexp_set_console_hook(void (*hook)(char c));
void (*kexp_get_console_hook(void))(char c);
/* Cursor escondido por k_cursor. As telas (vt.c) guardam e repoem. */
int kexp_cursor_hidden(void);
void kexp_cursor_apply(int hidden);

/* Chamado por mod_exec quando o programa pid termina (inclusive por Ctrl+C):
 * fecha a conexao TCP que ele deixou aberta e volta a fonte e o piscar que ele mudou. */
void kexp_release_pid(uint32_t pid);

/* Tela de texto de uma janela do WIN (Command): enquanto um programa roda la,
 * k_vput, k_cursor, k_clear, k_font_*, k_blink, k_getc, k_trygetc e
 * k_readline usam estas funcoes em vez da VGA e da porta do teclado.
 * key(wait): tecla (como k_getc), 0 se nao ha, -1 se a janela fechou. */
struct kexp_textscr {
  int (*vput)(int row, int col, const uint16_t* cells, int n);
  void (*cursor)(int row, int col);
  void (*clear)(void);
  int (*font_get)(int ch, uint8_t* glyph);
  int (*font_set)(int ch, const uint8_t* glyph);
  void (*font_reset)(void);
  int (*key)(int wait);
  void (*size)(int* cols, int* rows); /* k_textsize */
};
void kexp_set_textscr(const struct kexp_textscr* ops);

/* As mesmas funcoes de k_vput/k_cursor/k_getc/k_clear/k_puts para comandos
 * internos de tela cheia (EDIT): no DOS vao a VGA, no Command do WIN a janela. */
int kx_vput(int row, int col, const uint16_t* cells, int n);
void kx_cursor(int row, int col);
int kx_getc(void);
void kx_clear(void);
void kx_puts(const char* s);
int kx_textsize(int* cols, int* rows);
