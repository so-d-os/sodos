static void ctx_init(h264_t* h, int is_i, int qp, int idc) {
  const signed char* mn = h264_mn_I;
  int i;
  if (!is_i) {
    if (idc == 1) mn = h264_mn_P1;
    else if (idc >= 2) mn = h264_mn_P2;
    else mn = h264_mn_P0;
  }
  if (qp < 0) qp = 0;
  if (qp > 51) qp = 51;
  for (i = 0; i < 1024; i++) {
    int t = ((mn[i * 2] * qp) >> 4) + mn[i * 2 + 1];
    int pst, mps;
    if (t < 1) t = 1;
    if (t > 126) t = 126;
    if (t <= 63) {
      pst = 63 - t;
      mps = 0;
    } else {
      pst = t - 64;
      mps = 1;
    }
    h->ctx[i] = (uint8_t)((pst << 1) | mps);
  }
}

static int qcls4(int x, int y) {
  if (((x | y) & 1) == 0) return 0;
  if ((x & 1) && (y & 1)) return 2;
  return 1;
}

static void idct4(const int* d, int* r) {
  int col[4][4];
  int i;
  for (i = 0; i < 4; i++) {
    int d0 = d[i], d1 = d[4 + i], d2 = d[8 + i], d3 = d[12 + i];
    int t0 = d0 + d2;
    int t1 = d0 - d2;
    int t2 = (d1 >> 1) - d3;
    int t3 = d1 + (d3 >> 1);
    col[0][i] = t0 + t3;
    col[1][i] = t1 + t2;
    col[2][i] = t1 - t2;
    col[3][i] = t0 - t3;
  }
  for (i = 0; i < 4; i++) {
    int d0 = col[i][0], d1 = col[i][1], d2 = col[i][2], d3 = col[i][3];
    int t0 = d0 + d2;
    int t1 = d0 - d2;
    int t2 = (d1 >> 1) - d3;
    int t3 = d1 + (d3 >> 1);
    r[i * 4 + 0] = (t0 + t3 + 32) >> 6;
    r[i * 4 + 1] = (t1 + t2 + 32) >> 6;
    r[i * 4 + 2] = (t1 - t2 + 32) >> 6;
    r[i * 4 + 3] = (t0 - t3 + 32) >> 6;
  }
}

static void ihada4(const int* s, int* d) {
  int tmp[16];
  int i;
  for (i = 0; i < 4; i++) {
    int a = s[i * 4], b = s[i * 4 + 1], c = s[i * 4 + 2], e = s[i * 4 + 3];
    tmp[i * 4 + 0] = a + b + c + e;
    tmp[i * 4 + 1] = a + b - c - e;
    tmp[i * 4 + 2] = a - b - c + e;
    tmp[i * 4 + 3] = a - b + c - e;
  }
  for (i = 0; i < 4; i++) {
    int a = tmp[i], b = tmp[4 + i], c = tmp[8 + i], e = tmp[12 + i];
    d[i] = a + b + c + e;
    d[4 + i] = a + b - c - e;
    d[8 + i] = a - b - c + e;
    d[12 + i] = a - b + c - e;
  }
}

static void idct8_1d(const int* d, int* r, int shift) {
  int d0 = d[0], d1 = d[1], d2 = d[2], d3 = d[3], d4 = d[4], d5 = d[5], d6 = d[6], d7 = d[7];
  int a0 = d0 + d4;
  int a2 = d0 - d4;
  int a4 = (d2 >> 1) - d6;
  int a6 = d2 + (d6 >> 1);
  int a1 = -d3 + d5 - d7 - (d7 >> 1);
  int a3 = d1 + d7 - d3 - (d3 >> 1);
  int a5 = -d1 + d7 + d5 + (d5 >> 1);
  int a7 = d3 + d5 + d1 + (d1 >> 1);
  int b0 = a0 + a6;
  int b2 = a2 + a4;
  int b4 = a2 - a4;
  int b6 = a0 - a6;
  int b1 = a1 + (a7 >> 2);
  int b3 = a3 + (a5 >> 2);
  int b5 = (a3 >> 2) - a5;
  int b7 = a7 - (a1 >> 2);
  int add = shift ? 32 : 0;
  int sh = shift ? 6 : 0;
  r[0] = (b0 + b7 + add) >> sh;
  r[1] = (b2 + b5 + add) >> sh;
  r[2] = (b4 + b3 + add) >> sh;
  r[3] = (b6 + b1 + add) >> sh;
  r[4] = (b6 - b1 + add) >> sh;
  r[5] = (b4 - b3 + add) >> sh;
  r[6] = (b2 - b5 + add) >> sh;
  r[7] = (b0 - b7 + add) >> sh;
}

static void idct8(const int* d, int* r) {
  int tmp[64];
  int row[8], col[8], o[8];
  int i, j;
  for (i = 0; i < 8; i++) {
    for (j = 0; j < 8; j++) row[j] = d[i * 8 + j];
    idct8_1d(row, o, 0);
    for (j = 0; j < 8; j++) tmp[i * 8 + j] = o[j];
  }
  for (j = 0; j < 8; j++) {
    for (i = 0; i < 8; i++) col[i] = tmp[i * 8 + j];
    idct8_1d(col, o, 1);
    for (i = 0; i < 8; i++) r[i * 8 + j] = o[i];
  }
}

static int dequant4(int level, int qp, int x, int y) {
  int q = qp < 0 ? 0 : qp;
  int cls;
  if (q > 51) q = 51;
  cls = qcls4(x, y);
  return (level * v4[q % 6][cls]) << (q / 6);
}

static int dequant8(int level, int qp, int x, int y, int intra) {
  int q = qp < 0 ? 0 : qp;
  const uint8_t* w = intra ? w8i : w8p;
  int cls;
  if (q > 51) q = 51;
  cls = pos8[y & 7][x & 7];
  {
    int ls = (int)w[(y & 7) * 8 + (x & 7)] * norm8[q % 6][cls];
    int sh = q / 6;
    if (sh >= 6) return (level * ls) << (sh - 6);
    return (level * ls + (1 << (5 - sh))) >> (6 - sh);
  }
}

static int scale_dc16(int f, int qp) {
  int q = qp < 0 ? 0 : qp;
  int ls;
  if (q > 51) q = 51;
  ls = 16 * v4[q % 6][0];
  if (q / 6 >= 2) return (f * ls) << (q / 6 - 2);
  return (f * ls + (1 << (1 - q / 6))) >> (2 - q / 6);
}

static int scale_dc2(int f, int qp) {
  int q = qp < 0 ? 0 : qp;
  int ls;
  if (q > 51) q = 51;
  ls = 16 * v4[q % 6][0];
  if (q / 6 >= 1) return (f * ls) << (q / 6 - 1);
  return (f * ls) >> 1;
}

static int chroma_qp(h264_t* h, int qp, int plane) {
  int off = plane ? h->chroma_off2 : h->chroma_off;
  int i = qp + off;
  if (i < 0) i = 0;
  if (i > 51) i = 51;
  return chroma_qp_map[i];
}

static int y_at(h264_t* h, int x, int y) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= h->width) x = h->width - 1;
  if (y >= h->height) y = h->height - 1;
  return h->y[y * h->stride_y + x];
}
static int c_at(const uint8_t* p, int stride, int w, int ht, int x, int y) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= w) x = w - 1;
  if (y >= ht) y = ht - 1;
  return p[y * stride + x];
}

static int tap6(int a, int b, int c, int d, int e, int f) {
  return a - 5 * b + 20 * c + 20 * d - 5 * e + f;
}

static int luma_qpel(h264_t* h, const uint8_t* src, int xq, int yq) {
  int x = xq >> 2;
  int y = yq >> 2;
  int fx = xq & 3;
  int fy = yq & 3;
  int bh, bv, bb;
  int sw = h->width;
  int sh = h->height;
  int st = h->stride_y;
  if (fx == 0 && fy == 0) return c_at(src, st, sw, sh, x, y);
  bh = tap6(c_at(src, st, sw, sh, x - 2, y), c_at(src, st, sw, sh, x - 1, y),
            c_at(src, st, sw, sh, x, y), c_at(src, st, sw, sh, x + 1, y),
            c_at(src, st, sw, sh, x + 2, y), c_at(src, st, sw, sh, x + 3, y));
  bv = tap6(c_at(src, st, sw, sh, x, y - 2), c_at(src, st, sw, sh, x, y - 1),
            c_at(src, st, sw, sh, x, y), c_at(src, st, sw, sh, x, y + 1),
            c_at(src, st, sw, sh, x, y + 2), c_at(src, st, sw, sh, x, y + 3));
  if (fx == 0 && fy == 2) return clip8((bv + 16) >> 5);
  if (fx == 2 && fy == 0) return clip8((bh + 16) >> 5);
  if (fx == 0 && fy == 1) return (c_at(src, st, sw, sh, x, y) + clip8((bv + 16) >> 5) + 1) >> 1;
  if (fx == 0 && fy == 3)
    return (c_at(src, st, sw, sh, x, y + 1) + clip8((bv + 16) >> 5) + 1) >> 1;
  if (fx == 1 && fy == 0) return (c_at(src, st, sw, sh, x, y) + clip8((bh + 16) >> 5) + 1) >> 1;
  if (fx == 3 && fy == 0)
    return (c_at(src, st, sw, sh, x + 1, y) + clip8((bh + 16) >> 5) + 1) >> 1;
  bb = tap6(tap6(c_at(src, st, sw, sh, x - 2, y - 2), c_at(src, st, sw, sh, x - 2, y - 1),
                 c_at(src, st, sw, sh, x - 2, y), c_at(src, st, sw, sh, x - 2, y + 1),
                 c_at(src, st, sw, sh, x - 2, y + 2), c_at(src, st, sw, sh, x - 2, y + 3)),
            tap6(c_at(src, st, sw, sh, x - 1, y - 2), c_at(src, st, sw, sh, x - 1, y - 1),
                 c_at(src, st, sw, sh, x - 1, y), c_at(src, st, sw, sh, x - 1, y + 1),
                 c_at(src, st, sw, sh, x - 1, y + 2), c_at(src, st, sw, sh, x - 1, y + 3)),
            tap6(c_at(src, st, sw, sh, x, y - 2), c_at(src, st, sw, sh, x, y - 1),
                 c_at(src, st, sw, sh, x, y), c_at(src, st, sw, sh, x, y + 1),
                 c_at(src, st, sw, sh, x, y + 2), c_at(src, st, sw, sh, x, y + 3)),
            tap6(c_at(src, st, sw, sh, x + 1, y - 2), c_at(src, st, sw, sh, x + 1, y - 1),
                 c_at(src, st, sw, sh, x + 1, y), c_at(src, st, sw, sh, x + 1, y + 1),
                 c_at(src, st, sw, sh, x + 1, y + 2), c_at(src, st, sw, sh, x + 1, y + 3)),
            tap6(c_at(src, st, sw, sh, x + 2, y - 2), c_at(src, st, sw, sh, x + 2, y - 1),
                 c_at(src, st, sw, sh, x + 2, y), c_at(src, st, sw, sh, x + 2, y + 1),
                 c_at(src, st, sw, sh, x + 2, y + 2), c_at(src, st, sw, sh, x + 2, y + 3)),
            tap6(c_at(src, st, sw, sh, x + 3, y - 2), c_at(src, st, sw, sh, x + 3, y - 1),
                 c_at(src, st, sw, sh, x + 3, y), c_at(src, st, sw, sh, x + 3, y + 1),
                 c_at(src, st, sw, sh, x + 3, y + 2), c_at(src, st, sw, sh, x + 3, y + 3)));
  if (fx == 2 && fy == 2) return clip8((bb + 512) >> 10);
  if (fx == 2 && fy == 1) return (clip8((bh + 16) >> 5) + clip8((bb + 512) >> 10) + 1) >> 1;
  if (fx == 2 && fy == 3) {
    int bh2 = tap6(c_at(src, st, sw, sh, x - 2, y + 1), c_at(src, st, sw, sh, x - 1, y + 1),
                   c_at(src, st, sw, sh, x, y + 1), c_at(src, st, sw, sh, x + 1, y + 1),
                   c_at(src, st, sw, sh, x + 2, y + 1), c_at(src, st, sw, sh, x + 3, y + 1));
    return (clip8((bh2 + 16) >> 5) + clip8((bb + 512) >> 10) + 1) >> 1;
  }
  if (fy == 2 && fx == 1) return (clip8((bv + 16) >> 5) + clip8((bb + 512) >> 10) + 1) >> 1;
  if (fy == 2 && fx == 3) {
    int bv2 = tap6(c_at(src, st, sw, sh, x + 1, y - 2), c_at(src, st, sw, sh, x + 1, y - 1),
                   c_at(src, st, sw, sh, x + 1, y), c_at(src, st, sw, sh, x + 1, y + 1),
                   c_at(src, st, sw, sh, x + 1, y + 2), c_at(src, st, sw, sh, x + 1, y + 3));
    return (clip8((bv2 + 16) >> 5) + clip8((bb + 512) >> 10) + 1) >> 1;
  }
  if (fx == 1 && fy == 1) return (clip8((bh + 16) >> 5) + clip8((bv + 16) >> 5) + 1) >> 1;
  if (fx == 3 && fy == 1) {
    int bh2 = tap6(c_at(src, st, sw, sh, x - 1, y), c_at(src, st, sw, sh, x, y),
                   c_at(src, st, sw, sh, x + 1, y), c_at(src, st, sw, sh, x + 2, y),
                   c_at(src, st, sw, sh, x + 3, y), c_at(src, st, sw, sh, x + 4, y));
    return (clip8((bh2 + 16) >> 5) + clip8((bv + 16) >> 5) + 1) >> 1;
  }
  if (fx == 1 && fy == 3) {
    int bv2 = tap6(c_at(src, st, sw, sh, x, y - 1), c_at(src, st, sw, sh, x, y),
                   c_at(src, st, sw, sh, x, y + 1), c_at(src, st, sw, sh, x, y + 2),
                   c_at(src, st, sw, sh, x, y + 3), c_at(src, st, sw, sh, x, y + 4));
    return (clip8((bh + 16) >> 5) + clip8((bv2 + 16) >> 5) + 1) >> 1;
  }
  {
    int bh2 = tap6(c_at(src, st, sw, sh, x - 1, y + 1), c_at(src, st, sw, sh, x, y + 1),
                   c_at(src, st, sw, sh, x + 1, y + 1), c_at(src, st, sw, sh, x + 2, y + 1),
                   c_at(src, st, sw, sh, x + 3, y + 1), c_at(src, st, sw, sh, x + 4, y + 1));
    int bv2 = tap6(c_at(src, st, sw, sh, x + 1, y - 1), c_at(src, st, sw, sh, x + 1, y),
                   c_at(src, st, sw, sh, x + 1, y + 1), c_at(src, st, sw, sh, x + 1, y + 2),
                   c_at(src, st, sw, sh, x + 1, y + 3), c_at(src, st, sw, sh, x + 1, y + 4));
    return (clip8((bh2 + 16) >> 5) + clip8((bv2 + 16) >> 5) + 1) >> 1;
  }
}

static void mc_luma(h264_t* h, const uint8_t* src, int x, int y, int w, int ht, int mvx, int mvy) {
  int j, i;
  for (j = 0; j < ht; j++) {
    for (i = 0; i < w; i++) {
      int xq = (x + i) * 4 + mvx;
      int yq = (y + j) * 4 + mvy;
      h->y[(y + j) * h->stride_y + (x + i)] = (uint8_t)luma_qpel(h, src, xq, yq);
    }
  }
}

static void mc_chroma(h264_t* h, const uint8_t* src, int x, int y, int w, int ht, int mvx, int mvy) {
  int sw = h->width / 2;
  int sh = h->height / 2;
  int st = h->stride_c;
  int j, i;
  for (j = 0; j < ht; j++) {
    for (i = 0; i < w; i++) {
      int xq = (x + i) * 8 + mvx;
      int yq = (y + j) * 8 + mvy;
      int xi = xq >> 3;
      int yi = yq >> 3;
      int xf = xq & 7;
      int yf = yq & 7;
      int a = c_at(src, st, sw, sh, xi, yi);
      int b = c_at(src, st, sw, sh, xi + 1, yi);
      int c = c_at(src, st, sw, sh, xi, yi + 1);
      int d = c_at(src, st, sw, sh, xi + 1, yi + 1);
      int v = ((8 - xf) * (8 - yf) * a + xf * (8 - yf) * b + (8 - xf) * yf * c + xf * yf * d + 32) >> 6;
      if (src == h->u) h->u[(y + j) * st + (x + i)] = (uint8_t)clip8(v);
      else h->v[(y + j) * st + (x + i)] = (uint8_t)clip8(v);
    }
  }
}

static void apply_w(uint8_t* p, int n, int den, int w, int off) {
  int i;
  for (i = 0; i < n; i++) {
    int s = p[i];
    int o;
    if (den >= 1) o = ((s * w + (1 << (den - 1))) >> den) + off;
    else o = s * w + off;
    p[i] = (uint8_t)clip8(o);
  }
}

static int idx4(h264_t* h, int bx, int by) { return by * h->bw4 + bx; }

static int mb_ok(h264_t* h, int mx, int my) {
  if (mx < 0 || my < 0 || mx >= h->mb_w || my >= h->mb_h) return 0;
  return h->mbs[my * h->mb_w + mx] != 0;
}

static int pred_mv(h264_t* h, int bx, int by, int bw, int bh, int ref, int* ox, int* oy) {
  int ax = bx - 1, ay = by;
  int bxp = bx, byp = by - 1;
  int cx = bx + bw, cy = by - 1;
  int dx = bx - 1, dy = by - 1;
  int ha = 0, hb = 0, hc = 0, hd = 0;
  int ra = -1, rb = -1, rc = -1, rd = -1;
  int max, mby, mbx;
  if (ax >= 0 && ay >= 0 && ay < h->bh4 && ax < h->bw4) {
    ha = 1;
    ra = h->refi[idx4(h, ax, ay)];
  }
  if (bxp >= 0 && byp >= 0 && bxp < h->bw4) {
    hb = 1;
    rb = h->refi[idx4(h, bxp, byp)];
  }
  if (cx >= 0 && cy >= 0 && cx < h->bw4) {
    hc = 1;
    rc = h->refi[idx4(h, cx, cy)];
  } else if (dx >= 0 && dy >= 0 && dx < h->bw4) {
    hc = 1;
    rc = h->refi[idx4(h, dx, dy)];
    cx = dx;
    cy = dy;
    hd = 1;
  }
  (void)hd;
  if (bw == 4 && bh == 2 && hb && rb == ref) {
    *ox = h->mvx[idx4(h, bxp, byp)];
    *oy = h->mvy[idx4(h, bxp, byp)];
    return 0;
  }
  if (bw == 4 && bh == 2 && by > 0 && ha && ra == ref && (by & 2)) {
    *ox = h->mvx[idx4(h, ax, ay)];
    *oy = h->mvy[idx4(h, ax, ay)];
    return 0;
  }
  if (bw == 2 && bh == 4 && ha && ra == ref && (bx & 2) == 0) {
    *ox = h->mvx[idx4(h, ax, ay)];
    *oy = h->mvy[idx4(h, ax, ay)];
    return 0;
  }
  if (bw == 2 && bh == 4 && hc && rc == ref && (bx & 2)) {
    *ox = h->mvx[idx4(h, cx, cy)];
    *oy = h->mvy[idx4(h, cx, cy)];
    return 0;
  }
  {
    int n = ha + hb + hc;
    if (n == 0) {
      *ox = *oy = 0;
      return 0;
    }
    if (n == 1) {
      if (ha) {
        *ox = h->mvx[idx4(h, ax, ay)];
        *oy = h->mvy[idx4(h, ax, ay)];
      } else if (hb) {
        *ox = h->mvx[idx4(h, bxp, byp)];
        *oy = h->mvy[idx4(h, bxp, byp)];
      } else {
        *ox = h->mvx[idx4(h, cx, cy)];
        *oy = h->mvy[idx4(h, cx, cy)];
      }
      return 0;
    }
  }
  {
    int mxv[3], myv[3], rr[3], k = 0, i;
    if (ha) {
      mxv[k] = ra < 0 ? 0 : h->mvx[idx4(h, ax, ay)];
      myv[k] = ra < 0 ? 0 : h->mvy[idx4(h, ax, ay)];
      rr[k] = ra;
      k++;
    }
    if (hb) {
      mxv[k] = rb < 0 ? 0 : h->mvx[idx4(h, bxp, byp)];
      myv[k] = rb < 0 ? 0 : h->mvy[idx4(h, bxp, byp)];
      rr[k] = rb;
      k++;
    }
    if (hc) {
      mxv[k] = rc < 0 ? 0 : h->mvx[idx4(h, cx, cy)];
      myv[k] = rc < 0 ? 0 : h->mvy[idx4(h, cx, cy)];
      rr[k] = rc;
      k++;
    }
    if (k == 2) {
      mxv[2] = 0;
      myv[2] = 0;
      rr[2] = -1;
      k = 3;
    }
    {
      int same = 0, which = -1;
      for (i = 0; i < 3; i++)
        if (rr[i] == ref) {
          same++;
          which = i;
        }
      if (same == 1 && which >= 0) {
        *ox = mxv[which];
        *oy = myv[which];
        return 0;
      }
    }
    *ox = median3(mxv[0], mxv[1], mxv[2]);
    *oy = median3(myv[0], myv[1], myv[2]);
  }
  (void)max;
  (void)mbx;
  (void)mby;
  return 0;
}

static void put_mv(h264_t* h, int bx, int by, int bw, int bh, int mx, int my, int mdx, int mdy, int ref) {
  int y, x;
  for (y = 0; y < bh; y++) {
    for (x = 0; x < bw; x++) {
      int i = idx4(h, bx + x, by + y);
      h->mvx[i] = (int16_t)mx;
      h->mvy[i] = (int16_t)my;
      h->mvdx[i] = (int16_t)mdx;
      h->mvdy[i] = (int16_t)mdy;
      h->refi[i] = (int8_t)ref;
    }
  }
}

static int neigh_mvd(h264_t* h, int bx, int by, int comp) {
  int s = 0;
  if (bx > 0 && by >= 0) s += iabs(comp ? h->mvdy[idx4(h, bx - 1, by)] : h->mvdx[idx4(h, bx - 1, by)]);
  if (by > 0) s += iabs(comp ? h->mvdy[idx4(h, bx, by - 1)] : h->mvdx[idx4(h, bx, by - 1)]);
  return s;
}

static int dec_mvd(h264_t* h, int amvd) {
  int ctx = (amvd > 2) + (amvd > 32);
  int mvd;
  int base;
  if (!cab_dec(h, 40 + ctx) && 0) return 0;
  return 0;
}

static int dec_mvd_comp(h264_t* h, int base, int amvd) {
  int ctx = (amvd > 2) + (amvd > 32);
  int mvd = 0;
  int k;
  if (!cab_dec(h, base + ctx)) return 0;
  mvd = 1;
  ctx = 3;
  while (mvd < 9 && cab_dec(h, base + ctx)) {
    if (mvd < 4) ctx++;
    mvd++;
  }
  if (mvd >= 9) {
    k = 3;
    while (cab_bypass(h) && k < 24) {
      mvd += 1 << k;
      k++;
    }
    while (k--) mvd += cab_bypass(h) << k;
  }
  return cab_sign(h, mvd);
}

static int dec_ref(h264_t* h, int bx, int by) {
  int ra = bx > 0 ? h->refi[idx4(h, bx - 1, by)] : -1;
  int rb = by > 0 ? h->refi[idx4(h, bx, by - 1)] : -1;
  int ctx = 0;
  int ref = 0;
  if (ra > 0) ctx++;
  if (rb > 0) ctx += 2;
  while (cab_dec(h, 54 + ctx)) {
    ref++;
    ctx = (ctx >> 2) + 4;
    if (ref > 15) break;
  }
  return ref;
}

static void add_block(h264_t* h, int x, int y, const int* r, int n) {
  int j, i;
  for (j = 0; j < n; j++) {
    for (i = 0; i < n; i++) {
      int p = h->y[(y + j) * h->stride_y + (x + i)];
      h->y[(y + j) * h->stride_y + (x + i)] = (uint8_t)clip8(p + r[j * n + i]);
    }
  }
}
static void add_c(uint8_t* p, int st, int x, int y, const int* r, int n) {
  int j, i;
  for (j = 0; j < n; j++)
    for (i = 0; i < n; i++) p[(y + j) * st + (x + i)] = (uint8_t)clip8(p[(y + j) * st + (x + i)] + r[j * n + i]);
}

static void pred4(h264_t* h, int x, int y, int mode, uint8_t* dst) {
  int top[8], left[8], tl, ht = 1, hl = 1, i, j;
  if (y <= 0) ht = 0;
  if (x <= 0) hl = 0;
  tl = (ht && hl) ? y_at(h, x - 1, y - 1) : 128;
  for (i = 0; i < 8; i++) {
    top[i] = ht ? y_at(h, x + i, y - 1) : 128;
    left[i] = hl ? y_at(h, x - 1, y + i) : 128;
  }
  if (!ht && hl) {
    for (i = 0; i < 8; i++) top[i] = left[0];
    tl = left[0];
  }
  if (!hl && ht) {
    for (i = 0; i < 8; i++) left[i] = top[0];
    tl = top[0];
  }
  if (!ht || mode == 0 || mode == 3 || mode == 7) {
    if (!ht) mode = (!hl) ? 2 : 1;
  }
  if (!hl && (mode == 1 || mode == 8)) mode = ht ? 0 : 2;
  if (mode == 2) {
    int s = 0, n = 0, dc;
    if (ht) {
      for (i = 0; i < 4; i++) s += top[i];
      n += 4;
    }
    if (hl) {
      for (i = 0; i < 4; i++) s += left[i];
      n += 4;
    }
    dc = n ? (s + n / 2) / n : 128;
    for (i = 0; i < 16; i++) dst[i] = (uint8_t)dc;
    return;
  }
  for (j = 0; j < 4; j++) {
    for (i = 0; i < 4; i++) {
      int v = 128;
      if (mode == 0) v = top[i];
      else if (mode == 1) v = left[j];
      else if (mode == 3) v = (top[i + j] + 2 * top[i + j + 1] + top[i + j + 2] + 2) >> 2;
      else if (mode == 4) {
        if (i > j) v = (top[i - j - 2] + 2 * top[i - j - 1] + top[i - j] + 2) >> 2;
        else if (i == j) v = (tl + 2 * top[0] + top[1] + 2) >> 2;
        else if (j == i + 1) v = (left[0] + 2 * tl + top[0] + 2) >> 2;
        else v = (left[j - i - 2] + 2 * left[j - i - 1] + left[j - i] + 2) >> 2;
      } else if (mode == 5) {
        int z = 2 * i - j;
        if (z == 0 || z == 1 || z == -1) {
          int a = z >= 0 ? top[i - (j >> 1) - (j & 1)] : left[0];
          (void)a;
        }
        if (j == 0) v = (tl + top[i] + 1) >> 1;
        else if (j == 1) v = (tl + 2 * top[i] + top[i + 1 < 8 ? i + 1 : i] + 2) >> 2;
        else if (j == 2) v = (top[i > 0 ? i - 1 : 0] + 2 * top[i] + top[i + 1 < 8 ? i + 1 : i] + 2) >> 2;
        else v = (top[i > 1 ? i - 2 : 0] + 2 * top[i > 0 ? i - 1 : 0] + top[i] + 2) >> 2;
        if (i == 0 && j == 2) v = (left[0] + 2 * tl + top[0] + 2) >> 2;
        if (i == 0 && j == 3) v = (tl + 2 * left[0] + left[1] + 2) >> 2;
        if (i == 1 && j == 3) v = (left[0] + 2 * tl + top[0] + 2) >> 2;
      } else if (mode == 6) {
        if (j > i + 1) v = (left[j - i - 2] + 2 * left[j - i - 1] + left[j - i] + 2) >> 2;
        else if (j == i + 1) v = (left[0] + 2 * tl + top[0] + 2) >> 2;
        else if (i == j) v = (tl + 2 * left[j] + left[j + 1] + 2) >> 2;
        else v = (left[j] + 2 * left[j + 1] + left[j + 2] + 2) >> 2;
      } else if (mode == 7) {
        v = (top[i + j] + 2 * top[i + j + 1] + top[i + j + 2 < 8 ? i + j + 2 : 7] + 2) >> 2;
        if ((j & 1) == 0) v = (top[i + (j >> 1)] + top[i + (j >> 1) + 1] + 1) >> 1;
      } else if (mode == 8) {
        if (i == 0 && j < 2) v = (left[j] + left[j + 1] + 1) >> 1;
        else if (j + i < 3) v = (left[j + i] + 2 * left[j + i + 1] + left[j + i + 2] + 2) >> 2;
        else v = left[3];
      }
      dst[j * 4 + i] = (uint8_t)v;
    }
  }
}

static void fill_pred(h264_t* h, int x, int y, const uint8_t* blk, int n) {
  int j, i;
  for (j = 0; j < n; j++)
    for (i = 0; i < n; i++) h->y[(y + j) * h->stride_y + (x + i)] = blk[j * n + i];
}

static int cbf(h264_t* h, int cat, int left_nz, int top_nz) {
  static const int base[6] = {85, 89, 93, 97, 101, 1012};
  int ctx = 0;
  int b;
  if (cat < 0) cat = 0;
  if (cat > 5) cat = 5;
  if (left_nz > 0) ctx++;
  if (top_nz > 0) ctx += 2;
  b = base[cat];
  if (cat == 5) b = 1012;
  return cab_dec(h, b + ctx);
}

static int decode_coefs(h264_t* h, int* dst, int ncoef, int cat, int is8) {
  static const int sigb[6] = {105, 120, 134, 149, 152, 402};
  static const int lastb[6] = {166, 181, 195, 210, 213, 417};
  static const int absb[6] = {227, 237, 247, 257, 266, 426};
  int sigbase, lastbase, absbase;
  int index[64];
  int count = 0;
  int last;
  int node = 0;
  int k;
  static const int l1[8] = {1, 2, 3, 4, 0, 0, 0, 0};
  static const int gt[8] = {5, 5, 5, 5, 6, 7, 8, 9};
  static const int tr0[8] = {1, 2, 3, 3, 4, 5, 6, 7};
  static const int tr1[8] = {4, 4, 4, 4, 5, 6, 7, 7};
  if (cat < 0) cat = 0;
  if (cat > 5) cat = 5;
  sigbase = sigb[cat];
  lastbase = lastb[cat];
  absbase = absb[cat];
  for (k = 0; k < ncoef; k++) dst[k] = 0;
  for (last = 0; last < ncoef - 1; last++) {
    int so = is8 ? sig8[last] : last;
    int lo = is8 ? last8[last] : last;
    if (cab_dec(h, sigbase + so)) {
      index[count++] = last;
      if (cab_dec(h, lastbase + lo)) {
        last = ncoef;
        break;
      }
    }
  }
  if (last == ncoef - 1) index[count++] = last;
  for (k = count - 1; k >= 0; k--) {
    int ctx = absbase + l1[node];
    int mag;
    int pos = index[k];
    int zz = is8 ? zz8[pos] : (ncoef == 15 ? zz4[pos + 1] : (ncoef == 16 ? zz4[pos] : pos));
    if (ncoef == 4) zz = pos;
    if (!cab_dec(h, ctx)) {
      node = tr0[node];
      mag = 1;
    } else {
      mag = 2;
      ctx = absbase + gt[node];
      node = tr1[node];
      while (mag < 15 && cab_dec(h, ctx)) mag++;
      if (mag >= 15) {
        int len = 0;
        int suf = 0;
        int b;
        while (cab_bypass(h) && len < 16) len++;
        for (b = len - 1; b >= 0; b--)
          if (cab_bypass(h)) suf |= 1 << b;
        mag += ((1 << len) - 1) + suf;
      }
    }
    dst[zz] = cab_sign(h, mag);
  }
  return count;
}

static void recon4(h264_t* h, int x, int y, const int* coef, int qp, int dc) {
  int d[16], r[16], i;
  int xx, yy;
  for (i = 0; i < 16; i++) {
    yy = i >> 2;
    xx = i & 3;
    if (i == 0 && dc) d[0] = dc;
    else d[i] = dequant4(coef[i], qp, xx, yy);
  }
  idct4(d, r);
  add_block(h, x, y, r, 4);
}

static void recon8(h264_t* h, int x, int y, const int* coef, int qp, int intra) {
  int d[64], r[64], i;
  for (i = 0; i < 64; i++) {
    int rx = zz8[i] & 7;
    int ry = zz8[i] >> 3;
    d[ry * 8 + rx] = dequant8(coef[zz8[i]], qp, rx, ry, intra);
  }
  idct8(d, r);
  add_block(h, x, y, r, 8);
}

static int decode_mb(h264_t* h, int mb, int mx, int my, int st);
static void deblock_pic(h264_t* h);

static int decode_slice(h264_t* h, const uint8_t* rb, int n) {
  br_t br;
  int first, st, fn, i;
  int nal_is_idr = h->idr;
  br.p = rb;
  br.nbytes = n;
  br.i = 0;
  first = br_ue(&br);
  st = br_ue(&br) % 5;
  br_ue(&br);
  fn = br_bits(&br, h->log2_fn > 0 ? h->log2_fn : 4);
  h->frame_num = fn;
  h->slice_type = st;
  if (nal_is_idr) br_ue(&br);
  h->nref = h->ref_l0_def > 0 ? h->ref_l0_def : 1;
  if (st == 0) {
    if (br_bit(&br)) h->nref = br_ue(&br) + 1;
    if (h->nref < 1) h->nref = 1;
    if (h->nref > 16) h->nref = 16;
    if (br_bit(&br)) {
      int pred = fn;
      int li = 0;
      h->list_n = 0;
      for (i = 0; i < 32 && li < h->nref; i++) {
        int op = br_ue(&br);
        int pic, slot, k;
        if (op == 3) break;
        if (op == 0 || op == 1) {
          int ad = br_ue(&br) + 1;
          int nw;
          if (op == 0) {
            nw = pred - ad;
            if (nw < 0) nw += h->max_frame_num;
          } else {
            nw = pred + ad;
            if (nw >= h->max_frame_num) nw -= h->max_frame_num;
          }
          pred = nw;
          pic = nw;
          if (pic > fn) pic -= h->max_frame_num;
          slot = 0;
          for (k = 0; k < h->ref_n; k++) {
            int pn = h->ref_fn[k];
            if (pn > fn) pn -= h->max_frame_num;
            if (pn == pic) slot = k;
          }
          h->list_slot[li++] = slot;
        } else if (op == 2) {
          br_ue(&br);
          h->list_slot[li++] = 0;
        }
      }
      h->list_n = li;
    } else {
      int k;
      h->list_n = 0;
      for (k = h->ref_n - 1; k >= 0 && h->list_n < h->nref; k--) h->list_slot[h->list_n++] = k;
    }
    if (h->wpred) {
    h->w_den = br_ue(&br);
    h->wc_den = br_ue(&br);
    for (i = 0; i < h->nref; i++) {
      h->w_flag[i] = br_bit(&br);
      if (h->w_flag[i]) {
        h->w_luma[i] = br_se(&br);
        h->w_off[i] = br_se(&br);
      } else {
        h->w_luma[i] = 1 << h->w_den;
        h->w_off[i] = 0;
      }
      h->wc_flag[i] = br_bit(&br);
      if (h->wc_flag[i]) {
        h->wc_b[i] = br_se(&br);
        h->wc_bo[i] = br_se(&br);
        h->wc_r[i] = br_se(&br);
        h->wc_ro[i] = br_se(&br);
      }
    }
    }
  }
  if (h->nal_ref) {
    if (nal_is_idr) {
      br_bit(&br);
      br_bit(&br);
    } else if (br_bit(&br)) {
      for (;;) {
        int op = br_ue(&br);
        if (op == 0) break;
        if (op != 5) br_ue(&br);
      }
    }
  }
  if (st != 2) h->cabac_init = br_ue(&br);
  else h->cabac_init = 0;
  h->slice_qp = h->init_qp + br_se(&br);
  if (h->slice_qp < 0) h->slice_qp = 0;
  if (h->slice_qp > 51) h->slice_qp = 51;
  h->cur_qp = h->slice_qp;
  h->last_dqp = 0;
  h->disable_df = 0;
  h->aoff = h->boff = 0;
  if (br_more(&br)) {
    h->disable_df = br_ue(&br);
    if (h->disable_df != 1) {
      h->aoff = br_se(&br);
      h->boff = br_se(&br);
    }
  }
  br_bit(&br);
  while (br.i & 7) br_bit(&br);
  ctx_init(h, st == 2, h->slice_qp, h->cabac_init);
  cab_init(h, rb, n, br.i >> 3);
#ifdef H264_DEBUG
  printf("slice first %d st %d fn %d qp %d df %d aoff %d boff %d cab %d/%d nref %d\n", first, st, fn,
         h->slice_qp, h->disable_df, h->aoff, h->boff, h->cab_byte, n, h->nref);
#endif
  if (first < 0) first = 0;
  if (st == 1) {
    set_err(h, "Quadro B ainda nao entra");
    return -1;
  }
  {
    int mb = first;
    int total = h->mb_w * h->mb_h;
    if (mb > total) mb = 0;
    while (mb < total) {
      int mx = mb % h->mb_w;
      int my = mb / h->mb_w;
      int skip = 0;
      if (vid_stop) return -1;
      if (mx == 0) vid_h264_tick(my * 100 / (h->mb_h > 0 ? h->mb_h : 1));
      if (st != 2) {
        int ctx = 0;
        if (mx > 0 && h->mbt[mb - 1] != 5) ctx++;
        if (my > 0 && h->mbt[mb - h->mb_w] != 5) ctx++;
        skip = cab_dec(h, 11 + ctx);
      }
      h->mbs[mb] = 1;
      if (skip) {
        int px, py, ox, oy, slot;
        h->mbt[mb] = 5;
        h->mb8[mb] = 0;
        h->cbpb[mb] = 0;
        h->cmode[mb] = -1;
        h->qp[mb] = (int8_t)h->cur_qp;
        h->last_dqp = 0;
        pred_mv(h, mx * 4, my * 4, 4, 4, 0, &ox, &oy);
        put_mv(h, mx * 4, my * 4, 4, 4, ox, oy, 0, 0, 0);
        slot = h->list_n > 0 ? h->list_slot[0] : 0;
        if (slot < 0 || slot >= h->ref_n) slot = 0;
        if (h->ref_n > 0) {
          mc_luma(h, h->refy[slot], mx * 16, my * 16, 16, 16, ox, oy);
          mc_chroma(h, h->refu[slot], mx * 8, my * 8, 8, 8, ox, oy);
          mc_chroma(h, h->refv[slot], mx * 8, my * 8, 8, 8, ox, oy);
        }
        for (py = 0; py < 4; py++)
          for (px = 0; px < 4; px++) h->nnz[idx4(h, mx * 4 + px, my * 4 + py)] = 0;
      } else if (decode_mb(h, mb, mx, my, st) != 0) {
        return -1;
      }
      if (cab_term(h)) {
#ifdef H264_DEBUG
        static int eosn;
        if (mb == 0) eosn = 0;
        if (eosn < 3)
          printf("eos mb %d/%d cab %d/%d\n", mb, total, h->cab_byte, h->cab_n);
        eosn++;
        if (h->cab_byte + 64 >= h->cab_n || mb + 1 >= total)
          break;
#else
        break;
#endif
      }
      if (h->cab_byte > h->cab_n) break;
      mb++;
    }
#ifdef H264_DEBUG
    printf("fim mb %d cab %d/%d\n", mb, h->cab_byte, h->cab_n);
#endif
  }
  if (h->disable_df != 1) deblock_pic(h);
  return 0;
}

h264_t* h264_new(void) {
  h264_t* h = (h264_t*)calloc(1, sizeof(h264_t));
  if (h) {
    h->log2_fn = 4;
    h->max_frame_num = 16;
    h->max_ref = 1;
    h->nal_len = 4;
    h->chroma_off2 = 0;
  }
  return h;
}

void h264_free(h264_t* h) {
  int i;
  if (!h) return;
  free(h->y);
  free(h->u);
  free(h->v);
  for (i = 0; i < 8; i++) {
    free(h->refy[i]);
    free(h->refu[i]);
    free(h->refv[i]);
  }
  free(h->mbt);
  free(h->mb8);
  free(h->mbs);
  free(h->qp);
  free(h->mvx);
  free(h->mvy);
  free(h->mvdx);
  free(h->mvdy);
  free(h->refi);
  free(h->nnz);
  free(h->mode4);
  free(h->cbpb);
  free(h->cmode);
  free(h->cnnz);
  free(h->cdcz);
  free(h);
}

const char* h264_error(h264_t* h) { return h && h->err[0] ? h->err : ""; }

static int alloc_pic(h264_t* h) {
  int n = h->mb_w * h->mb_h;
  int ysz, csz;
  if (h->mb_w < 1 || h->mb_h < 1) return -1;
  if (h->width > H264_MAXW || h->height > H264_MAXH) {
    set_err(h, "Video grande demais");
    return -1;
  }
  h->bw4 = h->mb_w * 4;
  h->bh4 = h->mb_h * 4;
  h->stride_y = h->width;
  h->stride_c = h->width / 2;
  ysz = h->stride_y * h->height;
  csz = h->stride_c * (h->height / 2);
  free(h->y);
  free(h->u);
  free(h->v);
  h->y = (uint8_t*)calloc(1, (size_t)ysz);
  h->u = (uint8_t*)calloc(1, (size_t)csz);
  h->v = (uint8_t*)calloc(1, (size_t)csz);
  free(h->mbt);
  free(h->mb8);
  free(h->mbs);
  free(h->qp);
  free(h->mvx);
  free(h->mvy);
  free(h->mvdx);
  free(h->mvdy);
  free(h->refi);
  free(h->nnz);
  free(h->mode4);
  free(h->cbpb);
  free(h->cmode);
  free(h->cnnz);
  free(h->cdcz);
  h->mbt = (uint8_t*)calloc((size_t)n, 1);
  h->mb8 = (uint8_t*)calloc((size_t)n, 1);
  h->mbs = (uint8_t*)calloc((size_t)n, 1);
  h->qp = (int8_t*)calloc((size_t)n, 1);
  h->mvx = (int16_t*)calloc((size_t)h->bw4 * (size_t)h->bh4, 2);
  h->mvy = (int16_t*)calloc((size_t)h->bw4 * (size_t)h->bh4, 2);
  h->mvdx = (int16_t*)calloc((size_t)h->bw4 * (size_t)h->bh4, 2);
  h->mvdy = (int16_t*)calloc((size_t)h->bw4 * (size_t)h->bh4, 2);
  h->refi = (int8_t*)calloc((size_t)h->bw4 * (size_t)h->bh4, 1);
  h->nnz = (uint8_t*)calloc((size_t)h->bw4 * (size_t)h->bh4, 1);
  h->mode4 = (int8_t*)calloc((size_t)h->bw4 * (size_t)h->bh4, 1);
  h->cbpb = (uint8_t*)calloc((size_t)n, 1);
  h->cmode = (int8_t*)calloc((size_t)n, 1);
  h->cnnz = (uint8_t*)calloc((size_t)h->mb_w * 2 * (size_t)h->mb_h * 2 * 2, 1);
  h->cdcz = (uint8_t*)calloc((size_t)n * 2, 1);
  if (!h->y || !h->u || !h->v || !h->mbt || !h->mvx) {
    set_err(h, "Sem memoria para o quadro");
    return -1;
  }
  return 0;
}

static int parse_sps(h264_t* h, const uint8_t* rb, int n) {
  br_t b;
  int profile, i;
  b.p = rb;
  b.nbytes = n;
  b.i = 0;
  profile = br_bits(&b, 8);
  br_bits(&b, 8);
  br_bits(&b, 8);
  br_ue(&b);
  if (profile == 100 || profile == 110 || profile == 122 || profile == 144) {
    int chroma = br_ue(&b);
    if (chroma == 3) br_bit(&b);
    if (chroma != 1) {
      set_err(h, "So 4:2:0");
      return -1;
    }
    if (br_ue(&b) != 0 || br_ue(&b) != 0) {
      set_err(h, "So 8 bits");
      return -1;
    }
    br_bit(&b);
    if (br_bit(&b)) {
      set_err(h, "Matriz de quantizacao propria");
      return -1;
    }
  }
  h->log2_fn = br_ue(&b) + 4;
  if (h->log2_fn < 4) h->log2_fn = 4;
  if (h->log2_fn > 16) h->log2_fn = 16;
  h->max_frame_num = 1 << h->log2_fn;
  h->poc_type = br_ue(&b);
  if (h->poc_type == 0) br_ue(&b);
  else if (h->poc_type == 1) {
    int k, c;
    br_bit(&b);
    br_se(&b);
    br_se(&b);
    c = br_ue(&b);
    for (k = 0; k < c; k++) br_se(&b);
  }
  h->max_ref = br_ue(&b);
  if (h->max_ref < 1) h->max_ref = 1;
  if (h->max_ref > 8) h->max_ref = 8;
  br_bit(&b);
  h->mb_w = br_ue(&b) + 1;
  h->mb_h = br_ue(&b) + 1;
  if (!br_bit(&b)) {
    set_err(h, "Video entrelaçado");
    return -1;
  }
  br_bit(&b);
  h->crop_l = h->crop_r = h->crop_t = h->crop_b = 0;
  if (br_bit(&b)) {
    h->crop_l = br_ue(&b);
    h->crop_r = br_ue(&b);
    h->crop_t = br_ue(&b);
    h->crop_b = br_ue(&b);
  }
  h->width = h->mb_w * 16;
  h->height = h->mb_h * 16;
  h->disp_x = h->crop_l * 2;
  h->disp_y = h->crop_t * 2;
  h->disp_w = h->width - (h->crop_l + h->crop_r) * 2;
  h->disp_h = h->height - (h->crop_t + h->crop_b) * 2;
  if (h->disp_w < 2) h->disp_w = h->width;
  if (h->disp_h < 2) h->disp_h = h->height;
  h->has_sps = 1;
  if (alloc_pic(h) != 0) return -1;
  (void)i;
  return 0;
}

static int parse_pps(h264_t* h, const uint8_t* rb, int n) {
  br_t b;
  int groups;
  b.p = rb;
  b.nbytes = n;
  b.i = 0;
  br_ue(&b);
  br_ue(&b);
  h->cabac = br_bit(&b);
  br_bit(&b);
  groups = br_ue(&b) + 1;
  if (groups != 1) {
    set_err(h, "FMO nao entra");
    return -1;
  }
  h->ref_l0_def = br_ue(&b) + 1;
  br_ue(&b);
  h->wpred = br_bit(&b);
  br_bits(&b, 2);
  h->init_qp = br_se(&b) + 26;
  br_se(&b);
  h->chroma_off = br_se(&b);
  h->chroma_off2 = h->chroma_off;
  br_bit(&b);
  br_bit(&b);
  br_bit(&b);
  if ((b.nbytes * 8 - b.i) > 1) {
    h->t8x8 = br_bit(&b);
    if (br_bit(&b)) {
      set_err(h, "Matriz do PPS");
      return -1;
    }
    if ((b.nbytes * 8 - b.i) > 8) h->chroma_off2 = br_se(&b);
  }
  h->has_pps = 1;
  return 0;
}

int h264_avcc(h264_t* h, const uint8_t* p, int n) {
  int i, ns, ls, np, lp;
  if (!h || !p || n < 7) return -1;
  h->nal_len = (p[4] & 3) + 1;
  ns = p[5] & 31;
  i = 6;
  if (ns < 1 || i + 2 > n) return -1;
  ls = (p[i] << 8) | p[i + 1];
  i += 2;
  if (ls < 2 || i + ls > n) return -1;
  {
    uint8_t rb[256];
    int rn = rbsp_of(p + i, ls, rb, 256);
    if (parse_sps(h, rb, rn) != 0) return -1;
  }
  i += ls;
  if (i >= n) return -1;
  np = p[i++];
  if (np < 1 || i + 2 > n) return -1;
  lp = (p[i] << 8) | p[i + 1];
  i += 2;
  if (lp < 2 || i + lp > n) return -1;
  {
    uint8_t rb[128];
    int rn = rbsp_of(p + i, lp, rb, 128);
    if (parse_pps(h, rb, rn) != 0) return -1;
  }
  return h->cabac ? 0 : -1;
}

void h264_reset(h264_t* h) {
  int i;
  if (!h) return;
  for (i = 0; i < h->ref_n; i++) {
    free(h->refy[i]);
    free(h->refu[i]);
    free(h->refv[i]);
    h->refy[i] = 0;
    h->refu[i] = 0;
    h->refv[i] = 0;
  }
  h->ref_n = 0;
  h->age = 0;
}

static void store_ref(h264_t* h) {
  int ysz = h->stride_y * h->height;
  int csz = h->stride_c * (h->height / 2);
  int slot;
  uint8_t* y;
  uint8_t* u;
  uint8_t* v;
  if (!h->nal_ref) return;
  if (h->idr) h264_reset(h);
  if (h->ref_n >= h->max_ref) {
    int oldest = 0, k;
    for (k = 1; k < h->ref_n; k++)
      if (h->ref_age[k] < h->ref_age[oldest]) oldest = k;
    free(h->refy[oldest]);
    free(h->refu[oldest]);
    free(h->refv[oldest]);
    if (oldest != h->ref_n - 1) {
      h->refy[oldest] = h->refy[h->ref_n - 1];
      h->refu[oldest] = h->refu[h->ref_n - 1];
      h->refv[oldest] = h->refv[h->ref_n - 1];
      h->ref_fn[oldest] = h->ref_fn[h->ref_n - 1];
      h->ref_age[oldest] = h->ref_age[h->ref_n - 1];
    }
    h->ref_n--;
  }
  y = (uint8_t*)malloc((size_t)ysz);
  u = (uint8_t*)malloc((size_t)csz);
  v = (uint8_t*)malloc((size_t)csz);
  if (!y || !u || !v) {
    free(y);
    free(u);
    free(v);
    return;
  }
  memcpy(y, h->y, (size_t)ysz);
  memcpy(u, h->u, (size_t)csz);
  memcpy(v, h->v, (size_t)csz);
  slot = h->ref_n++;
  h->refy[slot] = y;
  h->refu[slot] = u;
  h->refv[slot] = v;
  h->ref_fn[slot] = h->frame_num;
  h->ref_age[slot] = h->age++;
}

int h264_au(h264_t* h, const uint8_t* data, int len) {
  int o = 0;
  int saw = 0;
  if (!h || !data || len < 5) return -1;
  if (!h->has_sps) {
    set_err(h, "MP4 sem SPS");
    return -1;
  }
  memset(h->mbs, 0, (size_t)h->mb_w * (size_t)h->mb_h);
  memset(h->mbt, 0, (size_t)h->mb_w * (size_t)h->mb_h);
  while (o + h->nal_len < len) {
    int nal_len = 0;
    int t;
    int k;
    const uint8_t* nal;
    for (k = 0; k < h->nal_len; k++) nal_len = (nal_len << 8) | data[o + k];
    o += h->nal_len;
    if (nal_len < 1 || o + nal_len > len) break;
    nal = data + o;
    o += nal_len;
    t = nal[0] & 31;
    h->nal_ref = (nal[0] >> 5) & 3;
    h->idr = (t == 5);
#ifdef H264_DEBUG
    printf("nal t %d ref %d len %d\n", t, h->nal_ref, nal_len);
#endif
    if (t == 7 || t == 8) {
      uint8_t rb[512];
      int rn = rbsp_of(nal, nal_len, rb, 512);
      if (t == 7) parse_sps(h, rb, rn);
      else parse_pps(h, rb, rn);
    } else if (t == 1 || t == 5) {
      uint8_t* rb = (uint8_t*)malloc((size_t)nal_len);
      int rn;
      if (!rb) return -1;
      rn = rbsp_of(nal, nal_len, rb, nal_len);
      if (decode_slice(h, rb, rn) != 0) {
        free(rb);
        return -1;
      }
      free(rb);
      saw = 1;
    }
  }
  if (!saw) {
    set_err(h, "Sem quadro");
    return -1;
  }
  store_ref(h);
  return 0;
}

int h264_rgba(h264_t* h, uint8_t** rgba, int* w, int* ht, int max_side) {
  int dw, dh, step, ow, oh, y, x;
  uint8_t* out;
  if (!h || !h->y || !rgba) return -1;
  dw = h->disp_w;
  dh = h->disp_h;
  if (dw < 1) dw = h->width;
  if (dh < 1) dh = h->height;
  step = 1;
  while ((dw + step - 1) / step > max_side || (dh + step - 1) / step > max_side) step++;
  ow = (dw + step - 1) / step;
  oh = (dh + step - 1) / step;
  if (ow < 1) ow = 1;
  if (oh < 1) oh = 1;
  out = (uint8_t*)malloc((size_t)ow * (size_t)oh * 4);
  if (!out) return -1;
  for (y = 0; y < oh; y++) {
    int sy = h->disp_y + y * step;
    if (sy >= h->height) sy = h->height - 1;
    for (x = 0; x < ow; x++) {
      int sx = h->disp_x + x * step;
      int Y, U, V, C, D, E, R, G, B;
      uint8_t* d;
      if (sx >= h->width) sx = h->width - 1;
      Y = h->y[sy * h->stride_y + sx];
      U = h->u[(sy / 2) * h->stride_c + (sx / 2)];
      V = h->v[(sy / 2) * h->stride_c + (sx / 2)];
      C = Y - 16;
      D = U - 128;
      E = V - 128;
      if (C < 0) C = 0;
      R = (298 * C + 409 * E + 128) >> 8;
      G = (298 * C - 100 * D - 208 * E + 128) >> 8;
      B = (298 * C + 516 * D + 128) >> 8;
      d = out + ((size_t)y * (size_t)ow + (size_t)x) * 4;
      d[0] = (uint8_t)clip8(R);
      d[1] = (uint8_t)clip8(G);
      d[2] = (uint8_t)clip8(B);
      d[3] = 255;
    }
  }
  *rgba = out;
  *w = ow;
  *ht = oh;
  return 0;
}

int h264_pic_w(h264_t* h) { return h ? h->disp_w : 0; }
int h264_pic_h(h264_t* h) { return h ? h->disp_h : 0; }
