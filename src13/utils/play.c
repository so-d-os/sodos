/* play.c - PLAY.EXE: toca um .WAV pelo som do SODOS (docs/AUDIO.md).
 *
 *   PLAY arquivo.wav   PCM de 8 ou 16 bits, mono ou estereo, 8 a 192 kHz
 *   PLAY /T            escala de teste (sem arquivo): 22050 Hz, mono, 8 bits,
 *                      para passar pela conversao do kernel
 *
 * Com SET AUDIO=0 o kernel usa o driver nulo: o PLAY roda e nada toca.
 * Ctrl+C para (o kernel cala a placa quando o programa termina).
 */
#include <stdio.h>
#include <string.h>
#include <so.h>

/* tambem em so.h (kernels a partir do build 61) */
int k_audio_open(int rate, int channels, int bits);
int k_audio_write(const void* buf, int bytes);
int k_audio_close(int wait);
int k_audio_pending(void);
int k_audio_info(char* out, int max);

#define CHUNK 16384

static unsigned char buf[CHUNK];

static unsigned rd16(const unsigned char* p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static unsigned long rd32(const unsigned char* p) {
  return (unsigned long)p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

static int up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static void status(void) {
  static char info[2048];
  k_audio_info(info, (int)sizeof(info));
  /* so as primeiras linhas: driver e placa */
  {
    int i, nl = 0;
    for (i = 0; info[i] && nl < 2; i++)
      if (info[i] == '\n') nl++;
    info[i] = 0;
  }
  printf("%s", info);
}

static int open_or_say(int rate, int ch, int bits) {
  int r = k_audio_open(rate, ch, bits);
  if (r == -2) printf("PLAY: outro programa esta tocando.\n");
  else if (r == -1) printf("PLAY: formato nao suportado (%d Hz, %d canais, %d bits).\n", rate, ch, bits);
  else if (r < 0) printf("PLAY: a placa recusou o som (%d).\n", r);
  return r;
}

/* Escala de do maior em onda triangular, 22050 Hz mono 8 bits. */
static int teste(void) {
  static const int hz[] = { 262, 294, 330, 349, 392, 440, 494, 523 };
  int n, i;
  if (open_or_say(22050, 1, 8) < 0) return 1;
  status();
  printf("Escala de teste (22050 Hz, mono, 8 bits)...\n");
  for (n = 0; n < 8; n++) {
    int frames = 22050 / 3, per = 22050 / hz[n], off = 0;
    while (off < frames) {
      int k = frames - off > CHUNK ? CHUNK : frames - off;
      for (i = 0; i < k; i++) {
        int t = (off + i) % per, v = t < per / 2 ? t * 4 * 40 / per - 40 : 120 - t * 4 * 40 / per;
        int fade = off + i < 200 ? off + i : frames - (off + i) < 200 ? frames - (off + i) : 200;
        buf[i] = (unsigned char)(128 + v * fade / 200);
      }
      if (k_audio_write(buf, k) < 0) {
        printf("PLAY: a placa parou.\n");
        k_audio_close(0);
        return 1;
      }
      off += k;
    }
  }
  k_audio_close(1);
  printf("Fim.\n");
  return 0;
}

int main(int argc, char** argv) {
  FILE* f;
  unsigned char h[44];
  unsigned long data = 0, left;
  unsigned fmt = 0, ch = 0, bits = 0;
  unsigned long rate = 0;
  int found = 0;
  if (argc < 2) {
    printf("Uso: PLAY arquivo.wav | PLAY /T (escala de teste)\n");
    status();
    return 1;
  }
  if ((argv[1][0] == '/' || argv[1][0] == '-') && up(argv[1][1]) == 'T') return teste();
  f = fopen(argv[1], "rb");
  if (!f) {
    printf("PLAY: nao abri %s\n", argv[1]);
    return 1;
  }
  if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
    printf("PLAY: %s nao e um WAV (RIFF/WAVE)\n", argv[1]);
    fclose(f);
    return 1;
  }
  /* blocos: "fmt " e "data" (os outros sao pulados) */
  while (fread(h, 1, 8, f) == 8) {
    unsigned long sz = rd32(h + 4);
    if (!memcmp(h, "fmt ", 4)) {
      unsigned char fb[40];
      unsigned long n = sz < sizeof(fb) ? sz : sizeof(fb);
      if (fread(fb, 1, n, f) != n) break;
      fmt = rd16(fb);
      ch = rd16(fb + 2);
      rate = rd32(fb + 4);
      bits = rd16(fb + 14);
      if (fmt == 0xFFFEu && n >= 26) fmt = rd16(fb + 24);   /* WAVE_FORMAT_EXTENSIBLE */
      if (sz > n) fseek(f, (long)(sz - n + (sz & 1)), SEEK_CUR);
      else if (sz & 1) fseek(f, 1, SEEK_CUR);
    } else if (!memcmp(h, "data", 4)) {
      data = sz;
      found = 1;
      break;
    } else {
      fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);
    }
  }
  if (!found || fmt != 1) {
    printf("PLAY: %s: so WAV PCM (formato %u)\n", argv[1], fmt);
    fclose(f);
    return 1;
  }
  if (open_or_say((int)rate, (int)ch, (int)bits) < 0) {
    fclose(f);
    return 1;
  }
  status();
  printf("%s: %lu Hz, %u canais, %u bits, %lu s\n", argv[1], rate, ch, bits,
         data / (rate * ch * (bits / 8) ? rate * ch * (bits / 8) : 1));
  left = data;
  while (left > 0) {
    size_t want = left > CHUNK ? CHUNK : (size_t)left;
    size_t got = fread(buf, 1, want, f);
    if (got == 0) break;
    if (k_audio_write(buf, (int)got) < 0) {
      printf("PLAY: a placa parou.\n");
      break;
    }
    left -= got;
  }
  fclose(f);
  k_audio_close(1);
  printf("Fim.\n");
  return 0;
}
