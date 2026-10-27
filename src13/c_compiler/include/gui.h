/* gui.h - janela canvas do WIN (GUI.DLL sobre k_gui_*). Cores 0xRRGGBB.
 * So funciona num .EXE aberto pelo WIN (Explorer ou menu Iniciar). */
#ifndef _GUI_H
#define _GUI_H

#include <so.h>

#define GUI_EV_NONE  0
#define GUI_EV_DOWN  1  /* ev.x, ev.y, ev.key = botao (1 esq, 2 dir) */
#define GUI_EV_KEY   2  /* ev.key = ASCII (Enter = '\r') ou GUI_KEY_* */
#define GUI_EV_CLOSE 3  /* clicou no X */
#define GUI_EV_UP    4
#define GUI_EV_DRAG  5  /* moveu com o botao esquerdo apertado */
#define GUI_EV_WHEEL 6  /* roda do rato: ev.key > 0 desce, < 0 sobe (passos) */

#define GUI_KEY_UP    0x101
#define GUI_KEY_DOWN  0x102
#define GUI_KEY_LEFT  0x103
#define GUI_KEY_RIGHT 0x104
#define GUI_KEY_PGUP  0x105
#define GUI_KEY_PGDN  0x106
#define GUI_KEY_HOME  0x107
#define GUI_KEY_END   0x108
#define GUI_KEY_DEL   0x109

#define GUI_TEXT_NOBG      1
#define GUI_TEXT_BOLD      2
#define GUI_TEXT_UNDERLINE 4

typedef struct {
  int type, x, y, key;
} gui_event_t;

/* abre a janela, acima da barra Iniciar; 0 = ok */
int gui_open(const char* title);
void gui_close(void);
void gui_title(const char* title);
int gui_width(void);
int gui_height(void);
int gui_char_w(void);
int gui_char_h(void);
void gui_fill(int x, int y, int w, int h, unsigned rgb);
/* rgba e R,G,B,A; A < 128 fica transparente. stride = bytes por linha. */
void gui_pixels(int x, int y, int w, int h, const void* rgba, int stride);
/* escala iw x ih para o retangulo w x h, cortando fora de (cx,cy,cw,ch). */
void gui_image(int x, int y, int w, int h, const void* rgba, int iw, int ih,
               int cx, int cy, int cw, int ch);
void gui_frame(int x, int y, int w, int h, unsigned rgb);
/* n < 0: ate o NUL. Devolve a largura desenhada em pixels. */
int gui_text(int x, int y, const char* s, int n, unsigned fg, unsigned bg, int flags);
/* corta o texto para caber em max_w pixels */
int gui_text_fit(int x, int y, const char* s, int max_w, unsigned fg, unsigned bg, int flags);
void gui_present(void);
/* timeout_ms < 0 espera para sempre; devolve ev->type */
int gui_wait(gui_event_t* ev, int timeout_ms);
int gui_hit(int x, int y, int w, int h, int px, int py);
void gui_button(int x, int y, int w, int h, const char* label, int enabled);

/* caixa de texto de uma linha */
typedef struct {
  char text[512];
  int len, caret, scroll;
  int all; /* texto todo selecionado: a proxima tecla substitui */
} gui_edit_t;

void gui_edit_set(gui_edit_t* e, const char* s);
void gui_edit_select_all(gui_edit_t* e);
/* trata uma tecla; devolve 1 se mudou, 2 se foi Enter, 0 se ignorou */
int gui_edit_key(gui_edit_t* e, int key);
void gui_edit_click(gui_edit_t* e, int x, int px);
void gui_edit_draw(gui_edit_t* e, int x, int y, int w, int h, int focused);

#endif
