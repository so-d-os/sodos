/* term.c - Terminal do console do WSL: interpreta a saida ANSI/VT do Linux
 * (TERM=linux) e desenha na tela de celulas do SODOS (k_vput), com cores,
 * cursor, apagar/rolar, regiao de rolagem e UTF-8 -> CP850. Sem tela de
 * celulas (WIN, saida desviada pela API) cai para texto puro: tira as
 * sequencias e manda o resto por k_putc. */
#include <string.h>
#include "term.h"
#include "../sodos_api.h"

#define MAXC 256
#define MAXR 128

static int g_cells;       /* 1 = k_vput funciona */
static int W = 80, H = 25;
static uint16_t scr[MAXR][MAXC];
static uint8_t dirty[MAXR];
static int cx, cy, sx, sy;           /* cursor e cursor salvo */
static int top, bot;                 /* regiao de rolagem [top, bot] */
static int fg, bg, bold, rev, uline;
static int cur_on = 1, wrap_next;
static int st;                       /* 0 normal, 1 ESC, 2 CSI, 3 OSC, 4 ESC (/) */
static int par[16], np, priv;
static uint32_t u8cp;
static int u8need;
static term_reply_fn g_reply;
static void* g_reply_arg;

/* CP850 0x80..0xFF -> Unicode */
static const uint16_t cp850[128] = {
  0xC7,0xFC,0xE9,0xE2,0xE4,0xE0,0xE5,0xE7,0xEA,0xEB,0xE8,0xEF,0xEE,0xEC,0xC4,0xC5,
  0xC9,0xE6,0xC6,0xF4,0xF6,0xF2,0xFB,0xF9,0xFF,0xD6,0xDC,0xF8,0xA3,0xD8,0xD7,0x192,
  0xE1,0xED,0xF3,0xFA,0xF1,0xD1,0xAA,0xBA,0xBF,0xAE,0xAC,0xBD,0xBC,0xA1,0xAB,0xBB,
  0x2591,0x2592,0x2593,0x2502,0x2524,0xC1,0xC2,0xC0,0xA9,0x2563,0x2551,0x2557,0x255D,0xA2,0xA5,0x2510,
  0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0xE3,0xC3,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0xA4,
  0xF0,0xD0,0xCA,0xCB,0xC8,0x131,0xCD,0xCE,0xCF,0x2518,0x250C,0x2588,0x2584,0xA6,0xCC,0x2580,
  0xD3,0xDF,0xD4,0xD2,0xF5,0xD5,0xB5,0xFE,0xDE,0xDA,0xDB,0xD9,0xFD,0xDD,0xAF,0xB4,
  0xAD,0xB1,0x2017,0xBE,0xB6,0xA7,0xF7,0xB8,0xB0,0xA8,0xB7,0xB9,0xB3,0xB2,0x25A0,0xA0};

static uint8_t to_cp850(uint32_t u) {
  int i;
  if (u < 0x80) return (uint8_t)u;
  for (i = 0; i < 128; i++)
    if (cp850[i] == u) return (uint8_t)(0x80 + i);
  /* parecidos */
  switch (u) {
  case 0x256D: case 0x250F: return 0xDA;
  case 0x256E: case 0x2513: return 0xBF;
  case 0x2570: case 0x2517: return 0xC0;
  case 0x256F: case 0x251B: return 0xD9;
  case 0x2501: return 0xC4;
  case 0x2503: return 0xB3;
  case 0x2018: case 0x2019: return '\'';
  case 0x201C: case 0x201D: return '"';
  case 0x2013: case 0x2014: return '-';
  case 0x2022: return 0xFE;
  case 0x2026: return '.';
  }
  return '?';
}

static const uint8_t ansi2vga[8] = {0, 4, 2, 6, 1, 5, 3, 7};

static uint16_t attr(void) {
  int f = fg, b = bg;
  if (bold && f < 8) f += 8;
  if (rev) {
    int t = f;
    f = b;
    b = t;
  }
  if (uline && !rev && f == 7) f = 11; /* sublinhado: ciano claro */
  return (uint16_t)(((b & 15) << 4 | (f & 15)) << 8);
}

static void blank_row(int r, int c0, int c1) {
  /* espacos com o fundo atual (frente cinza) */
  uint16_t a = (uint16_t)((attr() & 0xF000) | 0x0700 | 0x20);
  int c;
  for (c = c0; c < c1 && c < W; c++) scr[r][c] = a;
  dirty[r] = 1;
}

static void scroll_up(int n) {
  int r;
  if (n > bot - top + 1) n = bot - top + 1;
  for (r = top; r <= bot - n; r++) {
    memcpy(scr[r], scr[r + n], sizeof scr[r]);
    dirty[r] = 1;
  }
  for (r = bot - n + 1; r <= bot; r++) blank_row(r, 0, W);
}

static void scroll_down(int n) {
  int r;
  if (n > bot - top + 1) n = bot - top + 1;
  for (r = bot; r >= top + n; r--) {
    memcpy(scr[r], scr[r - n], sizeof scr[r]);
    dirty[r] = 1;
  }
  for (r = top; r < top + n; r++) blank_row(r, 0, W);
}

static void lf(void) {
  if (cy == bot) scroll_up(1);
  else if (cy < H - 1) cy++;
}

static void put(uint8_t ch) {
  if (wrap_next) {
    cx = 0;
    lf();
    wrap_next = 0;
  }
  scr[cy][cx] = (uint16_t)(attr() | ch);
  dirty[cy] = 1;
  if (cx == W - 1) wrap_next = 1;
  else cx++;
}

static int P(int i, int def) { return i < np && par[i] > 0 ? par[i] : def; }

static void clampc(void) {
  if (cx < 0) cx = 0;
  if (cx >= W) cx = W - 1;
  if (cy < 0) cy = 0;
  if (cy >= H) cy = H - 1;
  wrap_next = 0;
}

static void reply(const char* s) {
  if (g_reply) g_reply(g_reply_arg, s);
}

static void sgr(void) {
  int i;
  if (np == 0) {
    fg = 7; bg = 0; bold = rev = uline = 0;
    return;
  }
  for (i = 0; i < np; i++) {
    int p = par[i];
    if (p == 0) { fg = 7; bg = 0; bold = rev = uline = 0; }
    else if (p == 1) bold = 1;
    else if (p == 2 || p == 22) bold = 0;
    else if (p == 4) uline = 1;
    else if (p == 24) uline = 0;
    else if (p == 7) rev = 1;
    else if (p == 27) rev = 0;
    else if (p >= 30 && p <= 37) fg = ansi2vga[p - 30];
    else if (p == 39) fg = 7;
    else if (p >= 40 && p <= 47) bg = ansi2vga[p - 40];
    else if (p == 49) bg = 0;
    else if (p >= 90 && p <= 97) fg = ansi2vga[p - 90] + 8;
    else if (p >= 100 && p <= 107) bg = ansi2vga[p - 100] + 8;
    else if ((p == 38 || p == 48) && i + 1 < np) {
      int c = -1;
      if (par[i + 1] == 5 && i + 2 < np) {
        int n = par[i + 2];
        if (n < 8) c = ansi2vga[n];
        else if (n < 16) c = ansi2vga[n - 8] + 8;
        else if (n >= 232) c = n < 244 ? 8 : 7;
        else {
          int v = n - 16, r = v / 36, g = v / 6 % 6, b = v % 6;
          c = (r > 2 ? 4 : 0) | (g > 2 ? 2 : 0) | (b > 2 ? 1 : 0);
          if (r > 3 || g > 3 || b > 3) c += 8;
        }
        i += 2;
      } else if (par[i + 1] == 2 && i + 4 < np) {
        int r = par[i + 2], g = par[i + 3], b = par[i + 4];
        c = (r > 127 ? 4 : 0) | (g > 127 ? 2 : 0) | (b > 127 ? 1 : 0);
        if (r > 200 || g > 200 || b > 200) c += 8;
        i += 4;
      }
      if (c >= 0) {
        if (p == 38) fg = c;
        else bg = c;
      }
    }
  }
}

static void csi(uint8_t f) {
  int i, n;
  switch (f) {
  case 'm': sgr(); break;
  case 'H': case 'f': cy = P(0, 1) - 1; cx = P(1, 1) - 1; clampc(); break;
  case 'A': cy -= P(0, 1); clampc(); break;
  case 'B': case 'e': cy += P(0, 1); clampc(); break;
  case 'C': case 'a': cx += P(0, 1); clampc(); break;
  case 'D': cx -= P(0, 1); clampc(); break;
  case 'E': cy += P(0, 1); cx = 0; clampc(); break;
  case 'F': cy -= P(0, 1); cx = 0; clampc(); break;
  case 'G': case '`': cx = P(0, 1) - 1; clampc(); break;
  case 'd': cy = P(0, 1) - 1; clampc(); break;
  case 'J':
    n = np ? par[0] : 0;
    if (n == 0) {
      blank_row(cy, cx, W);
      for (i = cy + 1; i < H; i++) blank_row(i, 0, W);
    } else if (n == 1) {
      for (i = 0; i < cy; i++) blank_row(i, 0, W);
      blank_row(cy, 0, cx + 1);
    } else {
      for (i = 0; i < H; i++) blank_row(i, 0, W);
    }
    break;
  case 'K':
    n = np ? par[0] : 0;
    if (n == 0) blank_row(cy, cx, W);
    else if (n == 1) blank_row(cy, 0, cx + 1);
    else blank_row(cy, 0, W);
    break;
  case 'L':
    if (cy >= top && cy <= bot) {
      int t = top;
      top = cy;
      scroll_down(P(0, 1));
      top = t;
    }
    break;
  case 'M':
    if (cy >= top && cy <= bot) {
      int t = top;
      top = cy;
      scroll_up(P(0, 1));
      top = t;
    }
    break;
  case 'P':
    n = P(0, 1);
    if (n > W - cx) n = W - cx;
    memmove(&scr[cy][cx], &scr[cy][cx + n], (size_t)(W - cx - n) * 2);
    blank_row(cy, W - n, W);
    break;
  case '@':
    n = P(0, 1);
    if (n > W - cx) n = W - cx;
    memmove(&scr[cy][cx + n], &scr[cy][cx], (size_t)(W - cx - n) * 2);
    blank_row(cy, cx, cx + n);
    break;
  case 'X':
    blank_row(cy, cx, cx + P(0, 1));
    break;
  case 'S': scroll_up(P(0, 1)); break;
  case 'T': scroll_down(P(0, 1)); break;
  case 'r':
    top = P(0, 1) - 1;
    bot = P(1, H) - 1;
    if (top < 0 || top >= H) top = 0;
    if (bot <= top || bot >= H) bot = H - 1;
    cx = cy = 0;
    break;
  case 's': sx = cx; sy = cy; break;
  case 'u': cx = sx; cy = sy; clampc(); break;
  case 'h': case 'l':
    if (priv) {
      for (i = 0; i < np; i++) {
        if (par[i] == 25) cur_on = f == 'h';
        else if (par[i] == 1049 || par[i] == 47 || par[i] == 1047) {
          /* tela alternativa: so limpa */
          int r;
          for (r = 0; r < H; r++) blank_row(r, 0, W);
          if (f == 'h') { sx = cx; sy = cy; } else { cx = sx; cy = sy; }
        }
      }
    }
    break;
  case 'n':
    if (np && par[0] == 6) {
      char b[32];
      int l = 0, v;
      b[l++] = 27; b[l++] = '[';
      v = cy + 1;
      if (v >= 100) b[l++] = (char)('0' + v / 100);
      if (v >= 10) b[l++] = (char)('0' + v / 10 % 10);
      b[l++] = (char)('0' + v % 10);
      b[l++] = ';';
      v = cx + 1;
      if (v >= 100) b[l++] = (char)('0' + v / 100);
      if (v >= 10) b[l++] = (char)('0' + v / 10 % 10);
      b[l++] = (char)('0' + v % 10);
      b[l++] = 'R';
      b[l] = 0;
      reply(b);
    } else if (np && par[0] == 5) {
      reply("\033[0n");
    }
    break;
  case 'c':
    if (!priv) reply("\033[?6c");
    break;
  }
}

static void feed(uint8_t c) {
  switch (st) {
  case 1: /* ESC */
    st = 0;
    switch (c) {
    case '[': st = 2; np = 0; priv = 0; memset(par, 0, sizeof par); return;
    case ']': st = 3; return;
    case '(': case ')': case '*': case '+': case '#': case '%': st = 4; return;
    case '7': sx = cx; sy = cy; return;
    case '8': cx = sx; cy = sy; clampc(); return;
    case 'D': lf(); return;
    case 'E': cx = 0; lf(); return;
    case 'M':
      if (cy == top) scroll_down(1);
      else if (cy > 0) cy--;
      return;
    case 'c':
      fg = 7; bg = 0; bold = rev = uline = 0;
      top = 0; bot = H - 1;
      { int r; for (r = 0; r < H; r++) blank_row(r, 0, W); }
      cx = cy = 0;
      return;
    }
    return;
  case 2: /* CSI */
    if (c >= '0' && c <= '9') {
      if (np == 0) np = 1;
      if (par[np - 1] < 10000) par[np - 1] = par[np - 1] * 10 + (c - '0');
    } else if (c == ';' || c == ':') {
      if (np == 0) np = 1;
      if (np < 16) np++;
    } else if (c == '?' || c == '>' || c == '=' || c == '!') {
      priv = 1;
    } else if (c >= 0x40 && c <= 0x7E) {
      st = 0;
      csi(c);
    } else if (c == 27) {
      st = 1;
    }
    return;
  case 3: /* OSC: ate BEL ou ESC \ */
    if (c == 7) st = 0;
    else if (c == 27) st = 1;
    return;
  case 4:
    st = 0;
    return;
  }
  /* UTF-8 */
  if (u8need) {
    if ((c & 0xC0) == 0x80) {
      u8cp = (u8cp << 6) | (c & 0x3F);
      if (--u8need == 0) put(to_cp850(u8cp));
      return;
    }
    u8need = 0;
    put('?');
  }
  if (c >= 0xC0 && c < 0xF8) {
    u8need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
    u8cp = c & (c >= 0xF0 ? 0x07 : c >= 0xE0 ? 0x0F : 0x1F);
    return;
  }
  switch (c) {
  case 27: st = 1; return;
  case '\r': cx = 0; wrap_next = 0; return;
  case '\n': case 11: case 12: lf(); wrap_next = 0; return;
  case '\b': if (cx > 0) cx--; wrap_next = 0; return;
  case '\t': cx = (cx + 8) & ~7; if (cx >= W) cx = W - 1; return;
  case 7: case 0: case 14: case 15: return;
  }
  if (c < 32 || c == 127) return;
  put(c >= 0x80 ? '?' : c);
}

/* ---- texto puro (sem tela de celulas) ---- */
static void feed_plain(uint8_t c) {
  if (st == 1) {
    st = c == '[' ? 2 : c == ']' ? 3 : (c == '(' || c == ')') ? 4 : 0;
    return;
  }
  if (st == 2) {
    if (c >= 0x40 && c <= 0x7E) st = 0;
    return;
  }
  if (st == 3) {
    if (c == 7) st = 0;
    else if (c == 27) st = 1;
    return;
  }
  if (st == 4) {
    st = 0;
    return;
  }
  if (c == 27) {
    st = 1;
    return;
  }
  if (c == '\r' || c == 7) return;
  k_putc(c);
}

int term_init(term_reply_fn fn, void* arg) {
  int c, r;
  uint16_t probe = 0x0720;
  g_reply = fn;
  g_reply_arg = arg;
  fg = 7; bg = 0;
  st = 0;
  if (k_textsize(&c, &r) != 0 || c <= 0 || r <= 0) {
    g_cells = 0;
    return 0;
  }
  if (c > MAXC) c = MAXC;
  if (r > MAXR) r = MAXR;
  W = c;
  H = r;
  if (k_vput(H - 1, W - 1, &probe, 1) < 0) {
    g_cells = 0;
    return 0;
  }
  g_cells = 1;
  k_blink(0); /* fundo claro (cores 8-15) */
  top = 0;
  bot = H - 1;
  for (r = 0; r < H; r++) blank_row(r, 0, W);
  cx = cy = 0;
  return 1;
}

void term_size(int* cols, int* rows) {
  *cols = W;
  *rows = H;
}

void term_write(const uint8_t* s, int n) {
  int i;
  for (i = 0; i < n; i++) {
    if (g_cells) feed(s[i]);
    else feed_plain(s[i]);
  }
}

void term_flush(void) {
  int r;
  if (!g_cells) return;
  for (r = 0; r < H; r++)
    if (dirty[r]) {
      k_vput(r, 0, scr[r], W);
      dirty[r] = 0;
    }
  if (cur_on) k_cursor(cy, cx);
  else k_cursor(-1, 0);
}

void term_end(void) {
  if (!g_cells) return;
  term_flush();
  /* deixa o prompt do SODOS na ultima linha, numa tela limpa abaixo */
  k_cursor(H - 1, 0);
  k_blink(1);
  k_clear();
}
