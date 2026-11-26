/* face.c - acha o rosto entre as regioes de pele.
 *
 * Candidata: regiao grande (>= 0,8% da imagem) com o alto na parte de cima
 * da imagem. A caixa do rosto e a largura da regiao e no maximo 1,4 vez essa
 * largura de altura (o pescoco fica de fora); a proporcao tem de ser de rosto
 * e a caixa tem de estar cheia de pele (a mao aberta e mais vazada). Nota:
 * area de pele na caixa, mais um bonus para quem encosta no rosto do quadro
 * anterior (acompanhar e mais estavel que achar de novo). A caixa final e
 * suavizada (media com a anterior) para nao tremer. */
#include "gesture.h"

/* ---------- pela cascata Haar (OpenCV) ---------- */

/* Regiao de pele que mais cobre a caixa (as maos nao podem ser ela). */
static int blob_of_box(const gstate* s, int x0, int y0, int x1, int y1) {
  int i, best = -1, best_n = 0;
  for (i = 0; i < s->nb; i++) {
    int ox0 = s->blobs[i].x0 > x0 ? s->blobs[i].x0 : x0, ox1 = s->blobs[i].x1 < x1 ? s->blobs[i].x1 : x1;
    int oy0 = s->blobs[i].y0 > y0 ? s->blobs[i].y0 : y0, oy1 = s->blobs[i].y1 < y1 ? s->blobs[i].y1 : y1;
    int n = (ox1 > ox0 && oy1 > oy0) ? (ox1 - ox0) * (oy1 - oy0) : 0;
    if (n > best_n) {
      best_n = n;
      best = i;
    }
  }
  return best;
}

/* % de pele (faixa classica) no miolo da caixa: x de 25% a 75%, y de 35% a 85%. */
static int box_skin(const gimage* im, int x0, int y0, int x1, int y1) {
  int w = x1 - x0, h = y1 - y0, x, y, n = 0, all = 0;
  int ax0 = x0 + w / 4, ax1 = x1 - w / 4, ay0 = y0 + h * 35 / 100, ay1 = y0 + h * 85 / 100;
  if (ax0 < 0) ax0 = 0;
  if (ay0 < 0) ay0 = 0;
  if (ax1 > GW) ax1 = GW;
  if (ay1 > GH) ay1 = GH;
  for (y = ay0; y < ay1; y++)
    for (x = ax0; x < ax1; x++) {
      int i = y * GW + x, cb = im->cb.p[i], cr = im->cr.p[i];
      all++;
      if (cb >= 77 && cb <= 127 && cr >= 133 && cr <= 173) n++;
    }
  return all ? n * 100 / all : 0;
}

void face_find_haar(gface* f, gstate* s, const gimage* im) {
  grect r[8];
  int n = 0, i, best = -1, had = f->found;
  s->frame_no++;
  if (had && f->lost == 0 && s->frame_no % 15 != 0) {
    /* em volta do anterior, no tamanho de 0,8 a 1,25 vez */
    int fw = f->x1 - f->x0 + 1, mx = fw * 3 / 10;
    int x0 = f->x0 - mx, y0 = f->y0 - mx, x1 = f->x1 + mx, y1 = f->y1 + mx;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > GW) x1 = GW;
    if (y1 > GH) y1 = GH;
    n = haar_detect(&s->cface, im->y.p, GW, x0, y0, x1, y1, fw * 8 / 10, fw * 5 / 4, 2, r, 8);
  }
  /* sem rosto: a busca completa (a parte cara) so a cada 4 quadros */
  if (n == 0 && !had && s->frame_no % 4 != 1) { /* o 1o quadro (o /F) procura */
    if (++f->lost > 8) face_reset(f);
    return;
  }
  if (n == 0) {
    /* busca completa na imagem de meia resolucao (rosto >= 48 pixels aqui) */
    int x, y, k;
    for (y = 0; y < GH / 2; y++)
      for (x = 0; x < GW / 2; x++) {
        const unsigned char* p = im->y.p + (y * 2) * GW + x * 2;
        s->small.p[y * (GW / 2) + x] = (unsigned char)((p[0] + p[1] + p[GW] + p[GW + 1] + 2) / 4);
      }
    n = haar_detect(&s->cface, s->small.p, GW / 2, 0, 0, GW / 2, GH / 2, 24, GH / 2, 3, r, 8);
    for (k = 0; k < n; k++) {
      r[k].x *= 2;
      r[k].y *= 2;
      r[k].w *= 2;
      r[k].h *= 2;
    }
  }
  /* Rosto de verdade tem pele no miolo (faixa classica, que nao depende do
   * ajuste): a cascata achou "rosto" na coifa da cozinha (26/11). Entre os
   * que passam, a confianca da cascata (janelas no grupo) vezes a pele; o
   * que encosta no anterior conta em dobro. */
  for (i = 0; i < n; i++) {
    int sk = box_skin(im, r[i].x, r[i].y, r[i].x + r[i].w, r[i].y + r[i].h);
    int score = sk >= 15 ? r[i].n * sk : 0;
    if (!score) continue;
    if (had) {
      int dx = r[i].x + r[i].w / 2 - f->cx, dy = r[i].y + r[i].h / 2 - f->cy;
      if (dx < 0) dx = -dx;
      if (dy < 0) dy = -dy;
      if (dx < (f->x1 - f->x0) / 2 && dy < (f->y1 - f->y0) / 2) score *= 2;
    }
    if (best < 0 || score > r[best].n) {
      r[i].n = score;
      best = i;
    }
  }
  if (best < 0) {
    if (++f->lost > 8) face_reset(f);
    return;
  }
  if (had && f->lost < 8) { /* suaviza */
    f->x0 = (f->x0 + r[best].x) / 2;
    f->y0 = (f->y0 + r[best].y) / 2;
    f->x1 = (f->x1 + r[best].x + r[best].w - 1) / 2;
    f->y1 = (f->y1 + r[best].y + r[best].h - 1) / 2;
  } else {
    f->x0 = r[best].x;
    f->y0 = r[best].y;
    f->x1 = r[best].x + r[best].w - 1;
    f->y1 = r[best].y + r[best].h - 1;
    f->base_l = f->base_r = 0;
  }
  f->lost = 0;
  f->cx = (f->x0 + f->x1) / 2;
  f->cy = (f->y0 + f->y1) / 2;
  f->found = 1;
  f->blob = blob_of_box(s, f->x0, f->y0, f->x1, f->y1);
}

void face_reset(gface* f) {
  f->found = 0;
  f->eyes = 0;
  f->blob = -1;
  f->yaw = f->pitch = f->roll = 0;
}

static int overlap(int a0, int a1, int b0, int b1) {
  int lo = a0 > b0 ? a0 : b0, hi = a1 < b1 ? a1 : b1;
  return hi > lo ? hi - lo : 0;
}

void face_find(gface* f, const gblob* b, int nb, const gplane* mask) {
  int i, best = -1, best_score = 0, bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
  int had = f->found;
  for (i = 0; i < nb; i++) {
    int bw = b[i].x1 - b[i].x0 + 1, bh = b[i].y1 - b[i].y0 + 1, fh, skin, score, x0, y0, x1, y1;
    if (b[i].area < GN / 125) continue;            /* 0,8% */
    if (b[i].y0 > GH * 6 / 10) continue;           /* rosto nao comeca embaixo */
    if (bw < GW / 16) continue;
    fh = bh;
    if (fh > bw * 14 / 10) fh = bw * 14 / 10;
    if (fh * 100 < bw * 80) continue;              /* achatada demais (braco deitado) */
    x0 = b[i].x0;
    x1 = b[i].x1;
    y0 = b[i].y0;
    y1 = y0 + fh - 1;
    skin = pl_count(mask, x0, y0, x1, y1, 255);
    if (skin * 100 < bw * fh * 45) continue;       /* caixa vazada: mao, ruido */
    score = skin;
    if (had && overlap(x0, x1, f->x0, f->x1) * 2 > (f->x1 - f->x0) &&
        overlap(y0, y1, f->y0, f->y1) * 2 > (f->y1 - f->y0))
      score = score * 3 / 2;
    /* mais alto na imagem, melhor (maos costumam ficar mais baixo) */
    score = score * (GH * 2 - y0) / (GH * 2);
    if (score > best_score) {
      best_score = score;
      best = i;
      bx0 = x0;
      by0 = y0;
      bx1 = x1;
      by1 = y1;
    }
  }
  if (best < 0) {
    if (++f->lost > 8) face_reset(f);
    return;
  }
  f->lost = 0;
  f->blob = best;
  if (had) {
    f->x0 = (f->x0 + bx0) / 2;
    f->y0 = (f->y0 + by0) / 2;
    f->x1 = (f->x1 + bx1) / 2;
    f->y1 = (f->y1 + by1) / 2;
  } else {
    f->x0 = bx0;
    f->y0 = by0;
    f->x1 = bx1;
    f->y1 = by1;
    f->base_l = f->base_r = 0;
  }
  f->cx = (f->x0 + f->x1) / 2;
  f->cy = (f->y0 + f->y1) / 2;
  f->found = 1;
}
