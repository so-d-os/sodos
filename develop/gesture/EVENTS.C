/* events.c - nomes dos gestos e registro em arquivo. */
#include <stdio.h>
#include <string.h>
#include <so.h>
#include "gesture.h"

static const char* const names[G_COUNT] = {
    "nada",
    "rosto apareceu", "rosto saiu",
    "piscou", "piscadela (olho da esquerda)", "piscadela (olho da direita)", "olhos fechados", "olhos abertos",
    "virou a cabeca para a esquerda", "virou a cabeca para a direita", "cabeca para cima", "cabeca para baixo",
    "cabeca de frente",
    "sim (acenou com a cabeca)", "nao (balancou a cabeca)", "inclinou a cabeca para a esquerda",
    "inclinou a cabeca para a direita",
    "mao apareceu", "mao saiu", "mao levantada", "mao fechada", "mao espalmada", "dedos",
    "deslizou para a esquerda", "deslizou para a direita", "deslizou para cima", "deslizou para baixo", "tchau",
    "movimento", "parado"};

const char* ev_name(int type) { return type >= 0 && type < G_COUNT ? names[type] : "?"; }

void ev_text(const gevent* e, char* out, int max) {
  if (e->type == G_FINGERS) snprintf(out, (size_t)max, "dedos: %d", e->value);
  else snprintf(out, (size_t)max, "%s", ev_name(e->type));
}

/* ---------- marcadores: o que detectar ---------- */

static const char* const det_names[D_GROUPS] = {"mao espalmada", "mao fechada", "olhos", "rosto",
                                                 "cabeca", "dedos", "deslizar e tchau", "movimento"};
/* nomes curtos para /D: (sem espaco nem acento) */
static const char* const det_keys[D_GROUPS] = {"espalmada", "fechada", "olhos", "rosto",
                                                "cabeca", "dedos", "deslizar", "movimento"};

const char* det_name(int i) { return i >= 0 && i < D_GROUPS ? det_names[i] : "?"; }
int det_bit(int i) { return i >= 0 && i < D_GROUPS ? 1 << i : 0; }

int ev_group(int type) {
  switch (type) {
    case G_OPEN_HAND: return D_OPEN;
    case G_FIST: return D_FIST;
    case G_BLINK: case G_WINK_L: case G_WINK_R: case G_EYES_CLOSED: case G_EYES_OPEN: return D_EYES;
    case G_FACE_IN: case G_FACE_OUT: return D_FACE;
    case G_HEAD_LEFT: case G_HEAD_RIGHT: case G_HEAD_UP: case G_HEAD_DOWN: case G_HEAD_CENTER:
    case G_NOD: case G_SHAKE: case G_TILT_L: case G_TILT_R: return D_HEAD;
    case G_FINGERS: case G_HAND_IN: case G_HAND_OUT: case G_HAND_UP: return D_FINGERS;
    case G_SWIPE_L: case G_SWIPE_R: case G_SWIPE_UP: case G_SWIPE_DOWN: case G_WAVE: return D_MOVES;
    case G_MOTION: case G_STILL: return D_MOTION;
  }
  return 0;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* "espalmada,olhos" ou "1,3" -> bits; "todos" = D_ALL. */
int det_parse(const char* s) {
  int bits = 0;
  while (s && *s) {
    char w[24];
    int n = 0, i;
    while (*s == ',' || *s == ' ' || *s == '+') s++;
    while (*s && *s != ',' && *s != '+' && n < 23) w[n++] = (char)lower(*s++);
    w[n] = 0;
    if (!n) break;
    if (strcmp(w, "todos") == 0 || strcmp(w, "tudo") == 0) bits |= D_ALL;
    else if (n == 1 && w[0] >= '1' && w[0] < '1' + D_GROUPS) bits |= 1 << (w[0] - '1');
    else
      for (i = 0; i < D_GROUPS; i++)
        if (strncmp(w, det_keys[i], (size_t)n) == 0) bits |= 1 << i;
  }
  return bits;
}

#define DET_CFG "D:\\TEMP\\GESTURE.CFG"

int det_load(void) {
  FILE* f = fopen(DET_CFG, "r");
  char line[128];
  int bits = D_ALL;
  if (!f) return D_ALL;
  if (fgets(line, sizeof line, f) && strncmp(line, "detectar=", 9) == 0) {
    int b = det_parse(line + 9);
    if (b || strncmp(line + 9, "nada", 4) == 0) bits = b;
  }
  fclose(f);
  return bits;
}

void det_save(int bits) {
  FILE* f = fopen(DET_CFG, "w");
  int i, first = 1;
  if (!f) return;
  fprintf(f, "detectar=");
  for (i = 0; i < D_GROUPS; i++)
    if (bits & (1 << i)) {
      fprintf(f, "%s%s", first ? "" : ",", det_keys[i]);
      first = 0;
    }
  if (first) fprintf(f, "nada");
  fprintf(f, "\n# marcadores do GESTURE: %s\n", "espalmada,fechada,olhos,rosto,cabeca,dedos,deslizar,movimento");
  fclose(f);
}

static FILE* logf;
static unsigned log_t0;

void ev_log_open(const char* path) {
  if (logf) fclose(logf);
  logf = path ? fopen(path, "w") : 0;
  log_t0 = k_ms();
  if (logf) fprintf(logf, "# GESTURE: tempo (s) e gesto\n");
}

void ev_log(const gevent* e) {
  char t[80];
  if (!logf) return;
  ev_text(e, t, (int)sizeof t);
  fprintf(logf, "%u.%02u %s\n", (e->t_ms - log_t0) / 1000, (e->t_ms - log_t0) % 1000 / 10, t);
  fflush(logf);
}

void ev_log_close(void) {
  if (logf) fclose(logf);
  logf = 0;
}
