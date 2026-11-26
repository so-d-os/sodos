/* haar.c - detector de cascatas Haar (Viola-Jones), com os dados treinados
 * da OpenCV (develop\gesture\opencv\*.xml, convertidos por
 * tools\cascade2bin.py para .CAS).
 *
 * Mesma conta da OpenCV 2.4+: a janela fica no tamanho do treino (24x24 ou
 * 20x20) e a IMAGEM e reduzida em piramide (fator 1,1). Para cada janela:
 * nf = sqrt(area * soma_quadrados - soma^2) no retangulo interno (1 pixel de
 * borda), e cada no compara (soma ponderada dos retangulos) / nf com o
 * limiar. Estagio passa se a soma das folhas >= limiar do estagio; todos os
 * estagios passam = objeto. Depois junta as janelas parecidas
 * (groupRectangles, eps 0,2) e so fica com grupos de mais de min_nb.
 * C portavel: compila no cc do SODOS e no gcc (teste na VM). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "haar.h"

static int rd_i(const unsigned char** p) {
  int v = (int)((*p)[0] | ((*p)[1] << 8) | ((*p)[2] << 16) | ((unsigned)(*p)[3] << 24));
  *p += 4;
  return v;
}

static float rd_f(const unsigned char** p) {
  union {
    unsigned u;
    float f;
  } c;
  c.u = (unsigned)((*p)[0] | ((*p)[1] << 8) | ((*p)[2] << 16) | ((unsigned)(*p)[3] << 24));
  *p += 4;
  return c.f;
}

int haar_load(gcascade* c, const char* path) {
  FILE* f = fopen(path, "rb");
  long n;
  unsigned char* buf;
  const unsigned char* p;
  int i, k, nw = 0, nn = 0, nl = 0, s;
  memset(c, 0, sizeof(*c));
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = (unsigned char*)malloc((size_t)n);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    free(buf);
    return -1;
  }
  fclose(f);
  p = buf;
  if (memcmp(p, "CAS1", 4) != 0) {
    free(buf);
    return -2;
  }
  p += 4;
  c->w = rd_i(&p);
  c->h = rd_i(&p);
  c->nst = rd_i(&p);
  c->nf = rd_i(&p);
  c->f = (hfeat*)malloc(sizeof(hfeat) * (size_t)c->nf);
  c->st = (hstage*)malloc(sizeof(hstage) * (size_t)c->nst);
  if (!c->f || !c->st) return -3;
  for (i = 0; i < c->nf; i++) {
    c->f[i].nr = rd_i(&p);
    for (k = 0; k < 3; k++) {
      c->f[i].x[k] = rd_i(&p);
      c->f[i].y[k] = rd_i(&p);
      c->f[i].w[k] = rd_i(&p);
      c->f[i].h[k] = rd_i(&p);
      c->f[i].wt[k] = rd_f(&p);
    }
  }
  /* primeira passada: conta fracos, nos e folhas */
  {
    const unsigned char* q = p;
    for (s = 0; s < c->nst; s++) {
      int w = rd_i(&q);
      (void)rd_f(&q);
      for (k = 0; k < w; k++) {
        int a = rd_i(&q), b;
        q += 16 * a;
        nn += a;
        b = rd_i(&q);
        q += 4 * b;
        nl += b;
        nw++;
      }
    }
  }
  c->wk = (hweak*)malloc(sizeof(hweak) * (size_t)nw);
  c->nd = (hnode*)malloc(sizeof(hnode) * (size_t)nn);
  c->lv = (float*)malloc(sizeof(float) * (size_t)nl);
  if (!c->wk || !c->nd || !c->lv) return -3;
  nw = nn = nl = 0;
  for (s = 0; s < c->nst; s++) {
    c->st[s].nweak = rd_i(&p);
    c->st[s].thr = rd_f(&p);
    c->st[s].weak0 = nw;
    for (k = 0; k < c->st[s].nweak; k++) {
      hweak* w = &c->wk[nw++];
      int j;
      w->nn = rd_i(&p);
      w->node0 = nn;
      for (j = 0; j < w->nn; j++) {
        c->nd[nn].left = rd_i(&p);
        c->nd[nn].right = rd_i(&p);
        c->nd[nn].feat = rd_i(&p);
        c->nd[nn].thr = rd_f(&p);
        nn++;
      }
      w->nleaf = rd_i(&p);
      w->leaf0 = nl;
      for (j = 0; j < w->nleaf; j++) c->lv[nl++] = rd_f(&p);
    }
  }
  free(buf);
  return 0;
}

/* ---------- piramide e integrais (buffers reutilizados) ---------- */
static unsigned char* sm;      /* imagem reduzida */
static int* ii;                /* integral */
static double* ii2;            /* integral dos quadrados */
static int cap;

static int ensure(int w, int h) {
  int need = (w + 1) * (h + 1);
  if (need <= cap) return 0;
  free(sm);
  free(ii);
  free(ii2);
  sm = (unsigned char*)malloc((size_t)need);
  ii = (int*)malloc(sizeof(int) * (size_t)need);
  ii2 = (double*)malloc(sizeof(double) * (size_t)need);
  if (!sm || !ii || !ii2) {
    cap = 0;
    return -1;
  }
  cap = need;
  return 0;
}

/* Reduz [x0,x1)x[y0,y1) de src para sw x sh (bilinear). */
static void shrink(const unsigned char* src, int stride, int x0, int y0, int rw, int rh, int sw, int sh) {
  int x, y;
  for (y = 0; y < sh; y++) {
    float fy = ((float)y + 0.5f) * rh / sh - 0.5f;
    int iy = (int)fy;
    float ay;
    if (fy < 0) fy = 0, iy = 0;
    if (iy >= rh - 1) iy = rh - 2 < 0 ? 0 : rh - 2;
    ay = fy - iy;
    if (ay > 1) ay = 1;
    for (x = 0; x < sw; x++) {
      float fx = ((float)x + 0.5f) * rw / sw - 0.5f;
      int ix = (int)fx;
      float ax;
      const unsigned char* p;
      float v;
      if (fx < 0) fx = 0, ix = 0;
      if (ix >= rw - 1) ix = rw - 2 < 0 ? 0 : rw - 2;
      ax = fx - ix;
      if (ax > 1) ax = 1;
      p = src + (y0 + iy) * stride + x0 + ix;
      if (rw < 2 || rh < 2) v = p[0];
      else
        v = (p[0] * (1 - ax) + p[1] * ax) * (1 - ay) + (p[stride] * (1 - ax) + p[stride + 1] * ax) * ay;
      sm[y * sw + x] = (unsigned char)(v + 0.5f);
    }
  }
}

static void integrate(int sw, int sh) {
  int x, y, W = sw + 1;
  for (x = 0; x <= sw; x++) {
    ii[x] = 0;
    ii2[x] = 0;
  }
  for (y = 0; y < sh; y++) {
    int row = 0;
    double row2 = 0;
    ii[(y + 1) * W] = 0;
    ii2[(y + 1) * W] = 0;
    for (x = 0; x < sw; x++) {
      int v = sm[y * sw + x];
      row += v;
      row2 += (double)(v * v);
      ii[(y + 1) * W + x + 1] = ii[y * W + x + 1] + row;
      ii2[(y + 1) * W + x + 1] = ii2[y * W + x + 1] + row2;
    }
  }
}

#define RS(I, W, x, y, w, h) ((I)[((y) + (h)) * (W) + (x) + (w)] - (I)[(y) * (W) + (x) + (w)] - (I)[((y) + (h)) * (W) + (x)] + (I)[(y) * (W) + (x)])

/* 1 = a janela (x, y) da imagem reduzida passa todos os estagios. */
static int eval(const gcascade* c, int W, int x, int y) {
  int s, k, area = (c->w - 2) * (c->h - 2);
  double sum = RS(ii, W, x + 1, y + 1, c->w - 2, c->h - 2);
  double sq = RS(ii2, W, x + 1, y + 1, c->w - 2, c->h - 2);
  double nf = area * sq - sum * sum;
  float inv = (float)(nf > 0 ? 1.0 / sqrt(nf) : 1.0);
  for (s = 0; s < c->nst; s++) {
    const hstage* st = &c->st[s];
    float acc = 0;
    for (k = 0; k < st->nweak; k++) {
      const hweak* w = &c->wk[st->weak0 + k];
      int idx = 0;
      for (;;) {
        const hnode* nd = &c->nd[w->node0 + idx];
        const hfeat* f = &c->f[nd->feat];
        float v = f->wt[0] * RS(ii, W, x + f->x[0], y + f->y[0], f->w[0], f->h[0]) +
                  f->wt[1] * RS(ii, W, x + f->x[1], y + f->y[1], f->w[1], f->h[1]);
        if (f->nr > 2) v += f->wt[2] * RS(ii, W, x + f->x[2], y + f->y[2], f->w[2], f->h[2]);
        idx = v * inv < nd->thr ? nd->left : nd->right;
        if (idx <= 0) {
          acc += c->lv[w->leaf0 - idx];
          break;
        }
      }
    }
    if (acc < st->thr) return 0;
  }
  return 1;
}

static int similar(const grect* a, const grect* b) {
  float d = 0.2f * ((a->w < b->w ? a->w : b->w) + (a->h < b->h ? a->h : b->h)) * 0.5f;
  return abs(a->x - b->x) <= d && abs(a->y - b->y) <= d && abs(a->x + a->w - b->x - b->w) <= d &&
         abs(a->y + a->h - b->y - b->h) <= d;
}

#define RAW_MAX 1024
static grect raw[RAW_MAX];
static int lab[RAW_MAX];

static int find_root(int i) {
  while (lab[i] != i) i = lab[i] = lab[lab[i]];
  return i;
}

int haar_detect(const gcascade* c, const unsigned char* img, int stride, int x0, int y0, int x1, int y1,
                int min_size, int max_size, int min_nb, grect* out, int max) {
  int rw = x1 - x0, rh = y1 - y0, nraw = 0, i, j, n = 0;
  float s;
  if (!c->nst || rw < 4 || rh < 4) return 0;
  /* min_size menor que a janela do treino: a regiao e ampliada (escala < 1),
   * para olhos pequenos dentro do rosto; no maximo 2x. */
  if (min_size < c->w / 2) min_size = c->w / 2;
  for (s = (float)min_size / c->w; c->w * s <= max_size && rw / s >= c->w && rh / s >= c->h; s *= 1.1f) {
    int sw = (int)(rw / s), sh = (int)(rh / s), x, y, step = s > 2 ? 1 : 2;
    if (ensure(sw, sh)) break;
    shrink(img, stride, x0, y0, rw, rh, sw, sh);
    integrate(sw, sh);
    for (y = 0; y + c->h <= sh; y += step)
      for (x = 0; x + c->w <= sw; x += step)
        if (nraw < RAW_MAX && eval(c, sw + 1, x, y)) {
          raw[nraw].x = x0 + (int)(x * s + 0.5f);
          raw[nraw].y = y0 + (int)(y * s + 0.5f);
          raw[nraw].w = (int)(c->w * s + 0.5f);
          raw[nraw].h = (int)(c->h * s + 0.5f);
          raw[nraw].n = 1;
          nraw++;
        }
  }
  /* junta parecidas */
  for (i = 0; i < nraw; i++) lab[i] = i;
  for (i = 0; i < nraw; i++)
    for (j = i + 1; j < nraw; j++)
      if (similar(&raw[i], &raw[j])) lab[find_root(i)] = find_root(j);
  for (i = 0; i < nraw; i++) {
    int r = find_root(i), k, cnt = 0;
    long sx = 0, sy = 0, swd = 0, sht = 0;
    if (r != i) continue;
    for (k = 0; k < nraw; k++)
      if (find_root(k) == r) {
        sx += raw[k].x;
        sy += raw[k].y;
        swd += raw[k].w;
        sht += raw[k].h;
        cnt++;
      }
    if (cnt <= min_nb || n >= max) continue;
    out[n].x = (int)(sx / cnt);
    out[n].y = (int)(sy / cnt);
    out[n].w = (int)(swd / cnt);
    out[n].h = (int)(sht / cnt);
    out[n].n = cnt;
    n++;
  }
  return n;
}
