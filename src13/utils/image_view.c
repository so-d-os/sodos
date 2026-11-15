/* image_view.c - visualizador de imagem do WIN.
 *   cc -o imagevw.exe image_view.c gui.dll imgbig.dll
 * Abre o arquivo passado na linha de comando (D:\FOTO.PNG).
 *
 * Usa o IMGBIG.DLL: a imagem fica na resolucao original (ate 3840 px de
 * lado). O IMG.DLL normal reduz a 800 px e estragava os screenshots.
 *
 * Zoom: roda do rato (no ponto do cursor), + e -, 0 ajusta a janela, 1 = 100%.
 * Arrastar com o botao esquerdo ou as setas movem a imagem ampliada.
 * A barra do topo mostra o percentual e tem os botoes - + Ajustar 100%.
 * Esc, q ou o X fecham. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui.h>
#include <img.h>

#define MAX_FILE (48 * 1024 * 1024)

/* Niveis de zoom em por mil (1000 = 100%). */
static const int zlevels[] = {50,   100,  125,  167,  250,  333,  500,  667,  750, 1000,
                              1250, 1500, 2000, 3000, 4000, 6000, 8000, 12000, 16000};
#define NZ ((int)(sizeof(zlevels) / sizeof(zlevels[0])))
#define ZMIN 20
#define ZMAX 16000

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
  if (n < 8 || n > MAX_FILE) {
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

/* ---- estado da visao ---- */
static img_t im;
static int zoom = 1000;   /* por mil */
static int fit_mode = 1;  /* segue o tamanho da janela */
static int px, py;        /* canto da imagem na area de desenho (pode ser < 0) */
static int area_y, area_h, area_w; /* area entre a barra do topo e a de baixo */

/* Reducao com media (caixa): texto de screenshot fica legivel abaixo de 100%.
 * Guardada para o zoom atual; refeita so quando o zoom muda. */
static unsigned char* small_px;
static int small_w, small_h, small_zoom;

static int img_dw(void) { return (int)((long long)im.w * zoom / 1000); }
static int img_dh(void) { return (int)((long long)im.h * zoom / 1000); }

static void build_small(void) {
  int dw = img_dw(), dh = img_dh(), x, y;
  if (small_px && small_zoom == zoom) return;
  free(small_px);
  small_px = 0;
  small_zoom = zoom;
  if (zoom >= 1000 || dw < 1 || dh < 1) return;
  small_px = (unsigned char*)malloc((size_t)dw * (size_t)dh * 4);
  if (!small_px) return;
  small_w = dw;
  small_h = dh;
  for (y = 0; y < dh; y++) {
    int sy0 = (int)((long long)y * im.h / dh);
    int sy1 = (int)((long long)(y + 1) * im.h / dh);
    unsigned char* d = small_px + (size_t)y * dw * 4;
    if (sy1 <= sy0) sy1 = sy0 + 1;
    for (x = 0; x < dw; x++) {
      int sx0 = (int)((long long)x * im.w / dw);
      int sx1 = (int)((long long)(x + 1) * im.w / dw);
      unsigned r = 0, g = 0, b = 0, a = 0, n = 0;
      int sx, sy;
      if (sx1 <= sx0) sx1 = sx0 + 1;
      for (sy = sy0; sy < sy1; sy++) {
        const unsigned char* s = im.rgba + ((size_t)sy * im.w + sx0) * 4;
        for (sx = sx0; sx < sx1; sx++, s += 4) {
          r += s[0];
          g += s[1];
          b += s[2];
          a += s[3];
          n++;
        }
      }
      d[0] = (unsigned char)(r / n);
      d[1] = (unsigned char)(g / n);
      d[2] = (unsigned char)(b / n);
      d[3] = (unsigned char)(a / n);
      d += 4;
    }
  }
}

/* Mantem a imagem dentro da area: centrada se cabe, sem sobrar borda se nao. */
static void clamp_pos(void) {
  int dw = img_dw(), dh = img_dh();
  if (dw <= area_w) px = (area_w - dw) / 2;
  else {
    if (px > 0) px = 0;
    if (px < area_w - dw) px = area_w - dw;
  }
  if (dh <= area_h) py = (area_h - dh) / 2;
  else {
    if (py > 0) py = 0;
    if (py < area_h - dh) py = area_h - dh;
  }
}

static int fit_zoom(void) {
  long long zw, zh, z;
  if (im.w < 1 || im.h < 1) return 1000;
  zw = (long long)(area_w - 8) * 1000 / im.w;
  zh = (long long)(area_h - 8) * 1000 / im.h;
  z = zw < zh ? zw : zh;
  if (z > 1000) z = 1000; /* imagem pequena: 100%, sem ampliar */
  if (z < ZMIN) z = ZMIN;
  return (int)z;
}

/* Novo zoom mantendo o ponto (ax, ay) da area no mesmo lugar da imagem. */
static void set_zoom(int nz, int ax, int ay) {
  long long ix, iy;
  if (nz < ZMIN) nz = ZMIN;
  if (nz > ZMAX) nz = ZMAX;
  if (nz == zoom) return;
  ix = (long long)(ax - px) * 1000 / zoom; /* ponto na imagem original */
  iy = (long long)(ay - py) * 1000 / zoom;
  zoom = nz;
  px = ax - (int)(ix * zoom / 1000);
  py = ay - (int)(iy * zoom / 1000);
  fit_mode = 0;
  clamp_pos();
}

static int next_level(int dir) {
  int i;
  if (dir > 0) {
    for (i = 0; i < NZ; i++)
      if (zlevels[i] > zoom) return zlevels[i];
    return ZMAX;
  }
  for (i = NZ - 1; i >= 0; i--)
    if (zlevels[i] < zoom) return zlevels[i];
  return ZMIN;
}

/* botoes da barra do topo */
#define B_MINUS 0
#define B_PLUS 1
#define B_FIT 2
#define B_ONE 3
static int bx[4], bw[4], by, bh;

static void layout(int sw, int sh, int ch) {
  int bar = ch + 10;
  int cw = gui_char_w();
  int x;
  area_y = bar;
  area_h = sh - bar - (ch + 8);
  area_w = sw;
  if (area_h < 1) area_h = 1;
  by = 3;
  bh = bar - 6;
  x = 8;
  bw[B_MINUS] = cw * 3;
  bw[B_PLUS] = cw * 3;
  bw[B_FIT] = cw * 9;
  bw[B_ONE] = cw * 6;
  bx[B_MINUS] = x;
  x += bw[B_MINUS] + 4;
  bx[B_PLUS] = x;
  x += bw[B_PLUS] + 4;
  bx[B_FIT] = x;
  x += bw[B_FIT] + 4;
  bx[B_ONE] = x;
}

static void draw(const char* title, int ok, int sw, int sh, int ch) {
  char msg[200];
  int barh = area_y;
  gui_fill(0, area_y, sw, area_h, 0x202020);
  if (ok) {
    int dw = img_dw(), dh = img_dh();
    build_small();
    if (small_px && zoom < 1000)
      gui_image(px, area_y + py, small_w, small_h, small_px, small_w, small_h, 0, area_y, area_w,
                area_h);
    else
      gui_image(px, area_y + py, dw, dh, im.rgba, im.w, im.h, 0, area_y, area_w, area_h);
  } else {
    snprintf(msg, sizeof msg, "Nao foi possivel abrir %s", title);
    gui_text(16, area_y + 16, msg, -1, 0xFFFFFF, 0x202020, GUI_TEXT_NOBG);
  }

  /* barra do topo: botoes e percentual */
  gui_fill(0, 0, sw, barh, 0x303030);
  gui_fill(0, barh - 1, sw, 1, 0x505050);
  gui_button(bx[B_MINUS], by, bw[B_MINUS], bh, "-", ok);
  gui_button(bx[B_PLUS], by, bw[B_PLUS], bh, "+", ok);
  gui_button(bx[B_FIT], by, bw[B_FIT], bh, "Ajustar", ok);
  gui_button(bx[B_ONE], by, bw[B_ONE], bh, "100%", ok);
  if (ok) {
    int pct10 = zoom; /* por mil = decimos de % */
    if (pct10 % 10 == 0)
      snprintf(msg, sizeof msg, "Zoom %d%%%s", pct10 / 10, fit_mode ? " (ajustado)" : "");
    else
      snprintf(msg, sizeof msg, "Zoom %d,%d%%%s", pct10 / 10, pct10 % 10,
               fit_mode ? " (ajustado)" : "");
    gui_text(bx[B_ONE] + bw[B_ONE] + 16, (barh - ch) / 2, msg, -1, 0xFFFFFF, 0x303030,
             GUI_TEXT_NOBG | GUI_TEXT_BOLD);
  }

  /* barra de baixo */
  gui_fill(0, sh - ch - 8, sw, ch + 8, 0x101010);
  if (ok)
    snprintf(msg, sizeof msg, "%s  %dx%d   roda/+/- zoom  0 ajusta  1 = 100%%  arraste move  Esc fecha",
             title, im.w, im.h);
  else
    snprintf(msg, sizeof msg, "Esc fecha");
  gui_text_fit(8, sh - ch - 4, msg, sw - 16, 0xE0E0E0, 0x101010, GUI_TEXT_NOBG);
  gui_present();
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : 0;
  const char* title;
  unsigned char* data = 0;
  int len = 0;
  int ok = 0;
  int drag = 0, dx0 = 0, dy0 = 0, px0 = 0, py0 = 0;
  int last_sw = -1, last_sh = -1;
  im.w = im.h = 0;
  im.rgba = 0;
  title = base_name(path);
  if (path && load_file(path, &data, &len) == 0 && img_decode(data, len, &im) == 0) ok = 1;
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
    int step;
    layout(sw, sh, ch);
    if (sw != last_sw || sh != last_sh) {
      last_sw = sw;
      last_sh = sh;
      if (fit_mode) zoom = fit_zoom();
      clamp_pos();
    }
    draw(title, ok, sw, sh, ch);
    if (gui_wait(&ev, -1) == GUI_EV_NONE) continue;
    if (ev.type == GUI_EV_CLOSE || (ev.type == GUI_EV_KEY && (ev.key == 27 || ev.key == 'q')))
      break;
    if (!ok) continue;
    step = area_w / 10 > 16 ? area_w / 10 : 16;
    switch (ev.type) {
      case GUI_EV_WHEEL:
        /* roda para cima (key < 0) aproxima, no ponto do cursor */
        if (ev.y >= area_y && ev.y < area_y + area_h)
          set_zoom(next_level(ev.key < 0 ? 1 : -1), ev.x, ev.y - area_y);
        else
          set_zoom(next_level(ev.key < 0 ? 1 : -1), area_w / 2, area_h / 2);
        break;
      case GUI_EV_KEY:
        if (ev.key == '+' || ev.key == '=')
          set_zoom(next_level(1), area_w / 2, area_h / 2);
        else if (ev.key == '-' || ev.key == '_')
          set_zoom(next_level(-1), area_w / 2, area_h / 2);
        else if (ev.key == '0' || ev.key == 'f' || ev.key == 'F') {
          fit_mode = 1;
          zoom = fit_zoom();
          clamp_pos();
        } else if (ev.key == '1')
          set_zoom(1000, area_w / 2, area_h / 2);
        else if (ev.key == GUI_KEY_LEFT) px += step;
        else if (ev.key == GUI_KEY_RIGHT) px -= step;
        else if (ev.key == GUI_KEY_UP) py += step;
        else if (ev.key == GUI_KEY_DOWN) py -= step;
        else if (ev.key == GUI_KEY_PGUP) py += area_h - 32;
        else if (ev.key == GUI_KEY_PGDN) py -= area_h - 32;
        else if (ev.key == GUI_KEY_HOME) px = py = 1 << 20;
        else if (ev.key == GUI_KEY_END) px = py = -(1 << 28);
        clamp_pos();
        break;
      case GUI_EV_DOWN:
        if (ev.y < area_y) {
          if (gui_hit(bx[B_MINUS], by, bw[B_MINUS], bh, ev.x, ev.y))
            set_zoom(next_level(-1), area_w / 2, area_h / 2);
          else if (gui_hit(bx[B_PLUS], by, bw[B_PLUS], bh, ev.x, ev.y))
            set_zoom(next_level(1), area_w / 2, area_h / 2);
          else if (gui_hit(bx[B_FIT], by, bw[B_FIT], bh, ev.x, ev.y)) {
            fit_mode = 1;
            zoom = fit_zoom();
            clamp_pos();
          } else if (gui_hit(bx[B_ONE], by, bw[B_ONE], bh, ev.x, ev.y))
            set_zoom(1000, area_w / 2, area_h / 2);
        } else if (ev.key == 1) {
          drag = 1;
          dx0 = ev.x;
          dy0 = ev.y;
          px0 = px;
          py0 = py;
        }
        break;
      case GUI_EV_DRAG:
        if (drag) {
          px = px0 + (ev.x - dx0);
          py = py0 + (ev.y - dy0);
          clamp_pos();
        }
        break;
      case GUI_EV_UP:
        drag = 0;
        break;
    }
  }
  gui_close();
  free(small_px);
  img_free(&im);
  return ok ? 0 : 1;
}
