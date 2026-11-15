/* temas.c - escolha do tema da interface (TEMAS.EXE).
 *
 * Iniciar > Configuracoes > Temas. Cada tema e uma pasta \SODOS\ICONS\<NOME>
 * com TEMA.CFG (cores e estilo) e os icones NOME.ICN (ou NOME.PNG: este
 * programa converte os que ainda nao tem .ICN ao aplicar). A escolha fica em
 * D:\SODOS\TEMA.CFG ("tema=NOME"); sem ela o WIN usa CLASSICO, o de sempre.
 * Um tema novo e so uma pasta nova: aparece aqui sozinho.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui.h>
#include <widgets.h>
#include <img.h>
#include <so.h>

#define MAXT 24
#define CFG_USER "D:\\SODOS\\TEMA.CFG"
#define CFG_USER_USB "U:\\SODOS\\TEMA.CFG"

typedef struct {
  char dir[16];        /* nome da pasta */
  char nome[32];       /* nome= do TEMA.CFG */
  char path[48];       /* X:\SODOS\ICONS\NOME\ */
  int modern, icons;
  unsigned desk, barra, painel, destaque, texto, titulo, borda, rotulo, prato_ok;
} tema_t;

static tema_t g_t[MAXT];
static const char* g_items[MAXT];
static int g_n;
static wg_list_t g_list;
static char g_status[96];
static char g_cur[16];
static int g_mx, g_my, g_down;
static int g_sw_demo = 1, g_sl_demo = 60, g_ck_demo = 1;
static const char id_list, id_apply, id_classic, id_exit, id_sw, id_sl, id_ck, id_btn;

static void set_status(const char* s) {
  strncpy(g_status, s, sizeof(g_status) - 1);
  g_status[sizeof(g_status) - 1] = 0;
}

static unsigned hex6(const char* s) {
  unsigned v = 0;
  int i;
  for (i = 0; i < 6; i++) {
    char c = s[i];
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return v;
    v = v * 16 + (unsigned)d;
  }
  return v;
}

/* Le o TEMA.CFG da pasta. As cores que faltam ficam com as do classico. */
static void tema_read(tema_t* t) {
  char path[64], buf[2048];
  FILE* f;
  size_t n;
  char* p;
  t->modern = 0;
  t->icons = 0;
  t->desk = 0x008080;
  t->barra = 0x0B1522;
  t->painel = 0x101C2B;
  t->destaque = 0x2F80D0;
  t->texto = 0xE8F0FA;
  t->titulo = 0x16283E;
  t->borda = 0x2E4A6C;
  t->rotulo = 0xFFFFFF;
  strncpy(t->nome, t->dir, sizeof(t->nome) - 1);
  snprintf(path, sizeof(path), "%sTEMA.CFG", t->path);
  f = fopen(path, "rb");
  if (!f) return;
  n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  for (p = buf; *p;) {
    char* e = p;
    char* v;
    while (*e && *e != '\n' && *e != '\r') e++;
    v = strchr(p, '=');
    if (*p != ';' && *p != '#' && v && v < e) {
      int kl = (int)(v - p);
      v++;
#define K(s) (kl == (int)strlen(s) && strncmp(p, s, (size_t)kl) == 0)
      if (K("nome")) {
        int l = (int)(e - v);
        if (l > (int)sizeof(t->nome) - 1) l = (int)sizeof(t->nome) - 1;
        memcpy(t->nome, v, (size_t)l);
        t->nome[l] = 0;
      } else if (K("estilo")) t->modern = (*v == 'm' || *v == 'M');
      else if (K("icones")) t->icons = (*v == 'a' || *v == 'A');
      else if (K("desk")) t->desk = hex6(v);
      else if (K("barra")) t->barra = hex6(v);
      else if (K("painel")) t->painel = hex6(v);
      else if (K("destaque")) t->destaque = hex6(v);
      else if (K("texto")) t->texto = hex6(v);
      else if (K("titulo")) t->titulo = hex6(v);
      else if (K("borda")) t->borda = hex6(v);
      else if (K("rotulo")) t->rotulo = hex6(v);
#undef K
    }
    p = e;
    while (*p == '\n' || *p == '\r') p++;
  }
}

/* Junta as pastas de \SODOS\ICONS de D:, U: e Z: (a primeira vence). */
static void scan(void) {
  static const char drives[] = "DUZC";
  static struct k_dirent ent[MAXT * 2];
  int d, i, j;
  g_n = 0;
  for (d = 0; drives[d]; d++) {
    char base[24];
    int n;
    snprintf(base, sizeof(base), "%c:\\SODOS\\ICONS", drives[d]);
    n = k_dir(base, ent, MAXT * 2);
    for (i = 0; i < n && g_n < MAXT; i++) {
      int dup = 0;
      if (!ent[i].dir || ent[i].name[0] == '.') continue;
      for (j = 0; j < g_n; j++)
        if (strcmp(g_t[j].dir, ent[i].name) == 0) dup = 1;
      if (dup || strlen(ent[i].name) > 15) continue;
      strcpy(g_t[g_n].dir, ent[i].name);
      snprintf(g_t[g_n].path, sizeof(g_t[g_n].path), "%s\\%s\\", base, ent[i].name);
      tema_read(&g_t[g_n]);
      g_n++;
    }
  }
  if (g_n == 0) {
    /* sem a pasta no disco: pelo menos o classico, embutido no kernel */
    strcpy(g_t[0].dir, "CLASSICO");
    g_t[0].path[0] = 0;
    tema_read(&g_t[0]);
    strcpy(g_t[0].nome, "Classico");
    g_n = 1;
  }
  for (i = 0; i < g_n; i++) g_items[i] = g_t[i].nome;
  g_list.items = g_items;
  g_list.n = g_n;
  g_list.sel = 0;
  for (i = 0; i < g_n; i++)
    if (strcmp(g_t[i].dir, g_cur) == 0) g_list.sel = i;
}

/* ------------------------------------------------ icones da previa (.ICN) --- */
#define PI_MAX 12
static struct {
  char key[64];
  unsigned char* px;
  int w, h;
  unsigned char* sc;
  int ss, sw, sh;
} g_pi[PI_MAX];
static int g_pi_n;

static void prev_reset(void) {
  int i;
  for (i = 0; i < g_pi_n; i++) {
    free(g_pi[i].px);
    free(g_pi[i].sc);
  }
  g_pi_n = 0;
}

static int prev_load(const tema_t* t, const char* name) {
  char key[64];
  FILE* f;
  unsigned char hd[8];
  int i, w, h;
  size_t n;
  snprintf(key, sizeof(key), "%s%s.ICN", t->path, name);
  for (i = 0; i < g_pi_n; i++)
    if (strcmp(g_pi[i].key, key) == 0) return g_pi[i].px ? i : -1;
  if (g_pi_n >= PI_MAX) return -1;
  i = g_pi_n++;
  memset(&g_pi[i], 0, sizeof(g_pi[i]));
  strcpy(g_pi[i].key, key);
  f = fopen(key, "rb");
  if (!f) return -1;
  if (fread(hd, 1, 8, f) != 8 || memcmp(hd, "ICN1", 4) != 0) {
    fclose(f);
    return -1;
  }
  w = hd[4] | (hd[5] << 8);
  h = hd[6] | (hd[7] << 8);
  n = (size_t)w * (size_t)h * 4;
  if (w < 1 || h < 1 || w > 128 || h > 128 || !(g_pi[i].px = (unsigned char*)malloc(n)) ||
      fread(g_pi[i].px, 1, n, f) != n) {
    free(g_pi[i].px);
    g_pi[i].px = 0;
    fclose(f);
    return -1;
  }
  fclose(f);
  g_pi[i].w = w;
  g_pi[i].h = h;
  return i;
}

/* Reduz (media da area) e desenha com alfa no quadrado (x,y,s). */
static int prev_icon(const tema_t* t, const char* name, int x, int y, int s) {
  int i = prev_load(t, name), xx, yy, w, h;
  if (i < 0 || s < 1) return 0;
  w = g_pi[i].w;
  h = g_pi[i].h;
  if (!g_pi[i].sc || g_pi[i].ss != s) {
    free(g_pi[i].sc);
    g_pi[i].sc = (unsigned char*)malloc((size_t)s * (size_t)s * 4);
    if (!g_pi[i].sc) return 0;
    for (yy = 0; yy < s; yy++)
      for (xx = 0; xx < s; xx++) {
        int x0 = xx * w / s, x1 = (xx + 1) * w / s, y0 = yy * h / s, y1 = (yy + 1) * h / s;
        int sx, sy;
        unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
        unsigned char* o = g_pi[i].sc + (yy * s + xx) * 4;
        if (x1 <= x0) x1 = x0 + 1;
        if (y1 <= y0) y1 = y0 + 1;
        for (sy = y0; sy < y1; sy++)
          for (sx = x0; sx < x1; sx++) {
            const unsigned char* p = g_pi[i].px + (sy * w + sx) * 4;
            r += p[0] * p[3];
            g += p[1] * p[3];
            b += p[2] * p[3];
            a += p[3];
            n++;
          }
        o[0] = (unsigned char)(a ? r / a : 0);
        o[1] = (unsigned char)(a ? g / a : 0);
        o[2] = (unsigned char)(a ? b / a : 0);
        o[3] = (unsigned char)(a / n);
      }
    g_pi[i].ss = s;
  }
  gui_pixels(x, y, s, s, g_pi[i].sc, s * 4);
  return 1;
}

/* Area de trabalho em miniatura com as cores e os icones do tema. */
static void draw_preview(const tema_t* t, int x, int y, int w, int h) {
  static const char* const desk[] = {"DOS", "ARQ", "WEB", "CMD", "PASTA", "IMAGEM"};
  static const char* const lab[] = {"DOS", "ARQ", "WEB", "CMD", "Fotos", "Praia"};
  int bar = 30, i, s = 40;
  gui_fill(x, y, w, h, t->desk);
  for (i = 0; i < 6; i++) {
    int ix = x + 14 + (i / 3) * (s + 26), iy = y + 12 + (i % 3) * (s + 24);
    int ok = 0;
    if (t->icons) {
      int c = s * 29 / 64, gs = s * 54 / 100;
      if (prev_icon(t, "FUNDO", ix, iy, s))
        ok = prev_icon(t, desk[i], ix + c - gs / 2, iy + c - gs / 2, gs);
      else
        ok = prev_icon(t, desk[i], ix, iy, s);
    }
    if (!ok) {
      unsigned c = i == 0 ? 0xC0C0C0 : i == 3 ? 0x000000 : i == 4 ? 0xDCB020 : 0x000080;
      gui_fill(ix + 4, iy + 4, s - 8, s - 8, c);
      gui_frame(ix + 4, iy + 4, s - 8, s - 8, i == 3 ? 0xFFFFFF : 0x000000);
    }
    gui_text(ix + (s - wg_text_w(lab[i])) / 2, iy + s + 2, lab[i], -1, t->rotulo, t->desk,
             GUI_TEXT_NOBG);
  }
  /* janela */
  {
    int wx = x + w / 2 - 40, wy = y + 18, ww = w / 2 + 20, wh = h - bar - 40;
    int th = 26;
    if (ww > 10 && wh > th + 10) {
      gui_fill(wx, wy, ww, th, t->titulo);
      gui_fill(wx, wy + th, ww, wh - th, t->modern ? t->painel : 0xC0C0C0);
      gui_frame(wx, wy, ww, wh, t->borda);
      gui_text(wx + 10, wy + 5, "Explorer", -1, t->modern ? t->texto : 0xE8F0FA, t->titulo,
               GUI_TEXT_NOBG);
      gui_fill(wx + ww - 22, wy + 6, 14, 14, 0xE5484D);
      if (t->icons) {
        int k;
        static const char* const fi[] = {"PASTA", "ARQUIVO", "IMAGEM", "VIDEO"};
        for (k = 0; k < 4; k++) {
          int fy = wy + th + 8 + k * 26;
          if (fy + 22 > wy + wh) break;
          prev_icon(t, fi[k], wx + 10, fy, 20);
          gui_text(wx + 38, fy + 2, fi[k], -1, t->texto, t->painel, GUI_TEXT_NOBG);
        }
      }
    }
  }
  /* barra de tarefas */
  gui_fill(x, y + h - bar, w, bar, t->barra);
  gui_fill(x + 6, y + h - bar + 4, 40, bar - 8, t->destaque);
  gui_text(x + 22, y + h - bar + 7, "S", 1, 0xFFFFFF, t->destaque, GUI_TEXT_NOBG);
  gui_text(x + 56, y + h - bar + 7, "SODOS", -1, t->texto, t->barra, GUI_TEXT_NOBG);
  gui_text(x + w - 56, y + h - bar + 7, "13:24", -1, t->texto, t->barra, GUI_TEXT_NOBG);
}

/* ------------------------------------------------------------- aplicar --- */
/* Converte NOME.PNG sem NOME.ICN da pasta (icones novos do usuario). */
static int convert_pngs(const tema_t* t) {
  static struct k_dirent ent[128];
  char dir[48];
  int n, i, done = 0;
  if (!t->path[0]) return 0;
  strcpy(dir, t->path);
  dir[strlen(dir) - 1] = 0;   /* sem a barra do fim */
  n = k_dir(dir, ent, 128);
  for (i = 0; i < n; i++) {
    char png[64], icn[64];
    int l = (int)strlen(ent[i].name);
    FILE* f;
    unsigned char* buf;
    img_t im;
    if (ent[i].dir || l < 5 || l > 12) continue;
    if (strcmp(ent[i].name + l - 4, ".PNG") != 0 && strcmp(ent[i].name + l - 4, ".png") != 0)
      continue;
    snprintf(png, sizeof(png), "%s%s", t->path, ent[i].name);
    snprintf(icn, sizeof(icn), "%s%.*s.ICN", t->path, l - 4, ent[i].name);
    f = fopen(icn, "rb");
    if (f) {
      fclose(f);
      continue;
    }
    if (ent[i].size < 8 || ent[i].size > 512 * 1024) continue;
    buf = (unsigned char*)malloc(ent[i].size);
    if (!buf) continue;
    f = fopen(png, "rb");
    if (!f || fread(buf, 1, ent[i].size, f) != ent[i].size) {
      if (f) fclose(f);
      free(buf);
      continue;
    }
    fclose(f);
    im.rgba = 0;
    if (img_decode(buf, (int)ent[i].size, &im) == 0 && im.w >= 1 && im.h >= 1 &&
        im.w <= 128 && im.h <= 128) {
      f = fopen(icn, "wb");
      if (f) {
        unsigned char hd[8] = {'I', 'C', 'N', '1', 0, 0, 0, 0};
        hd[4] = (unsigned char)(im.w & 255);
        hd[5] = (unsigned char)(im.w >> 8);
        hd[6] = (unsigned char)(im.h & 255);
        hd[7] = (unsigned char)(im.h >> 8);
        fwrite(hd, 1, 8, f);
        fwrite(im.rgba, 1, (size_t)im.w * im.h * 4, f);
        fclose(f);
        done++;
      }
    }
    if (im.rgba) img_free(&im);
    free(buf);
  }
  return done;
}

static void apply(int i) {
  const tema_t* t = &g_t[i];
  char line[48];
  FILE* f;
  int conv = 0, rc;
  if (t->icons) conv = convert_pngs(t);
  rc = k_ui_theme(t->dir, 0);
  if (rc != 0) {
    set_status("tema sem pasta \\SODOS\\ICONS: nao aplicou");
    return;
  }
  snprintf(line, sizeof(line), "tema=%s\r\n", t->dir);
  f = fopen(CFG_USER, "wb");
  if (!f) f = fopen(CFG_USER_USB, "wb");
  if (f) {
    fwrite(line, 1, strlen(line), f);
    fclose(f);
  }
  strcpy(g_cur, t->dir);
  prev_reset();
  if (!f) set_status("aplicado, mas nao gravou TEMA.CFG");
  else if (conv) {
    char b[64];
    snprintf(b, sizeof(b), "tema %s aplicado (%d icones convertidos)", t->nome, conv);
    set_status(b);
  } else {
    char b[64];
    snprintf(b, sizeof(b), "tema %s aplicado", t->nome);
    set_status(b);
  }
}

int main(void) {
  gui_event_t ev;
  int run = 1, ready = 0, last_sel = -1;
  k_ui_theme_t kt;

  if (gui_open_pct("Temas", 62, 70) != 0) {
    printf("TEMAS: abra pelo WIN (Iniciar > Configuracoes > Temas)\n");
    return 1;
  }
  memset(&kt, 0, sizeof(kt));
  k_ui_theme(0, &kt);
  strcpy(g_cur, kt.name[0] ? kt.name : "CLASSICO");
  scan();
  set_status("escolha um tema e clique em Aplicar");
  wg_focus_set(&id_list);

  while (run) {
    const wg_theme_t* th;
    int sw = gui_width(), sh = gui_height(), pad = 14, lw = 210, bh, st_h, py, ph, px, pw;
    if (!ready && k_ms() > 400) ready = 1;
    wg_input(g_mx, g_my, g_down);
    th = wg_theme();
    bh = th->btn_h;
    st_h = gui_char_h() + 10;
    gui_fill(0, 0, sw, sh, th->bg);

    /* lista de temas */
    wg_label_dim(pad, pad, "Temas instalados");
    wg_listbox(&id_list, &g_list, pad, pad + 24, lw, sh - pad * 3 - 24 - bh - st_h - 8);
    if (g_list.sel != last_sel) {
      last_sel = g_list.sel;
      prev_reset();
    }

    /* previa */
    px = pad + lw + pad;
    pw = sw - px - pad;
    py = pad;
    ph = sh - pad * 3 - bh - st_h - 8 - 120;
    if (ph < 160) ph = 160;
    wg_card(px, py, pw, ph);
    if (g_list.sel >= 0 && g_list.sel < g_n) {
      const tema_t* t = &g_t[g_list.sel];
      char info[64];
      gui_text(px + 14, py + 10, t->nome, -1, th->text, th->panel, GUI_TEXT_NOBG);
      snprintf(info, sizeof(info), "%s - %s", t->modern ? "moderno" : "classico",
               t->icons ? "icones da pasta" : "icones internos");
      gui_text(px + 14 + wg_text_w(t->nome) + 16, py + 10, info, -1, th->text_dim, th->panel,
               GUI_TEXT_NOBG);
      if (strcmp(t->dir, g_cur) == 0)
        wg_badge(px + pw - 90, py + 8, "em uso", 0x2E9E5B);
      draw_preview(t, px + 12, py + 36, pw - 24, ph - 48);
    }

    /* amostra dos controles (no estilo do tema em uso) */
    {
      int cy = py + ph + 14;
      wg_group(px, cy, pw, 104, "Controles do tema em uso");
      wg_switch(&id_sw, px + 16, cy + 34, "Interruptor", &g_sw_demo);
      wg_check(&id_ck, px + 16, cy + 68, "Caixa de marcar", &g_ck_demo);
      wg_slider(&id_sl, px + pw / 2, cy + 30, pw / 2 - 24, &g_sl_demo, 0, 100);
      wg_progress(px + pw / 2, cy + 70, pw / 2 - 24, 10, g_sl_demo, 0);
    }

    {
      int by = sh - st_h - bh - pad;
      if (wg_button_primary(&id_apply, pad, by, 120, bh, "Aplicar", ready && g_n > 0))
        apply(g_list.sel);
      if (wg_button(&id_classic, pad + 130, by, 120, bh, "Classico", ready)) {
        int i;
        for (i = 0; i < g_n; i++)
          if (strcmp(g_t[i].dir, "CLASSICO") == 0) g_list.sel = i;
        if (k_ui_theme("CLASSICO", 0) == 0) {
          FILE* f = fopen(CFG_USER, "wb");
          if (!f) f = fopen(CFG_USER_USB, "wb");
          if (f) {
            fputs("tema=CLASSICO\r\n", f);
            fclose(f);
          }
          strcpy(g_cur, "CLASSICO");
          set_status("voltou ao classico");
        }
      }
      if (wg_button(&id_exit, sw - pad - 100, by, 100, bh, "Sair", ready)) run = 0;
      (void)id_btn;
    }
    wg_status(0, sh - st_h, sw, g_status, g_cur);
    gui_present();

    if (gui_wait(&ev, 120) != GUI_EV_NONE) {
      if (ev.type == GUI_EV_CLOSE) run = 0;
      else if (ev.type == GUI_EV_DOWN) {
        g_mx = ev.x;
        g_my = ev.y;
        g_down = 1;
      } else if (ev.type == GUI_EV_UP) {
        g_mx = ev.x;
        g_my = ev.y;
        g_down = 0;
      } else if (ev.type == GUI_EV_DRAG || ev.type == GUI_EV_MOVE) {
        g_mx = ev.x;
        g_my = ev.y;
      } else if (ev.type == GUI_EV_KEY) {
        if (ev.key == 27) run = 0;
        else if (ev.key == '\r' && ready) apply(g_list.sel);
        else wg_list_key(&g_list, ev.key, 8);
      }
    }
  }
  prev_reset();
  gui_close();
  return 0;
}
