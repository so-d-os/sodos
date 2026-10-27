#ifndef TTFRAS_H
#define TTFRAS_H

#include <stdint.h>

/* Desenha phrase com o TTF em memoria. *rgba sai em R,G,B,A (o chamador da free).
 * Devolve 0 quando a frase foi desenhada. */
int ttf_phrase(const uint8_t* data, int data_len, const char* phrase, int px,
               uint8_t** rgba, int* out_w, int* out_h,
               char* name, int name_n, char* full, int full_n,
               char* ver, int ver_n, char* copyr, int copy_n,
               char* tm, int tm_n);

#endif
