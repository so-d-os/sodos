/* capture.c - quadros da webcam (k_cam_*, kernel build 79+) e de arquivos
 * para os planos de analise Y, Cb, Cr (GW x GH).
 *
 * A camera manda YUY2 (Y0 U Y1 V): Y, Cb e Cr ja vem prontos, so reduz para
 * GW x GH. Arquivos chegam em RGBA (IMG.DLL) e passam pela conversao
 * BT.601. */
#include <stdlib.h>
#include <string.h>
#include <so.h>
#include "gesture.h"

/* Os nomes PRECISAM comecar com k_: o cc do SODOS so passa os argumentos em
 * registradores (a convencao do kernel) para chamadas a nomes k_*. Com
 * "kc_modes" o kernel lia o buffer do RDI com lixo e escrevia a lista de modos
 * dentro do proprio kernel: tres quedas em 26/11/2026. */
static int (*k_cam_modes)(unsigned int*, int);
static int (*k_cam_open)(int, int, int, int*);
static int (*k_cam_frame)(void*, int, int*, int);
static int (*k_cam_close)(void);
static unsigned char* raw;
static int raw_max, cw, ch, seq, on;
static const char* err = "";

int img_alloc_g(gimage* im) {
  memset(im, 0, sizeof(*im));
  if (pl_alloc(&im->y, GW, GH) || pl_alloc(&im->cb, GW, GH) || pl_alloc(&im->cr, GW, GH)) return -1;
  return 0;
}

void img_free_g(gimage* im) {
  pl_free(&im->y);
  pl_free(&im->cb);
  pl_free(&im->cr);
}

const char* cam_error(void) { return err; }
int cam_w(void) { return cw; }
int cam_h(void) { return ch; }

int cam_start(int w, int h) {
  unsigned int m[4 * 32];
  int n, i, ok = 0, r;
  k_cam_modes = (int (*)(unsigned int*, int))k_dl_sym(0, "k_cam_modes", 0);
  k_cam_open = (int (*)(int, int, int, int*))k_dl_sym(0, "k_cam_open", 0);
  k_cam_frame = (int (*)(void*, int, int*, int))k_dl_sym(0, "k_cam_frame", 0);
  k_cam_close = (int (*)(void))k_dl_sym(0, "k_cam_close", 0);
  trace_p("cam_start: m em", m);
  trace_p("cam_start: n em", &n);
  trace_p("cam_start: k_cam_modes em", (const void*)k_cam_modes);
  if (!k_cam_modes || !k_cam_open || !k_cam_frame || !k_cam_close) {
    err = "o kernel nao tem o stream da webcam (precisa do build 79+)";
    return -1;
  }
  n = k_cam_modes(m, 32);
  if (n <= 0) {
    err = "nenhuma webcam USB";
    return -1;
  }
  for (i = 0; i < n; i++)
    if (m[i * 4] == 2 && (int)m[i * 4 + 1] == w && (int)m[i * 4 + 2] == h) ok = 1;
  if (!ok) {
    err = "a webcam nao tem esse tamanho em YUY2 (webcam /L)";
    return -1;
  }
  r = k_cam_open(2, w, h, &raw_max);
  if (r != 0) {
    err = r == -2 ? "a webcam esta em uso por outro programa" : "a webcam nao ligou";
    return -1;
  }
  raw = (unsigned char*)malloc((size_t)raw_max);
  if (!raw) {
    k_cam_close();
    err = "sem memoria";
    return -1;
  }
  cw = w;
  ch = h;
  seq = 0;
  on = 1;
  return 0;
}

void cam_stop(void) {
  if (on) k_cam_close();
  on = 0;
  free(raw);
  raw = 0;
}

static unsigned char clamp8(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* YUY2 w x h -> planos GW x GH (media dos pixels que cada um cobre, em
 * passos inteiros) e RGBA w x h opcional. */
void yuy2_to_g(gimage* im, unsigned char* rgba, const unsigned char* s, int w, int h) {
  int x, y, fx = w / GW, fy = h / GH;
  if (fx < 1) fx = 1;
  if (fy < 1) fy = 1;
  for (y = 0; y < GH; y++) {
    int sy = y * h / GH;
    for (x = 0; x < GW; x++) {
      int sx = (x * w / GW) & ~1, yy, xx, ys = 0, us = 0, vs = 0, n = 0;
      for (yy = 0; yy < fy && sy + yy < h; yy++) {
        const unsigned char* p = s + ((long)(sy + yy) * w + sx) * 2;
        for (xx = 0; xx < fx && sx + xx < w; xx += 2) {
          ys += p[xx * 2] + p[xx * 2 + 2];
          us += p[xx * 2 + 1];
          vs += p[xx * 2 + 3];
          n++;
        }
      }
      if (!n) n = 1;
      im->y.p[y * GW + x] = (unsigned char)(ys / (2 * n));
      im->cb.p[y * GW + x] = (unsigned char)(us / n);
      im->cr.p[y * GW + x] = (unsigned char)(vs / n);
    }
  }
  if (rgba)
    for (y = 0; y < h; y++) {
      const unsigned char* p = s + (long)y * w * 2;
      unsigned char* o = rgba + (long)y * w * 4;
      for (x = 0; x < w; x++) {
        int Y = p[x * 2] - 16, U = p[(x & ~1) * 2 + 1] - 128, V = p[(x & ~1) * 2 + 3] - 128;
        int c = 298 * Y;
        o[0] = clamp8((c + 409 * V + 128) >> 8);
        o[1] = clamp8((c - 100 * U - 208 * V + 128) >> 8);
        o[2] = clamp8((c + 516 * U + 128) >> 8);
        o[3] = 255;
        o += 4;
      }
    }
}

int cam_grab(gimage* im, unsigned char* rgba, int timeout_ms) {
  int n;
  if (!on) return -1;
  n = k_cam_frame(raw, raw_max, &seq, timeout_ms);
  if (n < 0) {
    on = 0;
    err = "a webcam parou";
    return -1;
  }
  if (n < cw * ch * 2) return 0;
  yuy2_to_g(im, rgba, raw, cw, ch);
  im->t_ms = k_ms();
  im->seq = seq;
  return 1;
}

/* RGBA w x h (qualquer tamanho) -> planos GW x GH, BT.601. */
void rgba_to_g(gimage* im, const unsigned char* rgba, int w, int h) {
  int x, y;
  for (y = 0; y < GH; y++) {
    int sy = y * h / GH;
    for (x = 0; x < GW; x++) {
      int sx = x * w / GW;
      const unsigned char* p = rgba + ((long)sy * w + sx) * 4;
      int r = p[0], g = p[1], b = p[2];
      im->y.p[y * GW + x] = clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
      im->cb.p[y * GW + x] = clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
      im->cr.p[y * GW + x] = clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
    }
  }
  im->t_ms = k_ms();
}
