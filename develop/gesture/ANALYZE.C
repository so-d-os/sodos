/* analyze.c - um quadro: pele -> regioes -> rosto -> olhos -> pose -> maos
 * -> movimento -> gestos. */
#include <stdlib.h>
#include <string.h>
#include <so.h>
#include "gesture.h"

int an_init(gstate* s) {
  memset(s, 0, sizeof(*s));
  skin_init(&s->skin);
  face_reset(&s->face);
  track_init(&s->track);
  s->detect = D_ALL;
  s->labels = (unsigned short*)malloc(sizeof(unsigned short) * GN);
  s->labels2 = (unsigned short*)malloc(sizeof(unsigned short) * GN);
  if (!s->labels || !s->labels2 || pl_alloc(&s->mask, GW, GH) || pl_alloc(&s->tmp, GW, GH) || pl_alloc(&s->prevy, GW, GH) ||
      pl_alloc(&s->small, GW / 2, GH / 2))
    return -1;
  /* cascatas da OpenCV: na pasta atual ou na do projeto */
  s->haar_ok = haar_load(&s->cface, "FACE.CAS") == 0 ||
               haar_load(&s->cface, "D:\\SOURCE\\SODOS\\develop\\gesture\\FACE.CAS") == 0;
  s->eye_ok = haar_load(&s->ceye, "EYE.CAS") == 0 ||
              haar_load(&s->ceye, "D:\\SOURCE\\SODOS\\develop\\gesture\\EYE.CAS") == 0;
  return 0;
}

void an_free(gstate* s) {
  free(s->labels);
  free(s->labels2);
  pl_free(&s->mask);
  pl_free(&s->tmp);
  pl_free(&s->prevy);
}

/* So calcula o que algum marcador ligado precisa (s->detect). O rosto e
 * procurado tambem quando so as maos estao ligadas: e ele que separa a
 * regiao do rosto das maos. Os gestos de grupos desligados sao jogados fora. */
int an_frame(gstate* s, const gimage* im, gevent* ev, int max) {
  gface* f = &s->face;
  int d = s->detect, n, i, k;
  gevent all[24];
  if (d & (D_NEED_FACE | D_NEED_HAND)) {
    skin_mask(&s->skin, im, &s->mask);
    pl_open_close(&s->mask, &s->tmp);
    s->nb = blobs_find(&s->mask, s->labels, s->blobs, BLOB_MAX, GN / 400);
    if (s->haar_ok) face_find_haar(f, s, im);
    else face_find(f, s->blobs, s->nb, &s->mask);
  } else {
    s->nb = 0;
    face_reset(f);
  }
  if (f->found && (d & (D_EYES | D_HEAD))) {
    skin_adapt(&s->skin, im, f->x0, f->y0, f->x1, f->y1);
    if (s->haar_ok) eyes_find_haar(f, s, im);
    else eyes_find(f, im, &s->mask);
  } else {
    if (f->found) skin_adapt(&s->skin, im, f->x0, f->y0, f->x1, f->y1);
    f->eyes = 0;
    f->open_l = f->open_r = -1;
  }
  if (d & D_HEAD) head_pose(f);
  else f->yaw = f->pitch = f->roll = 0;
  s->nh = (d & D_NEED_HAND) ? hands_find(s->hand, s->blobs, s->nb, f, s->labels, &s->tmp, s->labels2) : 0;
  /* movimento: brilho que mudou mais de 20 desde o quadro anterior */
  s->motion = (d & D_MOTION) && s->have_prev ? pl_absdiff_count(&im->y, &s->prevy, 20) * 1000 / GN : 0;
  pl_copy(&s->prevy, &im->y);
  s->have_prev = 1;
  s->ms_last = im->t_ms;
  n = track_update(&s->track, f, s->hand, s->nh, s->motion, im->t_ms, all, 24);
  for (i = 0, k = 0; i < n && k < max; i++)
    if (ev_group(all[i].type) & d) ev[k++] = all[i];
  return k;
}
