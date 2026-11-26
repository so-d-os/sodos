/* gesture.c - analisador de imagem e gestos do SODOS.
 *
 *   GESTURE                 janela ao vivo: webcam, marcas e gestos
 *   GESTURE /T [segundos]   sem janela: escreve os gestos (e no log)
 *   GESTURE /F foto.jpg [saida.bmp]
 *                           analisa uma foto: escreve o que achou e grava a
 *                           foto com as marcas (padrao D:\TEMP\GESTOUT.BMP)
 *
 * Na janela: marcadores no painel escolhem o que detectar (clique ou tecla
 * 1-8: 1 mao espalmada, 2 mao fechada, 3 olhos, 4 rosto, 5 cabeca, 6 dedos,
 * 7 deslizar e tchau, 8 movimento; a escolha fica em D:\TEMP\GESTURE.CFG e
 * /D:lista escolhe pela linha de comando). M mostra a mascara de pele, R
 * recomeca (cor da pele e referencias dos olhos), Esc fecha. Os gestos tambem vao para
 * D:\TEMP\GESTURE.LOG. Marcas: verde = rosto, ciano = olhos, amarelo =
 * para onde a cabeca aponta, laranja = mao levantada, vermelho = mao. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>
#include <gui.h>
#include <img.h>
#include "gesture.h"

#define CAM_W 640
#define CAM_H 480
#define LOG_PATH "D:\\TEMP\\GESTURE.LOG"

static gstate st;
static gimage gim;

/* Rastro das etapas em D:\TEMP\GESTRUN.LOG (gravado a cada linha): se o
 * programa cair, o arquivo mostra ate onde chegou. */
static FILE* runf;
void trace(const char* what, int v) {
  if (!runf) runf = fopen("D:\\TEMP\\GESTRUN.LOG", "w");
  if (!runf) return;
  fprintf(runf, "%u %s %d\n", k_ms(), what, v);
  fflush(runf);
}
void trace_p(const char* what, const void* p) {
  if (!runf) runf = fopen("D:\\TEMP\\GESTRUN.LOG", "w");
  if (!runf) return;
  fprintf(runf, "%u %s %08x%08x\n", k_ms(), what, (unsigned)((unsigned long long)(unsigned long)p >> 32),
          (unsigned)(unsigned long)p);
  fflush(runf);
}

/* ---------- modo arquivo ---------- */

static int save_bmp(const char* path, const unsigned char* rgba, int w, int h) {
  FILE* f = fopen(path, "wb");
  unsigned char hd[54];
  int row = (w * 3 + 3) & ~3, y, x;
  unsigned size = 54u + (unsigned)row * h;
  unsigned char* line;
  if (!f) return -1;
  memset(hd, 0, sizeof hd);
  hd[0] = 'B';
  hd[1] = 'M';
  hd[2] = (unsigned char)size;
  hd[3] = (unsigned char)(size >> 8);
  hd[4] = (unsigned char)(size >> 16);
  hd[5] = (unsigned char)(size >> 24);
  hd[10] = 54;
  hd[14] = 40;
  hd[18] = (unsigned char)w;
  hd[19] = (unsigned char)(w >> 8);
  hd[22] = (unsigned char)h;
  hd[23] = (unsigned char)(h >> 8);
  hd[26] = 1;
  hd[28] = 24;
  fwrite(hd, 1, 54, f);
  line = (unsigned char*)calloc(1, (size_t)row);
  for (y = h - 1; y >= 0; y--) {
    const unsigned char* p = rgba + (long)y * w * 4;
    for (x = 0; x < w; x++) {
      line[x * 3] = p[x * 4 + 2];
      line[x * 3 + 1] = p[x * 4 + 1];
      line[x * 3 + 2] = p[x * 4];
    }
    fwrite(line, 1, (size_t)row, f);
  }
  free(line);
  fclose(f);
  return 0;
}

static void print_state(void) {
  const gface* f = &st.face;
  int i;
  if (f->found) {
    printf("rosto: (%d,%d)-(%d,%d)  yaw %d  pitch %d  roll %d\n", f->x0, f->y0, f->x1, f->y1, f->yaw, f->pitch,
           f->roll);
    printf("olhos: %d achados", f->eyes);
    if (f->open_l >= 0) printf("  esquerdo (%d,%d) abertura %d", f->lx, f->ly, f->open_l);
    if (f->open_r >= 0) printf("  direito (%d,%d) abertura %d", f->rx, f->ry, f->open_r);
    printf("\n");
  } else {
    printf("rosto: nenhum\n");
  }
  for (i = 0; i < st.nh; i++)
    printf("mao %d: (%d,%d)-(%d,%d)  dedos %d%s\n", i + 1, st.hand[i].x0, st.hand[i].y0, st.hand[i].x1,
           st.hand[i].y1, st.hand[i].fingers, st.hand[i].raised ? "  levantada" : "");
  printf("pele: Cb %d..%d  Cr %d..%d%s  regioes %d\n", st.skin.cb_lo, st.skin.cb_hi, st.skin.cr_lo,
         st.skin.cr_hi, st.skin.adapted ? " (ajustada pelo rosto)" : "", st.nb);
}

static int run_file(const char* path, const char* out) {
  FILE* f = fopen(path, "rb");
  long n;
  unsigned char* data;
  img_t im;
  gevent ev[8];
  int i;
  if (!f) {
    printf("GESTURE: nao abri %s\n", path);
    return 1;
  }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  data = (unsigned char*)malloc((size_t)n);
  if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
    fclose(f);
    printf("GESTURE: nao li %s\n", path);
    return 1;
  }
  fclose(f);
  im.rgba = 0;
  if (img_decode(data, (int)n, &im) != 0 || !im.rgba) {
    printf("GESTURE: %s nao e uma imagem que o IMG.DLL le\n", path);
    return 1;
  }
  rgba_to_g(&gim, im.rgba, im.w, im.h);
  {
    /* planos crus (Y, Cb, Cr; GW x GH cada) para calibrar a cor da pele */
    FILE* r = fopen("D:\\TEMP\\GESTPLAN.RAW", "wb");
    if (r) {
      fwrite(gim.y.p, 1, GN, r);
      fwrite(gim.cb.p, 1, GN, r);
      fwrite(gim.cr.p, 1, GN, r);
      fclose(r);
    }
  }
  /* tres passadas: a cor da pele se ajusta ao rosto achado na primeira */
  for (i = 0; i < 3; i++) {
    gim.t_ms += 40;
    (void)an_frame(&st, &gim, ev, 8);
  }
  printf("%s: %dx%d\n", path, im.w, im.h);
  print_state();
  draw_analysis(im.rgba, im.w, im.h, &st.face, st.hand, st.nh, &st.mask, 0, st.detect);
  if (save_bmp(out, im.rgba, im.w, im.h) == 0) printf("marcas em %s\n", out);
  /* a mascara tambem, para ajustar a cor da pele */
  {
    unsigned char* m = (unsigned char*)malloc((size_t)GN * 4);
    if (m) {
      int k;
      char mp[200];
      for (k = 0; k < GN; k++) {
        unsigned char v = st.mask.p[k];
        m[k * 4] = m[k * 4 + 1] = m[k * 4 + 2] = v;
        m[k * 4 + 3] = 255;
      }
      snprintf(mp, sizeof mp, "%s.MASK.BMP", out);
      save_bmp(mp, m, GW, GH);
      free(m);
    }
  }
  img_free(&im);
  free(data);
  return 0;
}

/* ---------- modo texto ---------- */

static int run_text(int secs) {
  gevent ev[16];
  unsigned t0;
  int i, n;
  if (cam_start(CAM_W, CAM_H) != 0) {
    printf("GESTURE: %s\n", cam_error());
    return 1;
  }
  ev_log_open(LOG_PATH);
  printf("GESTURE: olhando a webcam por %d s (gestos tambem em %s)\n", secs, LOG_PATH);
  t0 = k_ms();
  while (k_ms() - t0 < (unsigned)secs * 1000u) {
    int r = cam_grab(&gim, 0, 200);
    if (r < 0) {
      printf("GESTURE: %s\n", cam_error());
      break;
    }
    if (r == 0) continue;
    n = an_frame(&st, &gim, ev, 16);
    for (i = 0; i < n; i++) {
      char t[80];
      ev_text(&ev[i], t, (int)sizeof t);
      printf("%6.1f  %s\n", (ev[i].t_ms - t0) / 1000.0, t);
      ev_log(&ev[i]);
    }
  }
  cam_stop();
  ev_log_close();
  return 0;
}

/* ---------- janela ---------- */

#define EVSHOW 14

static int ck_x0, ck_h, ck_y[D_GROUPS];   /* onde ficam os marcadores no painel */

/* Linha do painel: texto com fundo e o resto da linha apagado (sem apagar o
 * painel inteiro a cada quadro, que piscava). */
static void panel_text(int x, int y, int x_end, const char* s, unsigned fg, int flags) {
  int tw = s[0] ? gui_text(x, y, s, -1, fg, 0x101010, flags & ~GUI_TEXT_NOBG) : 0;
  if (x + tw < x_end) gui_fill(x + tw, y, x_end - x - tw, gui_char_h(), 0x101010);
}

static void push_line(char lines[][80], unsigned* lt, int* nl, const char* s, unsigned t) {
  int k;
  for (k = EVSHOW - 1; k > 0; k--) {
    strcpy(lines[k], lines[k - 1]);
    lt[k] = lt[k - 1];
  }
  strncpy(lines[0], s, 79);
  lines[0][79] = 0;
  lt[0] = t;
  if (*nl < EVSHOW) (*nl)++;
}

/* Liga/desliga o marcador i, grava a escolha e recomeca o historico. */
static void toggle_det(int i, char lines[][80], unsigned* lt, int* nl) {
  char t[80];
  st.detect ^= det_bit(i);
  det_save(st.detect);
  track_init(&st.track);
  snprintf(t, sizeof t, "[%s %s]", det_name(i), (st.detect & det_bit(i)) ? "ligado" : "desligado");
  push_line(lines, lt, nl, t, k_ms());
}

static int run_window(void) {
  unsigned char* rgba = (unsigned char*)malloc((size_t)CAM_W * CAM_H * 4);
  gevent ev[16];
  char lines[EVSHOW][80];
  unsigned lt[EVSHOW];
  int nl = 0, show_mask = 0, alive = 1, frames = 0, fps10 = 0, i, n, last_sw = 0, last_sh = 0;
  unsigned t_fps;
  trace("janela: abrindo", 0);
  trace_p("run_window: local em", &nl);
  trace_p("run_window: st em", &st);
  if (gui_open_pct("Gestos (GESTURE)", 75, 80) != 0) {
    printf("GESTURE: abra pelo WIN (ou use /T ou /F)\n");
    return 1;
  }
  trace("janela: aberta, ligando a camera", rgba != 0);
  if (!rgba || cam_start(CAM_W, CAM_H) != 0) {
    trace("camera: falhou", 0);
    gui_event_t e;
    gui_fill(0, 0, gui_width(), gui_height(), 0x101010);
    gui_text(16, 24, rgba ? cam_error() : "sem memoria", -1, 0xFFFFFF, 0x101010, GUI_TEXT_NOBG);
    gui_text(16, 24 + gui_char_h() + 6, "Esc fecha", -1, 0xC0C0C0, 0x101010, GUI_TEXT_NOBG);
    gui_present();
    for (;;) {
      e.type = GUI_EV_NONE;
      if (gui_wait(&e, -1) == GUI_EV_CLOSE || e.type == GUI_EV_CLOSE) break;
      if (e.type == GUI_EV_KEY && (e.key == 27 || e.key == 'q')) break;
    }
    gui_close();
    return 1;
  }
  ev_log_open(LOG_PATH);
  trace("camera: ligada", 0);
  t_fps = k_ms();
  while (alive) {
    gui_event_t e;
    int sw, sh, chh, iw = CAM_W, ih = CAM_H, x, y, w, h, px, py;
    static int nq;
    char t[160];
    const gface* f = &st.face;
    for (;;) {
      int ty;
      e.type = GUI_EV_NONE;
      ty = gui_wait(&e, 0);
      if (ty == GUI_EV_NONE && e.type == GUI_EV_NONE) break;
      if (ty == GUI_EV_CLOSE || e.type == GUI_EV_CLOSE) alive = 0;
      if (e.type == GUI_EV_DOWN && e.x >= ck_x0 && ck_h > 0) {
        int k;
        for (k = 0; k < D_GROUPS; k++)
          if (e.y >= ck_y[k] && e.y < ck_y[k] + ck_h) toggle_det(k, lines, lt, &nl);
      }
      if (e.type == GUI_EV_KEY) {
        if (e.key == 27 || e.key == 'q') alive = 0;
        if (e.key >= '1' && e.key < '1' + D_GROUPS) toggle_det(e.key - '1', lines, lt, &nl);
        if (e.key == 'm' || e.key == 'M') show_mask = !show_mask;
        if (e.key == 'r' || e.key == 'R') {
          skin_init(&st.skin);
          face_reset(&st.face);
          st.face.base_l = st.face.base_r = 0;
          track_init(&st.track);
        }
      }
    }
    if (!alive) break;
    i = cam_grab(&gim, rgba, 200);
    if (i < 0) {
      trace("camera: parou", 0);
      break;
    }
    if (i == 0) continue;
    /* quadro cru (antes das marcas) a cada 2 s, dez em rodizio, para
     * calibrar com GESTURE /F: D:\TEMP\GESTLIVE\F0.BMP .. F9.BMP */
    {
      static unsigned t_shot;
      static int n_shot;
      if (gim.t_ms - t_shot >= 2000u) {
        char sp[64];
        t_shot = gim.t_ms;
        snprintf(sp, sizeof sp, "D:\\TEMP\\GESTLIVE\\F%d.BMP", n_shot);
        n_shot = (n_shot + 1) % 10;
        (void)save_bmp(sp, rgba, CAM_W, CAM_H);
      }
    }
    if (nq < 3 || nq % 100 == 0) trace("quadro: analisando", nq);
    n = an_frame(&st, &gim, ev, 16);
    if (nq < 3) trace("quadro: analisado, gestos", n);
    nq++;
    for (i = 0; i < n; i++) {
      int k;
      for (k = EVSHOW - 1; k > 0; k--) {
        strcpy(lines[k], lines[k - 1]);
        lt[k] = lt[k - 1];
      }
      ev_text(&ev[i], lines[0], 80);
      lt[0] = ev[i].t_ms;
      if (nl < EVSHOW) nl++;
      ev_log(&ev[i]);
    }
    draw_analysis(rgba, CAM_W, CAM_H, f, st.hand, st.nh, &st.mask, show_mask, st.detect);
    frames++;
    if (k_ms() - t_fps >= 1000) {
      fps10 = frames * 10000 / (int)(k_ms() - t_fps);
      frames = 0;
      t_fps = k_ms();
    }
    /* layout: imagem a esquerda (cabe na altura), painel de texto a direita */
    sw = gui_width();
    sh = gui_height();
    chh = gui_char_h();
    h = sh;
    w = iw * h / ih;
    if (w > sw * 70 / 100) {
      w = sw * 70 / 100;
      h = ih * w / iw;
    }
    x = 0;
    y = 0;
    if (sw != last_sw || sh != last_sh) {
      gui_fill(0, 0, sw, sh, 0x101010);
      last_sw = sw;
      last_sh = sh;
    }
    if (w == iw && h == ih) gui_pixels(x, y, w, h, rgba, iw * 4);
    else gui_image(x, y, w, h, rgba, iw, ih, 0, 0, sw, sh);
    px = w + 12;
    py = 8;
    ck_x0 = px;
    snprintf(t, sizeof t, "%d.%d q/s   M mascara  R recomeca  Esc", fps10 / 10, fps10 % 10);
    panel_text(px, py, sw, t, 0xC0C0C0, 0);
    py += chh + 8;
    /* marcadores: o que detectar */
    panel_text(px, py, sw, "Detectar (clique ou tecla 1-8):", 0xFFFFFF, GUI_TEXT_BOLD);
    py += chh + 6;
    for (i = 0; i < D_GROUPS; i++) {
      int on = (st.detect & det_bit(i)) != 0, bx = chh - 4;
      ck_y[i] = py;
      gui_fill(px, py + 2, bx, bx, 0x101010);
      gui_frame(px, py + 2, bx, bx, on ? 0x80FF80 : 0x808080);
      if (on) gui_fill(px + 3, py + 5, bx - 6, bx - 6, 0x40D040);
      snprintf(t, sizeof t, "%d  %s", i + 1, det_name(i));
      panel_text(px + bx + 8, py, sw, t, on ? 0xFFFFFF : 0x808080, 0);
      py += chh + 4;
    }
    ck_h = chh + 4;
    py += 8;
    if (st.detect & (D_FACE | D_HEAD | D_EYES)) {
      if (f->found)
        snprintf(t, sizeof t, "rosto: yaw %d  pitch %d  roll %d", f->yaw, f->pitch, f->roll);
      else
        snprintf(t, sizeof t, "rosto: nenhum");
      panel_text(px, py, sw, t, f->found ? 0x80FF80 : 0xA0A0A0, 0);
    } else {
      panel_text(px, py, sw, "", 0, 0);
    }
    py += chh + 4;
    if ((st.detect & D_EYES) && f->found) {
      snprintf(t, sizeof t, "olhos: %d   esq %d%%   dir %d%%", f->eyes,
               f->open_l >= 0 && f->base_l > 0 ? f->open_l * 100 / f->base_l : -1,
               f->open_r >= 0 && f->base_r > 0 ? f->open_r * 100 / f->base_r : -1);
      panel_text(px, py, sw, t, 0x80FFFF, 0);
    } else {
      panel_text(px, py, sw, "", 0, 0);
    }
    py += chh + 4;
    if (st.detect & D_NEED_HAND) {
      if (st.nh > 0)
        snprintf(t, sizeof t, "mao: %s, dedos %d%s",
                 st.hand[0].fingers == 0 ? "fechada" : st.hand[0].fingers >= 4 ? "espalmada" : "aberta",
                 st.hand[0].fingers, st.hand[0].raised ? ", levantada" : "");
      else
        snprintf(t, sizeof t, "mao: nenhuma");
      panel_text(px, py, sw, t, st.nh > 0 ? 0xFFB080 : 0xA0A0A0, 0);
    } else {
      panel_text(px, py, sw, "", 0, 0);
    }
    py += chh + 4;
    if (st.detect & D_MOTION) snprintf(t, sizeof t, "movimento: %d por mil", st.motion);
    else t[0] = 0;
    panel_text(px, py, sw, t, 0xA0A0A0, 0);
    py += chh + 14;
    panel_text(px, py, sw, "Gestos:", 0xFFFFFF, GUI_TEXT_BOLD);
    py += chh + 6;
    for (i = 0; i < nl; i++) {
      unsigned age = gim.t_ms - lt[i];
      panel_text(px, py, sw, lines[i], age < 1500 ? 0xFFFF60 : 0xD0D0D0, 0);
      py += chh + 4;
    }
    gui_present();
  }
  trace("saindo: desligando a camera", 0);
  cam_stop();
  ev_log_close();
  gui_close();
  trace("saiu", 0);
  return 0;
}

int main(int argc, char** argv) {
  int i, j;
  if (an_init(&st) != 0 || img_alloc_g(&gim) != 0) {
    printf("GESTURE: sem memoria\n");
    return 1;
  }
  /* marcadores: os gravados, ou /D:lista (em qualquer posicao; sai da lista) */
  st.detect = det_load();
  for (i = 1, j = 1; i < argc; i++) {
    if ((argv[i][0] == '/' || argv[i][0] == '-') && (argv[i][1] == 'D' || argv[i][1] == 'd') && argv[i][2] == ':') {
      int b = det_parse(argv[i] + 3);
      if (b) st.detect = b;
      else printf("GESTURE: /D: sem nada valido (%s); use espalmada,fechada,olhos,rosto,cabeca,dedos,deslizar,movimento ou 1-8\n",
                  argv[i] + 3);
      continue;
    }
    argv[j++] = argv[i];
  }
  argc = j;
  if (argc >= 2 && (strcmp(argv[1], "/F") == 0 || strcmp(argv[1], "/f") == 0)) {
    if (argc < 3) {
      printf("uso: GESTURE /F foto.jpg [saida.bmp]\n");
      return 1;
    }
    return run_file(argv[2], argc >= 4 ? argv[3] : "D:\\TEMP\\GESTOUT.BMP");
  }
  if (argc >= 2 && (strcmp(argv[1], "/T") == 0 || strcmp(argv[1], "/t") == 0))
    return run_text(argc >= 3 ? atoi(argv[2]) : 30);
  if (argc >= 2 && (strcmp(argv[1], "/?") == 0 || strcmp(argv[1], "-h") == 0)) {
    printf("GESTURE            janela ao vivo (webcam)\n"
           "GESTURE /T [s]     so texto, por s segundos (padrao 30)\n"
           "GESTURE /F foto [saida.bmp]  analisa uma foto\n"
           "  /D:lista         o que detectar (na janela: marcadores, clique ou 1-8):\n"
           "                   1 espalmada  2 fechada  3 olhos  4 rosto  5 cabeca\n"
           "                   6 dedos  7 deslizar  8 movimento  (ou todos)\n"
           "                   a escolha fica em D:\\TEMP\\GESTURE.CFG\n");
    return 0;
  }
  return run_window();
}
