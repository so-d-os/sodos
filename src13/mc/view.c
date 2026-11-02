/* view.c - F3: mostra um arquivo em tela cheia, como o mcview.
 * Texto UTF-8 (feito no Linux) vira CP850; bytes que nao sao UTF-8 valido
 * ja sao CP850 e passam como estao. F2 quebra as linhas, F4 mostra em hexa. */
#include "mc.h"

#define VIEW_MAX (4 * 1024 * 1024)
#define VIEW_ROWS 23

/* U+00A0..U+00FF -> CP850 */
static const unsigned char latin1_cp850[96] = {
  0xFF, 0xAD, 0xBD, 0x9C, 0xCF, 0xBE, 0xDD, 0xF5, 0xF9, 0xB8, 0xA6, 0xAE,
  0xAA, 0xF0, 0xA9, 0xEE, 0xF8, 0xF1, 0xFD, 0xFC, 0xEF, 0xE6, 0xF4, 0xFA,
  0xF7, 0xFB, 0xA7, 0xAF, 0xAC, 0xAB, 0xF3, 0xA8, 0xB7, 0xB5, 0xB6, 0xC7,
  0x8E, 0x8F, 0x92, 0x80, 0xD4, 0x90, 0xD2, 0xD3, 0xDE, 0xD6, 0xD7, 0xD8,
  0xD1, 0xA5, 0xE3, 0xE0, 0xE2, 0xE5, 0x99, 0x9E, 0x9D, 0xEB, 0xE9, 0xEA,
  0x9A, 0xED, 0xE8, 0xE1, 0x85, 0xA0, 0x83, 0xC6, 0x84, 0x86, 0x91, 0x87,
  0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B, 0xD0, 0xA4, 0x95, 0xA2,
  0x93, 0xE4, 0x94, 0xF6, 0x9B, 0x97, 0xA3, 0x96, 0x81, 0xEC, 0xE7, 0x98,
};

/* Aspas, tracos e molduras comuns; o resto vira '?'. */
static const unsigned short uni_from[] = {
  0x2018, 0x2019, 0x201C, 0x201D, 0x2013, 0x2014, 0x2022, 0x2026, 0x20AC,
  0x2500, 0x2502, 0x250C, 0x2510, 0x2514, 0x2518, 0x251C, 0x2524, 0x252C,
  0x2534, 0x253C, 0x2550, 0x2551, 0x2554, 0x2557, 0x255A, 0x255D, 0x2588,
  0x2591, 0x2592, 0x2593, 0x2192, 0x2190, 0x2191, 0x2193,
};
static const unsigned char uni_to[] = {
  0x27, 0x27, 0x22, 0x22, 0x2D, 0x2D, 0x07, 0x2E, 0x45,
  0xC4, 0xB3, 0xDA, 0xBF, 0xC0, 0xD9, 0xC3, 0xB4, 0xC2,
  0xC1, 0xC5, 0xCD, 0xBA, 0xC9, 0xBB, 0xC8, 0xBC, 0xDB,
  0xB0, 0xB1, 0xB2, 0x1A, 0x1B, 0x18, 0x19,
};

static unsigned char* vb;     /* arquivo */
static int vlen;
static int* vline;            /* inicio de cada linha mostrada */
static int vlines;
static int wrap, hex;

/* Um caractere na tela a partir de p; *used = bytes consumidos. */
static int next_char(const unsigned char* p, int n, int* used) {
  int c = p[0];
  unsigned u;
  unsigned i;
  *used = 1;
  if (c < 0x80) return c;
  if ((c & 0xE0) == 0xC0 && n >= 2 && (p[1] & 0xC0) == 0x80) {
    u = ((unsigned)(c & 0x1F) << 6) | (p[1] & 0x3Fu);
    if (u < 0x80) return c;
    *used = 2;
  } else if ((c & 0xF0) == 0xE0 && n >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
    u = ((unsigned)(c & 0x0F) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
    if (u < 0x800) return c;
    *used = 3;
  } else if ((c & 0xF8) == 0xF0 && n >= 4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
             (p[3] & 0xC0) == 0x80) {
    *used = 4;
    return '?';
  } else {
    return c;   /* CP850 cru */
  }
  if (u >= 0xA0 && u <= 0xFF) return latin1_cp850[u - 0xA0];
  if (u == 0xFEFF) return -1;   /* BOM: nao ocupa coluna */
  for (i = 0; i < sizeof(uni_to); i++)
    if (uni_from[i] == u) return uni_to[i];
  return '?';
}

/* Indice das linhas; com wrap quebra na coluna 80. */
static int index_lines(int store) {
  int i = 0, n = 0, col = 0;
  if (store) vline[0] = 0;
  n = 1;
  while (i < vlen) {
    int used, c;
    if (vb[i] == '\n') {
      i++;
      col = 0;
      if (i < vlen) {
        if (store) vline[n] = i;
        n++;
      }
      continue;
    }
    c = next_char(vb + i, vlen - i, &used);
    if (c == '\t') {
      col = (col + 8) & ~7;
    } else if (c != '\r' && c >= 0) {
      col++;
    }
    if (wrap && col > SCR_W) {
      if (store) vline[n] = i;
      n++;
      col = 0;
      continue;
    }
    i += used;
  }
  return n;
}

static int build_index(void) {
  int n;
  if (vline) free(vline);
  vline = 0;
  n = index_lines(0);
  vline = (int*)malloc(sizeof(int) * (size_t)(n + 1));
  if (!vline) return -1;
  vlines = index_lines(1);
  return 0;
}

static void draw_text(int top, int left) {
  int r;
  for (r = 0; r < VIEW_ROWS; r++) {
    int ln = top + r, i, end, col = 0;
    scr_fill(1 + r, 0, 1, SCR_W, ' ', C_VIEW);
    if (ln >= vlines) continue;
    i = vline[ln];
    end = ln + 1 < vlines ? vline[ln + 1] : vlen;
    while (i < end && vb[i] != '\n') {
      int used, c = next_char(vb + i, end - i, &used);
      i += used;
      if (c < 0 || c == '\r') continue;
      if (c == '\t') {
        int to = (col + 8) & ~7;
        while (col < to) col++;
        continue;
      }
      if (col - left >= 0 && col - left < SCR_W) scr_put(1 + r, col - left, c, C_VIEW);
      col++;
    }
  }
}

static void hex_byte(char* o, int v) {
  const char* h = "0123456789ABCDEF";
  o[0] = h[(v >> 4) & 15];
  o[1] = h[v & 15];
}

static void draw_hex(int top) {
  int r, k;
  for (r = 0; r < VIEW_ROWS; r++) {
    int off = (top + r) * 16;
    char line[SCR_W + 1];
    scr_fill(1 + r, 0, 1, SCR_W, ' ', C_VIEW);
    if (off >= vlen) continue;
    sprintf(line, "%08X", off);
    scr_text(1 + r, 0, line, C_HEAD);
    for (k = 0; k < 16 && off + k < vlen; k++) {
      char h[3];
      int c = vb[off + k];
      hex_byte(h, c);
      h[2] = 0;
      scr_text(1 + r, 10 + k * 3 + (k >= 8 ? 1 : 0), h, C_VIEW);
      scr_put(1 + r, 61 + k, c < 32 ? '.' : c, C_DIR);
    }
  }
}

static void draw_bar(const char* path, int top, int total) {
  char t[SCR_W + 1];
  int pct = total > VIEW_ROWS ? (top + VIEW_ROWS) * 100 / total : 100;
  if (pct > 100) pct = 100;
  scr_fill(0, 0, 1, SCR_W, ' ', C_MENU);
  scr_textw(0, 1, path, 44, C_MENU);
  if (hex) sprintf(t, "Desl %08X  %d bytes  %3d%%", top * 16, vlen, pct);
  else sprintf(t, "Linha %d/%d  %d bytes  %3d%%", top + 1, total, vlen, pct);
  scr_text(0, SCR_W - 1 - (int)strlen(t), t, C_MENU);
}

static void draw_keys(void) {
  static const char* lbl[10] = {
    "", "Quebra", "Sair", "", "", "", "", "", "", "Sair"
  };
  int i, x = 0;
  for (i = 0; i < 10; i++) {
    char num[4];
    const char* l = lbl[i];
    if (i == 1 && wrap) l = "Inteir";
    if (i == 3) l = hex ? "Texto" : "Hexa";
    sprintf(num, "%d", i + 1);
    scr_text(24, x, num, C_KEYNUM);
    x += (int)strlen(num);
    scr_textw(24, x, l, 7, C_KEYLBL);
    x += 7;
  }
  while (x < SCR_W) scr_put(24, x++, ' ', C_KEYLBL);
}

void view_file(const char* path) {
  int size = k_fsize(path), top = 0, left = 0, total, k, i;
  if (size < 0) {
    dlg_msg("Ver", "Nao consegui abrir o arquivo.", 1);
    return;
  }
  if (size > VIEW_MAX) size = VIEW_MAX;
  vb = (unsigned char*)malloc((size_t)size + 1);
  if (!vb) {
    dlg_msg("Ver", "Sem memoria para o arquivo.", 1);
    return;
  }
  vlen = size > 0 ? k_fread(path, 0, (char*)vb, size) : 0;
  if (vlen < 0) vlen = 0;
  wrap = 0;
  hex = 0;
  for (i = 0; i < vlen && i < 4096; i++)
    if (vb[i] == 0) hex = 1;
  vline = 0;
  if (build_index() != 0) hex = 1;
  for (;;) {
    int page;
    total = hex ? (vlen + 15) / 16 : vlines;
    if (total < 1) total = 1;
    if (top > total - VIEW_ROWS) top = total - VIEW_ROWS;
    if (top < 0) top = 0;
    if (hex) draw_hex(top);
    else draw_text(top, left);
    draw_bar(path, top, total);
    draw_keys();
    k_cursor(-1, 0);
    k = scr_key();
    page = VIEW_ROWS - 1;
    if (k == K_ESC || k == K_F(3) || k == K_F(10) || k == 'q' || k == 'Q' || k == K_EOF) break;
    if (k == K_UP) top--;
    else if (k == K_DOWN || k == K_ENTER) top++;
    else if (k == K_PGUP || k == 'b') top -= page;
    else if (k == K_PGDN || k == ' ') top += page;
    else if (k == K_HOME) {
      top = 0;
      left = 0;
    } else if (k == K_END) {
      top = total;
    } else if (k == K_LEFT) {
      left -= 8;
      if (left < 0) left = 0;
    } else if (k == K_RIGHT) {
      if (!wrap) left += 8;
    } else if (k == K_F(2) && !hex) {
      int keep = top < vlines ? vline[top] : 0;
      wrap = !wrap;
      left = 0;
      if (build_index() != 0) break;
      for (top = 0; top + 1 < vlines && vline[top + 1] <= keep; top++) {}
    } else if (k == K_F(4)) {
      if (hex) {
        int off = top * 16;
        hex = 0;
        for (top = 0; top + 1 < vlines && vline[top + 1] <= off; top++) {}
      } else {
        top = top < vlines ? vline[top] / 16 : 0;
        hex = 1;
      }
    }
  }
  free(vb);
  if (vline) free(vline);
  vb = 0;
  vline = 0;
  draw_all();
}
