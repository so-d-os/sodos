/*
 * text.c - Fonte da interface dos programas Windows.
 *
 * Usa o mesmo rasterizador TrueType do WIN (video/wfont.c, compilado aqui com
 * WFONT_LAST=255 para ter o Latin-1) e o mesmo arquivo de fonte do desktop
 * (\SODOS\FONTS\INTER.TTF ou NOTOSANS.TTF). Os glifos sao desenhados uma vez,
 * no primeiro texto, numa area estatica do DLL: valem para todos os processos
 * enquanto o WINE.DLL esta carregado e somem com ele (SET WINE=0).
 */
#include <so.h>
#include <string.h>
#include "gui_int.h"
#include "wfont.h"

#define UI_PX 12                      /* ~9 pt a 96 dpi, como o Segoe UI */
#define ARENA (448u * 1024u)

static uint8_t arena[ARENA] __attribute__((aligned(16)));
static uint32_t arena_used;
static volatile int font_state;       /* 0 nao tentou, 1 pronta, -1 sem fonte */
static volatile int font_lock;

static void* arena_alloc(uint32_t n) {
  void* p;
  n = (n + 15u) & ~15u;
  if (arena_used + n > ARENA) return 0;
  p = arena + arena_used;
  arena_used += n;
  return p;
}
static void arena_release(void* p) { (void)p; }

static int try_font(const char* path) {
  int sz = k_fsize(path), rc;
  uint8_t* buf;
  if (sz < 1024 || sz > 8 * 1024 * 1024) return 0;
  buf = (uint8_t*)k_malloc((unsigned)sz + 1u);
  if (!buf) return 0;
  if (k_fload(path, (char*)buf, sz + 1) != sz) {
    k_free(buf);
    return 0;
  }
  arena_used = 0;
  rc = wfont_build(buf, sz, UI_PX, arena_alloc, arena_release);
  k_free(buf);
  if (rc == 0) wlog("fonte da interface: ", path);
  return rc == 0;
}

int text_ready(void) {
  static const char* const names[] = {"INTER.TTF", "NOTOSANS.TTF", "ROBOTO.TTF"};
  static const char drives[] = "DUZC";
  char dir[160], path[200];
  int i, d;
  if (font_state) return font_state > 0;
  while (__sync_lock_test_and_set(&font_lock, 1)) k_thread_yield();
  if (!font_state) {
    int ok = 0;
    if (k_getenv("PATHSODOS", dir, (int)sizeof(dir)) > 0) {
      for (i = 0; i < 3 && !ok; i++) {
        strcpy(path, dir);
        strcat(path, "\\FONTS\\");
        strcat(path, names[i]);
        ok = try_font(path);
      }
    }
    for (d = 0; drives[d] && !ok; d++)
      for (i = 0; i < 3 && !ok; i++) {
        strcpy(path, "D:\\SODOS\\FONTS\\");
        path[0] = drives[d];
        strcat(path, names[i]);
        ok = try_font(path);
      }
    if (!ok) wlog("sem fonte TrueType em \\SODOS\\FONTS: janelas do Windows sem texto", 0);
    font_state = ok ? 1 : -1;
  }
  __sync_lock_release(&font_lock);
  return font_state > 0;
}

int text_height(void) { return text_ready() ? wfont_height() + 1 : 16; }
int text_ascent(void) { return text_ready() ? wfont_ascent() : 12; }

/* UTF-16 -> codigo do glifo (Latin-1; alguns sinais tipograficos). */
static unsigned char glyph_code(wchar16 c) {
  if (c >= 32 && c < 127) return (unsigned char)c;
  if (c >= 0xA0 && c <= 0xFF) return (unsigned char)c;
  switch (c) {
    case 0x2018: case 0x2019: case 0x201A: return '\'';
    case 0x201C: case 0x201D: case 0x201E: return '"';
    case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2022: return 0xB7;
    case 0x2026: return '.';
    case 0x00A0: return ' ';
    case 9: return ' ';
  }
  return '?';
}

static int glyph_adv(unsigned char c) {
  const struct wfont_glyph* g = wfont_glyph(c);
  return g ? g->adv : 0;
}

int text_width(const wchar16* s, int n) {
  int i;
  int32_t pen = 0;
  if (!text_ready() || !s) return 0;
  if (n < 0) n = (int)wstrlen16(s);
  for (i = 0; i < n; i++) pen += glyph_adv(glyph_code(s[i]));
  return (pen + 255) >> 8;
}

static void blend(struct surf* s, int x, int y, DWORD c, int a) {
  uint8_t* p;
  int r = (int)(c & 0xFF), g = (int)((c >> 8) & 0xFF), b = (int)((c >> 16) & 0xFF);
  p = s->px + (size_t)(s->bottom_up ? s->h - 1 - y : y) * (size_t)s->w * 4u + (size_t)x * 4u;
  if (!s->rgba) {
    int t = r;
    r = b;
    b = t;
  }
  p[0] = (uint8_t)(p[0] + (((r - p[0]) * a) >> 8));
  p[1] = (uint8_t)(p[1] + (((g - p[1]) * a) >> 8));
  p[2] = (uint8_t)(p[2] + (((b - p[2]) * a) >> 8));
  p[3] = 255;
}

int text_draw(struct surf* s, const struct rect* clip, int x, int y, const wchar16* str, int n, DWORD color,
              int bold) {
  int i, cl, ct, cr, cb, asc;
  int32_t pen;
  if (!text_ready() || !s || !s->px || !str) return 0;
  if (n < 0) n = (int)wstrlen16(str);
  cl = clip ? clip->left : 0;
  ct = clip ? clip->top : 0;
  cr = clip ? clip->right : s->w;
  cb = clip ? clip->bottom : s->h;
  if (cl < 0) cl = 0;
  if (ct < 0) ct = 0;
  if (cr > s->w) cr = s->w;
  if (cb > s->h) cb = s->h;
  asc = wfont_ascent();
  pen = (int32_t)x << 8;
  for (i = 0; i < n; i++) {
    const struct wfont_glyph* g = wfont_glyph(glyph_code(str[i]));
    int gx, gy, pass;
    if (!g) continue;
    for (pass = 0; pass <= (bold ? 1 : 0); pass++) {
      int x0 = (pen >> 8) + g->left + pass, y0 = y + asc - g->top;
      for (gy = 0; gy < g->h; gy++) {
        int yy = y0 + gy;
        if (yy < ct || yy >= cb) continue;
        for (gx = 0; gx < g->w; gx++) {
          int xx = x0 + gx, a = g->a[gy * g->w + gx];
          if (!a || xx < cl || xx >= cr) continue;
          blend(s, xx, yy, color, a + (a >> 7));
        }
      }
    }
    pen += g->adv + (bold ? 256 : 0);
  }
  return ((pen + 255) >> 8) - x;
}

/* Tira o '&' de atalho ("&Install" -> "Install"; "&&" -> "&"). */
static int strip_prefix(const wchar16* s, int n, wchar16* out, int max) {
  int i, o = 0;
  for (i = 0; i < n && o < max; i++) {
    if (s[i] == '&') {
      if (i + 1 < n && s[i + 1] == '&') i++;
      else continue;
    }
    out[o++] = s[i];
  }
  return o;
}

int text_box(struct surf* s, const struct rect* clip, const struct rect* r, const wchar16* str0, int n, DWORD color,
             int bold, DWORD fmt) {
  wchar16 buf[2048];
  const wchar16* str = str0;
  int lh = text_height(), y, start, total = 0, lines = 0, pass;
  int width = r->right - r->left;
  if (!str0) return 0;
  if (n < 0) n = (int)wstrlen16(str0);
  if (!(fmt & DT_NOPREFIX)) {
    n = strip_prefix(str0, n, buf, (int)(sizeof(buf) / 2));
    str = buf;
  } else if (n > (int)(sizeof(buf) / 2)) {
    n = (int)(sizeof(buf) / 2);
  }
  if (fmt & DT_SINGLELINE) {
    int w = text_width(str, n), x = r->left;
    if (fmt & DT_CALCRECT) return lh;
    y = r->top;
    if (fmt & DT_VCENTER) y = r->top + (r->bottom - r->top - lh) / 2;
    else if (fmt & DT_BOTTOM) y = r->bottom - lh;
    if (fmt & DT_CENTER) x = r->left + (width - w) / 2;
    else if (fmt & DT_RIGHT) x = r->right - w;
    if (s) {
      struct rect c = *r;
      if (clip) {
        if (c.left < clip->left) c.left = clip->left;
        if (c.top < clip->top) c.top = clip->top;
        if (c.right > clip->right) c.right = clip->right;
        if (c.bottom > clip->bottom) c.bottom = clip->bottom;
      }
      text_draw(s, &c, x, y, str, n, color, bold);
    }
    return lh;
  }
  /* duas passadas: conta as linhas (para centralizar) e desenha */
  for (pass = 0; pass < 2; pass++) {
    int i = 0;
    y = r->top;
    if (pass == 1) {
      if (fmt & DT_CALCRECT || !s) break;
      if (fmt & DT_VCENTER) y = r->top + (r->bottom - r->top - total) / 2;
    }
    lines = 0;
    while (i <= n) {
      int end, lw, best = -1;
      start = i;
      /* acha o fim da linha: '\n' ou a ultima palavra que cabe */
      for (end = i; end < n && str[end] != '\n'; end++) {
        if ((fmt & DT_WORDBREAK) && (str[end] == ' ' || str[end] == '\t')) {
          if (text_width(str + start, end - start) > width && best >= 0) break;
          best = end;
        }
      }
      lw = end;
      if ((fmt & DT_WORDBREAK) && end > start && text_width(str + start, end - start) > width && best > start)
        lw = best;
      if (pass == 1) {
        int w, x = r->left, len = lw - start;
        if (len > 0 && str[start + len - 1] == '\r') len--;
        w = text_width(str + start, len);
        if (fmt & DT_CENTER) x = r->left + (width - w) / 2;
        else if (fmt & DT_RIGHT) x = r->right - w;
        {
          struct rect c = *r;
          if (clip) {
            if (c.left < clip->left) c.left = clip->left;
            if (c.top < clip->top) c.top = clip->top;
            if (c.right > clip->right) c.right = clip->right;
            if (c.bottom > clip->bottom) c.bottom = clip->bottom;
          }
          text_draw(s, &c, x, y, str + start, len, color, bold);
        }
      }
      y += lh;
      lines++;
      i = lw;
      if (i < n && (str[i] == '\n' || str[i] == ' ' || str[i] == '\t')) i++;
      if (i >= n) break;
    }
    total = lines * lh;
  }
  return total;
}
