#include "shell.h"
#include "audio.h"
#include "mm.h"

/* AUDIO              estado: driver, placa, codec, saidas, volume
 * AUDIO TESTE [hz] [ms]   toca um tom (440 Hz, 1 s), sem esperar
 * AUDIO VOL [0..100]      le ou muda o volume
 * Ligar e desligar e com SET AUDIO=1 / SET AUDIO=0 (docs/AUDIO.md). */

static int same(const char* a, const char* b) {
  while (*a && *b) {
    char x = *a++, y = *b++;
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
  }
  return *a == 0 && *b == 0;
}

static int num(const char* s, int def) {
  int v = 0, any = 0;
  while (s && *s >= '0' && *s <= '9') {
    v = v * 10 + (*s++ - '0');
    any = 1;
    if (v > 1000000) break;
  }
  return any ? v : def;
}

static void put_num(int v) {
  char t[12];
  int i = 0;
  if (v < 0) {
    putc('-');
    v = -v;
  }
  do t[i++] = (char)('0' + v % 10); while ((v /= 10) && i < 11);
  while (i) putc(t[--i]);
}

int cmd_audio(int argc, char** argv) {
  char* out;
  if (argc >= 2 && (same(argv[1], "teste") || same(argv[1], "test") || same(argv[1], "beep"))) {
    int hz = num(argc >= 3 ? argv[2] : 0, 440), ms = num(argc >= 4 ? argv[3] : 0, 1000);
    if (k_audio_beep(hz, ms) != 0) {
      puts_k("Sem som: AUDIO desligado, sem placa, ou um programa esta tocando. Veja: AUDIO\n");
      return 1;
    }
    puts_k("Tocando ");
    put_num(hz);
    puts_k(" Hz por ");
    put_num(ms);
    puts_k(" ms.\n");
    return 0;
  }
  if (argc >= 2 && (same(argv[1], "vol") || same(argv[1], "volume"))) {
    int v = k_audio_volume(argc >= 3 ? num(argv[2], -1) : -1);
    if (v < 0) {
      puts_k("Sem volume: AUDIO desligado ou sem placa.\n");
      return 1;
    }
    puts_k("Volume ");
    put_num(v);
    puts_k("%\n");
    return 0;
  }
  if (argc >= 2) {
    puts_k("Uso: AUDIO | AUDIO TESTE [hz] [ms] | AUDIO VOL [0..100]\n"
           "Liga: SET AUDIO=1   Desliga: SET AUDIO=0\n");
    return 1;
  }
  /* sem buffer estatico: o TROCA KERNEL nao precisa achar variavel nova */
  out = (char*)umalloc_owned(4096, 0);
  if (!out) return 1;
  k_audio_info(out, 4096);
  puts_k(out);
  ufree(out);
  return 0;
}
