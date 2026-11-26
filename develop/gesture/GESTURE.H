/* GESTURE.H - analisador de imagem e gestos do SODOS (develop\gesture).
 *
 * Sem modelos treinados: visao classica em C puro, sobre a webcam USB do
 * kernel (k_cam_*, build 79+) ou sobre um arquivo de imagem.
 *
 *   capture.c  webcam (YUY2) e arquivos -> planos Y, Cb, Cr em GW x GH
 *   image.c    planos, borrar, erosao/dilatacao, diferenca, estatisticas
 *   skin.c     mascara de pele (Cb/Cr), que se ajusta pela cor do rosto
 *   blobs.c    regioes conectadas da mascara (caixa, centro, area)
 *   face.c     rosto: a regiao de pele com cara de rosto, acompanhada
 *   eyes.c     olhos: posicao e abertura de cada um
 *   head.c     cabeca: virar (yaw), subir/descer (pitch), inclinar (roll)
 *   hands.c    maos: regioes de pele fora do rosto, dedos esticados
 *   track.c    historico no tempo e gestos (piscar, sim, nao, deslizar...)
 *   events.c   nomes, repeticao e registro dos gestos reconhecidos
 *   draw.c     desenho das marcas sobre a imagem (RGBA)
 *   gesture.c  o programa: janela ao vivo, modo texto e modo arquivo
 *
 * Coordenadas sempre no espaco de analise (GW x GH, origem no canto de
 * cima a esquerda). "Esquerda" e a da imagem (a camera nao e espelhada). */
#ifndef GESTURE_H
#define GESTURE_H

#include "haar.h"

#define GW 320          /* largura de analise */
#define GH 240          /* altura de analise */
#define GN (GW * GH)

/* ---------- planos (image.c) ---------- */
typedef struct {
  int w, h;
  unsigned char* p;     /* w * h bytes */
} gplane;

int pl_alloc(gplane* p, int w, int h);
void pl_free(gplane* p);
void pl_copy(gplane* d, const gplane* s);
void pl_blur3(gplane* d, const gplane* s);           /* caixa 3x3 */
void pl_erode(gplane* d, const gplane* s);           /* binaria 3x3 (0/255) */
void pl_dilate(gplane* d, const gplane* s);
void pl_open_close(gplane* m, gplane* tmp);          /* abre e fecha a mascara */
int pl_mean(const gplane* p, int x0, int y0, int x1, int y1);
int pl_count(const gplane* p, int x0, int y0, int x1, int y1, int min);
int pl_absdiff_count(const gplane* a, const gplane* b, int thr); /* pixels que mudaram */

/* ---------- quadro (capture.c) ---------- */
typedef struct {
  gplane y, cb, cr;     /* GW x GH */
  unsigned t_ms;        /* quando chegou */
  int seq;
} gimage;

int img_alloc_g(gimage* im);
void img_free_g(gimage* im);
/* Webcam: liga no tamanho pedido (YUY2), 0 = ok. */
int cam_start(int w, int h);
void cam_stop(void);
const char* cam_error(void);
/* Proximo quadro (espera ate timeout_ms). 1 = chegou; 0 = nao; -1 = parou.
 * rgba (opcional, cam_w*cam_h*4) recebe a imagem colorida para mostrar. */
int cam_grab(gimage* im, unsigned char* rgba, int timeout_ms);
int cam_w(void);
int cam_h(void);
/* Imagem RGBA qualquer (arquivo) -> planos de analise. */
void rgba_to_g(gimage* im, const unsigned char* rgba, int w, int h);
/* YUY2 de w x h -> planos de analise (e RGBA, opcional). */
void yuy2_to_g(gimage* im, unsigned char* rgba, const unsigned char* s, int w, int h);

/* ---------- pele (skin.c) ---------- */
typedef struct {
  int cb_lo, cb_hi, cr_lo, cr_hi, y_lo;
  int adapted;          /* 1 = ajustada pela cor do rosto */
} gskin;

void skin_init(gskin* s);
void skin_mask(const gskin* s, const gimage* im, gplane* mask);
/* Ajusta os limites pela cor media dentro da caixa (o rosto). */
void skin_adapt(gskin* s, const gimage* im, int x0, int y0, int x1, int y1);

/* ---------- regioes (blobs.c) ---------- */
#define BLOB_MAX 32
typedef struct {
  int x0, y0, x1, y1;   /* caixa (x1, y1 inclusive) */
  int area;
  int cx, cy;           /* centro de massa */
  int label;
} gblob;

/* Rotula a mascara (255 = pele). labels: GN shorts (0 = fundo). Devolve
 * quantas regioes (ate max), da maior para a menor, ignorando as menores
 * que min_area. */
int blobs_find(const gplane* mask, unsigned short* labels, gblob* out, int max, int min_area);

/* ---------- rosto e olhos (face.c, eyes.c) ---------- */
typedef struct {
  int found;
  int x0, y0, x1, y1;   /* caixa do rosto (so o rosto, sem pescoco) */
  int cx, cy;
  int blob;             /* indice em blobs[] */
  int lost;             /* quadros seguidos sem rosto */
  /* olhos */
  int eyes;             /* 0, 1 ou 2 achados */
  int lx, ly, rx, ry;   /* centros (l = o da esquerda da imagem) */
  int open_l, open_r;   /* abertura medida (pixels escuros na janela do olho) */
  int base_l, base_r;   /* abertura de referencia (olho aberto) */
  /* cabeca */
  int yaw;              /* -100 (virou para a esquerda da imagem) .. 100 */
  int pitch;            /* -100 (para cima) .. 100 (para baixo) */
  int roll;             /* graus, positivo = olho direito mais baixo */
} gface;

void face_reset(gface* f);
/* Escolhe o rosto entre as regioes; usa o rosto anterior para acompanhar. */
void face_find(gface* f, const gblob* b, int nb, const gplane* mask);
void eyes_find(gface* f, const gimage* im, const gplane* mask);
void head_pose(gface* f);

/* ---------- maos (hands.c) ---------- */
#define HAND_MAX 2
typedef struct {
  int found;
  int x0, y0, x1, y1, cx, cy, area;
  int fingers;          /* dedos esticados (0 = punho), -1 = nao deu para contar */
  int raised;           /* 1 = acima do queixo */
} ghand;

/* work e labels2: rascunho (GW x GH) para separar a mao grudada no corpo. */
int hands_find(ghand* h, const gblob* b, int nb, const gface* f, const unsigned short* labels, gplane* work,
               unsigned short* labels2);
/* Ao vivo: maos nas regioes de uma mascara ja sem o rosto (fundo aprendido). */
int hands_from_mask(ghand* h, const gblob* b, int nb, const gface* f, const unsigned short* labels);

/* ---------- gestos no tempo (track.c, events.c) ---------- */
enum {
  G_NONE = 0,
  G_FACE_IN, G_FACE_OUT,
  G_BLINK, G_WINK_L, G_WINK_R, G_EYES_CLOSED, G_EYES_OPEN,
  G_HEAD_LEFT, G_HEAD_RIGHT, G_HEAD_UP, G_HEAD_DOWN, G_HEAD_CENTER,
  G_NOD, G_SHAKE, G_TILT_L, G_TILT_R,
  G_HAND_IN, G_HAND_OUT, G_HAND_UP, G_FIST, G_OPEN_HAND, G_FINGERS,
  G_SWIPE_L, G_SWIPE_R, G_SWIPE_UP, G_SWIPE_DOWN, G_WAVE,
  G_MOTION, G_STILL,
  G_COUNT
};

typedef struct {
  int type;
  int value;            /* G_FINGERS: quantos dedos */
  unsigned t_ms;
} gevent;

#define HIST 64         /* ~2 s a 30 q/s */
typedef struct {
  unsigned t[HIST];
  int fx[HIST], fy[HIST], yaw[HIST], pitch[HIST], roll[HIST];
  int face[HIST], eyes[HIST], ol[HIST], or_[HIST];
  int hx[HIST], hy[HIST], hand[HIST], fingers[HIST];
  int motion[HIST];
  int n, head;          /* quantos e onde entra o proximo */
  /* estado */
  int fh[HIST];         /* altura do rosto (amplitude do "sim") */
  int had_face, closed_both, eyes_shut, closed_l, closed_r;
  int head_state, pitch_state, tilt_state, hand_state, raised_state, fingers_state;
  int fingers_run, fingers_cand, hand_in, hand_gone, still, still_ms;
  unsigned still_since;
  unsigned last[G_COUNT]; /* ultima vez de cada evento (repeticao) */
} gtrack;

void track_init(gtrack* t);
/* Junta o quadro ao historico e devolve os eventos novos (ate max). */
int track_update(gtrack* t, const gface* f, const ghand* h, int nh, int motion, unsigned t_ms,
                 gevent* out, int max);

const char* ev_name(int type);
/* Texto do evento ("dedos: 3"). */
void ev_text(const gevent* e, char* out, int max);
/* Registro em arquivo (0 desliga). */
void ev_log_open(const char* path);
void ev_log(const gevent* e);
void ev_log_close(void);

/* ---------- o que detectar (marcadores do painel, /D:) ---------- */
/* Ordem dos marcadores = a pedida pelo usuario (1 espalmada, 2 fechada,
 * 3 olhos), depois o resto. A tecla de cada um e o numero dele. */
#define D_OPEN    1     /* 1 mao espalmada (os 5 dedos abertos) */
#define D_FIST    2     /* 2 mao fechada (punho) */
#define D_EYES    4     /* 3 olhos: piscar, piscadela, fechados/abertos */
#define D_FACE    8     /* 4 rosto apareceu/saiu */
#define D_HEAD    16    /* 5 cabeca: virar, cima/baixo, inclinar, sim, nao */
#define D_FINGERS 32    /* 6 dedos (1 a 4), mao apareceu/saiu, levantada */
#define D_MOVES   64    /* 7 deslizar e tchau */
#define D_MOTION  128   /* 8 movimento geral, parado */
#define D_ALL     255
#define D_GROUPS  8
/* precisa achar a mao / o rosto */
#define D_NEED_HAND (D_OPEN | D_FIST | D_FINGERS | D_MOVES)
#define D_NEED_FACE (D_FACE | D_EYES | D_HEAD)

/* Grupo do gesto (D_*), para filtrar. */
int ev_group(int type);
/* Nome do grupo i (0..D_GROUPS-1) e seu bit. */
const char* det_name(int i);
int det_bit(int i);
/* "rosto,olhos,..." -> bits (0 se nada valido). */
int det_parse(const char* s);
/* Escolhas gravadas (D:\TEMP\GESTURE.CFG); D_ALL se nao houver. */
int det_load(void);
void det_save(int bits);

/* ---------- um quadro inteiro (analyze.c) ---------- */
typedef struct {
  gskin skin;
  gplane mask, tmp, prevy;
  unsigned short* labels;
  unsigned short* labels2;  /* rascunho das maos (hands.c) */
  gblob blobs[BLOB_MAX];
  int nb;
  gface face;
  ghand hand[HAND_MAX];
  int nh;
  int motion;           /* pixels que mudaram desde o quadro anterior, por mil */
  int have_prev;
  gtrack track;
  unsigned ms_last;     /* tempo da ultima analise */
  int detect;           /* D_*: o que detectar */
  /* cascatas Haar da OpenCV (FACE.CAS, EYE.CAS); haar_ok = 0 -> pela pele */
  gcascade cface, ceye;
  int haar_ok, eye_ok;
  gplane small;         /* GW/2 x GH/2 para a busca completa do rosto */
  int frame_no;
  /* ao vivo: fundo aprendido (Y, Cb, Cr x16) e o que difere dele (an_frame) */
  int live;             /* 1 = quadros da camera (nao o /F) */
  unsigned short* bg;   /* 3 * GN */
  int bg_n;             /* quadros aprendidos */
  gplane fg;            /* 255 = mao candidata: difere do fundo e e quase pele */
} gstate;

/* Rosto pela cascata (face.c): busca completa em small a cada tanto ou sem
 * rosto, senao so em volta do anterior. Marca f->blob com a regiao de pele
 * que mais cobre o rosto (para as maos nao a pegarem). */
void face_find_haar(gface* f, gstate* s, const gimage* im);
/* Olhos pela cascata dentro do rosto; sem achar, ficam na ultima posicao. */
void eyes_find_haar(gface* f, gstate* s, const gimage* im);

int an_init(gstate* s);
void an_free(gstate* s);
/* Analisa o quadro; devolve os gestos novos (ate max). */
int an_frame(gstate* s, const gimage* im, gevent* ev, int max);

/* ---------- desenho (draw.c) ---------- */
/* rgba de w x h; coordenadas de analise escaladas por sx = w/GW. */
void draw_rect(unsigned char* rgba, int w, int h, int x0, int y0, int x1, int y1, unsigned rgb);
void draw_cross(unsigned char* rgba, int w, int h, int x, int y, int r, unsigned rgb);
void draw_line(unsigned char* rgba, int w, int h, int x0, int y0, int x1, int y1, unsigned rgb);
void draw_mask(unsigned char* rgba, int w, int h, const gplane* mask, unsigned rgb);
void draw_analysis(unsigned char* rgba, int w, int h, const gface* f, const ghand* hd, int nh,
                   const gplane* mask, int show_mask, int detect);

/* Rastro das etapas (gesture.c, D:\TEMP\GESTRUN.LOG). */
void trace(const char* what, int v);
void trace_p(const char* what, const void* p);

#endif
