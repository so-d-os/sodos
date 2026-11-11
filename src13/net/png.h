/* png.h - codificador PNG (RGB, 8 bits) para o screenshot da API. */
#pragma once
#include <stdint.h>

/* Codifica rgb (3 bytes por pixel, linhas de cima para baixo) em PNG.
 * stride e a distancia em bytes entre o inicio de duas linhas.
 * level 1..9 e o esforco do deflate (1 = mais rapido/maior, 9 = menor).
 * Devolve o buffer alocado no heap alto (libere com ufree) ou NULL.
 * *out_len recebe o tamanho do PNG. */
uint8_t* png_encode_rgb(const uint8_t* rgb, int w, int h, int stride, int level,
                        int* out_len);
