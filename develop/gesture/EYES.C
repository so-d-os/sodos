/* eyes.c - olhos dentro do rosto e quanto cada um esta aberto.
 *
 * Referencia de brilho: bochechas e nariz (miolo da metade de baixo do
 * rosto). Escuro = abaixo de 70% disso; muito escuro (pupila, cilios,
 * sombra do olho) = abaixo de 55%. Em cada metade do rosto, na faixa de 20%
 * a 58% da altura, uma janela do tamanho de um olho (22% x 12% da largura do
 * rosto) anda pixel a pixel e fica onde soma mais escuro (peso 2 para muito
 * escuro), pela imagem integral. Abertura = pixels muito escuros na janela:
 * de olho fechado a palpebra (cor de pele) cobre a iris e a conta cai. A
 * referencia de olho aberto (base_l/base_r) acompanha devagar e so sobe ou
 * se mantem com o olho aberto. */
#include "gesture.h"

static int integ[(GW + 1) * (GH + 1)];

static int win_sum(int x0, int y0, int x1, int y1) {   /* [x0,x1) x [y0,y1) */
  return integ[y1 * (GW + 1) + x1] - integ[y0 * (GW + 1) + x1] - integ[y1 * (GW + 1) + x0] +
         integ[y0 * (GW + 1) + x0];
}

static void base_update(int* base, int v) {
  if (*base <= 0) *base = v;
  else if (v * 10 >= *base * 7) *base += (v - *base) / 16 + (v > *base ? 1 : 0);
}

/* ---------- pela cascata Haar (OpenCV haarcascade_eye) ---------- */

static int rel_x[2], rel_y[2], rel_w[2], seen[2]; /* por mil da caixa do rosto */

static int dark_count(const gimage* im, int cx, int cy, int ew, int eh, int very) {
  int x, y, n = 0;
  for (y = cy - eh / 2; y < cy + eh / 2; y++)
    for (x = cx - ew / 2; x < cx + ew / 2; x++)
      if (x >= 0 && y >= 0 && x < GW && y < GH && im->y.p[y * GW + x] < very) n++;
  return n;
}

void eyes_find_haar(gface* f, gstate* s, const gimage* im) {
  int fw = f->x1 - f->x0 + 1, fh = f->y1 - f->y0 + 1, n = 0, i, side, ref, very;
  grect e[8];
  int best[2] = {-1, -1};
  f->eyes = 0;
  f->open_l = f->open_r = -1;
  if (!f->found || fw < 24) {
    seen[0] = seen[1] = 0;
    return;
  }
  if (s->eye_ok)
    n = haar_detect(&s->ceye, im->y.p, GW, f->x0, f->y0 + fh / 6, f->x1 + 1, f->y0 + fh * 6 / 10, fw / 10, fw / 3, 1,
                    e, 8);
  for (i = 0; i < n; i++) {
    int ex = e[i].x + e[i].w / 2;
    side = ex < f->cx ? 0 : 1;
    if (best[side] < 0 || e[i].n > e[best[side]].n) best[side] = i;
  }
  ref = pl_mean(&im->y, f->x0 + fw / 4, f->y0 + fh * 45 / 100, f->x1 - fw / 4, f->y0 + fh * 75 / 100);
  very = ref * 55 / 100;
  for (side = 0; side < 2; side++) {
    int cx, cy, ew, eh, open;
    if (best[side] >= 0) {
      const grect* r = &e[best[side]];
      rel_x[side] = (r->x + r->w / 2 - f->x0) * 1000 / fw;
      rel_y[side] = (r->y + r->h / 2 - f->y0) * 1000 / fh;
      rel_w[side] = r->w * 1000 / fw;
      seen[side] = 45; /* ~1,5 s de memoria sem achar de novo */
    } else if (seen[side] > 0) {
      seen[side]--;
    }
    if (seen[side] <= 0) continue;
    cx = f->x0 + rel_x[side] * fw / 1000;
    cy = f->y0 + rel_y[side] * fh / 1000;
    ew = rel_w[side] * fw / 1000;
    if (ew < 6) ew = 6;
    eh = ew * 6 / 10;
    if (eh < 4) eh = 4;
    open = dark_count(im, cx, cy, ew, eh, very);
    if (side == 0) {
      f->lx = cx;
      f->ly = cy;
      f->open_l = open;
      base_update(&f->base_l, open);
    } else {
      f->rx = cx;
      f->ry = cy;
      f->open_r = open;
      base_update(&f->base_r, open);
    }
    f->eyes++;
  }
}

void eyes_find(gface* f, const gimage* im, const gplane* mask) {
  int fw = f->x1 - f->x0 + 1, fh = f->y1 - f->y0 + 1, ref, thr, very, ew, eh, sy0, sy1, x, y, half;
  int ex[2], ey[2], ok[2], open[2];
  (void)mask;
  f->eyes = 0;
  if (!f->found || fw < 24 || fh < 24) return;
  ref = pl_mean(&im->y, f->x0 + fw / 4, f->y0 + fh * 45 / 100, f->x1 - fw / 4, f->y0 + fh * 75 / 100);
  thr = ref * 70 / 100;
  very = ref * 55 / 100;
  ew = fw * 22 / 100;
  eh = fw * 12 / 100;
  if (ew < 5) ew = 5;
  if (eh < 3) eh = 3;
  sy0 = f->y0 + fh * 20 / 100;
  sy1 = f->y0 + fh * 58 / 100;
  if (sy1 > GH) sy1 = GH;
  /* integral do peso de escuro (0, 1 ou 2) */
  for (x = 0; x <= GW; x++) integ[x] = 0;
  for (y = 0; y < GH; y++) {
    int row = 0;
    integ[(y + 1) * (GW + 1)] = 0;
    for (x = 0; x < GW; x++) {
      int v = im->y.p[y * GW + x], wgt = v < very ? 2 : v < thr ? 1 : 0;
      row += wgt;
      integ[(y + 1) * (GW + 1) + x + 1] = integ[y * (GW + 1) + x + 1] + row;
    }
  }
  for (half = 0; half < 2; half++) {
    int hx0 = half == 0 ? f->x0 + fw * 6 / 100 : f->cx, hx1 = half == 0 ? f->cx : f->x1 - fw * 6 / 100;
    int best = -1, bx = 0, by = 0;
    if (hx0 < 0) hx0 = 0;
    if (hx1 > GW) hx1 = GW;
    for (y = sy0; y + eh <= sy1; y++)
      for (x = hx0; x + ew <= hx1; x++) {
        int s = win_sum(x, y, x + ew, y + eh);
        if (s > best) {
          best = s;
          bx = x;
          by = y;
        }
      }
    ok[half] = best >= ew * eh * 6 / 100;
    ex[half] = bx + ew / 2;
    ey[half] = by + eh / 2;
    open[half] = 0;
    if (ok[half]) {
      int yy, xx;
      for (yy = by; yy < by + eh; yy++)
        for (xx = bx; xx < bx + ew; xx++)
          if (im->y.p[yy * GW + xx] < very) open[half]++;
    }
  }
  /* par coerente: alturas parecidas e distancia de olhos */
  if (ok[0] && ok[1]) {
    int dy = ey[1] - ey[0], dx = ex[1] - ex[0];
    if (dy < 0) dy = -dy;
    if (dy > fw / 4 || dx < fw / 5 || dx > fw * 3 / 4) {
      /* fica com o mais escuro */
      if (open[0] >= open[1]) ok[1] = 0;
      else ok[0] = 0;
    }
  }
  f->lx = ex[0];
  f->ly = ey[0];
  f->rx = ex[1];
  f->ry = ey[1];
  f->open_l = ok[0] ? open[0] : -1;
  f->open_r = ok[1] ? open[1] : -1;
  f->eyes = ok[0] + ok[1];
  if (ok[0]) base_update(&f->base_l, open[0]);
  if (ok[1]) base_update(&f->base_r, open[1]);
}
