/* widgets.h - biblioteca de controles do SODOS (sobre GUI.DLL).
 *
 * Estilo IMGUI: o app desenha o controle a cada quadro e passa a posicao do
 * rato/estado do botao por wg_input(). O controle devolve 1 no quadro em que
 * foi acionado (clique solto dentro, ou tecla quando tem foco).
 *
 * Cores 0xRRGGBB. Todos os textos usam a fonte 8x16 do console.
 */
#ifndef _WIDGETS_H
#define _WIDGETS_H

#include <gui.h>

/* ---------------------------------------------------------------- tema --- */
typedef struct {
  unsigned face;      /* fundo dos controles */
  unsigned face_hi;   /* realce (topo/esquerda) */
  unsigned face_lo;   /* sombra (base/direita) */
  unsigned bg;        /* fundo da janela */
  unsigned panel;     /* fundo de painel/caixa */
  unsigned text;
  unsigned text_dim;
  unsigned text_dis;  /* desabilitado */
  unsigned sel_bg;    /* selecao */
  unsigned sel_text;
  unsigned field_bg;
  unsigned field_text;
  unsigned accent;    /* barra de progresso / destaque */
  int pad;            /* margem interna */
  int gap;            /* espaco entre controles */
  int btn_h;          /* altura padrao de botao */
  int row_h;          /* altura de linha de lista */
} wg_theme_t;

const wg_theme_t* wg_theme(void);
void wg_set_theme(const wg_theme_t* t);
/* 0 = claro (padrao), 1 = escuro */
int wg_theme_is_dark(void);
void wg_theme_use(int dark);

/* --------------------------------------------------------------- entrada --- */
/* Chame uma vez por quadro, antes de desenhar. down = botao esquerdo. */
void wg_input(int mx, int my, int down);
int wg_mouse_x(void);
int wg_mouse_y(void);
int wg_mouse_down(void);
/* id com foco de teclado (o app controla o Tab) */
void wg_focus_set(const void* id);
const void* wg_focus_get(void);

/* ------------------------------------------------------------ primitivas --- */
int wg_text_w(const char* s);
void wg_label(int x, int y, const char* s);
void wg_label_dim(int x, int y, const char* s);
void wg_label_c(int x, int y, int w, const char* s);
void wg_bevel(int x, int y, int w, int h, int raised);
void wg_panel(int x, int y, int w, int h);
void wg_group(int x, int y, int w, int h, const char* title);
void wg_separator(int x, int y, int w);
void wg_focus_rect(int x, int y, int w, int h);
/* circulo cheio (usado por radio e por LEDs) */
void wg_dot(int cx, int cy, int r, unsigned rgb);
/* barra de status no rodape da janela; devolve a altura usada */
int wg_status(int x, int y, int w, const char* left, const char* right);

/* -------------------------------------------------------------- botoes --- */
/* Devolve 1 no clique (ou se 'activate' e o foco e veio Enter/Espaco). */
int wg_button(const void* id, int x, int y, int w, int h, const char* label,
              int enabled);
/* Botao com "toggle": desenhado afundado quando *on. */
int wg_toggle(const void* id, int x, int y, int w, int h, const char* label,
              int* on);

/* ----------------------------------------------------------- check/radio --- */
int wg_check(const void* id, int x, int y, const char* label, int* value);
/* mine = indice deste radio; *value guarda o indice selecionado */
int wg_radio(const void* id, int x, int y, const char* label, int* value,
             int mine);

/* ------------------------------------------------------------- progresso --- */
void wg_progress(int x, int y, int w, int h, int pct, const char* caption);

/* ------------------------------------------------------------- scrollbar --- */
typedef struct {
  int pos;   /* primeira linha/coluna visivel */
  int vis;   /* quantas cabem */
  int total; /* total de itens */
} wg_scroll_t;

/* Desenha a barra (vertical se h > w) e trata o arrasto do thumb.
 * Devolve 1 se pos mudou. */
int wg_scrollbar(const void* id, wg_scroll_t* s, int x, int y, int w, int h);

/* --------------------------------------------------------------- listbox --- */
typedef struct {
  const char* const* items;
  int n;
  int sel;   /* -1 = nenhum */
  int top;   /* primeira linha visivel */
  int wheel; /* acumulado da roda do rato (o app soma em wg_list_wheel) */
} wg_list_t;

/* Desenha a lista (com scrollbar automatica). Devolve 1 se sel mudou. */
int wg_listbox(const void* id, wg_list_t* l, int x, int y, int w, int h);
/* Teclado: setas, Home/End, PageUp/PageDown. Devolve 1 se mudou. */
int wg_list_key(wg_list_t* l, int key, int rows_visible);
/* Roda do rato: delta > 0 desce. Devolve 1 se mudou. */
int wg_list_wheel(wg_list_t* l, int delta, int rows_visible);

/* ------------------------------------------------------------------ edit --- */
/* Desenha o campo (usa gui_edit_t) e devolve 1 se ele tem foco e mudou. */
int wg_edit_box(const void* id, gui_edit_t* e, int x, int y, int w, int h);
/* Enter/Espaco quando o botao/check tem foco: o app chama com a tecla. */
int wg_key_is_activate(int key);

/* Area clicavel generica com a mesma logica dos controles (aperta e
 * solta dentro): util para linhas de lista, celulas, icones. */
int wg_click_area(const void* id, int x, int y, int w, int h, int enabled);

#endif /* _WIDGETS_H */
