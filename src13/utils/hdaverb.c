/* hdaverb.c - HDAVERB.EXE: manda um verbo cru ao codec HD Audio em uso
 * (diagnostico do AUDIO.DLL, docs/AUDIO.md). Precisa de SET AUDIO=1.
 *
 *   HDAVERB no verbo [dado]     tudo em hexadecimal
 *
 * verbo de 12 bits (700..FFF): dado de 8 bits.  Ex.: HDAVERB 17 F07
 * verbo de 4 bits (2, 3, A, B): dado de 16 bits. Ex.: HDAVERB 10 B A000
 * A resposta sai em hexadecimal. -1 = sem placa ou o codec nao respondeu.
 *
 *   HDAVERB /T ms               espera ms milissegundos (decimal) sem prender
 *                               o desktop: para .BAT de teste sem PAUSE (o
 *                               PAUSE le o teclado em laco e congela o WIN)
 */
#include <stdio.h>
#include <so.h>

int k_audio_volume(int pct);

static unsigned long hex(const char* s) {
  unsigned long v = 0;
  for (; *s; s++) {
    int c = *s, d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else break;
    v = v * 16 + (unsigned long)d;
  }
  return v;
}

int main(int argc, char** argv) {
  unsigned long nid, verb, data, v;
  int r;
  if (argc >= 3 && (argv[1][0] == '/' || argv[1][0] == '-') && (argv[1][1] == 'T' || argv[1][1] == 't')) {
    static volatile int nunca;
    int ms = 0;
    const char* p = argv[2];
    unsigned t0;
    while (*p >= '0' && *p <= '9') ms = ms * 10 + (*p++ - '0');
    if (ms > 60000) ms = 60000;
    t0 = k_ms();
    while ((int)(k_ms() - t0) < ms)
      k_futex_wait(&nunca, 0, ms - (int)(k_ms() - t0));   /* dorme: ninguem acorda */
    return 0;
  }
  if (argc < 3) {
    printf("Uso: HDAVERB no verbo [dado]   (hexadecimal; SET AUDIO=1)\n");
    return 1;
  }
  nid = hex(argv[1]);
  verb = hex(argv[2]);
  data = argc > 3 ? hex(argv[3]) : 0;
  if (nid > 0x7F) {
    printf("HDAVERB: no fora do alcance\n");
    return 1;
  }
  if (verb <= 0xF) v = nid << 20 | verb << 16 | (data & 0xFFFF);
  else v = nid << 20 | (verb & 0xFFF) << 8 | (data & 0xFF);
  r = k_audio_volume(-2 - (int)v);
  printf("no %02lX verbo %lX dado %lX -> %08X\n", nid, verb, data, (unsigned)r);
  return r == -1 ? 1 : 0;
}
