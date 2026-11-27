/* camtest.c - testa o stream da webcam pelos exports k_cam_* (o caminho que o
 * WINE vai usar): lista os modos, liga, le quadros por N segundos, mede
 * quadros/s e grava o ultimo quadro.
 *
 *   CAMTEST [largura altura [segundos [formato]]]   (formato 1 MJPEG, 2 YUY2)
 *
 * Relatorio em D:\TEMP\CAMTEST.TXT; ultimo quadro em D:\TEMP\CAMTEST.JPG
 * (ou .YUV). Rode por "command": leva alguns segundos. */
#include <so.h>
#include <stdio.h>
#include <stdlib.h>

/* Exports novos (build 79): o linker ainda nao os conhece; busca na hora. */
static int (*p_cam_modes)(unsigned int* out, int max);
static int (*p_cam_open)(int kind, int w, int h, int* maxlen);
static int (*p_cam_frame)(void* buf, int max, int* seq, int timeout_ms);
static int (*p_cam_close)(void);

int main(int argc, char** argv) {
  unsigned int m[4 * 32];
  int n, i, w = 640, h = 480, secs = 10, kind = 1, maxlen = 0, seq = 0, r;
  int frames = 0, skipped = 0, timeouts = 0, last = 0, minb = 0x7FFFFFFF, maxb = 0;
  unsigned t0, t1, worst = 0, prev;
  unsigned char* buf;
  FILE* rep = fopen("D:\\TEMP\\CAMTEST.TXT", "w");
  FILE* f;
  if (!rep) rep = stdout;
  p_cam_modes = (int (*)(unsigned int*, int))k_dl_sym(0, "k_cam_modes", 0);
  p_cam_open = (int (*)(int, int, int, int*))k_dl_sym(0, "k_cam_open", 0);
  p_cam_frame = (int (*)(void*, int, int*, int))k_dl_sym(0, "k_cam_frame", 0);
  p_cam_close = (int (*)(void))k_dl_sym(0, "k_cam_close", 0);
  if (!p_cam_modes || !p_cam_open || !p_cam_frame || !p_cam_close) {
    fprintf(rep, "kernel sem k_cam_* (precisa do build 79)\n");
    if (rep != stdout) fclose(rep);
    return 1;
  }
  if (argc >= 3) {
    w = atoi(argv[1]);
    h = atoi(argv[2]);
  }
  if (argc >= 4) secs = atoi(argv[3]);
  if (argc >= 5) kind = atoi(argv[4]);
  n = p_cam_modes(m, 32);
  fprintf(rep, "modos: %d\n", n);
  for (i = 0; i < n; i++)
    fprintf(rep, "  %s %ux%u %u q/s\n", m[i * 4] == 1 ? "MJPEG" : "YUY2", m[i * 4 + 1], m[i * 4 + 2],
            m[i * 4 + 3] ? (10000000u + m[i * 4 + 3] / 2) / m[i * 4 + 3] : 0);
  r = p_cam_open(kind, w, h, &maxlen);
  fprintf(rep, "k_cam_open(%d, %d, %d) = %d, maxlen %d\n", kind, w, h, r, maxlen);
  if (r != 0) {
    if (rep != stdout) fclose(rep);
    return 1;
  }
  buf = (unsigned char*)malloc((unsigned)maxlen);
  if (!buf) {
    fprintf(rep, "sem memoria\n");
    p_cam_close();
    return 1;
  }
  t0 = prev = k_ms();
  while (k_ms() - t0 < (unsigned)secs * 1000u) {
    int before = seq;
    r = p_cam_frame(buf, maxlen, &seq, 1000);
    if (r < 0) {
      fprintf(rep, "k_cam_frame = %d (desligou)\n", r);
      break;
    }
    if (r == 0) {
      timeouts++;
      continue;
    }
    t1 = k_ms();
    if (frames && t1 - prev > worst) worst = t1 - prev;
    prev = t1;
    if (frames && seq - before > 1) skipped += seq - before - 1;
    frames++;
    last = r;
    if (r < minb) minb = r;
    if (r > maxb) maxb = r;
  }
  t1 = k_ms();
  r = p_cam_close();
  fprintf(rep, "quadros %d em %u ms = %u.%u q/s; pulados %d; esperas vazias %d\n", frames, t1 - t0,
          frames * 1000u / (t1 - t0 ? t1 - t0 : 1), (frames * 10000u / (t1 - t0 ? t1 - t0 : 1)) % 10u,
          skipped, timeouts);
  fprintf(rep, "maior intervalo %u ms; bytes por quadro %d..%d; p_cam_close = %d\n", worst,
          frames ? minb : 0, maxb, r);
  if (last > 0) {
    f = fopen(kind == 2 ? "D:\\TEMP\\CAMTEST.YUV" : "D:\\TEMP\\CAMTEST.JPG", "wb");
    if (f) {
      fwrite(buf, 1, (unsigned)last, f);
      fclose(f);
      fprintf(rep, "ultimo quadro gravado (%d bytes)\n", last);
    }
  }
  fprintf(rep, "FIM\n");
  if (rep != stdout) fclose(rep);
  return 0;
}
