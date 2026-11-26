/* skin.c - mascara de pele pela cor (Cb, Cr), que nao depende muito do
 * brilho. Comeca com os limites classicos (Chai e Ngan, 1999) e, quando o
 * rosto aparece, aperta a faixa em volta da cor media do centro do rosto: com
 * luz quente o fundo bege tambem cai na faixa larga. */
#include "gesture.h"

#define CB_LO 77
#define CB_HI 127
#define CR_LO 133
#define CR_HI 173

void skin_init(gskin* s) {
  s->cb_lo = CB_LO;
  s->cb_hi = CB_HI;
  s->cr_lo = CR_LO;
  s->cr_hi = CR_HI;
  s->y_lo = 30;
  s->adapted = 0;
}

void skin_mask(const gskin* s, const gimage* im, gplane* mask) {
  int i;
  for (i = 0; i < GN; i++) {
    int cb = im->cb.p[i], cr = im->cr.p[i];
    mask->p[i] = (unsigned char)((cb >= s->cb_lo && cb <= s->cb_hi && cr >= s->cr_lo && cr <= s->cr_hi &&
                                  im->y.p[i] >= s->y_lo)
                                     ? 255
                                     : 0);
  }
}

static int lim(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Cor media do miolo da caixa (bochechas e nariz: metade do meio) -> faixa
 * de +-14 em Cb e +-12 em Cr, mudando aos poucos. So entram os pixels do
 * miolo que ja sao pele pela faixa classica: barba, oculos e parede puxavam
 * a media para o cinza (Cr 125, 26/11), e a mao (Cr 134..143) virava uma
 * regiao so com a parede (Cr 125..131) e o peito. Por isso tambem o Cr
 * minimo nunca desce abaixo do classico. */
void skin_adapt(gskin* s, const gimage* im, int x0, int y0, int x1, int y1) {
  int w = x1 - x0, h = y1 - y0, mx0, my0, mx1, my1, cb, cr, x, y;
  long scb = 0, scr = 0, n = 0, all = 0;
  int ncb_lo, ncb_hi, ncr_lo, ncr_hi;
  if (w < 8 || h < 8) return;
  mx0 = x0 + w / 4;
  mx1 = x1 - w / 4;
  my0 = y0 + h * 2 / 5;
  my1 = y0 + h * 4 / 5;
  if (mx0 < 0) mx0 = 0;
  if (my0 < 0) my0 = 0;
  if (mx1 > GW) mx1 = GW;
  if (my1 > GH) my1 = GH;
  for (y = my0; y < my1; y++)
    for (x = mx0; x < mx1; x++) {
      int i = y * GW + x, b = im->cb.p[i], r = im->cr.p[i];
      all++;
      if (b < CB_LO || b > CB_HI || r < CR_LO || r > CR_HI || im->y.p[i] < 30) continue;
      scb += b;
      scr += r;
      n++;
    }
  /* pouca pele no miolo (o "rosto" era outra coisa, ou so barba): nao aprende */
  if (n < 30 || n * 5 < all) return;
  cb = (int)(scb / n);
  cr = (int)(scr / n);
  ncb_lo = lim(cb - 14, CB_LO - 8, CB_HI);
  ncb_hi = lim(cb + 14, CB_LO, CB_HI + 6);
  ncr_lo = lim(cr - 12, CR_LO, CR_HI);
  ncr_hi = lim(cr + 12, CR_LO + 6, CR_HI + 8);
  if (!s->adapted) {
    s->cb_lo = ncb_lo;
    s->cb_hi = ncb_hi;
    s->cr_lo = ncr_lo;
    s->cr_hi = ncr_hi;
  } else { /* 1/8 por quadro: a luz muda devagar */
    s->cb_lo += (ncb_lo - s->cb_lo) / 8;
    s->cb_hi += (ncb_hi - s->cb_hi) / 8;
    s->cr_lo += (ncr_lo - s->cr_lo) / 8;
    s->cr_hi += (ncr_hi - s->cr_hi) / 8;
  }
  s->adapted = 1;
}
