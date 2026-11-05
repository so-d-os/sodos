/* fbcon.h - modo texto 80x25 desenhado no framebuffer linear (UEFI/GOP).
 *
 * No UEFI nao existe o modo texto VGA em 0xB8000.  O shell continua
 * escrevendo celulas (caractere | atributo << 8) em vga_text; com o fbcon
 * ativo esse ponteiro aponta para uma copia em RAM, e fbcon_sync() desenha
 * no framebuffer as celulas que mudaram.  No BIOS vga_text e 0xB8000 e tudo
 * aqui vira no-op. */
#pragma once
#include <stdint.h>

struct BootInfo;

/* 0xB8000 no BIOS; copia em RAM com o fbcon ativo. */
extern volatile uint16_t* vga_text;

/* 1 se ha VGA de verdade (portas 0x3C0.., modo texto, fonte na plane 2).
 * No UEFI nao ha: nem tente programar registros VGA. */
int vga_legacy(void);

/* Liga o fbcon se o firmware for UEFI e houver framebuffer de 32 bpp. */
int fbcon_init(const struct BootInfo* bi);
int fbcon_active(void);
/* Desenha as celulas alteradas e o cursor (pos = linha * 80 + coluna). */
void fbcon_sync(void);
void fbcon_cursor(int pos);
/* Redesenha as celulas com o caractere ch (ch < 0: todas). */
void fbcon_font_changed(int ch);
/* O desktop grafico usa a tela: 1 para de desenhar; 0 redesenha tudo. */
void fbcon_suspend(int on);
