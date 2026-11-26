/* image.c - planos de 8 bits: alocar, borrar, morfologia, contas. */
#include <stdlib.h>
#include <string.h>
#include "gesture.h"

int pl_alloc(gplane* p, int w, int h) {
  p->w = w;
  p->h = h;
  p->p = (unsigned char*)malloc((size_t)w * h);
  if (!p->p) return -1;
  memset(p->p, 0, (size_t)w * h);
  return 0;
}

void pl_free(gplane* p) {
  free(p->p);
  p->p = 0;
}

void pl_copy(gplane* d, const gplane* s) { memcpy(d->p, s->p, (size_t)s->w * s->h); }

/* Media 3x3 (as bordas copiam a origem). */
void pl_blur3(gplane* d, const gplane* s) {
  int x, y, w = s->w, h = s->h;
  memcpy(d->p, s->p, (size_t)w * h);
  for (y = 1; y < h - 1; y++) {
    const unsigned char* a = s->p + (y - 1) * w;
    const unsigned char* b = a + w;
    const unsigned char* c = b + w;
    unsigned char* o = d->p + y * w;
    for (x = 1; x < w - 1; x++) {
      int v = a[x - 1] + a[x] + a[x + 1] + b[x - 1] + b[x] + b[x + 1] + c[x - 1] + c[x] + c[x + 1];
      o[x] = (unsigned char)(v / 9);
    }
  }
}

/* Mascara binaria (0/255): erosao = so fica se os 8 vizinhos sao 255. */
void pl_erode(gplane* d, const gplane* s) {
  int x, y, w = s->w, h = s->h;
  memset(d->p, 0, (size_t)w * h);
  for (y = 1; y < h - 1; y++) {
    const unsigned char* a = s->p + (y - 1) * w;
    const unsigned char* b = a + w;
    const unsigned char* c = b + w;
    unsigned char* o = d->p + y * w;
    for (x = 1; x < w - 1; x++)
      o[x] = (a[x - 1] & a[x] & a[x + 1] & b[x - 1] & b[x] & b[x + 1] & c[x - 1] & c[x] & c[x + 1]);
  }
}

void pl_dilate(gplane* d, const gplane* s) {
  int x, y, w = s->w, h = s->h;
  memset(d->p, 0, (size_t)w * h);
  for (y = 1; y < h - 1; y++) {
    const unsigned char* a = s->p + (y - 1) * w;
    const unsigned char* b = a + w;
    const unsigned char* c = b + w;
    unsigned char* o = d->p + y * w;
    for (x = 1; x < w - 1; x++)
      o[x] = (a[x - 1] | a[x] | a[x + 1] | b[x - 1] | b[x] | b[x + 1] | c[x - 1] | c[x] | c[x + 1]);
  }
}

/* Abre (tira pontos soltos) e fecha (tapa buracos pequenos). */
void pl_open_close(gplane* m, gplane* tmp) {
  pl_erode(tmp, m);
  pl_dilate(m, tmp);
  pl_dilate(tmp, m);
  pl_erode(m, tmp);
}

static void clip(const gplane* p, int* x0, int* y0, int* x1, int* y1) {
  if (*x0 < 0) *x0 = 0;
  if (*y0 < 0) *y0 = 0;
  if (*x1 >= p->w) *x1 = p->w - 1;
  if (*y1 >= p->h) *y1 = p->h - 1;
}

/* Media do retangulo (x1, y1 inclusive). */
int pl_mean(const gplane* p, int x0, int y0, int x1, int y1) {
  long s = 0, n = 0;
  int x, y;
  clip(p, &x0, &y0, &x1, &y1);
  for (y = y0; y <= y1; y++)
    for (x = x0; x <= x1; x++) {
      s += p->p[y * p->w + x];
      n++;
    }
  return n ? (int)(s / n) : 0;
}

/* Quantos pixels do retangulo valem pelo menos min. */
int pl_count(const gplane* p, int x0, int y0, int x1, int y1, int min) {
  int n = 0, x, y;
  clip(p, &x0, &y0, &x1, &y1);
  for (y = y0; y <= y1; y++)
    for (x = x0; x <= x1; x++)
      if (p->p[y * p->w + x] >= min) n++;
  return n;
}

int pl_absdiff_count(const gplane* a, const gplane* b, int thr) {
  int i, n = 0, N = a->w * a->h;
  for (i = 0; i < N; i++) {
    int d = a->p[i] - b->p[i];
    if (d > thr || d < -thr) n++;
  }
  return n;
}
