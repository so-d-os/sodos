/* hands.c - maos: regioes de pele que nao sao o rosto, e dedos esticados.
 *
 * Mao: regiao de 0,4% a 20% da imagem, que nao e a do rosto. Se for muito
 * comprida, so o pedaco de cima conta (1,6 vez a largura: o antebraco fica de
 * fora; a conta supoe a mao para cima, que e o comum na frente da camera).
 * Palma = centro de massa desse pedaco. Dedos: um arco de -15 a 195 graus
 * (a metade de cima e um pouco dos lados, para o polegar), a 70% da maior
 * distancia da palma a um pixel da mao acima dela; cada trecho de pele que
 * o arco cruza e um dedo. Arco quase todo dentro da mao = punho (0). */
#include "gesture.h"

/* seno * 1024 de 0 a 90 graus, de 3 em 3 */
static const short sin3[31] = {0,   54,  107, 160, 213, 265, 316, 367, 416, 465, 512,
                               558, 602, 644, 685, 724, 761, 796, 828, 859, 887, 912,
                               935, 956, 974, 989, 1002, 1011, 1018, 1023, 1024};

static int isin(int a) { /* a em graus, multiplo de 3 */
  while (a < 0) a += 360;
  a %= 360;
  if (a <= 90) return sin3[a / 3];
  if (a <= 180) return sin3[(180 - a) / 3];
  if (a <= 270) return -sin3[(a - 180) / 3];
  return -sin3[(360 - a) / 3];
}
static int icos(int a) { return isin(a + 90); }

static int is_lab(const unsigned short* labels, int lab, int x, int y) {
  if (x < 0 || y < 0 || x >= GW || y >= GH) return 0;
  return labels[y * GW + x] == lab;
}

/* Um vao entre dois trechos de pele so separa dedos se for fundo: o fundo
 * aparece tambem a 75% do raio do arco, no mesmo angulo. Entre os nos dos
 * dedos de um punho o vao e raso (o arco so raspa o contorno): sem isso o
 * punho contava 4 dedos (26/11). */
static int count_fingers(const unsigned short* labels, int lab, int cx, int cy, int maxd) {
  int r = maxd * 70 / 100, r2 = maxd * 70 * 75 / 10000, a, runs = 0, in = 0, total = 0, prev = 0, deep = 1;
  int len = 0, maxlen = 0;
  if (r < 6) return -1;
  for (a = -15; a <= 195; a += 3) {
    int x = cx + icos(a) * r / 1024, y = cy - isin(a) * r / 1024;
    int v = is_lab(labels, lab, x, y);
    if (!v && !is_lab(labels, lab, cx + icos(a) * r2 / 1024, cy - isin(a) * r2 / 1024)) deep = 1;
    if (v && !prev && deep) {
      runs++;
      deep = 0;
      len = 0;
    }
    if (v && ++len > maxlen) maxlen = len;
    prev = v;
    in += v;
    total++;
  }
  if (in * 100 >= total * 85) return 0; /* punho: o arco passa dentro da mao */
  /* um trecho so e largo (mais de 40 graus, 14 passos de 3): o topo do
   * punho; um dedo esticado (o indicador) e estreito */
  if (runs <= 1 && maxlen > 14) return 0;
  if (runs > 5) runs = 5;
  return runs;
}

/* Uma regiao candidata -> mao (o pedaco de cima, palma e dedos). 1 = ok. */
static int hand_from_blob(ghand* o, const gblob* b, const unsigned short* labels, const gface* f) {
  int bw = b->x1 - b->x0 + 1, bh = b->y1 - b->y0 + 1, hh, x, y, lab = b->label;
  long sx = 0, sy = 0, cnt = 0, maxd2 = 0;
  if (b->area < GN / 250 || b->area > GN / 5) return 0;
  if (bw < 6 || bh < 6) return 0;
  /* com rosto: a mao tem pelo menos 1/8 da area da caixa do rosto (a coifa
   * da cozinha, 17x27 pixels de "pele", virava mao levantada) */
  if (f->found && b->area * 8 < (long)(f->x1 - f->x0 + 1) * (f->y1 - f->y0 + 1)) return 0;
  /* teto ou parede de cor quente: encosta na borda da imagem em boa parte do
   * lado; a mao levantada so encosta com a ponta dos dedos */
  {
    int k, top = 0, side = 0;
    if (b->y0 <= 1)
      for (k = b->x0; k <= b->x1; k++) top += labels[k] == lab || labels[GW + k] == lab;
    if (b->x0 <= 1 || b->x1 >= GW - 2) {
      int ex = b->x0 <= 1 ? 0 : GW - 1;
      for (k = b->y0; k <= b->y1; k++) side += labels[k * GW + ex] == lab;
    }
    if (top * 10 > bw * 6 || side * 10 > bh * 7) return 0;
  }
  hh = bh;
  if (hh > bw * 16 / 10) hh = bw * 16 / 10;
  o->x0 = b->x0;
  o->x1 = b->x1;
  o->y0 = b->y0;
  o->y1 = b->y0 + hh - 1;
  for (y = o->y0; y <= o->y1; y++)
    for (x = o->x0; x <= o->x1; x++)
      if (labels[y * GW + x] == lab) {
        sx += x;
        sy += y;
        cnt++;
      }
  if (cnt < 20) return 0;
  o->cx = (int)(sx / cnt);
  o->cy = (int)(sy / cnt);
  o->area = (int)cnt;
  for (y = o->y0; y <= o->cy; y++)
    for (x = o->x0; x <= o->x1; x++)
      if (labels[y * GW + x] == lab) {
        long d2 = (long)(x - o->cx) * (x - o->cx) + (long)(y - o->cy) * (y - o->cy);
        if (d2 > maxd2) maxd2 = d2;
      }
  {
    int d = 0;
    while ((long)(d + 1) * (d + 1) <= maxd2) d++;
    o->fingers = count_fingers(labels, lab, o->cx, o->cy, d);
  }
  o->raised = f->found ? o->cy < f->y1 : o->cy < GH / 2;
  o->found = 1;
  return 1;
}

/* Mao grudada no corpo: com a pele continua (braco e peito a mostra, ou a
 * mao encostada no rosto) a mao, o braco, o peito e o rosto sao uma regiao
 * so, e a cor nao separa. Fica a pele dessa regiao acima do queixo e fora
 * da caixa do rosto (com folga para orelhas e cabelo): a mao levantada e o
 * antebraco, que o hand_from_blob corta pelo pedaco de cima. */
static int hands_split(ghand* h, int n, int lab, const gface* f, const unsigned short* labels, gplane* work,
                       unsigned short* labels2) {
  int fw = f->x1 - f->x0 + 1, fh = f->y1 - f->y0 + 1, x, y, k, m;
  int ex0 = f->x0 - fw * 2 / 10, ex1 = f->x1 + fw * 2 / 10, ey0 = f->y0 - fh * 4 / 10;
  int cut = f->y1 + fh / 10; /* o queixo, com um pouco de pescoco */
  gblob sb[8];
  for (y = 0; y < GH; y++)
    for (x = 0; x < GW; x++) {
      int in = labels[y * GW + x] == lab && y < cut && !(x >= ex0 && x <= ex1 && y >= ey0);
      work->p[y * GW + x] = (unsigned char)(in ? 255 : 0);
    }
  m = blobs_find(work, labels2, sb, 8, GN / 400);
  for (k = 0; k < m && n < HAND_MAX; k++) {
    /* do tamanho de uma mao: o teto e a parede de cor quente grudados no
     * rosto (26/11: 186x147 com o rosto de 75x75) nao sao mao */
    int bw = sb[k].x1 - sb[k].x0 + 1, bh = sb[k].y1 - sb[k].y0 + 1;
    if (bw > fw * 16 / 10 || bh > fh * 25 / 10 || sb[k].area > (long)fw * fh * 3 / 2) continue;
    /* levantada: passa acima do meio do rosto (ombro e lado da cabeca nao) */
    if (sb[k].y0 > f->cy - fh / 6) continue;
    if (hand_from_blob(&h[n], &sb[k], labels2, f)) n++;
  }
  return n;
}

int hands_from_mask(ghand* h, const gblob* b, int nb, const gface* f, const unsigned short* labels) {
  int i, n = 0;
  for (i = 0; i < HAND_MAX; i++) h[i].found = 0;
  for (i = 0; i < nb && n < HAND_MAX; i++)
    if (hand_from_blob(&h[n], &b[i], labels, f)) n++;
  return n;
}

int hands_find(ghand* h, const gblob* b, int nb, const gface* f, const unsigned short* labels, gplane* work,
               unsigned short* labels2) {
  int i, n = 0;
  for (i = 0; i < HAND_MAX; i++) h[i].found = 0;
  for (i = 0; i < nb && n < HAND_MAX; i++) {
    if (f->found && i == f->blob) continue;
    if (hand_from_blob(&h[n], &b[i], labels, f)) n++;
  }
  /* a regiao do rosto (com o corpo), se ainda cabe mao */
  if (f->found && f->blob >= 0 && f->blob < nb && n < HAND_MAX && work && labels2)
    n = hands_split(h, n, b[f->blob].label, f, labels, work, labels2);
  return n;
}
