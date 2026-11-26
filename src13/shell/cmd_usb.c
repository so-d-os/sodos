#include "shell.h"
#include "xhci.h"
#include "drives.h"
#include "mm.h"

/* Liga os controladores xHCI, procura o pendrive e lista o que achou.
 * "usb webcam ..." e o mesmo que "webcam ..." (vale depois de um TROCA, que
 * nao registra comandos novos). */
int cmd_usb(int argc, char** argv) {
  static char out[1536];
  if (argc >= 2 && argv[1][0] && (argv[1][0] | 32) == 'w' && (argv[1][1] | 32) == 'e' &&
      (argv[1][2] | 32) == 'b')
    return cmd_webcam(argc - 1, argv + 1);
  int L = drives_usb_letter();
  if (L) (void)xhci_disk_init();
  xhci_describe(out, (int)sizeof(out));
  puts_k(out);
  if (!L)
    puts_k("Pendrive desligado (SET PENDRIVE=0).\n");
  else if (xhci_disk_sectors()) {
    char m[32] = "Pendrive em X:  (dir X:)\n";
    m[12] = (char)L;
    m[21] = (char)L;
    puts_k(m);
  } else
    puts_k("Nenhum pendrive (porta do PC ou hub USB 2; FAT12/16/32).\n");
  (void)serial_log_save();   /* lista e log do USB -> D:\LOG\SERIAL.LOG */
  return 0;
}

/* ---------- webcam ---------- */

/* Tabelas de Huffman padrao do JPEG (anexo K). O MJPEG das webcams quase
 * sempre vem sem o DHT; sem ele nenhum visualizador abre o arquivo. */
static const uint8_t dht_bits_dc_l[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t dht_bits_dc_c[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t dht_bits_ac_l[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const uint8_t dht_bits_ac_c[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const uint8_t dht_val_ac_l[162] = {
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
  0xf9, 0xfa
};
static const uint8_t dht_val_ac_c[162] = {
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
  0xf9, 0xfa
};

/* Uma tabela (classe/id, 16 contagens, valores) em o; devolve o novo fim. */
static uint32_t dht_tab(uint8_t* o, uint32_t n, uint8_t tc, const uint8_t* bits, const uint8_t* val) {
  uint32_t i, cnt = 0;
  o[n++] = tc;
  for (i = 0; i < 16; i++) {
    o[n++] = bits[i];
    cnt += bits[i];
  }
  for (i = 0; i < cnt; i++) o[n++] = val ? val[i] : (uint8_t)i;
  return n;
}

/* 1 se o JPEG ja tem DHT antes do SOS. */
static int jpeg_has_dht(const uint8_t* p, uint32_t n) {
  uint32_t i = 2;
  while (i + 4 <= n) {
    uint8_t m;
    if (p[i] != 0xFF) return 0;
    m = p[i + 1];
    if (m == 0xC4) return 1;
    if (m == 0xDA || m == 0xD9) return 0;
    if (m == 0xFF || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
      i += (m == 0xFF) ? 1 : 2;
      continue;
    }
    i += 2 + ((uint32_t)p[i + 2] << 8 | p[i + 3]);
  }
  return 0;
}

static int arg_eq(const char* a, const char* b) {
  while (*a && *b) {
    char x = *a++, y = *b++;
    if (x >= 'a' && x <= 'z') x = (char)(x - 32);
    if (y >= 'a' && y <= 'z') y = (char)(y - 32);
    if (x != y) return 0;
  }
  return !*a && !*b;
}

static int arg_num(const char* s, int* v) {
  int n = 0, any = 0;
  while (*s >= '0' && *s <= '9') {
    n = n * 10 + (*s++ - '0');
    any = 1;
  }
  if (!any || *s) return 0;
  *v = n;
  return 1;
}

static void put_num(int v) {
  char t[12];
  int n = 0;
  char o[13];
  int k = 0;
  if (v < 0) v = 0;
  do {
    t[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v && n < 11);
  while (n) o[k++] = t[--n];
  o[k] = 0;
  puts_k(o);
}

static void wr16(uint8_t* b, uint32_t v) {
  b[0] = (uint8_t)v;
  b[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t* b, uint32_t v) {
  wr16(b, v);
  wr16(b + 2, v >> 16);
}

static uint8_t clamp8(int v) {
  return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

/* YUY2 -> BMP de 24 bits (de baixo para cima). */
static uint8_t* yuy2_bmp(const uint8_t* s, int w, int h, uint32_t* out_len) {
  uint32_t row = ((uint32_t)w * 3u + 3u) & ~3u;
  uint32_t size = 54u + row * (uint32_t)h;
  uint8_t* b = (uint8_t*)umalloc_owned(size, 0);
  int x, y;
  if (!b) return 0;
  for (x = 0; x < 54; x++) b[x] = 0;
  b[0] = 'B';
  b[1] = 'M';
  wr32(b + 2, size);
  wr32(b + 10, 54);
  wr32(b + 14, 40);
  wr32(b + 18, (uint32_t)w);
  wr32(b + 22, (uint32_t)h);
  wr16(b + 26, 1);
  wr16(b + 28, 24);
  wr32(b + 34, row * (uint32_t)h);
  for (y = 0; y < h; y++) {
    const uint8_t* p = s + (uint32_t)y * (uint32_t)w * 2u;
    uint8_t* o = b + 54u + (uint32_t)(h - 1 - y) * row;
    for (x = 0; x < w; x++) {
      int Y = p[x * 2] - 16;
      int U = p[(x & ~1) * 2 + 1] - 128;
      int V = p[(x & ~1) * 2 + 3] - 128;
      int c = 298 * Y;
      o[x * 3 + 0] = clamp8((c + 516 * U + 128) >> 8);
      o[x * 3 + 1] = clamp8((c - 100 * U - 208 * V + 128) >> 8);
      o[x * 3 + 2] = clamp8((c + 409 * V + 128) >> 8);
    }
    for (x = w * 3; (uint32_t)x < row; x++) o[x] = 0;
  }
  *out_len = size;
  return b;
}

static int ends_with_ci(const char* s, const char* suf) {
  int n = 0, m = 0;
  while (s[n]) n++;
  while (suf[m]) m++;
  return n >= m && arg_eq(s + n - m, suf);
}

/* webcam [arquivo] [/L] [/W:largura | /R:LxA] [/S:quadros] */
int cmd_webcam(int argc, char** argv) {
  static char wc_out[2048];
  char wc_path[24];
  static char wc_err[96];
  const char* file = 0;
  const uint8_t* data = 0;
  uint8_t* img = 0;
  uint32_t len = 0, ilen = 0;
  int i, want_w = 0, skip = 10, kind = 0, w = 0, h = 0, list = 0, rc;
  for (i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (!a || !a[0]) continue;
    if (arg_eq(a, "/?") || arg_eq(a, "-h")) {
      puts_k("WEBCAM [arquivo] [/L] [/W:largura | /R:LxA] [/S:quadros]\n"
             "  Tira uma foto da webcam USB (UVC) e grava em JPG (ou BMP, se a\n"
             "  camera so tiver YUY2). Sem arquivo: D:\\TEMP\\WEBCAM.JPG.\n"
             "  /L lista formatos e tamanhos.  /W largura maxima (padrao 640).\n"
             "  /R tamanho exato, ex.: /R:640x360.\n"
             "  /S quadros descartados antes da foto (padrao 10: a exposicao\n"
             "  automatica acerta neles).\n");
      return 0;
    }
    if (arg_eq(a, "/L")) list = 1;
    else if ((a[0] == '/' || a[0] == '-') && (a[1] == 'W' || a[1] == 'w') && a[2] == ':') {
      if (!arg_num(a + 3, &want_w)) {
        puts_k("webcam: /W:largura em pixels\n");
        return 1;
      }
    } else if ((a[0] == '/' || a[0] == '-') && (a[1] == 'R' || a[1] == 'r') && a[2] == ':') {
      /* /R:640x360 = tamanho exato */
      char t[12];
      int k = 0, rw = 0, rh = 0;
      const char* q = a + 3;
      while (*q && *q != 'x' && *q != 'X' && k < 11) t[k++] = *q++;
      t[k] = 0;
      if ((*q != 'x' && *q != 'X') || !arg_num(t, &rw) || !arg_num(q + 1, &rh) || rw < 1 ||
          rh < 1 || rw > 0xFFFF || rh > 0x7FFF) {
        puts_k("webcam: /R:larguraxaltura, ex.: /R:640x360\n");
        return 1;
      }
      want_w = rw | (rh << 16);
    } else if ((a[0] == '/' || a[0] == '-') && (a[1] == 'S' || a[1] == 's') && a[2] == ':') {
      if (!arg_num(a + 3, &skip) || skip > 300) {
        puts_k("webcam: /S:quadros (0 a 300)\n");
        return 1;
      }
    } else if (a[0] == '/' && a[1] && !a[2]) {
      puts_k("webcam: opcao desconhecida (webcam /? ajuda)\n");
      return 1;
    } else {
      file = a;
    }
  }
  if (list) {
    xhci_cam_describe(wc_out, (int)sizeof(wc_out));
    puts_k(wc_out);
    (void)serial_log_save();
    return 0;
  }
  rc = xhci_cam_grab(want_w, skip, &data, &len, &kind, &w, &h, wc_err, (int)sizeof(wc_err));
  if (rc != 0) {
    puts_k("webcam: ");
    puts_k(wc_err[0] ? wc_err : "falhou");
    puts_k("\n");
    (void)serial_log_save();
    return 1;
  }
  if (kind == 1) {
    /* SOI + DHT padrao (se faltar) + o resto */
    int add = !jpeg_has_dht(data, len);
    uint32_t n = 0, k;
    img = (uint8_t*)umalloc_owned(len + (add ? 432u : 0u), 0);
    if (!img) {
      puts_k("webcam: sem memoria\n");
      return 1;
    }
    img[n++] = 0xFF;
    img[n++] = 0xD8;
    if (add) {
      uint32_t at;
      img[n++] = 0xFF;
      img[n++] = 0xC4;
      at = n;
      n += 2;
      n = dht_tab(img, n, 0x00, dht_bits_dc_l, 0);
      n = dht_tab(img, n, 0x10, dht_bits_ac_l, dht_val_ac_l);
      n = dht_tab(img, n, 0x01, dht_bits_dc_c, 0);
      n = dht_tab(img, n, 0x11, dht_bits_ac_c, dht_val_ac_c);
      img[at] = (uint8_t)((n - at) >> 8);
      img[at + 1] = (uint8_t)(n - at);
    }
    for (k = 2; k < len; k++) img[n++] = data[k];
    ilen = n;
  } else {
    img = yuy2_bmp(data, w, h, &ilen);
    if (!img) {
      puts_k("webcam: sem memoria\n");
      return 1;
    }
  }
  if (!file) {
    const char* def = kind == 1 ? "D:\\TEMP\\WEBCAM.JPG" : "D:\\TEMP\\WEBCAM.BMP";
    for (i = 0; def[i]; i++) wc_path[i] = def[i];
    wc_path[i] = 0;
    (void)drives_mkdir("D:\\TEMP");
    file = wc_path;
  } else if (kind != 1 && ends_with_ci(file, ".JPG")) {
    puts_k("webcam: a camera mandou YUY2, o arquivo e BMP apesar do nome\n");
  }
  rc = file_save(file, (const char*)img, (int)ilen);
  ufree(img);
  if (rc != 0) {
    puts_k("webcam: nao consegui gravar ");
    puts_k(file);
    puts_k("\n");
    return 1;
  }
  puts_k(file);
  puts_k(": ");
  put_num(w);
  puts_k("x");
  put_num(h);
  puts_k(kind == 1 ? " JPEG, " : " BMP, ");
  put_num((int)ilen);
  puts_k(" bytes\n");
  (void)serial_log_save();
  return 0;
}
