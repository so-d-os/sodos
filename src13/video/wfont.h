/* wfont.h - fonte TrueType do WIN (titulos de janela e rotulos do desktop).
 *
 * wfont_build le um TTF ja em memoria (contornos glyf, cmap formato 4) e
 * desenha os caracteres ASCII 32..126 uma vez, no tamanho pedido, com
 * suavizacao (4x4 amostras por pixel, regra de enrolamento do TrueType). So
 * usa inteiros: roda no kernel (-mno-sse). Depois o TTF pode ser liberado. */
#ifndef WFONT_H
#define WFONT_H

#include <stdint.h>

typedef void* (*wfont_alloc_fn)(uint32_t n);
typedef void (*wfont_free_fn)(void* p);

/* 0 = pronto. Chamar de novo troca a fonte/tamanho (libera a anterior). */
int wfont_build(const uint8_t* ttf, int len, int px, wfont_alloc_fn alloc, wfont_free_fn release);
void wfont_drop(void);
int wfont_ready(void);
int wfont_px(void);
int wfont_ascent(void);   /* pixels acima da linha de base */
int wfont_height(void);   /* altura da linha (subida + descida) */
/* Largura do texto em pixels (avanco de cada letra). */
int wfont_width(const char* s);

struct wfont_glyph {
  int16_t left;    /* coluna do 1o pixel em relacao a caneta */
  int16_t top;     /* linhas acima da linha de base */
  int16_t w, h;    /* tamanho da mascara */
  int32_t adv;     /* avanco da caneta em 1/256 de pixel */
  const uint8_t* a;  /* alfa 0..255, w*h, linha a linha */
};

/* Glifo do caractere c ('?' para o que nao ha). */
const struct wfont_glyph* wfont_glyph(unsigned char c);

#endif
