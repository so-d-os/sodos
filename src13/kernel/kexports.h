/* kexports.h - Tabela de funcoes do kernel para programas (import "KERNEL"). */
#pragma once

void* kexport_find(const char* name);

/* Enquanto um .EXE roda dentro de uma janela do WIN, a saida de texto dele
 * vai para 'hook' em vez da tela de texto. 0 volta ao console. */
void kexp_set_console_hook(void (*hook)(char c));
