/* blobs.c - regioes conectadas (vizinhanca de 4) da mascara de pele, por
 * preenchimento com pilha propria (sem recursao). */
#include <stdlib.h>
#include <string.h>
#include "gesture.h"

static int* stk;

static void swap_b(gblob* a, gblob* b) {
  gblob t = *a;
  *a = *b;
  *b = t;
}

int blobs_find(const gplane* mask, unsigned short* labels, gblob* out, int max, int min_area) {
  int x, y, n = 0, next = 1, i, j;
  gblob all[256];
  int nall = 0;
  if (!stk) stk = (int*)malloc(sizeof(int) * GN);
  if (!stk) return 0;
  memset(labels, 0, sizeof(unsigned short) * GN);
  for (y = 0; y < GH; y++)
    for (x = 0; x < GW; x++) {
      int sp = 0, lab;
      gblob b;
      long sx = 0, sy = 0;
      if (!mask->p[y * GW + x] || labels[y * GW + x]) continue;
      if (next >= 65535) break;
      lab = next++;
      b.x0 = b.x1 = x;
      b.y0 = b.y1 = y;
      b.area = 0;
      b.label = lab;
      labels[y * GW + x] = (unsigned short)lab;
      stk[sp++] = y * GW + x;
      while (sp) {
        int p = stk[--sp], px = p % GW, py = p / GW;
        b.area++;
        sx += px;
        sy += py;
        if (px < b.x0) b.x0 = px;
        if (px > b.x1) b.x1 = px;
        if (py < b.y0) b.y0 = py;
        if (py > b.y1) b.y1 = py;
        if (px > 0 && mask->p[p - 1] && !labels[p - 1]) {
          labels[p - 1] = (unsigned short)lab;
          stk[sp++] = p - 1;
        }
        if (px < GW - 1 && mask->p[p + 1] && !labels[p + 1]) {
          labels[p + 1] = (unsigned short)lab;
          stk[sp++] = p + 1;
        }
        if (py > 0 && mask->p[p - GW] && !labels[p - GW]) {
          labels[p - GW] = (unsigned short)lab;
          stk[sp++] = p - GW;
        }
        if (py < GH - 1 && mask->p[p + GW] && !labels[p + GW]) {
          labels[p + GW] = (unsigned short)lab;
          stk[sp++] = p + GW;
        }
      }
      if (b.area < min_area) continue;
      b.cx = (int)(sx / b.area);
      b.cy = (int)(sy / b.area);
      if (nall < 256) all[nall++] = b;
    }
  /* as maiores primeiro */
  for (i = 0; i < nall; i++)
    for (j = i + 1; j < nall; j++)
      if (all[j].area > all[i].area) swap_b(&all[i], &all[j]);
  for (i = 0; i < nall && n < max; i++) out[n++] = all[i];
  return n;
}
