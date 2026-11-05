/* visual.h - Modulo grafico VGA */
#pragma once
#include <stdint.h>

void visual_run(void);
/* Desktop tipo Windows 3.1/95. ESC restaura modo texto 80x25. */
void win_run(void);
/* Carrega a fonte 8x16 CP850 (chamar no boot, ainda em modo texto). */
void vga_capture_bios_font(void);
/* Desenho em uso do caractere ch: 16 linhas, um byte por linha, bit 7 a esquerda. */
const uint8_t* vga_font_glyph(int ch);
int vga_font_get(int ch, uint8_t* glyph);
/* -1 se ch for invalido ou o modo grafico estiver na tela. */
int vga_font_set(int ch, const uint8_t* glyph);
/* Volta todos os caracteres ao desenho padrao. */
void vga_font_reset(void);
/* 0: bit 7 do atributo vira fundo claro em vez de piscar. 1: volta a piscar. */
void vga_text_blink(int on);
/* Forca modo texto 80x25 para que panic/exception seja visivel (ex.: em isr_dispatch). */
void vga_force_text_mode(void);

/* Janela "canvas" para programas .EXE rodando no WIN (exportada como k_gui_*).
 * Cores 0xRRGGBB; coordenadas relativas a area do cliente. */
#define GUI_EV_NONE  0
#define GUI_EV_DOWN  1  /* ev[1]=x ev[2]=y ev[3]=botao (1 esq, 2 dir) */
#define GUI_EV_KEY   2  /* ev[3]=tecla: ASCII ou GUI_KEY_* */
#define GUI_EV_CLOSE 3
#define GUI_EV_UP    4
#define GUI_EV_DRAG  5  /* movimento com o botao esquerdo apertado */
#define GUI_EV_WHEEL 6  /* roda do rato: ev[3] > 0 desce, < 0 sobe (passos) */
#define GUI_KEY_UP    0x101
#define GUI_KEY_DOWN  0x102
#define GUI_KEY_LEFT  0x103
#define GUI_KEY_RIGHT 0x104
#define GUI_KEY_PGUP  0x105
#define GUI_KEY_PGDN  0x106
#define GUI_KEY_HOME  0x107
#define GUI_KEY_END   0x108
#define GUI_KEY_DEL   0x109
#define GUI_TEXT_NOBG     1
#define GUI_TEXT_BOLD     2
#define GUI_TEXT_UNDERLINE 4

int gui_open(const char* title);
void gui_close(void);
void gui_title(const char* title);
void gui_size(int* w, int* h);
void gui_font(int* cw, int* ch);
void gui_fill(int x, int y, int w, int h, uint32_t rgb);
void gui_text(int x, int y, const char* s, int n, uint32_t fg, uint32_t bg, int flags);
void gui_present(void);
int gui_event(int* ev, int timeout_ms);
/* 1 se a janela esta aberta e ha tecla, clique ou fechar na fila. */
int gui_input_pending(void);
void gui_pixels(int x, int y, int w, int h, const void* rgba, int stride);
/* chamado pelo WIN em volta de mod_exec: so ai a janela pode abrir */
void gui_session(int allowed);
/* API HTTP: move o cursor. click 1 gera o aperto e o soltar do botao esquerdo. */
void mouse_api(int x, int y, int click);
void mouse_api_pos(int* x, int* y);
/* 1 se o WIN (ou o modo grafico) esta na tela. */
int win_screen_size(int* w, int* h);
/* Injeta uma tecla ASCII na janela GUI com foco (automacao/testes). */
int win_api_key(int ch);
/* Linha y em BGR. Devolve a largura, ou -1. */
int win_screen_bgr(int y, uint8_t* dst, int max_px);
/* Configuracao de video (VIDEOCFG.EXE): 0 = retro, 1 = avancado. */
int gui_quality_get(void);
int gui_quality_set(int mode);
/* Cor de fundo do desktop (0xRRGGBB; 0 volta ao teal padrao). */
int win_bg_set(uint32_t rgb);
/* Papel de parede: rgba w*h*4; NULL/0 limpa. */
int win_wall_set(const uint8_t* rgba, int w, int h);
/* interface: posicao do menu (0 esq, 1 centro, 2 dir), menu translucido,
 * painel de IA ligado. Os _get devolvem o valor atual. */
int ui_menu_pos_set(int pos);
int ui_glass_set(int on);
int ui_ia_set(int on);
int ui_menu_pos_get(void);
int ui_glass_get(void);
int ui_ia_get(void);
/* abre a janela canvas docada na lateral direita (painel de IA). */
int gui_dock_open(const char* title, int w);

/* Lista "titulo x y w h". -1 se o desktop nao esta aberto. */
int win_windows(char* out, int max);
/* Fecha a janela com esse titulo. 0 = fechou. */
int win_close(const char* name);
