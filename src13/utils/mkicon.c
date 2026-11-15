/* mkicon.c - converte um PNG de tema em .ICN (ferramenta do Linux).
 *   gcc -D__export= -idirafter include -o mkicon img/img.c mkicon.c
 *   mkicon entrada.png saida.ICN
 * O arquivo e "ICN1" + largura e altura em 16 bits + RGBA, sem escala: o
 * kernel (e o wg_icon do GUI.DLL) reduzem para o tamanho de cada lugar.
 * Lado maximo 128. O TEMAS.EXE faz a mesma conversao dentro do SODOS. */
#include <img.h>
#include <stdio.h>
#include <stdlib.h>

static void put16(FILE* f, int v) {
  fputc(v & 255, f);
  fputc((v >> 8) & 255, f);
}

int main(int argc, char** argv) {
  FILE* f;
  unsigned char* buf;
  long n;
  img_t im;
  if (argc < 3) {
    fputs("uso: mkicon entrada.png saida.ICN\n", stderr);
    return 1;
  }
  f = fopen(argv[1], "rb");
  if (!f) {
    fprintf(stderr, "mkicon: nao abriu %s\n", argv[1]);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = (unsigned char*)malloc((size_t)(n > 0 ? n : 1));
  if (!buf || n < 8 || (long)fread(buf, 1, (size_t)n, f) != n) {
    fclose(f);
    free(buf);
    return 1;
  }
  fclose(f);
  im.rgba = 0;
  if (img_decode(buf, (int)n, &im) != 0) {
    fprintf(stderr, "mkicon: %s nao decodificou\n", argv[1]);
    free(buf);
    return 1;
  }
  free(buf);
  if (im.w < 1 || im.h < 1 || im.w > 128 || im.h > 128) {
    fprintf(stderr, "mkicon: %s tem %dx%d (maximo 128)\n", argv[1], im.w, im.h);
    img_free(&im);
    return 1;
  }
  f = fopen(argv[2], "wb");
  if (!f) {
    img_free(&im);
    return 1;
  }
  fwrite("ICN1", 1, 4, f);
  put16(f, im.w);
  put16(f, im.h);
  fwrite(im.rgba, 1, (size_t)im.w * (size_t)im.h * 4, f);
  fclose(f);
  img_free(&im);
  return 0;
}
