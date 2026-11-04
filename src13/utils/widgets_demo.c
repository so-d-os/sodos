/* widgets_demo.c - demonstracao dos controles do SODOS (WIDGETS.EXE).
 *
 * Roda dentro do WIN:  Iniciar > Executar > D:\WIDGETS.EXE
 * Mouse: clica/arrasta.  Teclado: Tab/Shift+Tab alterna o foco, Enter/Espaco
 * aciona, setas/PgUp/PgDn/Home/End na lista, Esc fecha.
 */
#include <stdio.h>
#include <string.h>
#include <gui.h>
#include <widgets.h>

/* ids de foco */
static const char id_radio0 = 0, id_radio1 = 0, id_check0 = 0, id_check1 = 0;
static const char id_edit = 0, id_apply = 0, id_undo = 0, id_list = 0;
static const char id_exit = 0, id_anim = 0;

static const char* items[] = {
  "Alpha   - primeiro item",   "Bravo   - segundo item",
  "Charlie - terceiro item",   "Delta   - quarto item",
  "Echo    - quinto item",     "Foxtrot - sexto item",
  "Golf    - setimo item",     "Hotel   - oitavo item",
  "India   - nono item",       "Juliet  - decimo item",
  "Kilo    - decimo primeiro", "Lima    - decimo segundo",
  "Mike    - decimo terceiro", "November- decimo quarto",
  "Oscar   - decimo quinto",   "Papa    - decimo sexto"
};
#define NITEMS ((int)(sizeof(items) / sizeof(items[0])))

static gui_edit_t g_edit;
static wg_list_t g_list;
static int g_theme_sel;
static int g_anim, g_sound;
static int g_progress;
static int g_animating = 1;
static char g_status[160] = "pronto";

/* ordem do Tab */
static const void* const g_focus_order[] = {
  &id_radio0, &id_radio1, &id_check0, &id_check1,
  &id_edit, &id_anim, &id_apply, &id_undo, &id_list, &id_exit
};
#define NFOCUS ((int)(sizeof(g_focus_order) / sizeof(g_focus_order[0])))

static void set_status(const char* s) {
  int n = 0;
  while (s[n] && n < (int)sizeof(g_status) - 1) {
    g_status[n] = s[n];
    n++;
  }
  g_status[n] = 0;
}

static void focus_next(int dir) {
  const void* cur = wg_focus_get();
  int i, at = -1;
  for (i = 0; i < NFOCUS; i++)
    if (g_focus_order[i] == cur) at = i;
  if (at < 0) at = 0;
  else at = (at + dir + NFOCUS) % NFOCUS;
  wg_focus_set(g_focus_order[at]);
}

static int in_rect(int x, int y, int w, int h, int px, int py) {
  return px >= x && py >= y && px < x + w && py < y + h;
}

int main(void) {
  gui_event_t ev;
  int sw, sh, ch, run = 1;
  int mx = 0, my = 0, down = 0;
  int lx, ly, lw, lh;
  int rows;
  int last_click = 0;

  if (gui_open("Painel de Controle") != 0) {
    printf("WIDGETS: nao abriu a janela\n");
    return 1;
  }
  gui_edit_set(&g_edit, "texto de exemplo");
  memset(&g_list, 0, sizeof(g_list));
  g_list.items = items;
  g_list.n = NITEMS;
  g_list.sel = 0;
  g_list.top = 0;
  wg_theme_use(0);
  wg_focus_set(&id_radio0);

  while (run) {
    int x, y, w;
    int pad = wg_theme()->pad;
    int gap = wg_theme()->gap;
    int bh = wg_theme()->btn_h;
    char buf[160];

    sw = gui_width();
    sh = gui_height();
    ch = gui_char_h();

    wg_input(mx, my, down);
    gui_fill(0, 0, sw, sh, wg_theme()->bg);

    /* ---------------------------------------------------------- cabecalho */
    w = gui_text(16, 14, "Painel de Controle", -1, wg_theme()->text,
                 wg_theme()->bg, GUI_TEXT_BOLD);
    gui_text(16 + w + 12, 14 + 2, "- widgets do SODOS (GUI.DLL)", -1,
             wg_theme()->text_dim, wg_theme()->bg, GUI_TEXT_NOBG);
    wg_separator(16, 14 + ch + 10, sw - 32);

    /* ----------------------------------------------------------- coluna 1 */
    x = 16;
    y = 14 + ch + 24;
    w = 380;

    wg_group(x, y, w, 96, "Aparencia");
    wg_radio(&id_radio0, x + 14, y + 24, "Claro (padrao)", &g_theme_sel, 0);
    wg_radio(&id_radio1, x + 14, y + 24 + 24, "Escuro", &g_theme_sel, 1);
    wg_theme_use(g_theme_sel);

    y += 96 + gap;
    wg_group(x, y, w, 118, "Opcoes");
    wg_check(&id_check0, x + 14, y + 24, "Barra animada", &g_animating);
    wg_check(&id_check1, x + 14, y + 24 + 24, "Som ao clicar", &g_sound);
    if (g_list.sel >= 0) {
      snprintf(buf, sizeof(buf), "Item: %d/%d", g_list.sel + 1, g_list.n);
      gui_text(x + 14, y + 24 + 48, buf, -1, wg_theme()->text_dim,
               wg_theme()->panel, GUI_TEXT_NOBG);
    }

    y += 118 + gap;
    wg_group(x, y, w, 88, "Progresso");
    if (g_animating) {
      g_progress += 2;
      if (g_progress > 100) g_progress = 0;
    }
    snprintf(buf, sizeof(buf), "%d%%", g_progress);
    wg_progress(x + 14, y + 30, w - 28, 22, g_progress, buf);

    y += 88 + gap;
    wg_group(x, y, w, 96, "Texto");
    wg_edit_box(&id_edit, &g_edit, x + 14, y + 28, w - 28, 26);
    gui_text(x + 14, y + 62, "O campo aceita edicao com o teclado.", -1,
             wg_theme()->text_dim, wg_theme()->panel, GUI_TEXT_NOBG);

    /* ----------------------------------------------------------- coluna 2 */
    x = 16 + 380 + gap;
    y = 14 + ch + 24;
    w = sw - x - 16;
    if (w < 260) w = 260;
    lh = sh - y - (ch + 26) - 16;
    if (lh < 160) lh = 160;
    wg_group(x, y, w, lh, "Lista de itens");
    lx = x + 14;
    ly = y + 26;
    lw = w - 28;
    lh = lh - 26 - 14;
    rows = (lh - 4) / wg_theme()->row_h;
    if (wg_listbox(&id_list, &g_list, lx, ly, lw, lh)) {
      snprintf(buf, sizeof(buf), "selecionou: %s", items[g_list.sel]);
      set_status(buf);
    }

    /* -------------------------------------------------- botoes do rodape */
    y = sh - (ch + 26) - bh - 16;
    x = 16;
    if (wg_button(&id_apply, x, y, 120, bh, "Aplicar", 1)) {
      set_status("configuracoes aplicadas");
    }
    if (wg_button(&id_undo, x + 130, y, 120, bh, "Desfazer", 1)) {
      g_animating = 1;
      g_sound = 0;
      g_theme_sel = 0;
      g_progress = 0;
      gui_edit_set(&g_edit, "texto de exemplo");
      set_status("alteracoes desfeitas");
    }
    if (wg_button(&id_anim, x + 260, y, 170, bh, "Zerar progresso", 1)) {
      g_progress = 0;
      set_status("progresso zerado");
    }
    if (wg_button(&id_exit, sw - 16 - 110, y, 110, bh, "Sair", 1)) run = 0;

    snprintf(buf, sizeof(buf), "tema: %s", g_theme_sel ? "escuro" : "claro");
    wg_status(0, sh - (ch + 26), sw, g_status, buf);

    gui_present();

    /* ------------------------------------------------------------ eventos */
    if (gui_wait(&ev, 50) != GUI_EV_NONE) {
      if (ev.type == GUI_EV_CLOSE) run = 0;
      else if (ev.type == GUI_EV_DOWN) {
        mx = ev.x;
        my = ev.y;
        down = 1;
        last_click = ev.key;
        if (ev.key == 2) { /* botao direito: limpa a selecao */
          g_list.sel = -1;
          set_status("selecao limpa");
        }
      } else if (ev.type == GUI_EV_UP) {
        mx = ev.x;
        my = ev.y;
        down = 0;
      } else if (ev.type == GUI_EV_DRAG) {
        mx = ev.x;
        my = ev.y;
        down = 1;
      } else if (ev.type == GUI_EV_WHEEL) {
        if (in_rect(lx, ly, lw, lh, mx, my)) {
          wg_list_wheel(&g_list, ev.key > 0 ? 1 : -1, rows);
        }
      } else if (ev.type == GUI_EV_KEY) {
        int k = ev.key;
        if (k == 27) {
          run = 0;
        } else if (k == '\t') {
          focus_next(1);
        } else if (k == 9) {
          focus_next(1);
        } else if (wg_focus_get() == &id_list) {
          if (wg_list_key(&g_list, k, rows)) {
            if (g_list.sel >= 0) {
              snprintf(buf, sizeof(buf), "selecionou: %s", items[g_list.sel]);
              set_status(buf);
            }
          }
        } else if (wg_focus_get() == &id_edit) {
          gui_edit_key(&g_edit, k);
        } else if (wg_key_is_activate(k)) {
          const void* f = wg_focus_get();
          if (f == &id_exit) {
            run = 0;
          } else if (f == &id_apply) {
            set_status("configuracoes aplicadas");
          } else if (f == &id_undo) {
            set_status("alteracoes desfeitas");
          } else if (f == &id_anim) {
            g_progress = 0;
            set_status("progresso zerado");
          } else if (f == &id_check0) {
            g_animating = !g_animating;
          } else if (f == &id_check1) {
            g_sound = !g_sound;
          } else if (f == &id_radio0) {
            g_theme_sel = 0;
          } else if (f == &id_radio1) {
            g_theme_sel = 1;
          }
        }
      }
    } else if (g_animating && (g_progress % 20) == 0) {
      /* reaproveita o timeout para nao girar a toa */
    }
  }
  (void)last_click;
  gui_close();
  return 0;
}
