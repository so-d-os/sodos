/* HAAR.H - detector de cascatas Haar (haar.c). */
#ifndef HAAR_H
#define HAAR_H

typedef struct {
  int nr;
  int x[3], y[3], w[3], h[3];
  float wt[3];
} hfeat;

typedef struct {
  int left, right, feat;
  float thr;
} hnode;

typedef struct {
  int nn, node0, nleaf, leaf0;
} hweak;

typedef struct {
  int nweak, weak0;
  float thr;
} hstage;

typedef struct {
  int w, h, nst, nf;    /* janela do treino, estagios, features */
  hfeat* f;
  hstage* st;
  hweak* wk;
  hnode* nd;
  float* lv;
} gcascade;

typedef struct {
  int x, y, w, h;
  int n;                /* quantas janelas cairam no grupo (confianca) */
} grect;

/* Le um .CAS (tools\cascade2bin.py). 0 = ok. */
int haar_load(gcascade* c, const char* path);
/* Procura no retangulo [x0,x1)x[y0,y1) da imagem (8 bits, stride bytes por
 * linha) objetos de min_size a max_size pixels; grupos com mais de min_nb
 * janelas. Devolve quantos (ate max), em out. */
int haar_detect(const gcascade* c, const unsigned char* img, int stride, int x0, int y0, int x1, int y1,
                int min_size, int max_size, int min_nb, grect* out, int max);

#endif
