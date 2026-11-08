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
