/* keymap.c - Layouts US e ABNT2 (pt-BR) em CP850, a pagina do DOS em
 * portugues. ABNT2 segue /usr/share/X11/xkb/symbols/br (abnt2).
 * Os valores sao bytes CP850 (ç 0x87, ã 0xC6, õ 0xE4...); DEAD|n e tecla morta. */
#include "keymap.h"

#define DEAD 0x100
#define D_ACUTE 1
#define D_GRAVE 2
#define D_TILDE 3
#define D_CIRC  4
#define D_DIAER 5

static const uint16_t us_norm[128] = {
  0, 27, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,  /* 0x00 */
  0x37, 0x38, 0x39, 0x30, 0x2D, 0x3D, 8, 9,  /* 0x08 */
  0x71, 0x77, 0x65, 0x72, 0x74, 0x79, 0x75, 0x69,  /* 0x10 */
  0x6F, 0x70, 0x5B, 0x5D, 10, 0, 0x61, 0x73,  /* 0x18 */
  0x64, 0x66, 0x67, 0x68, 0x6A, 0x6B, 0x6C, 0x3B,  /* 0x20 */
  0x27, 0x60, 0, 0x5C, 0x7A, 0x78, 0x63, 0x76,  /* 0x28 */
  0x62, 0x6E, 0x6D, 0x2C, 0x2E, 0x2F, 0, 0x2A,  /* 0x30 */
  0, 0x20, 0, 0, 0, 0, 0, 0,  /* 0x38 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x40 */
  0, 0, 0x2D, 0, 0, 0, 0x2B, 0,  /* 0x48: - e + do teclado numerico */
  0, 0, 0, 0, 0, 0, 0x5C, 0,  /* 0x50 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x58 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x60 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x68 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x70 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x78 */
};
static const uint16_t us_shift[128] = {
  0, 27, 0x21, 0x40, 0x23, 0x24, 0x25, 0x5E,  /* 0x00 */
  0x26, 0x2A, 0x28, 0x29, 0x5F, 0x2B, 8, 9,  /* 0x08 */
  0x51, 0x57, 0x45, 0x52, 0x54, 0x59, 0x55, 0x49,  /* 0x10 */
  0x4F, 0x50, 0x7B, 0x7D, 10, 0, 0x41, 0x53,  /* 0x18 */
  0x44, 0x46, 0x47, 0x48, 0x4A, 0x4B, 0x4C, 0x3A,  /* 0x20 */
  0x22, 0x7E, 0, 0x7C, 0x5A, 0x58, 0x43, 0x56,  /* 0x28 */
  0x42, 0x4E, 0x4D, 0x3C, 0x3E, 0x3F, 0, 0x2A,  /* 0x30 */
  0, 0x20, 0, 0, 0, 0, 0, 0,  /* 0x38 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x40 */
  0, 0, 0x2D, 0, 0, 0, 0x2B, 0,  /* 0x48: - e + do teclado numerico */
  0, 0, 0, 0, 0, 0, 0x7C, 0,  /* 0x50 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x58 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x60 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x68 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x70 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x78 */
};
static const uint16_t br_norm[128] = {
  0, 27, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,  /* 0x00 */
  0x37, 0x38, 0x39, 0x30, 0x2D, 0x3D, 8, 9,  /* 0x08 */
  0x71, 0x77, 0x65, 0x72, 0x74, 0x79, 0x75, 0x69,  /* 0x10 */
  0x6F, 0x70, 0x101, 0x5B, 10, 0, 0x61, 0x73,  /* 0x18 */
  0x64, 0x66, 0x67, 0x68, 0x6A, 0x6B, 0x6C, 0x87,  /* 0x20 */
  0x103, 0x27, 0, 0x5D, 0x7A, 0x78, 0x63, 0x76,  /* 0x28 */
  0x62, 0x6E, 0x6D, 0x2C, 0x2E, 0x3B, 0, 0x2A,  /* 0x30 */
  0, 0x20, 0, 0, 0, 0, 0, 0,  /* 0x38 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x40 */
  0, 0, 0x2D, 0, 0, 0, 0x2B, 0,  /* 0x48: - e + do teclado numerico */
  0, 0, 0, 0, 0, 0, 0x5C, 0,  /* 0x50 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x58 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x60 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x68 */
  0, 0, 0, 0x2F, 0, 0, 0, 0,  /* 0x70 */
  0, 0, 0, 0, 0, 0, 0x2E, 0,  /* 0x78: 0x7E = . do teclado numerico ABNT2 */
};
static const uint16_t br_shift[128] = {
  0, 27, 0x21, 0x40, 0x23, 0x24, 0x25, 0x105,  /* 0x00 */
  0x26, 0x2A, 0x28, 0x29, 0x5F, 0x2B, 8, 9,  /* 0x08 */
  0x51, 0x57, 0x45, 0x52, 0x54, 0x59, 0x55, 0x49,  /* 0x10 */
  0x4F, 0x50, 0x102, 0x7B, 10, 0, 0x41, 0x53,  /* 0x18 */
  0x44, 0x46, 0x47, 0x48, 0x4A, 0x4B, 0x4C, 0x80,  /* 0x20 */
  0x104, 0x22, 0, 0x7D, 0x5A, 0x58, 0x43, 0x56,  /* 0x28 */
  0x42, 0x4E, 0x4D, 0x3C, 0x3E, 0x3A, 0, 0x2A,  /* 0x30 */
  0, 0x20, 0, 0, 0, 0, 0, 0,  /* 0x38 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x40 */
  0, 0, 0x2D, 0, 0, 0, 0x2B, 0,  /* 0x48: - e + do teclado numerico */
  0, 0, 0, 0, 0, 0, 0x7C, 0,  /* 0x50 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x58 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x60 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x68 */
  0, 0, 0, 0x3F, 0, 0, 0, 0,  /* 0x70 */
  0, 0, 0, 0, 0, 0, 0x2E, 0,  /* 0x78: 0x7E = . do teclado numerico ABNT2 */
};
static const uint16_t br_altgr[128] = {
  0, 0, 0xFB, 0xFD, 0xFC, 0x9C, 0xBD, 0xAA,  /* 0x00 */
  0, 0, 0, 0, 0, 0xF5, 0, 0,  /* 0x08 */
  0x2F, 0x3F, 0xF8, 0xA9, 0, 0, 0, 0,  /* 0x10 */
  0, 0, 0xEF, 0xA6, 0, 0, 0, 0,  /* 0x18 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x20 */
  0x7E, 0xAA, 0, 0xA7, 0, 0, 0xB8, 0,  /* 0x28 */
  0, 0, 0xE6, 0, 0, 0, 0, 0,  /* 0x30 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x38 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x40 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x48 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x50 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x58 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x60 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x68 */
  0, 0, 0, 0xF8, 0, 0, 0, 0,  /* 0x70 */
  0, 0, 0, 0, 0, 0, 0, 0,  /* 0x78 */
};
/* acento, letra, resultado (CP850) */
static const uint8_t dead_combo[][3] = {
  {1, 'a', 0xA0}, {1, 'e', 0x82}, {1, 'i', 0xA1}, {1, 'o', 0xA2}, {1, 'u', 0xA3}, {1, 'y', 0xEC}, {1, 'A', 0xB5}, {1, 'E', 0x90}, {1, 'I', 0xD6}, {1, 'O', 0xE0}, {1, 'U', 0xE9}, {1, 'Y', 0xED},
  {2, 'a', 0x85}, {2, 'e', 0x8A}, {2, 'i', 0x8D}, {2, 'o', 0x95}, {2, 'u', 0x97}, {2, 'A', 0xB7}, {2, 'E', 0xD4}, {2, 'I', 0xDE}, {2, 'O', 0xE3}, {2, 'U', 0xEB},
  {3, 'a', 0xC6}, {3, 'o', 0xE4}, {3, 'n', 0xA4}, {3, 'A', 0xC7}, {3, 'O', 0xE5}, {3, 'N', 0xA5},
  {4, 'a', 0x83}, {4, 'e', 0x88}, {4, 'i', 0x8C}, {4, 'o', 0x93}, {4, 'u', 0x96}, {4, 'A', 0xB6}, {4, 'E', 0xD2}, {4, 'I', 0xD7}, {4, 'O', 0xE2}, {4, 'U', 0xEA},
  {5, 'a', 0x84}, {5, 'e', 0x89}, {5, 'i', 0x8B}, {5, 'o', 0x94}, {5, 'u', 0x81}, {5, 'y', 0x98}, {5, 'A', 0x8E}, {5, 'E', 0xD3}, {5, 'I', 0xD8}, {5, 'O', 0x99}, {5, 'U', 0x9A},
};
/* o proprio acento (acento + espaco) */
static const uint8_t dead_alone[6] = {0, 0xEF, 0x60, 0x7E, 0x5E, 0xF9};

static int layout = KEYB_BR;
static uint8_t km_shift;   /* bit 1 esquerdo, bit 2 direito */
static int km_altgr;
static int km_caps;
static int km_num = 1;   /* NumLock: comeca ligado; o LED nao e sincronizado */
static int km_dead;
static int km_pending;

void keymap_set(int l) {
  layout = l == KEYB_US ? KEYB_US : KEYB_BR;
  km_dead = 0;
  km_pending = 0;
}

int keymap_get(void) { return layout; }

const char* keymap_name(int l) {
  return l == KEYB_US ? "us (EUA)" : "br (ABNT2)";
}

void keymap_watch(int ext, uint8_t key, int up) {
  uint8_t side;
  if (key == 0x2A || key == 0x36) {
    if (ext) return;   /* E0 2A: Shift falso das setas */
    side = key == 0x2A ? 1 : 2;
    if (up) km_shift &= (uint8_t)~side;
    else km_shift |= side;
    return;
  }
  if (key == 0x38 && ext) {
    km_altgr = !up;
    return;
  }
  if (key == 0x3A && !ext && !up)
    km_caps = !km_caps;
  if (key == 0x45 && !ext && !up)
    km_num = !km_num;
}

/* Teclado numerico sem E0, de 0x47 a 0x53. 0x4A e 0x4E (- e +) nao dependem
 * do NumLock e ficam nas tabelas. */
static const uint8_t kp_keys[13] = {
  '7', '8', '9', 0, '4', '5', '6', 0, '1', '2', '3', '0', '.'
};

static int is_kp(uint8_t sc) {
  return sc >= 0x47 && sc <= 0x53 && kp_keys[sc - 0x47];
}

int keymap_kp_nav(uint8_t sc) {
  return !km_num && is_kp(sc);
}

static int is_letter(uint16_t c) {
  return (c >= 'a' && c <= 'z') || c == 0x87;   /* 0x87 = ç */
}

/* Aplica a tecla morta pendente ao valor da tabela. */
static int km_resolve(uint16_t v) {
  unsigned i;
  if (!v) return 0;
  if (v & DEAD) {
    int d = v & 0xFF;
    if (km_dead == d) {   /* duas vezes: o acento sozinho */
      km_dead = 0;
      return dead_alone[d];
    }
    km_dead = d;
    return 0;
  }
  if (km_dead) {
    int d = km_dead;
    km_dead = 0;
    if (v == ' ') return dead_alone[d];
    for (i = 0; i < sizeof(dead_combo) / sizeof(dead_combo[0]); i++)
      if (dead_combo[i][0] == d && dead_combo[i][1] == v) return dead_combo[i][2];
    km_pending = v;
    return dead_alone[d];
  }
  return v;
}

int keymap_char(uint8_t sc) {
  const uint16_t* norm = layout == KEYB_US ? us_norm : br_norm;
  const uint16_t* shifted = layout == KEYB_US ? us_shift : br_shift;
  int shift = km_shift != 0;
  if (sc >= 128) return 0;
  if (km_num && is_kp(sc)) {
    /* No ABNT2 o separador decimal do teclado numerico e a virgula. */
    if (sc == 0x53 && layout == KEYB_BR) return km_resolve(',');
    return km_resolve(kp_keys[sc - 0x47]);
  }
  if (layout == KEYB_BR && km_altgr) return km_resolve(br_altgr[sc]);
  if (km_caps && is_letter(norm[sc])) shift = !shift;
  return km_resolve(shift ? shifted[sc] : norm[sc]);
}

int keymap_ext_char(uint8_t sc) {
  if (sc == 0x1C) return km_resolve(10);    /* E0 1C: Enter */
  if (sc == 0x35) return km_resolve('/');   /* E0 35: / */
  return 0;
}

int keymap_shift(void) { return km_shift != 0; }

int keymap_pending(void) {
  int p = km_pending;
  km_pending = 0;
  return p;
}
