/* mc.h - Midnight Commander do SODOS: dois paineis no modo texto 80x25.
 * Desenha celulas coloridas com k_vput (como o mc do Linux num console azul).
 * Compila com o CC do proprio sistema:  cc -o MC.EXE mc.c scr.c panel.c ops.c view.c */
#ifndef MC_H
#define MC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>

#define SCR_W 80
#define SCR_H 25

/* teclas de k_getc */
#define K_UP    0x100
#define K_DOWN  0x101
#define K_LEFT  0x102
#define K_RIGHT 0x103
#define K_PGUP  0x104
#define K_PGDN  0x105
#define K_HOME  0x106
#define K_END   0x107
#define K_DEL   0x108
#define K_INS   0x109
#define K_F(n)  (0x140 + (n))
#define K_ESC   27
#define K_ENTER '\n'
#define K_BS    8
#define K_TAB   '\t'
#define K_EOF   26     /* fim da entrada (pipe/soxrun): sai sem perguntar */

/* cores: fundo << 4 | frente (paleta da VGA) */
#define C_PANEL   0x17   /* cinza no azul */
#define C_DIR     0x1F   /* pasta: branco */
#define C_EXE     0x1A   /* .EXE .BAT .COM: verde claro */
#define C_LIB     0x12   /* .DLL .OBJ .IMP: verde */
#define C_ARCH    0x1D   /* .ZIP .GZ .TAR: magenta claro */
#define C_DOC     0x16   /* .TXT .MD .PDF: marrom */
#define C_SRC     0x13   /* .C .H .ASM: ciano */
#define C_IMG     0x1B   /* .PNG .JPG: ciano claro */
#define C_MEDIA   0x15   /* .AVI .MP4 .WAV: magenta */
#define C_TEMP    0x18   /* .TMP .BAK .LOG: cinza escuro */
#define C_SEL     0x30   /* barra do cursor: preto no ciano */
#define C_MARK    0x1E   /* marcado: amarelo */
#define C_MARKSEL 0x3E
#define C_HEAD    0x1E   /* cabecalho das colunas */
#define C_TITLE   0x30   /* caminho do painel ativo */
#define C_MENU    0x30   /* barra de menu */
#define C_MENUHOT 0x3E
#define C_MENUSEL 0x0F   /* item escolhido: branco no preto */
#define C_MENUSELHOT 0x0E
#define C_CMD     0x07   /* linha de comando */
#define C_KEYNUM  0x07
#define C_KEYLBL  0x30
#define C_DLG     0x70   /* dialogo: preto no cinza */
#define C_DLGTTL  0x71   /* titulo do dialogo: azul no cinza */
#define C_DLGBTN  0x70
#define C_DLGFOC  0x30   /* botao/campo em foco */
#define C_INPUT   0x30
#define C_ERR     0x4F   /* erro: branco no vermelho */
#define C_ERRFOC  0x70
#define C_VIEW    0x17
#define C_SHADOW  0x08

/* ---------- scr.c: tela, caixas e dialogos ---------- */
int scr_init(void);
void scr_done(void);
void scr_put(int y, int x, int ch, int color);
void scr_text(int y, int x, const char* s, int color);
/* s em w colunas: corta ou completa com espacos */
void scr_textw(int y, int x, const char* s, int w, int color);
void scr_fill(int y, int x, int h, int w, int ch, int color);
void scr_paint(int y, int x, int w, int color);   /* troca so a cor */
void scr_box(int y, int x, int h, int w, int color, int dbl);
void scr_shadow(int y, int x, int h, int w);
void scr_flush(void);
void scr_invalidate(void);
int scr_key(void);
int scr_cp850(int c);   /* letra minuscula/maiuscula para comparar */

/* dialogos; err = 1 usa as cores de erro */
void dlg_msg(const char* title, const char* msg, int err);
int dlg_yesno(const char* title, const char* msg, int err);
/* Sim / Nao / Todos / Cancelar -> 1 / 0 / 2 / -1 */
int dlg_ask4(const char* title, const char* msg);
int dlg_input(const char* title, const char* prompt, char* buf, int max);
/* lista de opcoes; devolve o indice ou -1 */
int dlg_list(const char* title, const char** items, int n, int cur);
/* caixa sem botoes (progresso); fica na tela ate o proximo desenho */
void dlg_note(const char* title, const char* msg);

/* ---------- panel.c ---------- */
#define PATH_MAX_MC 128
#define ENT_MAX 512

struct ent {
  char name[32];
  unsigned size;
  unsigned short date;
  unsigned short time;
  char dir;
  char mark;
  char up;       /* ".." */
  char pad;
};

#define SORT_NAME 0
#define SORT_EXT  1
#define SORT_SIZE 2
#define SORT_TIME 3

struct panel {
  char path[PATH_MAX_MC];   /* "U:\\" ou "U:\\SRC\\MC" */
  struct ent* e;
  int n;
  int cur;
  int top;
  int sort;
  int x;                    /* 0 ou 40 */
  int bad;                  /* a pasta nao abriu */
};

extern struct panel pan[2];
extern int act;             /* painel ativo */
extern int today;           /* AAAAMMDD ou 0 */

#define PANEL_ROWS 17       /* linhas de arquivos */

void panel_init(struct panel* p, int x, const char* path);
void panel_load(struct panel* p);
/* reler mantendo o cursor no mesmo nome */
void panel_reload(struct panel* p);
void panel_draw(struct panel* p, int active);
void panel_key_move(struct panel* p, int key);
struct ent* panel_cur(struct panel* p);
int panel_marked(struct panel* p, unsigned* bytes);
void panel_chdir(struct panel* p, const char* path);
void panel_up(struct panel* p);
void panel_enter_dir(struct panel* p, const char* name);
void panel_mark_pattern(struct panel* p, const char* pat, int on);
int path_join(char* out, int max, const char* dir, const char* name);
int is_root(const char* path);
int name_color(const struct ent* e);
int is_exec(const char* name);
int glob_match(const char* pat, const char* name);
int name_eq_path(const char* a, const char* b);   /* sem caixa */

/* ---------- ops.c ---------- */
void op_copy(int move);
void op_delete(void);
void op_mkdir(void);

/* ---------- view.c ---------- */
void view_file(const char* path);

/* ---------- mc.c ---------- */
void draw_all(void);
void fmt_size(char* out, unsigned v);   /* 1.234.567 */

#endif
