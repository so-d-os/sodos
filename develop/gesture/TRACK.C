/* track.c - historico dos ultimos ~2 s e os gestos que dependem do tempo.
 *
 * Olhos (so com o rosto de frente, |yaw| < 40): abertura em % da referencia
 * de olho aberto; fechado < 45%, aberto >= 60% (entre os dois mantem o
 * estado anterior). Olho que sumiu com o outro visivel conta como fechado.
 *   piscar        os dois fecham e reabrem em 1 a 12 quadros (~0,4 s)
 *   piscadela     so um fecha por 3 a 20 quadros e reabre
 *   olhos fechados os dois fechados mais de ~1 s (e "abriu" depois)
 * Cabeca:
 *   virar         yaw alem de 35 por 5 quadros; volta ao centro abaixo de 15
 *   cima/baixo    pitch alem de 40; inclinar: roll alem de 15 graus
 *   "sim"         zigue-zague vertical do rosto (6% da altura) em 1,2 s
 *   "nao"         zigue-zague do yaw (amplitude 25) com 2 inversoes em 1,5 s
 * Mao (a maior):
 *   dedos         mesma contagem 6 quadros seguidos: punho (0), mao aberta
 *                 (5) ou "dedos: n"
 *   deslizar      andou 30% da imagem em ate 0,7 s, quase reto
 *   tchau         3 inversoes horizontais (5% da largura) em 1,6 s, mao
 *                 levantada
 * Movimento: pixels que mudaram (por mil): mexeu depois de parado ~1 s, e
 * parado de novo por 3 s. Cada evento so se repete depois de um intervalo. */
#include <string.h>
#include "gesture.h"

void track_init(gtrack* t) {
  memset(t, 0, sizeof(*t));
  t->fingers_state = -2;
  t->fingers_cand = -2;
}

static int at(const gtrack* t, int back) { return (t->head - 1 - back + HIST * 2) % HIST; }

static void emit(gtrack* t, gevent* out, int* n, int max, int type, int value, unsigned now,
                 unsigned cool) {
  if (*n >= max) return;
  if (t->last[type] && now - t->last[type] < cool) return;
  t->last[type] = now;
  out[*n].type = type;
  out[*n].value = value;
  out[*n].t_ms = now;
  (*n)++;
}

/* Inversoes de direcao (com histerese amp) da serie s nos ultimos win ms,
 * so nos quadros em que ok[] != 0. */
static int zigzag(const gtrack* t, const int* s, const int* ok, unsigned now, unsigned win, int amp) {
  int k, first = -1, dir = 0, ext = 0, rev = 0;
  for (k = t->n - 1; k >= 0; k--) { /* do mais velho dentro da janela ao mais novo */
    int i = at(t, k);
    if (now - t->t[i] > win || !ok[i]) continue;
    if (first < 0) {
      first = i;
      ext = s[i];
      continue;
    }
    if (dir == 0) {
      if (s[i] >= ext + amp) {
        dir = 1;
        ext = s[i];
      } else if (s[i] <= ext - amp) {
        dir = -1;
        ext = s[i];
      }
    } else if (dir > 0) {
      if (s[i] > ext) ext = s[i];
      else if (s[i] <= ext - amp) {
        dir = -1;
        ext = s[i];
        rev++;
      }
    } else {
      if (s[i] < ext) ext = s[i];
      else if (s[i] >= ext + amp) {
        dir = 1;
        ext = s[i];
        rev++;
      }
    }
  }
  return rev;
}

/* Variacao total (max - min) da serie na janela. */
static int span(const gtrack* t, const int* s, const int* ok, unsigned now, unsigned win) {
  int k, lo = 0, hi = 0, any = 0;
  for (k = 0; k < t->n; k++) {
    int i = at(t, k);
    if (now - t->t[i] > win) break;
    if (!ok[i]) continue;
    if (!any || s[i] < lo) lo = s[i];
    if (!any || s[i] > hi) hi = s[i];
    any = 1;
  }
  return any ? hi - lo : 0;
}

/* Estado de um olho: 1 fechado, 0 aberto, -1 nao sei. */
static int eye_closed(int open, int base, int other_seen) {
  if (base <= 3) return -1;          /* ainda sem referencia de olho aberto */
  if (open < 0) return other_seen ? 1 : -1;
  if (open * 100 < base * 45) return 1;
  if (open * 100 >= base * 60) return 0;
  return -1;
}

int track_update(gtrack* t, const gface* f, const ghand* h, int nh, int motion, unsigned now,
                 gevent* out, int max) {
  int n = 0, i = t->head, cl, cr, frontal;
  /* ---- historico ---- */
  t->t[i] = now;
  t->face[i] = f->found;
  t->fx[i] = f->cx;
  t->fy[i] = f->cy;
  t->fh[i] = f->y1 - f->y0 + 1;
  t->yaw[i] = f->yaw;
  t->pitch[i] = f->pitch;
  t->roll[i] = f->roll;
  t->eyes[i] = f->eyes;
  t->ol[i] = f->open_l;
  t->or_[i] = f->open_r;
  t->hand[i] = nh > 0 && h[0].found;
  t->hx[i] = t->hand[i] ? h[0].cx : 0;
  t->hy[i] = t->hand[i] ? h[0].cy : 0;
  t->fingers[i] = t->hand[i] ? h[0].fingers : -1;
  t->motion[i] = motion;
  t->head = (t->head + 1) % HIST;
  if (t->n < HIST) t->n++;

  /* ---- rosto ---- */
  if (f->found && !t->had_face) emit(t, out, &n, max, G_FACE_IN, 0, now, 1500);
  if (!f->found && t->had_face) emit(t, out, &n, max, G_FACE_OUT, 0, now, 1500);
  t->had_face = f->found;

  if (f->found) {
    int yaw = f->yaw;
    /* ---- olhos ---- */
    frontal = yaw > -40 && yaw < 40;
    cl = eye_closed(f->open_l, f->base_l, f->open_r >= 0);
    cr = eye_closed(f->open_r, f->base_r, f->open_l >= 0);
    if (frontal && cl == 1 && cr == 1) {
      t->closed_both++;
      t->closed_l = t->closed_r = 0;
      if (t->closed_both > 30 && !t->eyes_shut) {
        t->eyes_shut = 1;
        emit(t, out, &n, max, G_EYES_CLOSED, 0, now, 2000);
      }
    } else if (frontal && cl == 0 && cr == 0) {
      if (t->eyes_shut) {
        t->eyes_shut = 0;
        emit(t, out, &n, max, G_EYES_OPEN, 0, now, 1000);
      } else if (t->closed_both >= 1 && t->closed_both <= 12) {
        emit(t, out, &n, max, G_BLINK, 0, now, 300);
      } else if (t->closed_l >= 3 && t->closed_l <= 20) {
        emit(t, out, &n, max, G_WINK_L, 0, now, 800);
      } else if (t->closed_r >= 3 && t->closed_r <= 20) {
        emit(t, out, &n, max, G_WINK_R, 0, now, 800);
      }
      t->closed_both = t->closed_l = t->closed_r = 0;
    } else if (frontal && cl == 1 && cr == 0) {
      t->closed_l++;
    } else if (frontal && cr == 1 && cl == 0) {
      t->closed_r++;
    }

    /* ---- cabeca: virar (5 quadros seguidos alem de 35) ---- */
    {
      int k, all_l = 1, all_r = 1;
      for (k = 0; k < 5 && k < t->n; k++) {
        int j = at(t, k);
        if (!t->face[j] || t->yaw[j] > -35) all_l = 0;
        if (!t->face[j] || t->yaw[j] < 35) all_r = 0;
      }
      if (all_l && t->head_state != -1) {
        t->head_state = -1;
        emit(t, out, &n, max, G_HEAD_LEFT, 0, now, 800);
      } else if (all_r && t->head_state != 1) {
        t->head_state = 1;
        emit(t, out, &n, max, G_HEAD_RIGHT, 0, now, 800);
      } else if (yaw > -15 && yaw < 15 && t->head_state != 0) {
        t->head_state = 0;
        emit(t, out, &n, max, G_HEAD_CENTER, 0, now, 800);
      }
    }
    /* ---- cabeca: cima/baixo ---- */
    if (f->pitch >= 40 && t->pitch_state != 1) {
      t->pitch_state = 1;
      emit(t, out, &n, max, G_HEAD_DOWN, 0, now, 800);
    } else if (f->pitch <= -40 && t->pitch_state != -1) {
      t->pitch_state = -1;
      emit(t, out, &n, max, G_HEAD_UP, 0, now, 800);
    } else if (f->pitch > -15 && f->pitch < 15) {
      t->pitch_state = 0;
    }
    /* ---- inclinar ---- */
    if (f->eyes == 2) {
      if (f->roll >= 15 && t->tilt_state != 1) {
        t->tilt_state = 1;
        emit(t, out, &n, max, G_TILT_R, 0, now, 800);
      } else if (f->roll <= -15 && t->tilt_state != -1) {
        t->tilt_state = -1;
        emit(t, out, &n, max, G_TILT_L, 0, now, 800);
      } else if (f->roll > -6 && f->roll < 6) {
        t->tilt_state = 0;
      }
    }
    /* ---- "sim" e "nao" ---- */
    {
      int fhh = f->y1 - f->y0 + 1, amp_y = fhh * 6 / 100, fw = f->x1 - f->x0 + 1;
      if (amp_y < 3) amp_y = 3;
      if (zigzag(t, t->yaw, t->face, now, 1500, 25) >= 2 ||
          zigzag(t, t->fx, t->face, now, 1500, fw * 8 / 100 + 2) >= 2)
        emit(t, out, &n, max, G_SHAKE, 0, now, 1500);
      else if (zigzag(t, t->fy, t->face, now, 1200, amp_y) >= 1 && span(t, t->fx, t->face, now, 1200) < fw / 6 &&
               span(t, t->yaw, t->face, now, 1200) < 30)
        emit(t, out, &n, max, G_NOD, 0, now, 1500);
    }
  }

  /* ---- mao ---- */
  if (t->hand[i]) {
    t->hand_gone = 0;
    if (++t->hand_in == 3) emit(t, out, &n, max, G_HAND_IN, 0, now, 1000);
    if (h[0].raised && !t->raised_state) {
      int k, ok = 1;
      for (k = 0; k < 5 && k < t->n; k++)
        if (!t->hand[at(t, k)]) ok = 0;
      if (ok) {
        t->raised_state = 1;
        emit(t, out, &n, max, G_HAND_UP, 0, now, 1000);
      }
    } else if (!h[0].raised) {
      t->raised_state = 0;
    }
    /* dedos estaveis */
    if (h[0].fingers == t->fingers_cand) t->fingers_run++;
    else {
      t->fingers_cand = h[0].fingers;
      t->fingers_run = 1;
    }
    if (t->fingers_run == 6 && t->fingers_cand >= 0 && t->fingers_cand != t->fingers_state) {
      t->fingers_state = t->fingers_cand;
      /* espalmada: 4 ou 5 (o arco as vezes junta o polegar ao indicador) */
      if (t->fingers_state == 0) emit(t, out, &n, max, G_FIST, 0, now, 600);
      else if (t->fingers_state >= 4) emit(t, out, &n, max, G_OPEN_HAND, t->fingers_state, now, 600);
      else emit(t, out, &n, max, G_FINGERS, t->fingers_state, now, 600);
    }
    /* deslizar: do ponto mais velho dentro de 0,7 s (mao presente o tempo todo) */
    {
      int k, j = -1;
      for (k = 0; k < t->n; k++) {
        int q = at(t, k);
        if (now - t->t[q] > 700 || !t->hand[q]) break;
        j = q;
      }
      if (j >= 0) {
        int dx = t->hx[i] - t->hx[j], dy = t->hy[i] - t->hy[j], ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        if (ax > GW * 30 / 100 && ay * 2 < ax) {
          emit(t, out, &n, max, dx < 0 ? G_SWIPE_L : G_SWIPE_R, 0, now, 1000);
        } else if (ay > GH * 30 / 100 && ax * 2 < ay) {
          emit(t, out, &n, max, dy < 0 ? G_SWIPE_UP : G_SWIPE_DOWN, 0, now, 1000);
        }
      }
    }
    /* tchau */
    if (h[0].raised && zigzag(t, t->hx, t->hand, now, 1600, GW * 5 / 100) >= 3)
      emit(t, out, &n, max, G_WAVE, 0, now, 2000);
  } else {
    t->fingers_run = 0;
    t->fingers_cand = -2;
    if (t->hand_gone < 1000) t->hand_gone++;
    if (t->hand_gone == 10) { /* ~0,3 s sem mao */
      if (t->hand_in >= 3) emit(t, out, &n, max, G_HAND_OUT, 0, now, 1000);
      t->hand_in = 0;
      t->fingers_state = -2;
      t->raised_state = 0;
    }
  }

  /* ---- movimento geral ---- */
  if (motion < 8) {
    if (!t->still) {
      t->still = 1;
      t->still_since = now;
    } else if (now - t->still_since > 3000 && t->still_ms == 0) {
      t->still_ms = 1;
      emit(t, out, &n, max, G_STILL, 0, now, 5000);
    }
  } else if (motion > 30) {
    if (t->still && now - t->still_since > 1000) emit(t, out, &n, max, G_MOTION, motion, now, 2000);
    t->still = 0;
    t->still_ms = 0;
  }
  return n;
}
