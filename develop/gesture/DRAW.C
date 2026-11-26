/* draw.c - marcas sobre a imagem RGBA (w x h). As coordenadas chegam no
 * espaco de analise (GW x GH) e sao escaladas para w x h aqui. */
#include "gesture.h"

static void put(unsigned char* rgba, int w, int h, int x, int y, unsigned rgb) {
  unsigned char* p;
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  p = rgba + ((long)y * w + x) * 4;
  p[0] = (unsigned char)(rgb >> 16);
  p[1] = (unsigned char)(rgb >> 8);
  p[2] = (unsigned char)rgb;
}

#define SX(v) ((v) * w / GW)
#define SY(v) ((v) * h / GH)

void draw_rect(unsigned char* rgba, int w, int h, int x0, int y0, int x1, int y1, unsigned rgb) {
  int a = SX(x0), b = SY(y0), c = SX(x1 + 1) - 1, d = SY(y1 + 1) - 1, x, y, k;
  for (k = 0; k < 2; k++) {
    for (x = a; x <= c; x++) {
      put(rgba, w, h, x, b + k, rgb);
      put(rgba, w, h, x, d - k, rgb);
    }
    for (y = b; y <= d; y++) {
      put(rgba, w, h, a + k, y, rgb);
      put(rgba, w, h, c - k, y, rgb);
    }
  }
}

void draw_cross(unsigned char* rgba, int w, int h, int x, int y, int r, unsigned rgb) {
  int cx = SX(x), cy = SY(y), R = SX(r), k;
  for (k = -R; k <= R; k++) {
    put(rgba, w, h, cx + k, cy, rgb);
    put(rgba, w, h, cx, cy + k, rgb);
    put(rgba, w, h, cx + k, cy + 1, rgb);
    put(rgba, w, h, cx + 1, cy + k, rgb);
  }
}

void draw_line(unsigned char* rgba, int w, int h, int x0, int y0, int x1, int y1, unsigned rgb) {
  int a = SX(x0), b = SY(y0), c = SX(x1), d = SY(y1), dx = c - a, dy = d - b, n, i;
  n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
  if (n < 1) n = 1;
  for (i = 0; i <= n; i++) {
    put(rgba, w, h, a + dx * i / n, b + dy * i / n, rgb);
    put(rgba, w, h, a + dx * i / n, b + dy * i / n + 1, rgb);
  }
}

/* Pinta (meio transparente) os pixels da mascara. */
void draw_mask(unsigned char* rgba, int w, int h, const gplane* mask, unsigned rgb) {
  int x, y;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      if (mask->p[(y * GH / h) * GW + x * GW / w]) {
        unsigned char* p = rgba + ((long)y * w + x) * 4;
        p[0] = (unsigned char)((p[0] + ((rgb >> 16) & 255)) / 2);
        p[1] = (unsigned char)((p[1] + ((rgb >> 8) & 255)) / 2);
        p[2] = (unsigned char)((p[2] + (rgb & 255)) / 2);
      }
}

/* So o que os marcadores ligados pedem (detect = D_*). Mao: verde-claro =
 * espalmada, azul = fechada, laranja/vermelho = outra (levantada/abaixada). */
void draw_analysis(unsigned char* rgba, int w, int h, const gface* f, const ghand* hd, int nh,
                   const gplane* mask, int show_mask, int detect) {
  int i;
  if (show_mask && mask) draw_mask(rgba, w, h, mask, 0xFF00FF);
  if (f->found) {
    if (detect & (D_FACE | D_HEAD)) draw_rect(rgba, w, h, f->x0, f->y0, f->x1, f->y1, 0x00FF00);
    if (detect & (D_EYES | D_HEAD)) {
      if (f->open_l >= 0) draw_cross(rgba, w, h, f->lx, f->ly, 3, 0x00FFFF);
      if (f->open_r >= 0) draw_cross(rgba, w, h, f->rx, f->ry, 3, 0x00FFFF);
    }
    if ((detect & D_HEAD) && f->eyes == 2) draw_line(rgba, w, h, f->lx, f->ly, f->rx, f->ry, 0x00A0FF);
    /* seta do yaw saindo do centro do rosto */
    if (detect & D_HEAD)
      draw_line(rgba, w, h, f->cx, f->cy, f->cx + f->yaw * (f->x1 - f->x0) / 250,
                f->cy + f->pitch * (f->y1 - f->y0) / 250, 0xFFFF00);
  }
  for (i = 0; i < nh; i++)
    if (hd[i].found) {
      unsigned c = hd[i].raised ? 0xFF8000 : 0xFF4040;
      if ((detect & D_OPEN) && hd[i].fingers >= 4) c = 0x80FF80;
      else if ((detect & D_FIST) && hd[i].fingers == 0) c = 0x4080FF;
      draw_rect(rgba, w, h, hd[i].x0, hd[i].y0, hd[i].x1, hd[i].y1, c);
      draw_cross(rgba, w, h, hd[i].cx, hd[i].cy, 4, 0xFFFFFF);
    }
}
