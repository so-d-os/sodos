/* clip.c - Area de transferencia. Fica no kernel para Ctrl+C no Command ou
 * no Bloco de notas do WIN e Ctrl+V no prompt do DOS (e o inverso). */
#include "clip.h"

static char clip_buf[CLIP_MAX];
static int clip_n;

void clip_set(const char* s, int n) {
  int i;
  if (!s) n = 0;
  if (n < 0) {
    n = 0;
    while (s[n] && n < CLIP_MAX - 1) n++;
  }
  if (n > CLIP_MAX - 1) n = CLIP_MAX - 1;
  for (i = 0; i < n; i++) clip_buf[i] = s[i];
  clip_buf[n] = '\0';
  clip_n = n;
}

int clip_get(char* out, int max) {
  int i;
  if (!out || max < 1) return 0;
  for (i = 0; i < clip_n && i < max - 1; i++) out[i] = clip_buf[i];
  out[i] = '\0';
  return i;
}

int clip_len(void) { return clip_n; }
