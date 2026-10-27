/* image_view.c - visualizador de imagem do WIN.
 *   cc -o imagevw.exe image_view.c gui.dll img.dll
 * Abre o arquivo passado na linha de comando (D:\FOTO.PNG).
 * Esc ou o X fecham. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui.h>
#include <img.h>

static const char* base_name(const char* path) {
  const char* s = path;
  const char* b = path;
  if (!path || !path[0]) return "(sem arquivo)";
  for (; *s; s++)
    if (*s == '\\' || *s == '/' || *s == ':') b = s + 1;
  return b[0] ? b : path;
}

static int load_file(const char* path, unsigned char** data, int* len) {
  FILE* f;
  long n;
  unsigned char* buf;
  *data = 0;
  *len = 0;
  f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 8 || n > 4 * 1024 * 1024) {
    fclose(f);
    return -1;
  }
  buf = (unsigned char*)malloc((size_t)n);
  if (!buf) {
    fclose(f);
    return -1;
  }
  if ((long)fread(buf, 1, (size_t)n, f) != n) {
    free(buf);
    fclose(f);
    return -1;
  }
  fclose(f);
  *data = buf;
  *len = (int)n;
  return 0;
}

static void fit_rect(int sw, int sh, int iw, int ih, int* x, int* y, int* w, int* h) {
  int aw = sw - 16;
  int ah = sh - 36;
  if (aw < 1) aw = 1;
  if (ah < 1) ah = 1;
  if (iw < 1) iw = 1;
  if (ih < 1) ih = 1;
  if (iw * ah > ih * aw) {
    *w = aw;
    *h = ih * aw / iw;
  } else {
    *h = ah;
    *w = iw * ah / ih;
  }
  if (*w < 1) *w = 1;
  if (*h < 1) *h = 1;
  *x = (sw - *w) / 2;
  *y = (ah - *h) / 2;
  if (*y < 0) *y = 0;
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : 0;
  const char* title;
  unsigned char* data = 0;
  int len = 0;
  img_t im;
  char msg[160];
  int ok = 0;
  im.w = im.h = 0;
  im.rgba = 0;
  title = base_name(path);
  if (path && load_file(path, &data, &len) == 0 && img_decode(data, len, &im) == 0)
    ok = 1;
  free(data);
  if (gui_open(title) != 0) {
    if (ok) printf("%s %dx%d\n", title, im.w, im.h);
    else printf("nao abriu %s\n", title);
    img_free(&im);
    return ok ? 0 : 1;
  }
  for (;;) {
    gui_event_t ev;
    int sw = gui_width();
    int sh = gui_height();
    int ch = gui_char_h();
    int x, y, w, h;
    gui_fill(0, 0, sw, sh, 0x202020);
    if (ok) {
      fit_rect(sw, sh, im.w, im.h, &x, &y, &w, &h);
      gui_image(x, y, w, h, im.rgba, im.w, im.h, 0, 0, sw, sh - ch - 8);
      snprintf(msg, sizeof msg, "%s  %dx%d   Esc fecha", title, im.w, im.h);
    } else {
      snprintf(msg, sizeof msg, "Nao foi possivel abrir %s", title);
      gui_text(16, 24, msg, -1, 0xFFFFFF, 0x202020, GUI_TEXT_NOBG);
      snprintf(msg, sizeof msg, "Esc fecha");
    }
    gui_fill(0, sh - ch - 8, sw, ch + 8, 0x101010);
    gui_text(8, sh - ch - 4, msg, -1, 0xE0E0E0, 0x101010, GUI_TEXT_NOBG);
    gui_present();
    if (gui_wait(&ev, -1) == GUI_EV_NONE) continue;
    if (ev.type == GUI_EV_CLOSE || (ev.type == GUI_EV_KEY && (ev.key == 27 || ev.key == 'q')))
      break;
  }
  gui_close();
  img_free(&im);
  return ok ? 0 : 1;
}
