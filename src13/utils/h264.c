/* h264.c - decodificador H.264 High 4:2:0 8 bits, CABAC, quadros progressivos.
 * Sem ponto flutuante. O percentual de cada linha de macrobloco vai para
 * vid_h264_tick. vid_stop != 0 interrompe. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "h264tab.h"

void vid_h264_tick(int pct);
extern int vid_stop;

#define H264_MAXW 2048
#define H264_MAXH 2048

typedef struct h264 h264_t;

struct h264 {
  int mb_w, mb_h, width, height;
  int crop_l, crop_r, crop_t, crop_b;
  int disp_x, disp_y, disp_w, disp_h;
  int max_frame_num, max_ref;
  int poc_type, log2_fn;
  int cabac, t8x8, wpred;
  int init_qp, chroma_off, chroma_off2;
  int ref_l0_def;
  int has_sps, has_pps;
  int nal_len;
  char err[96];
  uint8_t* y;
  uint8_t* u;
  uint8_t* v;
  int stride_y, stride_c;
  uint8_t* refy[8];
  uint8_t* refu[8];
  uint8_t* refv[8];
  int ref_fn[8];
  int ref_age[8];
  int ref_n;
  int age;
  uint8_t* mbt;
  uint8_t* mb8;
  uint8_t* mbs;
  int8_t* qp;
  int16_t* mvx;
  int16_t* mvy;
  int16_t* mvdx;
  int16_t* mvdy;
  int8_t* refi;
  uint8_t* nnz;
  int8_t* mode4;
  uint8_t* cbpb;
  int8_t* cmode;
  uint8_t* cnnz;
  uint8_t* cdcz;
  int bw4, bh4;
  int cur_qp;
  int last_dqp;
  int slice_qp;
  int slice_type;
  int cabac_init;
  int nref;
  int frame_num;
  int nal_ref;
  int idr;
  int disable_df;
  int aoff, boff;
  int list_n;
  int list_slot[16];
  int w_flag[16];
  int w_luma[16];
  int w_off[16];
  int wc_flag[16];
  int wc_b[16], wc_bo[16], wc_r[16], wc_ro[16];
  int w_den, wc_den;
  uint8_t ctx[1024];
  const uint8_t* cab_b;
  int cab_n, cab_byte, cab_bit;
  int cab_range, cab_off;
};

static int clip8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return v;
}
static int iabs(int v) { return v < 0 ? -v : v; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iclip(int v, int a, int b) {
  if (v < a) return a;
  if (v > b) return b;
  return v;
}
static int median3(int a, int b, int c) {
  return a + b + c - imin(a, imin(b, c)) - imax(a, imax(b, c));
}

static void set_err(h264_t* h, const char* s) {
  int i = 0;
  if (!s) s = "erro";
  while (s[i] && i < 95) {
    h->err[i] = s[i];
    i++;
  }
  h->err[i] = 0;
}

static const uint8_t zz4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const uint8_t zz8[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
static const uint8_t blk_x[16] = {0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3};
static const uint8_t blk_y[16] = {0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3};
static const uint8_t sig8[63] = {0, 1, 2, 3, 4, 5, 5, 4, 4, 3, 3, 4, 4, 4, 5, 5, 4, 4, 4, 4, 3,
                                 3, 6, 7, 7, 7, 8, 9, 10, 9, 8, 7, 7, 6, 11, 12, 13, 11, 6, 7, 8, 9,
                                 14, 10, 9, 8, 6, 11, 12, 13, 11, 6, 9, 14, 10, 9, 11, 12, 13, 11, 14, 10, 12};
static const uint8_t last8[63] = {0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2,
                                  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4,
                                  4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8};
static const int v4[6][3] = {{10, 13, 16}, {11, 14, 18}, {13, 16, 20},
                             {14, 18, 23}, {16, 20, 25}, {18, 23, 29}};
static const int norm8[6][6] = {{20, 18, 32, 19, 25, 24}, {22, 19, 35, 21, 28, 26},
                                {26, 23, 42, 24, 33, 31}, {28, 25, 45, 26, 35, 33},
                                {32, 28, 51, 30, 40, 38}, {36, 32, 58, 34, 46, 43}};
static const uint8_t pos8[8][8] = {
    {0, 3, 4, 3, 0, 3, 4, 3}, {3, 1, 5, 1, 3, 1, 5, 1}, {4, 5, 2, 5, 4, 5, 2, 5},
    {3, 1, 5, 1, 3, 1, 5, 1}, {0, 3, 4, 3, 0, 3, 4, 3}, {3, 1, 5, 1, 3, 1, 5, 1},
    {4, 5, 2, 5, 4, 5, 2, 5}, {3, 1, 5, 1, 3, 1, 5, 1}};
static const uint8_t w8i[64] = {6,  10, 13, 16, 18, 23, 25, 27, 10, 11, 16, 18, 23, 25, 27, 29,
                                13, 16, 18, 23, 25, 27, 29, 31, 16, 18, 23, 25, 27, 29, 31, 33,
                                18, 23, 25, 27, 29, 31, 33, 36, 23, 25, 27, 29, 31, 33, 36, 38,
                                25, 27, 29, 31, 33, 36, 38, 40, 27, 29, 31, 33, 36, 38, 40, 42};
static const uint8_t w8p[64] = {9,  13, 15, 17, 19, 21, 22, 24, 13, 13, 17, 19, 21, 22, 24, 25,
                                15, 17, 19, 21, 22, 24, 25, 27, 17, 19, 21, 22, 24, 25, 27, 28,
                                19, 21, 22, 24, 25, 27, 28, 30, 21, 22, 24, 25, 27, 28, 30, 32,
                                22, 24, 25, 27, 28, 30, 32, 33, 24, 25, 27, 28, 30, 32, 33, 35};
static const uint8_t alpha_t[52] = {0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,   0,   0,   0,
                                    4,  4,  5,  6,  7,  8,  9,  10, 12, 13, 15, 17, 20,  22,  25,  28,
                                    32, 36, 40, 45, 50, 56, 63, 71, 80, 90, 101, 113, 127, 144, 162, 182,
                                    203, 226, 255, 255};
static const uint8_t beta_t[52] = {0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 2,  2,  2,  3,  3,  3,
                                   3,  4,  4,  4,  6,  6,  7,  7,  8,  8,  9,  9,  10, 10, 11, 11, 12, 12, 13, 13, 14, 14,
                                   15, 15, 16, 16, 17, 17, 18, 18};
static const int8_t tc0_t[52][3] = {
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 1, 1}, {0, 1, 1}, {1, 1, 1}, {1, 1, 1},
    {1, 1, 1}, {1, 1, 1}, {1, 1, 2}, {1, 1, 2}, {1, 1, 2}, {1, 2, 2}, {1, 2, 2}, {1, 2, 3},
    {1, 2, 3}, {2, 2, 3}, {2, 2, 4}, {2, 3, 4}, {2, 3, 4}, {3, 3, 5}, {3, 4, 6}, {3, 4, 6},
    {4, 5, 7}, {4, 5, 8}, {4, 6, 9}, {5, 7, 10}, {6, 8, 11}, {6, 8, 13}, {7, 10, 14}, {8, 11, 16},
    {9, 12, 18}, {10, 13, 20}, {11, 15, 23}, {13, 17, 25}};
static const uint8_t lps_r[64][4] = {
    {128, 176, 208, 240}, {128, 167, 197, 227}, {128, 158, 187, 216}, {123, 150, 178, 205},
    {116, 142, 169, 195}, {111, 135, 160, 185}, {105, 128, 152, 175}, {100, 122, 144, 166},
    {95, 116, 137, 158},  {90, 110, 130, 150},  {85, 104, 123, 142},  {81, 99, 117, 135},
    {77, 94, 111, 128},   {73, 89, 105, 122},   {69, 85, 100, 116},   {66, 80, 95, 110},
    {62, 76, 90, 104},    {59, 72, 86, 99},     {56, 69, 81, 94},     {53, 65, 77, 89},
    {51, 62, 73, 85},     {48, 59, 69, 80},     {46, 56, 66, 76},     {43, 53, 63, 72},
    {41, 50, 59, 69},     {39, 48, 56, 65},     {37, 45, 54, 62},     {35, 43, 51, 59},
    {33, 41, 48, 56},     {32, 39, 46, 53},     {30, 37, 43, 50},     {29, 35, 41, 48},
    {27, 33, 39, 45},     {26, 31, 37, 43},     {24, 30, 35, 41},     {23, 28, 33, 39},
    {22, 27, 32, 37},     {21, 26, 30, 35},     {20, 24, 29, 33},     {19, 23, 27, 31},
    {18, 22, 26, 30},     {17, 21, 25, 28},     {16, 20, 23, 27},     {15, 19, 22, 25},
    {14, 18, 21, 24},     {14, 17, 20, 23},     {13, 16, 19, 22},     {12, 15, 18, 21},
    {12, 14, 17, 20},     {11, 14, 16, 19},     {11, 13, 15, 18},     {10, 12, 15, 17},
    {10, 12, 14, 16},     {9, 11, 13, 15},      {9, 11, 12, 14},      {8, 10, 12, 14},
    {8, 9, 11, 13},       {7, 9, 11, 12},       {7, 9, 10, 12},       {7, 8, 10, 11},
    {6, 8, 9, 11},        {6, 7, 9, 10},        {6, 7, 8, 9},         {2, 2, 2, 2}};
static const uint8_t trans_lps[64] = {0,  0,  1,  2,  2,  4,  4,  5,  6,  7,  8,  9,  9,  11, 11, 12,
                                      13, 13, 15, 15, 16, 16, 18, 18, 19, 19, 21, 21, 22, 22, 23, 24,
                                      24, 25, 26, 26, 27, 27, 28, 29, 29, 30, 30, 30, 31, 32, 32, 33,
                                      33, 33, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 37, 38, 38, 63};
static const uint8_t chroma_qp_map[52] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17,
                                         18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 29, 30, 31, 32, 32, 33,
                                         34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39};

typedef struct {
  const uint8_t* p;
  int nbytes;
  int i;
} br_t;

static int br_bit(br_t* b) {
  int v;
  if ((b->i >> 3) >= b->nbytes) return 0;
  v = (b->p[b->i >> 3] >> (7 - (b->i & 7))) & 1;
  b->i++;
  return v;
}
static int br_bits(br_t* b, int n) {
  int v = 0;
  int k;
  for (k = 0; k < n; k++) v = (v << 1) | br_bit(b);
  return v;
}
static int br_ue(br_t* b) {
  int z = 0;
  while (br_bit(b) == 0) {
    z++;
    if (z > 31) return 0;
  }
  if (z == 0) return 0;
  return ((1 << z) - 1) + br_bits(b, z);
}
static int br_se(br_t* b) {
  int v = br_ue(b);
  if (v & 1) return (v + 1) / 2;
  return -(v / 2);
}
static int br_more(br_t* b) { return (b->i >> 3) < b->nbytes; }

static int rbsp_of(const uint8_t* nal, int n, uint8_t* dst, int cap) {
  int i = 1;
  int o = 0;
  if (n < 2) return 0;
  while (i < n && o < cap) {
    if (i + 2 < n && nal[i] == 0 && nal[i + 1] == 0 && nal[i + 2] == 3) {
      dst[o++] = 0;
      dst[o++] = 0;
      i += 3;
    } else {
      dst[o++] = nal[i++];
    }
  }
  return o;
}

static int cab_getbit(h264_t* h) {
  int v;
  if (h->cab_byte >= h->cab_n) return 0;
  v = (h->cab_b[h->cab_byte] >> (7 - h->cab_bit)) & 1;
  h->cab_bit++;
  if (h->cab_bit == 8) {
    h->cab_bit = 0;
    h->cab_byte++;
  }
  return v;
}
static void cab_renorm(h264_t* h) {
  while (h->cab_range < 256) {
    h->cab_range <<= 1;
    h->cab_off = (h->cab_off << 1) | cab_getbit(h);
  }
}
static void cab_init(h264_t* h, const uint8_t* p, int n, int byte) {
  int k;
  h->cab_b = p;
  h->cab_n = n;
  h->cab_byte = byte;
  h->cab_bit = 0;
  h->cab_range = 510;
  h->cab_off = 0;
  for (k = 0; k < 9; k++) h->cab_off = (h->cab_off << 1) | cab_getbit(h);
#ifdef H264_DEBUG
  if (!h->idr || h->frame_num == 0)
    printf("cabinit range %d off %d byte %d bit %d\n", h->cab_range, h->cab_off, h->cab_byte, h->cab_bit);
#endif
}
static int cab_bins;
static int cab_dec(h264_t* h, int idx) {
  int s = h->ctx[idx];
  int pst = s >> 1;
  int mps = s & 1;
  int q = (h->cab_range >> 6) & 3;
  int rlps = lps_r[pst][q];
  int bin;
  h->cab_range -= rlps;
  if (h->cab_off >= h->cab_range) {
    bin = 1 - mps;
    h->cab_off -= h->cab_range;
    h->cab_range = rlps;
    if (pst == 0) mps = 1 - mps;
    pst = trans_lps[pst];
  } else {
    bin = mps;
    if (pst < 63) pst++;
  }
  h->ctx[idx] = (uint8_t)((pst << 1) | mps);
  cab_renorm(h);
#ifdef H264_DEBUG
  if (cab_bins < 0 && h->frame_num == 0 && h->idr) {
    printf("bin %d ctx %d = %d\n", cab_bins, idx, bin);
    cab_bins++;
  }
#endif
  return bin;
}
static int cab_bypass(h264_t* h) {
  h->cab_off = (h->cab_off << 1) | cab_getbit(h);
  if (h->cab_off >= h->cab_range) {
    h->cab_off -= h->cab_range;
    return 1;
  }
  return 0;
}
static int cab_term(h264_t* h) {
  h->cab_range -= 2;
  if (h->cab_off >= h->cab_range) return 1;
  cab_renorm(h);
  return 0;
}
static int cab_sign(h264_t* h, int mag) {
  if (cab_bypass(h)) return -mag;
  return mag;
}

#include "h264_more.c"
#include "h264_mb.c"
