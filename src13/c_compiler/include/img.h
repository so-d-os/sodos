/* img.h - IMG.DLL: le ICO, GIF, PNG e JPEG para um mapa RGBA. */
#ifndef IMG_H
#define IMG_H

typedef struct {
  int w, h;
  unsigned char* rgba; /* w*h*4 bytes, R G B A. A=0 e transparente */
} img_t;

/* 0 = ok. A saida tem no maximo 800 pixels de lado (a origem e reduzida). */
int img_decode(const void* data, int len, img_t* out);
void img_free(img_t* im);

#endif
