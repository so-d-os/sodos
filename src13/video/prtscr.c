/* prtscr.c - Tecla PrintScreen no WIN: a tela inteira vira PNG em
 * Z:\SAVE\SCREENSHOT\printscreen-AAAAMMDD-HHMMSS.png (Z: e o ramdisk).
 *
 * O nome tem 31 caracteres: cabe inteiro no k_dirent dos programas (o
 * DISKSYNC.EXE copia com o mesmo nome para o pendrive). A hora e a do RTC,
 * a mesma do relogio da barra de tarefas. Dois prints no mesmo segundo
 * andam o segundo para frente ate achar um nome livre.
 *
 * Depois do Z: (que some no boot) vai uma copia para D:\TEMP, para nao ter
 * que copiar a mao. SET PRTSCRCOPY=pasta troca o destino; SET PRTSCRCOPY=0
 * desliga.
 */
#include <stdint.h>
#include "prtscr.h"
#include "png.h"
#include "mm.h"
#include "drives.h"
#include "serial.h"
#include "kexports.h"
#include "env.h"

int win_screen_size(int* w, int* h);
int win_screen_bgr(int y, uint8_t* dst, int max);

#define SHOT_DIR "Z:\\SAVE\\SCREENSHOT"
#define SHOT_LEVEL 4   /* deflate: rapido o bastante para nao travar o WIN */

static char* put2(char* p, int v) {
  *p++ = (char)('0' + (v / 10) % 10);
  *p++ = (char)('0' + v % 10);
  return p;
}

/* Z:\SAVE\SCREENSHOT\printscreen-AAAAMMDD-HHMMSS.png */
static void shot_name(char* out, int date, int t) {
  const char* d = SHOT_DIR "\\printscreen-";
  char* p = out;
  while (*d) *p++ = *d++;
  p = put2(p, date / 1000000);
  p = put2(p, (date / 10000) % 100);
  p = put2(p, (date / 100) % 100);
  p = put2(p, date % 100);
  *p++ = '-';
  p = put2(p, t / 10000);
  p = put2(p, (t / 100) % 100);
  p = put2(p, t % 100);
  d = ".png";
  while (*d) *p++ = *d++;
  *p = 0;
}

/* Proximo segundo (sem mudar o dia: depois de 23:59:59 fica la). */
static int next_sec(int t) {
  int h = t / 10000, m = (t / 100) % 100, s = t % 100;
  if (++s > 59) {
    s = 0;
    if (++m > 59) {
      m = 0;
      if (h < 23) h++;
      else return 235959;
    }
  }
  return h * 10000 + m * 100 + s;
}

/* copia em D:\TEMP (ou SET PRTSCRCOPY=pasta; =0 desliga), mesmo nome */
static void shot_copy(const char* zpath, const uint8_t* png, int len) {
  const char* dir = env_get("PRTSCRCOPY");
  const char* name = zpath;
  const char* p;
  char dst[160];
  int n = 0, rc;
  if (!dir || !*dir) dir = "D:\\TEMP";
  if (dir[0] == '0' && !dir[1]) return;
  if (!drives_is_mounted(dir[0] & ~32)) return;
  for (p = zpath; *p; p++)
    if (*p == '\\') name = p + 1;
  drives_mkdir(dir);
  for (p = dir; *p && n < 120; p++) dst[n++] = *p;
  if (n && dst[n - 1] != '\\') dst[n++] = '\\';
  for (p = name; *p && n < 158; p++) dst[n++] = *p;
  dst[n] = 0;
  rc = file_save(dst, (const char*)png, len);
  serial_puts("[prtscr] copia ");
  serial_puts(dst);
  serial_puts(rc == 0 ? " ok\n" : " ERRO ao gravar\n");
}

int prtscr_save(char* out_path, int max) {
  int w = 0, h = 0, y, x, n, len = 0, date, t, tries, rc;
  uint8_t* row;
  uint8_t* rgb;
  uint8_t* png;
  char path[80];
  if (!win_screen_size(&w, &h) || w < 1 || h < 1) return -1;
  if (!drives_is_mounted('Z')) {
    serial_puts("[prtscr] sem Z:\n");
    return -2;
  }
  date = rtc_date();
  t = rtc_time();
  if (date <= 0) date = 19800101;
  if (t < 0) t = 0;
  drives_mkdir("Z:\\SAVE");
  drives_mkdir(SHOT_DIR);
  shot_name(path, date, t);
  for (tries = 0; tries < 60 && file_size(path) >= 0; tries++) {
    t = next_sec(t);
    shot_name(path, date, t);
  }

  row = (uint8_t*)kmalloc((uint32_t)(w * 3));
  rgb = (uint8_t*)umalloc_owned((uint32_t)w * (uint32_t)h * 3u, 0);
  if (!row || !rgb) {
    if (row) kfree(row);
    if (rgb) ufree(rgb);
    serial_puts("[prtscr] sem memoria\n");
    return -3;
  }
  for (y = 0; y < h; y++) {
    uint8_t* d = rgb + (uint32_t)y * (uint32_t)w * 3u;
    n = win_screen_bgr(y, row, w);
    if (n < 0) n = 0;
    for (x = 0; x < n; x++) {
      d[x * 3 + 0] = row[x * 3 + 2];
      d[x * 3 + 1] = row[x * 3 + 1];
      d[x * 3 + 2] = row[x * 3 + 0];
    }
    for (x = n * 3; x < w * 3; x++) d[x] = 0;
  }
  kfree(row);
  png = png_encode_rgb(rgb, w, h, w * 3, SHOT_LEVEL, &len);
  ufree(rgb);
  if (!png) {
    serial_puts("[prtscr] PNG falhou\n");
    return -3;
  }
  rc = file_save(path, (const char*)png, len);
  serial_puts("[prtscr] ");
  serial_puts(path);
  serial_puts(rc == 0 ? " ok\n" : " ERRO ao gravar\n");
  if (rc != 0) {
    ufree(png);
    return -4;
  }
  shot_copy(path, png, len);
  ufree(png);
  if (out_path && max > 0) {
    for (x = 0; path[x] && x < max - 1; x++) out_path[x] = path[x];
    out_path[x] = 0;
  }
  return len;
}
