/* webcamv.c - mostra a webcam ao vivo numa janela do WIN. WEBCAMV.EXE
 *   cc -o webcamv.exe webcamv.c gui.dll imgbig.dll
 *
 *   WEBCAMV [largura altura]      (padrao 640 480; "webcam /show" abre este)
 *
 * Le os quadros pelo stream do kernel (k_cam_*, kernel/xhci.c). Com YUY2 a
 * 25 q/s ou mais naquele tamanho, converte direto para RGBA; senao (1280x720,
 * so 10 q/s em YUY2) pede MJPEG e decodifica com o IMGBIG.DLL. A imagem cabe
 * na janela (reduz se precisar). Esc, q ou o X fecham; F (ou f) alterna
 * espelhado. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <img.h>
#include <gui.h>
#include <so.h>

/* Exports do build 79: o linker nao os conhece; busca na hora. */
static int (*p_cam_modes)(unsigned int* out, int max);
static int (*p_cam_open)(int kind, int w, int h, int* maxlen);
static int (*p_cam_frame)(void* buf, int max, int* seq, int timeout_ms);
static int (*p_cam_close)(void);

/* Tabelas de Huffman padrao (JPEG, anexo K): o MJPEG da webcam vem sem DHT. */
static const unsigned char dc_l[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const unsigned char dc_c[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const unsigned char ac_l[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const unsigned char ac_c[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const unsigned char ac_lv[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa};
static const unsigned char ac_cv[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa};

static int dht_tab(unsigned char* o, int n, int tc, const unsigned char* bits, const unsigned char* val) {
  int i, cnt = 0;
  o[n++] = (unsigned char)tc;
  for (i = 0; i < 16; i++) {
    o[n++] = bits[i];
    cnt += bits[i];
  }
  for (i = 0; i < cnt; i++) o[n++] = val ? val[i] : (unsigned char)i;
  return n;
}

static int has_dht(const unsigned char* p, int n) {
  int i = 2;
  while (i + 4 <= n) {
    int mk;
    if (p[i] != 0xFF) return 0;
    mk = p[i + 1];
    if (mk == 0xC4) return 1;
    if (mk == 0xDA || mk == 0xD9) return 0;
    if (mk == 0xFF || (mk >= 0xD0 && mk <= 0xD7) || mk == 0x01) {
      i += mk == 0xFF ? 1 : 2;
      continue;
    }
    i += 2 + (p[i + 2] << 8 | p[i + 3]);
  }
  return 0;
}

/* JPEG completo em dst (DHT padrao depois do SOI, se faltar). */
static int jpeg_fix(unsigned char* dst, const unsigned char* src, int n) {
  int o = 0, at;
  if (n < 4 || has_dht(src, n)) {
    memcpy(dst, src, (size_t)n);
    return n;
  }
  dst[o++] = 0xFF;
  dst[o++] = 0xD8;
  dst[o++] = 0xFF;
  dst[o++] = 0xC4;
  at = o;
  o += 2;
  o = dht_tab(dst, o, 0x00, dc_l, 0);
  o = dht_tab(dst, o, 0x10, ac_l, ac_lv);
  o = dht_tab(dst, o, 0x01, dc_c, 0);
  o = dht_tab(dst, o, 0x11, ac_c, ac_cv);
  dst[at] = (unsigned char)((o - at) >> 8);
  dst[at + 1] = (unsigned char)(o - at);
  memcpy(dst + o, src + 2, (size_t)(n - 2));
  return o + n - 2;
}

static unsigned char clamp8(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* YUY2 (Y0 U Y1 V) -> RGBA, opcionalmente espelhado. */
static void yuy2_rgba(const unsigned char* s, unsigned char* d, int w, int h, int mirror) {
  int x, y;
  for (y = 0; y < h; y++) {
    const unsigned char* p = s + (long)y * w * 2;
    unsigned char* o = d + (long)y * w * 4;
    for (x = 0; x < w; x++) {
      int sx = mirror ? w - 1 - x : x;
      int Y = p[sx * 2] - 16;
      int U = p[(sx & ~1) * 2 + 1] - 128;
      int V = p[(sx & ~1) * 2 + 3] - 128;
      int c = 298 * Y;
      o[0] = clamp8((c + 409 * V + 128) >> 8);
      o[1] = clamp8((c - 100 * U - 208 * V + 128) >> 8);
      o[2] = clamp8((c + 516 * U + 128) >> 8);
      o[3] = 255;
      o += 4;
    }
  }
}

static void mirror_rgba(unsigned char* p, int w, int h) {
  int x, y;
  for (y = 0; y < h; y++) {
    unsigned* r = (unsigned*)(p + (long)y * w * 4);
    for (x = 0; x < w / 2; x++) {
      unsigned t = r[x];
      r[x] = r[w - 1 - x];
      r[w - 1 - x] = t;
    }
  }
}

/* Mostra uma mensagem ate o usuario fechar. */
static int fail_wait(const char* a, const char* b) {
  gui_event_t ev;
  int ch = gui_char_h();
  for (;;) {
    gui_fill(0, 0, gui_width(), gui_height(), 0x101010);
    gui_text(16, 24, a, -1, 0xFFFFFF, 0x101010, GUI_TEXT_NOBG);
    if (b) gui_text(16, 24 + ch + 6, b, -1, 0xC0C0C0, 0x101010, GUI_TEXT_NOBG);
    gui_text(16, 24 + 2 * (ch + 6), "Esc fecha", -1, 0xC0C0C0, 0x101010, GUI_TEXT_NOBG);
    gui_present();
    ev.type = GUI_EV_NONE;
    if (gui_wait(&ev, -1) == GUI_EV_CLOSE || ev.type == GUI_EV_CLOSE) break;
    if (ev.type == GUI_EV_KEY && (ev.key == 27 || ev.key == 'q')) break;
  }
  gui_close();
  return 1;
}

int main(int argc, char** argv) {
  unsigned int m[4 * 32];
  int want_w = 640, want_h = 480, n, i, kind = 0, maxlen = 0, seq = 0, r, alive = 1, mirror = 1;
  int fps_ival = 0, frames = 0, shown = 0;
  int last_sw = 0, last_sh = 0, last_x = 0, last_y = 0, last_w = 0, last_h = 0;
  unsigned t_fps, fps10 = 0;
  unsigned char *raw = 0, *jpg = 0, *rgba = 0;
  char title[96], msg[200];
  if (argc >= 3) {
    want_w = atoi(argv[1]);
    want_h = atoi(argv[2]);
  }
  p_cam_modes = (int (*)(unsigned int*, int))k_dl_sym(0, "k_cam_modes", 0);
  p_cam_open = (int (*)(int, int, int, int*))k_dl_sym(0, "k_cam_open", 0);
  p_cam_frame = (int (*)(void*, int, int*, int))k_dl_sym(0, "k_cam_frame", 0);
  p_cam_close = (int (*)(void))k_dl_sym(0, "k_cam_close", 0);
  if (gui_open_pct("Webcam", 45, 55) != 0) {
    printf("WEBCAMV: abra pelo WIN (webcam /show)\n");
    return 1;
  }
  if (!p_cam_modes || !p_cam_open || !p_cam_frame || !p_cam_close)
    return fail_wait("Este kernel nao tem o stream da webcam.", "Precisa do SODOS build 79 ou mais novo.");
  n = p_cam_modes(m, 32);
  if (n <= 0) return fail_wait("Nenhuma webcam USB.", "Ligue a camera e abra de novo (comando usb mostra).");
  /* YUY2 com 25 q/s ou mais no tamanho pedido; senao MJPEG. */
  for (i = 0; i < n; i++)
    if ((int)m[i * 4 + 1] == want_w && (int)m[i * 4 + 2] == want_h && m[i * 4] == 2 && m[i * 4 + 3] &&
        m[i * 4 + 3] <= 400000u) {
      kind = 2;
      fps_ival = (int)m[i * 4 + 3];
    }
  if (!kind)
    for (i = 0; i < n; i++)
      if ((int)m[i * 4 + 1] == want_w && (int)m[i * 4 + 2] == want_h && m[i * 4] == 1) {
        kind = 1;
        fps_ival = (int)m[i * 4 + 3];
      }
  if (!kind) {
    snprintf(msg, sizeof msg, "A webcam nao tem %dx%d (webcam /L lista os tamanhos).", want_w, want_h);
    return fail_wait(msg, 0);
  }
  r = p_cam_open(kind, want_w, want_h, &maxlen);
  if (r != 0)
    return fail_wait(r == -2 ? "A webcam esta em uso por outro programa (Chrome?)." : "A webcam nao ligou.",
                     r == -2 ? "Feche o outro programa e abra de novo." : 0);
  raw = (unsigned char*)malloc((size_t)maxlen);
  jpg = (unsigned char*)malloc((size_t)maxlen + 1024);
  rgba = (unsigned char*)malloc((size_t)want_w * want_h * 4);
  if (!raw || !jpg || !rgba) {
    p_cam_close();
    return fail_wait("Sem memoria para os quadros.", 0);
  }
  snprintf(title, sizeof title, "Webcam %dx%d", want_w, want_h);
  gui_title(title);
  t_fps = k_ms();
  while (alive) {
    gui_event_t ev;
    int sw, sh, ch, iw = want_w, ih = want_h, x, y, w, h, got;
    /* eventos sem esperar */
    for (;;) {
      int t;
      ev.type = GUI_EV_NONE;
      t = gui_wait(&ev, 0);
      if (t == GUI_EV_NONE && ev.type == GUI_EV_NONE) break;
      if (t == GUI_EV_CLOSE || ev.type == GUI_EV_CLOSE) alive = 0;
      if (ev.type == GUI_EV_KEY) {
        if (ev.key == 27 || ev.key == 'q') alive = 0;
        if (ev.key == 'f' || ev.key == 'F') mirror = !mirror;
      }
      if (!alive) break;
    }
    if (!alive) break;
    got = p_cam_frame(raw, maxlen, &seq, 100);
    if (got < 0) {
      p_cam_close();
      return fail_wait("A webcam parou.", "Foi desligada ou outro programa a desligou.");
    }
    if (got == 0) continue;
    frames++;
    if (kind == 2) {
      if (got < want_w * want_h * 2) continue;
      yuy2_rgba(raw, rgba, want_w, want_h, mirror);
    } else {
      img_t im;
      int jn = jpeg_fix(jpg, raw, got);
      im.rgba = 0;
      if (img_decode(jpg, jn, &im) != 0 || !im.rgba) continue;
      iw = im.w;
      ih = im.h;
      if ((long)iw * ih > (long)want_w * want_h) {
        img_free(&im);
        continue;
      }
      memcpy(rgba, im.rgba, (size_t)iw * ih * 4);
      img_free(&im);
      if (mirror) mirror_rgba(rgba, iw, ih);
    }
    sw = gui_width();
    sh = gui_height();
    ch = gui_char_h();
    /* cabe na janela, sem aumentar alem de 2x */
    {
      int aw = sw, ah = sh - ch - 8;
      if (aw < 1) aw = 1;
      if (ah < 1) ah = 1;
      if ((long)iw * ah > (long)ih * aw) {
        w = aw;
        h = (int)((long)ih * aw / iw);
      } else {
        h = ah;
        w = (int)((long)iw * ah / ih);
      }
      if (w > iw * 2) {
        w = iw * 2;
        h = ih * 2;
      }
      x = (sw - w) / 2;
      y = (ah - h) / 2;
    }
    /* Fundo so quando o lugar da imagem muda (janela redimensionada): pintar
     * a janela toda a cada quadro fazia o video piscar. */
    if (shown == 0 || sw != last_sw || sh != last_sh || x != last_x || y != last_y || w != last_w ||
        h != last_h) {
      gui_fill(0, 0, sw, sh, 0x101010);
      gui_fill(0, sh - ch - 8, sw, ch + 8, 0x000000);
      last_sw = sw;
      last_sh = sh;
      last_x = x;
      last_y = y;
      last_w = w;
      last_h = h;
    }
    if (w == iw && h == ih) gui_pixels(x, y, w, h, rgba, iw * 4);
    else gui_image(x, y, w, h, rgba, iw, ih, 0, 0, sw, sh - ch - 8);
    if (k_ms() - t_fps >= 1000) {
      fps10 = (unsigned)frames * 10000u / (k_ms() - t_fps);
      frames = 0;
      t_fps = k_ms();
    }
    snprintf(msg, sizeof msg, "%dx%d %s  %u.%u q/s (camera %d)   F espelha%s   Esc fecha", iw, ih,
             kind == 2 ? "YUY2" : "MJPEG", fps10 / 10, fps10 % 10, fps_ival ? (10000000 + fps_ival / 2) / fps_ival : 0,
             mirror ? " (ligado)" : "");
    /* Texto com fundo proprio e o resto da linha apagado: sem apagar a barra
     * inteira antes (piscava). */
    {
      int tw = gui_text(8, sh - ch - 4, msg, -1, 0xE0E0E0, 0x000000, 0);
      if (8 + tw < sw) gui_fill(8 + tw, sh - ch - 4, sw - 8 - tw, ch, 0x000000);
    }
    gui_present();
    shown++;
  }
  p_cam_close();
  gui_close();
  return 0;
}
