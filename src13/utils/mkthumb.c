/* mkthumb.c - gera uma miniatura THM1 para o desktop (ferramenta do Linux).
 *   gcc -m32 -D__export= -Iinclude -o mkthumb img/img.c mkthumb.c
 *   mkthumb entrada.png saida.THM
 * O arquivo e "THM1" + largura e altura em 16 bits + RGBA. O lado maior cabe em 48. */
#include <img.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SIDE 48

static void put16(FILE* f, int v) {
  unsigned char b[2];
  b[0] = (unsigned char)(v & 255);
  b[1] = (unsigned char)((v >> 8) & 255);
  fwrite(b, 1, 2, f);
}

int main(int argc, char** argv) {
  FILE* in;
  FILE* out;
  unsigned char* buf;
  long n;
  img_t im;
  int step, ow, oh, y, x;
  if (argc < 3) return 1;
  in = fopen(argv[1], "rb");
  if (!in) {
    if (write(2, "mkthumb: nao abriu\n", 19) < 0) return 1;
    return 1;
  }
  fseek(in, 0, SEEK_END);
  n = ftell(in);
  fseek(in, 0, SEEK_SET);
  if (n < 8 || n > 8 * 1024 * 1024) {
    if (write(2, "mkthumb: tamanho\n", 17) < 0) return 1;
    fclose(in);
    return 1;
  }
  buf = (unsigned char*)malloc((size_t)n);
  if (!buf || (long)fread(buf, 1, (size_t)n, in) != n) {
    free(buf);
    fclose(in);
    return 1;
  }
  fclose(in);
  im.rgba = 0;
  if (img_decode(buf, (int)n, &im) != 0) {
    if (write(2, "mkthumb: decode falhou\n", 23) < 0) return 1;
    free(buf);
    return 1;
  }
  free(buf);
  step = 1;
  while ((im.w + step - 1) / step > SIDE || (im.h + step - 1) / step > SIDE) step++;
  ow = (im.w + step - 1) / step;
  oh = (im.h + step - 1) / step;
  if (ow < 1) ow = 1;
  if (oh < 1) oh = 1;
  out = fopen(argv[2], "wb");
  if (!out) {
    img_free(&im);
    return 1;
  }
  fwrite("THM1", 1, 4, out);
  put16(out, ow);
  put16(out, oh);
  for (y = 0; y < oh; y++) {
    int sy = y * step;
    if (sy >= im.h) sy = im.h - 1;
    for (x = 0; x < ow; x++) {
      int sx = x * step;
      unsigned char* p;
      if (sx >= im.w) sx = im.w - 1;
      p = im.rgba + ((size_t)sy * (size_t)im.w + (size_t)sx) * 4;
      fwrite(p, 1, 4, out);
    }
  }
  fclose(out);
  img_free(&im);
  return 0;
}
