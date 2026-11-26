/* head.c - pose da cabeca pelos olhos dentro da caixa do rosto.
 *
 *   yaw   distancia de cada olho a borda do rosto do mesmo lado: virando, o
 *         olho do lado da virada encosta na borda. (dl - dr) / (dl + dr).
 *         Com um olho so, o lado dele (o outro sumiu atras do nariz).
 *   pitch altura dos olhos na caixa, comparada com a referencia (media
 *         lenta): cabeca baixa = testa ocupa mais caixa = olhos mais baixos.
 *   roll  angulo da linha entre os olhos, em graus (tan ~ angulo ate ~30). */
#include "gesture.h"

static int pitch_ref = -1;

void head_pose(gface* f) {
  int fw = f->x1 - f->x0 + 1, fh = f->y1 - f->y0 + 1;
  if (!f->found || fw < 8) {
    f->yaw = f->pitch = f->roll = 0;
    return;
  }
  if (f->eyes == 2) {
    int dl = f->lx - f->x0, dr = f->x1 - f->rx, dx = f->rx - f->lx, dy = f->ry - f->ly, ey, r;
    if (dl < 0) dl = 0;
    if (dr < 0) dr = 0;
    f->yaw = dl + dr ? (dl - dr) * 100 / (dl + dr) : 0;
    f->roll = dx > 0 ? dy * 57 / dx : 0;
    ey = (f->ly + f->ry) / 2;
    r = (ey - f->y0) * 1000 / fh;          /* altura dos olhos, por mil */
    if (pitch_ref < 0) pitch_ref = r;
    else pitch_ref += (r - pitch_ref) / 32;
    f->pitch = (r - pitch_ref) * 2;
    if (f->pitch > 100) f->pitch = 100;
    if (f->pitch < -100) f->pitch = -100;
  } else if (f->eyes == 1) {
    /* um olho so: o rosto virou para o lado em que ele esta */
    int x = f->open_l >= 0 ? f->lx : f->rx;
    f->yaw = x < f->cx ? -70 : 70;
    f->roll = 0;
  } else {
    f->yaw = 0;
    f->roll = 0;
  }
  if (f->yaw > 100) f->yaw = 100;
  if (f->yaw < -100) f->yaw = -100;
}
