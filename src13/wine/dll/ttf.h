#ifndef WINE_TTF_H
#define WINE_TTF_H
/* ttf.c: leitura de TrueType/OpenType e rasterizacao para o DirectWrite. */
#include <stdint.h>

#define TTF_NAME 64
struct ttfont {
  const uint8_t* data;
  uint32_t len, dir, index, nfaces;
  int cff;
  uint32_t upem, nglyphs, nhmtx;
  int asc, desc, gap, x_height, cap_height, ul_pos, ul_size, strike_pos, strike_size;
  int xmin, ymin, xmax, ymax;
  int weight, width, italic;
  uint32_t mac_style;
  int loca_long;
  const uint8_t *hmtx, *glyf, *loca, *cmap;
  uint32_t hmtx_len, glyf_len, loca_len, cmap_len;
  int cmap_fmt;
  wchar16 family[TTF_NAME], style[TTF_NAME], win_family[TTF_NAME], win_style[TTF_NAME], full[TTF_NAME],
      postscript[TTF_NAME];
};

/* contorno em unidades da fonte (y para cima) */
struct ttf_sink {
  void* ctx;
  void (*move)(void* ctx, float x, float y);
  void (*line)(void* ctx, float x, float y);
  void (*quad)(void* ctx, float cx, float cy, float x, float y);
  void (*close)(void* ctx);
};

int ttf_open(struct ttfont* f, const uint8_t* data, uint32_t len, uint32_t index);
const uint8_t* ttf_table(const struct ttfont* f, uint32_t tag, uint32_t* len);
uint32_t ttf_glyph(const struct ttfont* f, uint32_t codepoint);
int ttf_advance(const struct ttfont* f, uint32_t g);
int ttf_lsb(const struct ttfont* f, uint32_t g);
int ttf_bbox(const struct ttfont* f, uint32_t g, int* x0, int* y0, int* x1, int* y1);
int ttf_outline(const struct ttfont* f, uint32_t g, const struct ttf_sink* s);

/* rasterizacao de varios glifos juntos (uma "glyph run") */
struct ttf_raster {
  void* edges;
  int n, cap;
};
void ttf_raster_begin(struct ttf_raster* rs);
/* m: unidades da fonte -> pixels (y para baixo): x' = m0*x + m2*y + m4, y' = m1*x + m3*y + m5 */
void ttf_raster_add(struct ttf_raster* rs, const struct ttfont* f, uint32_t g, const float* m);
void ttf_raster_bounds(const struct ttf_raster* rs, int* l, int* t, int* r, int* b);
void ttf_raster_fill(const struct ttf_raster* rs, int l, int t, int r, int b, uint8_t* out, int stride, int bpp);
void ttf_raster_end(struct ttf_raster* rs);
#endif
