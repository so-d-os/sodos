/* linkext_cfg.c - LINKEXT.EXE: "Abrir com" (Iniciar > Configuracoes).
 *
 * Administra qual programa abre cada extensao (LINKEXT.CFG, lido pelo
 * LINKEXT.DLL a cada arquivo aberto) e liga/desliga o recurso:
 * o check grava SET LINKEXT=1 ou SET LINKEXT=0 no AUTOEXEC.BAT (D:, ou U:
 * sem D:) e faz o mesmo SET agora (k_setenv), o que carrega ou descarrega
 * o DLL na hora. Desligado, o SO nao carrega nada e o launcher nao consulta.
 *
 * LINKEXT.CFG: PATHSODOS\LINKEXT.CFG (U:\SODOS no pendrive); sem a pasta do
 * PATHSODOS, D:\SODOS\LINKEXT.CFG. Linhas "EXT[,EXT...]=programa".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui.h>
#include <widgets.h>
#include <so.h>

#define MAXA 64
#define EXT_MAX 40
#define PROG_MAX 128

struct assoc {
  char ext[EXT_MAX];
  char prog[PROG_MAX];
};

static struct assoc g_a[MAXA];
static int g_n;
static char g_rows[MAXA][EXT_MAX + PROG_MAX + 8];
static const char* g_items[MAXA];
static wg_list_t g_list;
static gui_edit_t g_ext, g_prog;
static int g_on;
static char g_cfg[96];
static char g_bat[24];
static char g_status[160];
static int g_mx, g_my, g_down;
static const char id_on, id_list, id_ext, id_prog, id_save, id_del, id_new, id_exit;

static void set_status(const char* s) { snprintf(g_status, sizeof(g_status), "%s", s); }

static int up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static int ieq(const char* a, const char* b) {
  while (*a && up(*a) == up(*b)) {
    a++;
    b++;
  }
  return up(*a) == up(*b);
}

static void trim(char* s) {
  int n = (int)strlen(s), i = 0;
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
  while (s[i] == ' ' || s[i] == '\t') i++;
  if (i) memmove(s, s + i, (size_t)(n - i + 1));
}

/* Onde fica o LINKEXT.CFG: PATHSODOS se a pasta existe, senao D:\SODOS. */
static void pick_paths(void) {
  char sodos[96];
  struct k_dirent tmp[1];
  unsigned mask = k_drive_mask();
  if (k_getenv("PATHSODOS", sodos, (int)sizeof(sodos)) <= 0) strcpy(sodos, "U:\\SODOS");
  {
    int n = (int)strlen(sodos);
    if (n > 0 && sodos[n - 1] == '\\') sodos[n - 1] = 0;
  }
  snprintf(g_cfg, sizeof(g_cfg), "%s\\LINKEXT.CFG", sodos);
  if (k_fsize(g_cfg) < 0 && k_dir(sodos, tmp, 1) < 0) strcpy(g_cfg, "D:\\SODOS\\LINKEXT.CFG");
  strcpy(g_bat, (mask & (1u << ('D' - 'A'))) ? "D:\\AUTOEXEC.BAT" : "U:\\AUTOEXEC.BAT");
}

static void rebuild_rows(void) {
  int i;
  for (i = 0; i < g_n; i++) {
    snprintf(g_rows[i], sizeof(g_rows[i]), "%-10s  %s", g_a[i].ext, g_a[i].prog);
    g_items[i] = g_rows[i];
  }
  g_list.items = g_items;
  g_list.n = g_n;
  if (g_list.sel >= g_n) g_list.sel = g_n - 1;
}

static void load_cfg(void) {
  static char buf[8192];
  int n = k_fload(g_cfg, buf, (int)sizeof(buf));
  char* p = buf;
  g_n = 0;
  if (n > 0) {
    buf[n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1] = 0;
    while (*p && g_n < MAXA) {
      char line[256];
      char* eq;
      int k = 0;
      while (*p && *p != '\n') {
        if (k < (int)sizeof(line) - 1) line[k++] = *p;
        p++;
      }
      if (*p) p++;
      line[k] = 0;
      trim(line);
      if (!line[0] || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
      eq = strchr(line, '=');
      if (!eq) continue;
      *eq = 0;
      trim(line);
      trim(eq + 1);
      if (!line[0] || !eq[1]) continue;
      snprintf(g_a[g_n].ext, EXT_MAX, "%s", line);
      snprintf(g_a[g_n].prog, PROG_MAX, "%s", eq + 1);
      g_n++;
    }
  }
  rebuild_rows();
}

static int save_cfg(void) {
  static char out[MAXA * (EXT_MAX + PROG_MAX + 4) + 512];
  int k = 0, i;
  k += snprintf(out + k, sizeof(out) - k,
                "; LINKEXT.CFG - qual programa abre cada extensao (LINKEXT.EXE)\r\n"
                "; EXT[,EXT...]=programa  (sem letra: relativo ao PATHSODOS)\r\n"
                "; Visores do WIN: @NOTEPAD (o NOTEPAD.EXE), @IMAGEM, @VIDEO\r\n");
  for (i = 0; i < g_n; i++)
    k += snprintf(out + k, sizeof(out) - k, "%s=%s\r\n", g_a[i].ext, g_a[i].prog);
  if (k_fsave(g_cfg, out, k) < 0) {
    char b[160];
    snprintf(b, sizeof(b), "nao gravou %s", g_cfg);
    set_status(b);
    return -1;
  }
  return 0;
}

/* SET LINKEXT=... no AUTOEXEC.BAT: troca a linha que existe (todas) ou poe
 * depois do @ECHO OFF do comeco (ou na primeira linha). */
static int is_linkext_line(const char* l) {
  while (*l == ' ' || *l == '\t' || *l == '@') l++;
  if (up(l[0]) != 'S' || up(l[1]) != 'E' || up(l[2]) != 'T' || (l[3] != ' ' && l[3] != '\t'))
    return 0;
  l += 3;
  while (*l == ' ' || *l == '\t') l++;
  {
    static const char nm[] = "LINKEXT";
    int i;
    for (i = 0; nm[i]; i++)
      if (up(l[i]) != nm[i]) return 0;
    return l[7] == '=' || l[7] == ' ' || l[7] == '\t';
  }
}

static int is_echo_off(const char* l) {
  char t[16];
  int i = 0;
  while (*l == ' ' || *l == '\t' || *l == '@') l++;
  while (*l && *l != '\r' && *l != '\n' && i < 15) t[i++] = (char)up(*l++);
  t[i] = 0;
  return strcmp(t, "ECHO OFF") == 0;
}

static int save_autoexec(int on) {
  static char in[16384], out[16384 + 64];
  int n = k_fload(g_bat, in, (int)sizeof(in));
  int k = 0, done = 0;
  const char* set = on ? "SET LINKEXT=1\r\n" : "SET LINKEXT=0\r\n";
  char* p = in;
  if (n < 0) n = 0;
  if (n >= (int)sizeof(in)) n = (int)sizeof(in) - 1;
  in[n] = 0;
  /* sem ECHO OFF no comeco: o SET vai na primeira linha */
  if (!is_echo_off(in)) {
    k += snprintf(out + k, sizeof(out) - k, "%s", set);
    done = 1;
  }
  while (*p) {
    char* l = p;
    int len;
    while (*p && *p != '\n') p++;
    if (*p) p++;
    len = (int)(p - l);
    {
      char tmp[300];
      int m = len < (int)sizeof(tmp) - 1 ? len : (int)sizeof(tmp) - 1;
      memcpy(tmp, l, (size_t)m);
      tmp[m] = 0;
      if (is_linkext_line(tmp)) continue;
      if (k + len + 2 >= (int)sizeof(out)) break;
      memcpy(out + k, l, (size_t)len);
      k += len;
      if (len > 0 && l[len - 1] != '\n') {
        out[k++] = '\r';
        out[k++] = '\n';
      }
      if (!done && is_echo_off(tmp)) {
        k += snprintf(out + k, sizeof(out) - k, "%s", set);
        done = 1;
      }
    }
  }
  if (!done) k += snprintf(out + k, sizeof(out) - k, "%s", set);
  return k_fsave(g_bat, out, k) < 0 ? -1 : 0;
}

static void toggle(void) {
  char b[160];
  int rc = save_autoexec(g_on);
  k_setenv("LINKEXT", g_on ? "1" : "0");
  if (rc < 0) snprintf(b, sizeof(b), "aplicado agora, mas nao gravou %s", g_bat);
  else snprintf(b, sizeof(b), "%s (SET LINKEXT=%d em %s)", g_on ? "ligado" : "desligado", g_on, g_bat);
  set_status(b);
}

static void fill_edits(void) {
  if (g_list.sel < 0 || g_list.sel >= g_n) return;
  gui_edit_set(&g_ext, g_a[g_list.sel].ext);
  gui_edit_set(&g_prog, g_a[g_list.sel].prog);
}

/* Normaliza "  .txt , png" -> "TXT,PNG". */
static void norm_ext(const char* in, char* out, int max) {
  int k = 0;
  int sep = 0;
  for (; *in; in++) {
    char c = *in;
    if (c == ' ' || c == '\t' || c == '.' || c == ',' || c == ';') {
      if (c == ',' || c == ';') sep = 1;
      continue;
    }
    if (c == '=') continue;
    if (sep && k > 0 && k < max - 1) out[k++] = ',';
    sep = 0;
    if (k < max - 1) out[k++] = (char)up(c);
  }
  out[k] = 0;
}

static void do_save(void) {
  char ext[EXT_MAX];
  char prog[PROG_MAX];
  int i;
  norm_ext(g_ext.text, ext, (int)sizeof(ext));
  snprintf(prog, sizeof(prog), "%s", g_prog.text);
  trim(prog);
  if (!ext[0] || !prog[0]) {
    set_status("preencha a extensao e o programa");
    return;
  }
  if (prog[0] != '@' && prog[1] == ':' && k_fsize(prog) < 0) {
    char b[200];
    snprintf(b, sizeof(b), "aviso: %s nao existe (gravado mesmo assim)", prog);
    set_status(b);
  } else {
    set_status("associacao gravada");
  }
  for (i = 0; i < g_n; i++)
    if (ieq(g_a[i].ext, ext)) break;
  if (i == g_n) {
    if (g_n >= MAXA) {
      set_status("lista cheia");
      return;
    }
    g_n++;
  }
  snprintf(g_a[i].ext, EXT_MAX, "%s", ext);
  snprintf(g_a[i].prog, PROG_MAX, "%s", prog);
  g_list.sel = i;
  rebuild_rows();
  save_cfg();
}

static void do_del(void) {
  int i = g_list.sel;
  if (i < 0 || i >= g_n) {
    set_status("escolha uma linha da lista");
    return;
  }
  memmove(&g_a[i], &g_a[i + 1], sizeof(g_a[0]) * (size_t)(g_n - i - 1));
  g_n--;
  if (g_list.sel >= g_n) g_list.sel = g_n - 1;
  rebuild_rows();
  if (save_cfg() == 0) set_status("associacao removida");
  fill_edits();
}

int main(void) {
  gui_event_t ev;
  int run = 1, pad = 12, rows = 1;
  int lx = 0, ly = 0, lw = 0, lh = 0;
  static const void* order[] = {&id_on, &id_list, &id_ext, &id_prog, &id_save, &id_del, &id_new, &id_exit};
  char v[8];

  if (gui_open("Abrir com") != 0) {
    printf("LINKEXT: abra pelo WIN (Iniciar > Configuracoes > Abrir com)\n");
    return 1;
  }
  pick_paths();
  g_on = k_getenv("LINKEXT", v, (int)sizeof(v)) > 0 && !(v[0] == '0' && v[1] == 0);
  g_list.sel = -1;
  load_cfg();
  {
    char b[160];
    snprintf(b, sizeof(b), "%d associacao(oes) em %s", g_n, g_cfg);
    set_status(b);
  }
  wg_focus_set(&id_list);

  while (run) {
    int sw = gui_width(), sh = gui_height();
    int bh = wg_theme()->btn_h;
    int y = pad;
    wg_input(g_mx, g_my, g_down);
    gui_fill(0, 0, sw, sh, wg_theme()->bg);

    wg_group(pad, y, sw - pad * 2, 64, "Ativacao");
    if (wg_check(&id_on, pad + 16, y + 26, "Abrir arquivos pelo programa associado (LINKEXT)", &g_on))
      toggle();
    y += 64 + 10;

    {
      int gh = sh - y - 24 - 10 - bh - 10 - 96;
      if (gh < 120) gh = 120;
      wg_group(pad, y, sw - pad * 2, gh, "Extensao -> programa");
      lx = pad + 12;
      ly = y + 24;
      lw = sw - pad * 2 - 24;
      lh = gh - 34;
      rows = (lh - 4) / wg_theme()->row_h;
      if (rows < 1) rows = 1;
      if (wg_listbox(&id_list, &g_list, lx, ly, lw, lh)) fill_edits();
      y += gh + 10;
    }

    wg_group(pad, y, sw - pad * 2, 86, "Editar");
    wg_label(pad + 16, y + 28, "Extensao");
    wg_edit_box(&id_ext, &g_ext, pad + 100, y + 22, 140, 26);
    wg_label(pad + 256, y + 28, "Programa");
    wg_edit_box(&id_prog, &g_prog, pad + 340, y + 22, sw - pad * 2 - 340 - 4, 26);
    wg_label_dim(pad + 16, y + 58, "Ex.: TXT=D:\\SODOS\\NOTEPAD.EXE  JPG,PNG=@IMAGEM  MP4=@VIDEO");
    y += 86 + 10;

    if (wg_button(&id_save, pad, y, 150, bh, "Salvar", 1)) do_save();
    if (wg_button(&id_del, pad + 160, y, 110, bh, "Remover", g_list.sel >= 0)) do_del();
    if (wg_button(&id_new, pad + 280, y, 110, bh, "Nova", 1)) {
      g_list.sel = -1;
      gui_edit_set(&g_ext, "");
      gui_edit_set(&g_prog, "");
      wg_focus_set(&id_ext);
      set_status("digite a extensao e o programa e clique Salvar");
    }
    if (wg_button(&id_exit, sw - pad - 100, y, 100, bh, "Sair", 1)) run = 0;

    wg_status(0, sh - 24, sw, g_status, g_on ? "LINKEXT ligado" : "LINKEXT desligado");
    gui_present();

    if (gui_wait(&ev, 150) != GUI_EV_NONE) {
      if (ev.type == GUI_EV_CLOSE) run = 0;
      else if (ev.type == GUI_EV_DOWN) { g_mx = ev.x; g_my = ev.y; g_down = 1; }
      else if (ev.type == GUI_EV_UP) { g_mx = ev.x; g_my = ev.y; g_down = 0; }
      else if (ev.type == GUI_EV_DRAG) { g_mx = ev.x; g_my = ev.y; g_down = 1; }
      else if (ev.type == GUI_EV_WHEEL) {
        if (g_mx >= lx && g_mx < lx + lw && g_my >= ly && g_my < ly + lh &&
            wg_list_wheel(&g_list, ev.key > 0 ? 1 : -1, rows))
          fill_edits();
      } else if (ev.type == GUI_EV_KEY) {
        int k = ev.key;
        const void* f = wg_focus_get();
        if (k == 27) run = 0;
        else if (k == '\t') {
          int i;
          for (i = 0; i < 8 && order[i] != f; i++) {
          }
          wg_focus_set(order[(i + 1) % 8]);
        } else if (f == &id_list) {
          if (wg_list_key(&g_list, k, rows)) fill_edits();
        } else if (f == &id_ext || f == &id_prog) {
          if (gui_edit_key(f == &id_ext ? &g_ext : &g_prog, k) == 2) do_save();
        } else if (wg_key_is_activate(k)) {
          if (f == &id_on) {
            g_on = !g_on;
            toggle();
          } else if (f == &id_save) do_save();
          else if (f == &id_del) do_del();
          else if (f == &id_exit) run = 0;
        }
      }
    }
  }
  gui_close();
  return 0;
}
