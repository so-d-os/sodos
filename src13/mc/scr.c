/* scr.c - Tela de celulas (caractere | cor << 8), caixas e dialogos do MC.
 * Desenha numa copia em memoria; scr_flush manda para a tela so as linhas
 * que mudaram. */
#include "mc.h"

static unsigned short scr[SCR_H * SCR_W];
static unsigned short shown[SCR_H * SCR_W];

int scr_init(void) {
  int i;
  if (k_vput(0, 0, scr, 0) != 0) return -1;
  for (i = 0; i < SCR_H * SCR_W; i++) scr[i] = 0x0720;
  scr_invalidate();
  return 0;
}

void scr_done(void) {
  k_clear();
  k_cursor(0, 0);
}

void scr_invalidate(void) {
  int i;
  for (i = 0; i < SCR_H * SCR_W; i++) shown[i] = 0xFFFF;
}

void scr_put(int y, int x, int ch, int color) {
  if (y < 0 || y >= SCR_H || x < 0 || x >= SCR_W) return;
  scr[y * SCR_W + x] = (unsigned short)((ch & 0xFF) | ((color & 0xFF) << 8));
}

void scr_text(int y, int x, const char* s, int color) {
  while (*s && x < SCR_W) scr_put(y, x++, (unsigned char)*s++, color);
}

void scr_textw(int y, int x, const char* s, int w, int color) {
  int i = 0;
  while (i < w && s[i]) {
    scr_put(y, x + i, (unsigned char)s[i], color);
    i++;
  }
  while (i < w) scr_put(y, x + i++, ' ', color);
}

void scr_fill(int y, int x, int h, int w, int ch, int color) {
  int r, c;
  for (r = 0; r < h; r++)
    for (c = 0; c < w; c++) scr_put(y + r, x + c, ch, color);
}

void scr_paint(int y, int x, int w, int color) {
  int i;
  if (y < 0 || y >= SCR_H) return;
  for (i = 0; i < w; i++) {
    int c = x + i;
    if (c < 0 || c >= SCR_W) continue;
    scr[y * SCR_W + c] = (unsigned short)((scr[y * SCR_W + c] & 0xFF) | ((color & 0xFF) << 8));
  }
}

/* CP850: simples ┌─┐│└┘, dupla ╔═╗║╚╝ */
void scr_box(int y, int x, int h, int w, int color, int dbl) {
  int hl = dbl ? 0xCD : 0xC4, vl = dbl ? 0xBA : 0xB3;
  int i;
  scr_put(y, x, dbl ? 0xC9 : 0xDA, color);
  scr_put(y, x + w - 1, dbl ? 0xBB : 0xBF, color);
  scr_put(y + h - 1, x, dbl ? 0xC8 : 0xC0, color);
  scr_put(y + h - 1, x + w - 1, dbl ? 0xBC : 0xD9, color);
  for (i = 1; i < w - 1; i++) {
    scr_put(y, x + i, hl, color);
    scr_put(y + h - 1, x + i, hl, color);
  }
  for (i = 1; i < h - 1; i++) {
    scr_put(y + i, x, vl, color);
    scr_put(y + i, x + w - 1, vl, color);
  }
}

/* Sombra a direita e embaixo, como no mc: escurece o que esta atras. */
void scr_shadow(int y, int x, int h, int w) {
  int i;
  for (i = 1; i <= h; i++) scr_paint(y + i, x + w, 2, C_SHADOW);
  scr_paint(y + h, x + 2, w, C_SHADOW);
}

void scr_flush(void) {
  int r, c;
  for (r = 0; r < SCR_H; r++) {
    int a = -1, b = -1;
    for (c = 0; c < SCR_W; c++) {
      if (scr[r * SCR_W + c] != shown[r * SCR_W + c]) {
        if (a < 0) a = c;
        b = c;
      }
    }
    if (a < 0) continue;
    k_vput(r, a, scr + r * SCR_W + a, b - a + 1);
    for (c = a; c <= b; c++) shown[r * SCR_W + c] = scr[r * SCR_W + c];
  }
}

int scr_key(void) {
  int c;
  scr_flush();
  c = k_getc();
  if (c < 0) return K_EOF;
  if (c == '\r') return K_ENTER;
  return c;
}

int scr_cp850(int c) {
  if (c >= 'a' && c <= 'z') return c - 32;
  return c;
}

/* ---------- dialogos ---------- */

static int str_w(const char* s) {
  int n = 0;
  while (s[n] && s[n] != '\n') n++;
  return n;
}

static int msg_lines(const char* msg, int* width) {
  int n = 1, w = 0;
  const char* p = msg;
  for (;;) {
    int l = str_w(p);
    if (l > w) w = l;
    p += l;
    if (!*p) break;
    p++;
    n++;
  }
  *width = w;
  return n;
}

/* Caixa centrada com titulo; devolve y e x do canto. */
static void dlg_frame(const char* title, int h, int w, int color, int* py, int* px) {
  int y = (SCR_H - h) / 2 - 1;
  int x = (SCR_W - w) / 2;
  int tl = (int)strlen(title);
  scr_fill(y, x, h, w, ' ', color);
  scr_box(y + 1, x + 2, h - 2, w - 4, color, 1);
  scr_shadow(y, x, h, w);
  if (tl) {
    int tx = x + (w - tl - 2) / 2;
    scr_put(y + 1, tx, ' ', color);
    scr_text(y + 1, tx + 1, title, (color & 0xF0) == 0x70 ? C_DLGTTL : color);
    scr_put(y + 1, tx + 1 + tl, ' ', color);
  }
  *py = y;
  *px = x;
}

/* Bloco de linhas centrado; linha unica fica no meio, varias alinham a esquerda. */
static void draw_lines(int y, int x, int w, const char* msg, int color) {
  const char* p = msg;
  int r = 0, lw;
  int bx;
  msg_lines(msg, &lw);
  bx = x + (w - lw) / 2;
  for (;;) {
    int l = str_w(p);
    int i;
    int cx = (r == 0 && !p[l]) ? x + (w - l) / 2 : bx;
    for (i = 0; i < l; i++) scr_put(y + r, cx + i, (unsigned char)p[i], color);
    p += l;
    if (!*p) break;
    p++;
    r++;
  }
}

/* Botoes centrados na linha y. Devolve a largura. */
static void draw_buttons(int y, int x, int w, const char** b, int n, int foc, int color, int fcolor) {
  int total = 0, i, cx;
  for (i = 0; i < n; i++) total += (int)strlen(b[i]) + 4 + (i ? 1 : 0);
  cx = x + (w - total) / 2;
  for (i = 0; i < n; i++) {
    int c = (i == foc) ? fcolor : color;
    int l = (int)strlen(b[i]);
    if (i) cx++;
    scr_text(y, cx, "[ ", c);
    scr_text(y, cx + 2, b[i], c);
    scr_put(y, cx + 2, (unsigned char)b[i][0], (c & 0xF0) | ((color & 0xF0) == 0x40 ? 0x0E : 0x04));
    scr_text(y, cx + 2 + l, " ]", c);
    cx += l + 4;
  }
}

static int buttons_run(const char* title, const char* msg, const char** b, int n, int err, int esc) {
  int lw, lines = msg_lines(msg, &lw);
  int bw = 0, i, w, h, y, x, foc = 0;
  int color = err ? C_ERR : C_DLG, fcolor = err ? C_ERRFOC : C_DLGFOC;
  for (i = 0; i < n; i++) bw += (int)strlen(b[i]) + 5;
  w = lw > bw ? lw : bw;
  if ((int)strlen(title) + 2 > w) w = (int)strlen(title) + 2;
  w += 8;
  if (w > SCR_W - 2) w = SCR_W - 2;
  h = lines + 6;
  for (;;) {
    int k;
    dlg_frame(title, h, w, color, &y, &x);
    draw_lines(y + 2, x + 3, w - 6, msg, color);
    draw_buttons(y + h - 3, x + 3, w - 6, b, n, foc, color, fcolor);
    k_cursor(-1, 0);
    k = scr_key();
    if (k == K_ESC || k == K_F(10) || k == K_EOF) return esc;
    if (k == K_ENTER || k == ' ') return foc;
    if (k == K_LEFT || k == K_UP) foc = (foc + n - 1) % n;
    else if (k == K_RIGHT || k == K_DOWN || k == K_TAB) foc = (foc + 1) % n;
    else if (k < 0x100)
      for (i = 0; i < n; i++)
        if (scr_cp850(k) == scr_cp850((unsigned char)b[i][0])) return i;
  }
}

void dlg_msg(const char* title, const char* msg, int err) {
  const char* b[1];
  b[0] = "OK";
  buttons_run(title, msg, b, 1, err, 0);
  draw_all();
}

int dlg_yesno(const char* title, const char* msg, int err) {
  const char* b[2];
  int r;
  b[0] = "Sim";
  b[1] = "Nao";
  r = buttons_run(title, msg, b, 2, err, 1);
  draw_all();
  return r == 0;
}

int dlg_ask4(const char* title, const char* msg) {
  const char* b[4];
  int r;
  b[0] = "Sim";
  b[1] = "Nao";
  b[2] = "Todos";
  b[3] = "Cancelar";
  r = buttons_run(title, msg, b, 4, 1, 3);
  if (r == 0) return 1;
  if (r == 1) return 0;
  if (r == 2) return 2;
  return -1;
}

void dlg_note(const char* title, const char* msg) {
  int lw, lines = msg_lines(msg, &lw);
  int w = lw + 10, y, x;
  if (w < 40) w = 40;
  if (w > SCR_W - 2) w = SCR_W - 2;
  dlg_frame(title, lines + 4, w, C_DLG, &y, &x);
  draw_lines(y + 2, x + 3, w - 6, msg, C_DLG);
  scr_flush();
}

/* Campo de texto com cursor; Enter = 1, Esc = 0. */
int dlg_input(const char* title, const char* prompt, char* buf, int max) {
  int w = 64, h = 8, y, x;
  int len = (int)strlen(buf), pos = len, off = 0, fw = w - 10;
  const char* b[2];
  b[0] = "OK";
  b[1] = "Cancelar";
  for (;;) {
    int k, i;
    dlg_frame(title, h, w, C_DLG, &y, &x);
    scr_textw(y + 2, x + 4, prompt, w - 8, C_DLG);
    if (pos < off) off = pos;
    if (pos > off + fw - 1) off = pos - fw + 1;
    for (i = 0; i < fw; i++) {
      int c = off + i < len ? (unsigned char)buf[off + i] : ' ';
      scr_put(y + 3, x + 4 + i, c, C_INPUT);
    }
    draw_buttons(y + h - 3, x + 3, w - 6, b, 2, -1, C_DLG, C_DLGFOC);
    k_cursor(y + 3, x + 4 + pos - off);
    k = scr_key();
    if (k == K_ESC || k == K_F(10) || k == K_EOF) {
      k_cursor(-1, 0);
      draw_all();
      return 0;
    }
    if (k == K_ENTER) {
      k_cursor(-1, 0);
      draw_all();
      return 1;
    }
    if (k == K_LEFT) {
      if (pos > 0) pos--;
    } else if (k == K_RIGHT) {
      if (pos < len) pos++;
    } else if (k == K_HOME) {
      pos = 0;
    } else if (k == K_END) {
      pos = len;
    } else if (k == K_BS) {
      if (pos > 0) {
        memmove(buf + pos - 1, buf + pos, (size_t)(len - pos + 1));
        pos--;
        len--;
      }
    } else if (k == K_DEL) {
      if (pos < len) {
        memmove(buf + pos, buf + pos + 1, (size_t)(len - pos));
        len--;
      }
    } else if (k >= 32 && k < 256 && k != 127 && len < max - 1) {
      memmove(buf + pos + 1, buf + pos, (size_t)(len - pos + 1));
      buf[pos++] = (char)k;
      len++;
    }
  }
}

int dlg_list(const char* title, const char** items, int n, int cur) {
  int w = (int)strlen(title) + 4, i, y, x, h, top = 0, rows;
  for (i = 0; i < n; i++)
    if ((int)strlen(items[i]) + 2 > w) w = (int)strlen(items[i]) + 2;
  w += 8;
  rows = n < 15 ? n : 15;
  h = rows + 4;
  if (cur < 0 || cur >= n) cur = 0;
  for (;;) {
    int k;
    dlg_frame(title, h, w, C_DLG, &y, &x);
    if (cur < top) top = cur;
    if (cur >= top + rows) top = cur - rows + 1;
    for (i = 0; i < rows && top + i < n; i++) {
      int c = (top + i == cur) ? C_DLGFOC : C_DLG;
      scr_put(y + 2 + i, x + 3, ' ', c);
      scr_textw(y + 2 + i, x + 4, items[top + i], w - 8, c);
      scr_put(y + 2 + i, x + w - 4, ' ', c);
    }
    k_cursor(-1, 0);
    k = scr_key();
    if (k == K_ESC || k == K_F(10) || k == K_EOF) {
      draw_all();
      return -1;
    }
    if (k == K_ENTER) {
      draw_all();
      return cur;
    }
    if (k == K_UP) cur = (cur + n - 1) % n;
    else if (k == K_DOWN) cur = (cur + 1) % n;
    else if (k == K_HOME || k == K_PGUP) cur = 0;
    else if (k == K_END || k == K_PGDN) cur = n - 1;
    else if (k < 0x100)
      for (i = 0; i < n; i++)
        if (scr_cp850(k) == scr_cp850((unsigned char)items[i][0])) {
          draw_all();
          return i;
        }
  }
}
