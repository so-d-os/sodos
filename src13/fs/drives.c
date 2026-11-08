#include "drives.h"
#include "shell.h"
#include "mm.h"
#include "disk.h"
#include "net.h"
#include "xhci.h"

extern void fat_set_drive(uint32_t drive);
extern void fat_set_partition_offset(uint32_t offset);
extern int fat_mount(void);
extern int fat_is_mounted(void);
extern int fat_read_file(const char* filename, void* buffer, uint32_t max_size);
extern int fat_read_span(const char* filename, uint32_t offset, void* buffer, uint32_t len);
extern int fat_write_file(const char* filename, const void* data, uint32_t size);
extern int fat_list_files(char* output, int max_output);
extern int fat_list_dir(const char* path, char* output, int max_output);
extern int fat_is_dir(const char* path);
extern int fat_mkdir(const char* path);
extern int fat_rmdir(const char* path);
extern int fat_file_size(const char* filename);
extern int fat_dir_items(const char* path, struct dir_item* out, int max);
extern int fat_long_name(const char* path, char* out, int max);
extern int fat_delete(const char* path);
extern int fat_mkdir(const char* path);
extern int fat_rmdir(const char* path);
extern int fat_get_space_info(uint32_t* total_bytes, uint32_t* free_bytes);
extern int get_partition_info(uint32_t drive, uint8_t partition_num, uint32_t* lba_start,
                             uint32_t* sector_count, uint8_t* type);

#define DRV_NONE 0
#define DRV_RAM  1
#define DRV_FAT  2
#define D_RAM_MAX 8
#define D_RAM_CAP 1024

static int drv_kind[26];
static uint32_t drv_ata[26];
static char cur_letter = 'C';
/* Pasta atual de cada unidade, sem a letra e sem barra no inicio. */
static char cur_dir[26][DRV_PATH];
static uint32_t drv_lba[26];   /* inicio da particao FAT */
/* fat.c guarda um FAT por vez: a unidade dele agora (-1 = nenhuma valida). */
static int fat_cur = -1;
#define USB_LETTER 'U'
static uint32_t usb_gen;       /* xhci_disk_gen() do pendrive montado em U: */
static uint32_t usb_try_ms;    /* ultima procura sem achar (0 = nenhuma) */

static struct mem_file d_ram[D_RAM_MAX];
static int d_ram_count;

static int letter_idx(char L) {
  if (L >= 'a' && L <= 'z') L = (char)(L - 32);
  if (L < 'A' || L > 'Z') return -1;
  return L - 'A';
}

static char norm_letter(char L) {
  if (L >= 'a' && L <= 'z') L = (char)(L - 32);
  return L;
}

/* Carrega em fat.c o FAT da unidade idx, se for outro que o carregado. */
static int fat_use(int idx) {
  if (fat_cur == idx) return 0;
  fat_cur = -1;
  fat_set_drive(drv_ata[idx]);
  fat_set_partition_offset(drv_lba[idx]);
  if (fat_mount() != 0) return -1;
  fat_cur = idx;
  return 0;
}

/* Pendrive em U:: a primeira particao FAT12/16 do MBR ou, sem particao
 * nenhuma, o disco inteiro (alguns pendrives vem assim). */
static int usb_mount(void) {
  int idx = letter_idx(USB_LETTER);
  uint32_t lba = 0;
  uint8_t type = 0;
  uint8_t s0[512];
  int p, rc, found = 0, fat32 = 0;
  if (xhci_disk_init() != 0) return -1;
  if (drv_kind[idx] == DRV_FAT && usb_gen == xhci_disk_gen()) return 0;
  if (fat_cur == idx) fat_cur = -1;
  drv_kind[idx] = DRV_NONE;
  cur_dir[idx][0] = '\0';
  if (disk_read_sector(DISK_USB, 0, s0) != 0) return -1;
  if ((s0[54] == 'F' && s0[55] == 'A' && s0[56] == 'T') ||
      (s0[82] == 'F' && s0[83] == 'A' && s0[84] == 'T')) {
    found = 1;  /* boot sector FAT no setor 0, sem MBR */
  }
  /* Tabela do MBR lida aqui mesmo (o read_mbr do fdisk escreve log na tela). */
  for (p = 0; p < 4 && !found && s0[510] == 0x55 && s0[511] == 0xAA; p++) {
    const uint8_t* e = s0 + 446 + p * 16;
    type = e[4];
    lba = e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
    if (type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0E) found = 1;
    else if (type == 0x0B || type == 0x0C) fat32 = 1;
  }
  if (!found) {
    puts_k(fat32 ? "U: o pendrive esta em FAT32; o SODOS le FAT12/FAT16.\n"
                 : "U: o pendrive nao tem particao FAT.\n");
    return -2;
  }
  fat_cur = -1;
  fat_set_drive(DISK_USB);
  fat_set_partition_offset(lba);
  rc = fat_mount();
  if (rc != 0) {
    puts_k(rc == -2 ? "U: o pendrive esta em FAT32; o SODOS le FAT12/FAT16.\n"
                    : "U: o pendrive nao tem FAT12/FAT16.\n");
    return -2;
  }
  drv_kind[idx] = DRV_FAT;
  drv_ata[idx] = DISK_USB;
  drv_lba[idx] = lba;
  fat_cur = idx;
  usb_gen = xhci_disk_gen();
  puts_k("U: pendrive montado (");
  puts_k(xhci_disk_name());
  puts_k(").\n");
  return 0;
}

/* 1 se a unidade existe. U: liga o pendrive no primeiro uso e de novo
 * quando ele e trocado; sem pendrive, procura no maximo uma vez por segundo. */
static int drv_present(int idx) {
  if (idx < 0) return 0;
  if (idx == letter_idx(USB_LETTER) &&
      (drv_kind[idx] == DRV_NONE || !xhci_disk_sectors() || usb_gen != xhci_disk_gen())) {
    if (drv_kind[idx] != DRV_NONE && !xhci_disk_sectors()) {
      drv_kind[idx] = DRV_NONE;
      if (fat_cur == idx) fat_cur = -1;
    }
    if (!usb_try_ms || !net_has_clock() || net_ms() - usb_try_ms >= 1000u) {
      usb_try_ms = 0;
      if (usb_mount() != 0) usb_try_ms = net_ms() | 1u;
    }
  }
  return drv_kind[idx] != DRV_NONE;
}

/* 1 se a unidade pode ser lida agora (com o FAT dela carregado). */
static int drv_ready(int idx) {
  if (!drv_present(idx)) return 0;
  if (drv_kind[idx] == DRV_FAT && fat_use(idx) != 0) return 0;
  return 1;
}

int drives_use(char letter) {
  return drv_ready(letter_idx(letter)) ? 0 : -1;
}

int drives_mount_usb(void) {
  usb_try_ms = 0;
  return usb_mount();
}

void drives_init(void) {
  int i;
  for (i = 0; i < 26; i++) {
    drv_kind[i] = DRV_NONE;
    drv_ata[i] = 0;
    drv_lba[i] = 0;
  }
  fat_cur = -1;
  drv_kind['C' - 'A'] = DRV_RAM;
  cur_letter = 'C';
  d_ram_count = 0;
  for (i = 0; i < D_RAM_MAX; i++) {
    d_ram[i].name[0] = '\0';
    d_ram[i].size = 0;
    d_ram[i].capacity = D_RAM_CAP;
    d_ram[i].content = (char*)kmalloc(D_RAM_CAP);
    if (!d_ram[i].content) {
      d_ram[i].capacity = 0;
      continue;
    }
    d_ram[i].content[0] = '\0';
  }
}

char drives_current(void) {
  return cur_letter;
}

int drives_set_current(char letter) {
  char L = norm_letter(letter);
  int i = letter_idx(L);
  if (!drv_present(i)) return -1;
  cur_letter = L;
  return 0;
}

int drives_is_mounted(char letter) {
  return drv_present(letter_idx(letter));
}

const char* drives_kind_name(char letter) {
  int i = letter_idx(letter);
  if (i < 0 || drv_kind[i] == DRV_NONE) return "nao montado";
  if (drv_kind[i] == DRV_FAT) return "FAT";
  return "RAM";
}

int drives_mount_ram(char letter) {
  int i = letter_idx(letter);
  if (i < 0) return -1;
  if (letter == 'C' || letter == 'c') return 0;
  drv_kind[i] = DRV_RAM;
  drv_ata[i] = 0;
  return 0;
}

int drives_mount_fat(uint32_t ata_drive, char letter, uint32_t partition_num) {
  uint32_t lba_start = 0, sector_count = 0;
  uint8_t type = 0;
  int i = letter_idx(letter);
  int result;
  if (i < 0) return -1;
  fat_cur = -1;
  fat_set_drive(ata_drive);
  result = get_partition_info(ata_drive, (uint8_t)partition_num, &lba_start, &sector_count, &type);
  if (result != 0) {
    puts_k("Particao nao encontrada. Usando disco inteiro.\n");
    lba_start = 0;
  }
  fat_set_partition_offset(lba_start);
  result = fat_mount();
  if (result != 0) return result;
  drv_kind[i] = DRV_FAT;
  drv_ata[i] = ata_drive;
  drv_lba[i] = lba_start;
  fat_cur = i;
  return 0;
}

/* Disco 1 (disk.img) com particao FAT16 vira D: no boot. Sem particao, nada muda. */
void drives_automount(void) {
  uint32_t lba = 0, count = 0;
  uint8_t type = 0;
  int idx = letter_idx('D');
  if (drv_kind[idx] != DRV_NONE) return;
  if (get_partition_info(1, 0, &lba, &count, &type) != 0) return;
  if (type != 0x04 && type != 0x06 && type != 0x0E) return;
  fat_cur = -1;
  fat_set_drive(1);
  fat_set_partition_offset(lba);
  if (fat_mount() != 0) return;
  drv_kind[idx] = DRV_FAT;
  drv_ata[idx] = 1;
  drv_lba[idx] = lba;
  fat_cur = idx;
  puts_k("D: FAT16 montado (disco 1).\n");
}

void drives_print_status(void) {
  int i;
  puts_k("Unidades:\n");
  for (i = 0; i < 26; i++) {
    if (drv_kind[i] == DRV_NONE) continue;
    putc((char)('A' + i));
    puts_k(":  ");
    puts_k(drives_kind_name((char)('A' + i)));
    if (drv_kind[i] == DRV_FAT && drv_ata[i] == DISK_USB) {
      puts_k("  USB ");
      puts_k(xhci_disk_name());
    } else if (drv_kind[i] == DRV_FAT) {
      puts_k("  ATA ");
      {
        char n[12];
        uint32_to_string(drv_ata[i], n, sizeof(n));
        puts_k(n);
      }
    }
    puts_k("\n");
  }
}

/* Junta . e .. no proprio s. Nomes de qualquer tamanho, ate 32 niveis. */
static void path_norm(char* s, int max) {
  int start[32];
  int n = 0;
  int pos = 0;
  int i = 0;
  if (!s || max < 2) return;
  for (;;) {
    int b, m, k;
    while (s[pos] == '\\' || s[pos] == '/') pos++;
    if (!s[pos]) break;
    b = pos;
    while (s[pos] && s[pos] != '\\' && s[pos] != '/') pos++;
    m = pos - b;
    if (m == 1 && s[b] == '.') continue;
    if (m == 2 && s[b] == '.' && s[b + 1] == '.') {
      if (n > 0) i = start[--n];
      if (i > 0) i--;   /* a barra antes do nome tirado */
      continue;
    }
    if (n >= 32) break;
    if (i > 0) s[i++] = '\\';
    start[n++] = i;
    /* i <= b: escreve no mesmo lugar ou antes do que ainda vai ler */
    for (k = 0; k < m; k++) s[i++] = s[b + k];
  }
  s[i] = 0;
}

void path_parse(const char* path, char* letter_out, char* name_out, int name_max) {
  const char* p = path ? path : "";
  char L = cur_letter;
  int absolute = 0;
  int i = 0;
  int li;
  if (name_max < 1) {
    *letter_out = L;
    return;
  }
  name_out[0] = 0;
  if (p[0] && p[1] == ':') {
    L = norm_letter(p[0]);
    p += 2;
  }
  if (*p == '\\' || *p == '/') {
    absolute = 1;
    while (*p == '\\' || *p == '/') p++;
  }
  *letter_out = L;
  li = letter_idx(L);
  if (!absolute && li >= 0 && cur_dir[li][0]) {
    const char* b = cur_dir[li];
    while (*b && i < name_max - 1) name_out[i++] = *b++;
  }
  if (*p && i > 0 && name_out[i - 1] != '\\' && i < name_max - 1) name_out[i++] = '\\';
  while (*p && i < name_max - 1) {
    if (*p == '\\' || *p == '/') {
      if (i > 0 && name_out[i - 1] != '\\') name_out[i++] = '\\';
      p++;
      continue;
    }
    name_out[i++] = *p++;
  }
  while (i > 0 && name_out[i - 1] == '\\') i--;
  name_out[i] = 0;
  path_norm(name_out, name_max);
}

char path_letter(const char* path) {
  char L;
  char name[DRV_PATH];
  path_parse(path, &L, name, sizeof(name));
  return L;
}

static struct mem_file* d_find(const char* name) {
  int i;
  for (i = 0; i < d_ram_count; i++) {
    if (name_eq(d_ram[i].name, name)) return &d_ram[i];
  }
  return 0;
}

static struct mem_file* d_create(const char* name) {
  struct mem_file* f;
  int i = 0;
  if (d_ram_count >= D_RAM_MAX) return 0;
  f = &d_ram[d_ram_count];
  if (!f->content) return 0;
  d_ram_count++;
  while (name[i] && i < 31) {
    f->name[i] = name[i];
    i++;
  }
  f->name[i] = '\0';
  f->size = 0;
  f->content[0] = '\0';
  return f;
}

static int d_write(const char* name, const char* data, int size) {
  struct mem_file* f = d_find(name);
  int n = size;
  if (!f) {
    f = d_create(name);
    if (!f) return -1;
  }
  if (n < 0) n = 0;
  if (n > 32768) return -1;
  if (n > f->capacity - 1) {
    char* buf = (char*)kmalloc((uint32_t)n + 1u);
    if (!buf) return -1;
    kfree(f->content);
    f->content = buf;
    f->capacity = n + 1;
  }
  for (int i = 0; i < n; i++) f->content[i] = data[i];
  f->content[n] = '\0';
  f->size = n;
  return 0;
}

int file_read_at(const char* path, uint32_t offset, char* buf, int len) {
  char L;
  char name[DRV_PATH];
  int idx;
  if (!path || !buf || len < 1) return -1;
  path_parse(path, &L, name, sizeof(name));
  if (name[0] == '\0') return -1;
  idx = letter_idx(L);
  if (!drv_ready(idx)) return -2;
  if (drv_kind[idx] == DRV_FAT) return fat_read_span(name, offset, buf, (uint32_t)len);
  if (L == 'D') {
    struct mem_file* f = d_find(name);
    int n;
    int i;
    if (!f) return -1;
    if (offset >= (uint32_t)f->size) return 0;
    n = f->size - (int)offset;
    if (n > len) n = len;
    for (i = 0; i < n; i++) buf[i] = f->content[offset + (uint32_t)i];
    return n;
  }
  {
    /* C: arquivos na RAM e embutidos no kernel */
    struct mem_file* f = find_mem_file(name);
    const struct file_entry* fe = f ? 0 : find_file(name);
    const char* data;
    int size;
    int n;
    int i;
    if (f) {
      data = f->content;
      size = f->size;
    } else if (fe) {
      data = fe->content;
      size = (int)fe->size;
    } else {
      return -1;
    }
    if (offset >= (uint32_t)size) return 0;
    n = size - (int)offset;
    if (n > len) n = len;
    for (i = 0; i < n; i++) buf[i] = data[offset + (uint32_t)i];
    return n;
  }
}

int file_load(const char* path, char* buf, int max_size) {
  char L;
  char name[DRV_PATH];
  int idx;
  int n;
  if (!path || !buf || max_size < 2) return -1;
  path_parse(path, &L, name, sizeof(name));
  if (name[0] == '\0') return -1;
  idx = letter_idx(L);
  if (!drv_ready(idx)) return -2;
  buf[0] = '\0';
  if (drv_kind[idx] == DRV_FAT) {
    n = fat_read_file(name, buf, (uint32_t)(max_size - 1));
    if (n < 0) return -1;
    if (n >= max_size) n = max_size - 1;
    buf[n] = '\0';
    return n;
  }
  if (L == 'D') {
    struct mem_file* f = d_find(name);
    if (!f) return -1;
    n = f->size;
    if (n > max_size - 1) n = max_size - 1;
    for (int i = 0; i < n; i++) buf[i] = f->content[i];
    buf[n] = '\0';
    return n;
  }
  {
    struct mem_file* f = find_mem_file(name);
    if (f) {
      n = f->size;
      if (n > max_size - 1) n = max_size - 1;
      for (int i = 0; i < n; i++) buf[i] = f->content[i];
      buf[n] = '\0';
      return n;
    }
  }
  {
    const struct file_entry* fe = find_file(name);
    uint32_t i;
    if (!fe) return -1;
    n = (int)fe->size;
    if (n > max_size - 1) n = max_size - 1;
    for (i = 0; i < (uint32_t)n; i++) buf[i] = fe->content[i];
    buf[n] = '\0';
    return n;
  }
}

int file_size(const char* path) {
  char L;
  char name[DRV_PATH];
  int idx;
  if (!path) return -1;
  path_parse(path, &L, name, sizeof(name));
  if (name[0] == '\0') return -1;
  idx = letter_idx(L);
  if (!drv_ready(idx)) return -2;
  if (drv_kind[idx] == DRV_FAT) return fat_file_size(name);
  if (L == 'D') {
    struct mem_file* f = d_find(name);
    return f ? f->size : -1;
  }
  {
    struct mem_file* f = find_mem_file(name);
    const struct file_entry* fe;
    if (f) return f->size;
    fe = find_file(name);
    return fe ? (int)fe->size : -1;
  }
}

int file_save(const char* path, const char* data, int size) {
  char L;
  char name[DRV_PATH];
  int idx;
  if (!path || !data) return -1;
  if (size < 0) size = 0;
  path_parse(path, &L, name, sizeof(name));
  if (name[0] == '\0') return -1;
  idx = letter_idx(L);
  if (!drv_ready(idx)) return -2;
  if (drv_kind[idx] == DRV_FAT)
    return fat_write_file(name, data, (uint32_t)size);
  if (L == 'D')
    return d_write(name, data, size);
  return write_mem_bytes(name, data, size);
}

static int push_name(char out[][32], int n, int max, const char* name) {
  int i;
  int k = 0;
  if (!name || !name[0] || n >= max) return n;
  for (i = 0; i < n; i++) {
    if (name_eq(out[i], name)) return n;
  }
  while (name[k] && k < 31) {
    out[n][k] = name[k];
    k++;
  }
  out[n][k] = '\0';
  return n + 1;
}

static void copy_str(char* dst, int max, const char* src) {
  int i = 0;
  if (!dst || max < 1) return;
  if (!src) src = "";
  while (src[i] && i < max - 1) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = 0;
}

void drives_pwd(char* out, int max) {
  int i = 0;
  int li = letter_idx(cur_letter);
  const char* d = (li >= 0) ? cur_dir[li] : "";
  if (!out || max < 4) return;
  out[i++] = cur_letter;
  out[i++] = ':';
  out[i++] = '\\';
  while (*d && i < max - 1) out[i++] = *d++;
  out[i] = 0;
}

void drives_prompt(char* out, int max) {
  int n = 0;
  drives_pwd(out, max > 3 ? max - 2 : max);
  while (out[n]) n++;
  if (n < max - 1) out[n++] = '>';
  if (n < max - 1) out[n++] = ' ';
  out[n] = 0;
}

int drives_chdir(const char* spec) {
  char L;
  char name[DRV_PATH];
  int idx;
  if (!spec || !spec[0]) {
    char pwd[DRV_PATH + 4];
    drives_pwd(pwd, (int)sizeof(pwd));
    puts_k(pwd);
    puts_k("\n");
    return 0;
  }
  path_parse(spec, &L, name, (int)sizeof(name));
  idx = letter_idx(L);
  if (!drv_ready(idx)) {
    puts_k("unidade nao montada\n");
    return -1;
  }
  if (name[0]) {
    if (drv_kind[idx] != DRV_FAT) {
      puts_k("esta unidade nao tem pastas\n");
      return -1;
    }
    if (!fat_is_dir(name)) {
      puts_k("pasta nao encontrada\n");
      return -1;
    }
  }
  cur_letter = L;
  copy_str(cur_dir[idx], (int)sizeof(cur_dir[idx]), name);
  return 0;
}

int file_list(const char* path, char* out, int max) {
  char L;
  char name[DRV_PATH];
  int idx;
  int pos = 0;
  if (!out || max < 2) return -1;
  out[0] = 0;
  path_parse(path ? path : "", &L, name, (int)sizeof(name));
  idx = letter_idx(L);
  if (!drv_ready(idx)) return -1;
  if (drv_kind[idx] == DRV_FAT) return fat_list_dir(name, out, max);
  /* RAM / embutidos: so a raiz da unidade. */
  if (name[0]) return -1;
  {
    char names[64][32];
    int n = drives_names(L, names, 64);
    int i;
    for (i = 0; i < n && pos < max - 2; i++) {
      int k = 0;
      while (names[i][k] && pos < max - 2) out[pos++] = names[i][k++];
      if (pos < max - 1) out[pos++] = '\n';
    }
  }
  out[pos] = 0;
  return pos > 0 ? 1 : 0;
}

static void print_where(char L, const char* name) {
  putc(L);
  puts_k(":\\");
  if (name && name[0]) puts_k(name);
  puts_k("\n");
}

#define DIR_ITEMS_MAX 1024
#define DIR_DEPTH_MAX 8
#define DIR_WIDE_COL  15
#define DIR_WIDE_N    5

static char up_char(char c) {
  return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static int has_tilde(const char* s) {
  for (; s && *s; s++)
    if (*s == '~') return 1;
  return 0;
}

static int has_wild(const char* s) {
  for (; s && *s; s++)
    if (*s == '*' || *s == '?') return 1;
  return 0;
}

/* Glob de um pedaco (nome ou extensao). ? no fim tambem casa com nada,
 * como no DOS: ???.txt pega AB.TXT. */
static int glob_part(const char* p, int pn, const char* s, int sn) {
  if (pn == 0) return sn == 0;
  if (*p == '*') {
    int k;
    for (k = 0; k <= sn; k++)
      if (glob_part(p + 1, pn - 1, s + k, sn - k)) return 1;
    return 0;
  }
  if (sn == 0) return *p == '?' && glob_part(p + 1, pn - 1, s, 0);
  if (*p != '?' && up_char(*p) != up_char(*s)) return 0;
  return glob_part(p + 1, pn - 1, s + 1, sn - 1);
}

static int last_dot(const char* s) {
  int i, d = -1;
  for (i = 0; s[i]; i++)
    if (s[i] == '.') d = i;
  return d;
}

static int str_len(const char* s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

/* Com ponto no padrao casa nome e extensao em separado (*.* pega LEIAME). */
static int dir_match(const char* pat, const char* name) {
  int pd, nd, pl, nl;
  if (!pat || !pat[0]) return 1;
  pl = str_len(pat);
  nl = str_len(name);
  pd = last_dot(pat);
  if (pd < 0) return glob_part(pat, pl, name, nl);
  nd = last_dot(name);
  if (nd < 0) nd = nl;
  if (!glob_part(pat, pd, name, nd)) return 0;
  if (nd == nl) return glob_part(pat + pd + 1, pl - pd - 1, "", 0);
  return glob_part(pat + pd + 1, pl - pd - 1, name + nd + 1, nl - nd - 1);
}

/* Numero com ponto de milhar, alinhado a direita em width. */
static void put_num(uint32_t v, int width) {
  char t[16];
  int n = 0, d = 0;
  do {
    if (d && d % 3 == 0) t[n++] = '.';
    t[n++] = (char)('0' + v % 10u);
    v /= 10u;
    d++;
  } while (v);
  while (width-- > n) putc(' ');
  while (n) putc(t[--n]);
}

static void put_2(unsigned v) {
  putc((char)('0' + (v / 10u) % 10u));
  putc((char)('0' + v % 10u));
}

static void put_pad(const char* s, int width) {
  int n = 0;
  while (s[n] && n < width) putc(s[n++]);
  while (n++ < width) putc(' ');
}

/* Le a pasta name da unidade idx (com o FAT carregado). -1 se nao e pasta. */
static int dir_collect(int idx, char L, const char* name, struct dir_item* it, int max) {
  char names[16][32];
  char path[40];
  int n, i;
  if (drv_kind[idx] == DRV_FAT) return fat_dir_items(name, it, max);
  if (name[0]) return -1;
  n = drives_names(L, names, max < 16 ? max : 16);
  for (i = 0; i < n; i++) {
    int sz;
    path[0] = L;
    path[1] = ':';
    copy_str(path + 2, (int)sizeof(path) - 2, names[i]);
    sz = file_size(path);
    copy_str(it[i].name, (int)sizeof(it[i].name), names[i]);
    it[i].size = sz > 0 ? (uint32_t)sz : 0u;
    it[i].date = 0;
    it[i].time = 0;
    it[i].dir = 0;
  }
  return n;
}

/* Data, hora, tamanho e o nome no fim (nome longo de qualquer tamanho).
 * Na FAT o apelido 8.3 (NOME~1.EXT) vira o nome longo inteiro. */
static void dir_line(const struct dir_item* e, int idx, const char* folder) {
  char full[DRV_PATH];
  char lname[FAT_NAME_MAX];
  const char* show = e->name;
  /* Sem dia ou mes e data que o sistema nao gravou: fica em branco. */
  if ((e->date & 31u) && ((e->date >> 5) & 15u) && ((e->date >> 5) & 15u) <= 12u) {
    unsigned d = e->date, t = e->time;
    unsigned y = 1980u + (d >> 9);
    put_2(d & 31u);
    putc('/');
    put_2((d >> 5) & 15u);
    putc('/');
    put_2(y / 100u);
    put_2(y % 100u);
    putc(' ');
    put_2(t >> 11);
    putc(':');
    put_2((t >> 5) & 63u);
  } else {
    put_pad("", 16);
  }
  if (e->dir) puts_k("    <DIR>         ");
  else {
    put_num(e->size, 17);
    putc(' ');
  }
  if (drv_kind[idx] == DRV_FAT && has_tilde(e->name)) {
    int k = 0;
    if (folder[0]) {
      copy_str(full, (int)sizeof(full) - 33, folder);
      k = str_len(full);
      full[k++] = '\\';
    }
    copy_str(full + k, (int)sizeof(full) - k, e->name);
    if (fat_long_name(full, lname, (int)sizeof(lname)) == 0) show = lname;
  }
  puts_k(show);
  putc('\n');
}

static void dir_wide_cell(const struct dir_item* e, int last) {
  char cell[34];
  int k = 0, j = 0;
  if (e->dir) cell[k++] = '[';
  while (e->name[j] && k < DIR_WIDE_COL - 2) cell[k++] = e->name[j++];
  if (e->dir) cell[k++] = ']';
  cell[k] = 0;
  if (last) puts_k(cell);
  else put_pad(cell, DIR_WIDE_COL);
}

static void dir_wide(struct dir_item* it, const int* sel, int n) {
  int rows = (n + DIR_WIDE_N - 1) / DIR_WIDE_N;
  int r, c;
  for (r = 0; r < rows; r++) {
    for (c = 0; c < DIR_WIDE_N; c++) {
      int k = r * DIR_WIDE_N + c;
      if (k >= n) break;
      dir_wide_cell(&it[sel[k]], c == DIR_WIDE_N - 1 || k + 1 >= n);
    }
    putc('\n');
  }
}

struct dir_sum {
  uint32_t files;
  uint32_t dirs;
  uint32_t bytes;
  int shown;
};

/* Imprime uma pasta e, com DIR_SUB, as de dentro. */
static int dir_walk(int idx, char L, const char* name, const char* pat, int flags,
                    int depth, struct dir_sum* tot) {
  struct dir_item* it;
  int* sel;
  int n, i, cnt = 0;
  uint32_t files = 0, dirs = 0, bytes = 0;
  it = (struct dir_item*)kmalloc(DIR_ITEMS_MAX * (uint32_t)sizeof(struct dir_item));
  sel = (int*)kmalloc(DIR_ITEMS_MAX * (uint32_t)sizeof(int));
  if (!it || !sel) {
    if (it) kfree(it);
    if (sel) kfree(sel);
    puts_k("sem memoria\n");
    return -1;
  }
  n = dir_collect(idx, L, name, it, DIR_ITEMS_MAX);
  if (n < 0) {
    kfree(it);
    kfree(sel);
    return -1;
  }
  for (i = 0; i < n; i++) {
    if (!dir_match(pat, it[i].name)) continue;
    if ((flags & DIR_DIRS) && !it[i].dir) continue;
    sel[cnt++] = i;
    if (it[i].dir) dirs++;
    else {
      files++;
      bytes += it[i].size;
    }
  }
  /* Com /s so mostra as pastas que tem algo que casa. */
  if (cnt > 0 || !(flags & DIR_SUB)) {
    if (tot->shown) putc('\n');
    tot->shown = 1;
    puts_k(" Diretorio de ");
    print_where(L, name);
    putc('\n');
    if (cnt == 0) puts_k((flags & DIR_DIRS) ? "(nenhuma pasta)\n" : "(nenhum arquivo)\n");
    else if (flags & DIR_WIDE) dir_wide(it, sel, cnt);
    else
      for (i = 0; i < cnt; i++) dir_line(&it[sel[i]], idx, name);
    put_num(files, 10);
    puts_k(" arquivo(s) ");
    put_num(bytes, 15);
    puts_k(" bytes\n");
    put_num(dirs, 10);
    puts_k(" pasta(s)\n");
  }
  tot->files += files;
  tot->dirs += dirs;
  tot->bytes += bytes;
  if ((flags & DIR_SUB) && depth < DIR_DEPTH_MAX) {
    for (i = 0; i < n; i++) {
      char sub[DRV_PATH];
      int k = 0, j = 0;
      if (!it[i].dir) continue;
      while (name[j] && k < (int)sizeof(sub) - 2) sub[k++] = name[j++];
      if (k) sub[k++] = '\\';
      for (j = 0; it[i].name[j] && k < (int)sizeof(sub) - 1; j++) sub[k++] = it[i].name[j];
      sub[k] = 0;
      dir_walk(idx, L, sub, pat, flags, depth + 1, tot);
    }
  }
  kfree(it);
  kfree(sel);
  return 0;
}

void drives_dir(const char* spec, int flags) {
  char dspec[DRV_PATH];
  char pat[32];
  char L;
  char name[DRV_PATH];
  int idx;
  int cut = -1;
  int i;
  struct dir_sum tot;
  if (!spec) spec = "";
  pat[0] = 0;
  /* Ultimo nome com * ou ? vira o padrao; o resto e a pasta. */
  for (i = 0; spec[i]; i++)
    if (spec[i] == '\\' || spec[i] == '/' || spec[i] == ':') cut = i;
  if (has_wild(spec + cut + 1)) {
    copy_str(pat, (int)sizeof(pat), spec + cut + 1);
    for (i = 0; i <= cut && i < (int)sizeof(dspec) - 1; i++) dspec[i] = spec[i];
    dspec[i] = 0;
  } else {
    copy_str(dspec, (int)sizeof(dspec), spec);
  }
  path_parse(dspec, &L, name, (int)sizeof(name));
  idx = letter_idx(L);
  if (!drv_ready(idx)) {
    putc(L ? L : '?');
    puts_k(": nao montado\n");
    return;
  }
  /* dir arquivo.txt: mostra so ele, a partir da pasta de cima. */
  if (!pat[0] && name[0] && drv_kind[idx] == DRV_FAT && !fat_is_dir(name)) {
    int s = -1;
    for (i = 0; name[i]; i++)
      if (name[i] == '\\') s = i;
    copy_str(pat, (int)sizeof(pat), name + s + 1);
    name[s < 0 ? 0 : s] = 0;
  }
  if (!pat[0] && name[0] && drv_kind[idx] != DRV_FAT) {
    copy_str(pat, (int)sizeof(pat), name);
    name[0] = 0;
  }
  tot.files = tot.dirs = tot.bytes = 0;
  tot.shown = 0;
  if (dir_walk(idx, L, name, pat, flags, 0, &tot) != 0) {
    puts_k("pasta nao encontrada\n");
    return;
  }
  if ((flags & DIR_SUB) && !tot.shown)
    puts_k((flags & DIR_DIRS) ? "pasta nao encontrada\n" : "arquivo nao encontrado\n");
  if (flags & DIR_SUB) {
    puts_k("\n Total:\n");
    put_num(tot.files, 10);
    puts_k(" arquivo(s) ");
    put_num(tot.bytes, 15);
    puts_k(" bytes\n");
    put_num(tot.dirs, 10);
    puts_k(" pasta(s)\n");
  }
  if (drv_kind[idx] == DRV_FAT) {
    uint32_t total, livre;
    if (fat_get_space_info(&total, &livre) == 0) {
      put_num(livre, 37);
      puts_k(" bytes livres\n");
    }
  }
}

/* Apaga um arquivo de D: na RAM: o ultimo vem para o lugar dele. */
static int d_delete(const char* name) {
  struct mem_file* f = d_find(name);
  struct mem_file t;
  if (!f) return -1;
  d_ram_count--;
  t = *f;
  *f = d_ram[d_ram_count];
  d_ram[d_ram_count] = t;
  d_ram[d_ram_count].name[0] = '\0';
  d_ram[d_ram_count].size = 0;
  return 0;
}

/* Apaga um arquivo (0) ou, com * e ? no nome, todos que casam (quantos).
 * -1 nao achou, -2 pasta/unidade invalida, -3 e pasta, -4 somente leitura,
 * -5 a unidade nao deixa apagar. */
int drives_del(const char* spec) {
  char dspec[DRV_PATH];
  char pat[32];
  char L;
  char name[DRV_PATH];
  char full[DRV_PATH];
  struct dir_item* it;
  int idx, n, i, k, cut = -1, done = 0, rc;
  if (!spec || !spec[0]) return -2;
  for (i = 0; spec[i]; i++)
    if (spec[i] == '\\' || spec[i] == '/' || spec[i] == ':') cut = i;
  pat[0] = 0;
  if (has_wild(spec + cut + 1)) {
    copy_str(pat, (int)sizeof(pat), spec + cut + 1);
    for (i = 0; i <= cut && i < (int)sizeof(dspec) - 1; i++) dspec[i] = spec[i];
    dspec[i] = 0;
  } else {
    copy_str(dspec, (int)sizeof(dspec), spec);
  }
  path_parse(dspec, &L, name, (int)sizeof(name));
  idx = letter_idx(L);
  if (idx < 0 || !drv_ready(idx)) return -2;
  if (drv_kind[idx] != DRV_FAT && !(L == 'D' && drv_kind[idx] == DRV_RAM)) return -5;
  if (!pat[0]) {
    if (drv_kind[idx] == DRV_FAT) return fat_delete(name);
    return d_delete(name);
  }
  it = (struct dir_item*)kmalloc(DIR_ITEMS_MAX * (uint32_t)sizeof(struct dir_item));
  if (!it) return -2;
  n = dir_collect(idx, L, name, it, DIR_ITEMS_MAX);
  if (n < 0) {
    kfree(it);
    return -2;
  }
  for (i = 0; i < n; i++) {
    if (it[i].dir || !dir_match(pat, it[i].name)) continue;
    k = 0;
    if (name[0]) {
      copy_str(full, (int)sizeof(full) - 33, name);
      k = str_len(full);
      full[k++] = '\\';
    }
    copy_str(full + k, (int)sizeof(full) - k, it[i].name);
    if (drv_kind[idx] == DRV_FAT) {
      if (drv_ready(idx)) rc = fat_delete(full);
      else rc = -2;
    } else {
      rc = d_delete(full);
    }
    if (rc == 0) done++;
  }
  kfree(it);
  return done > 0 ? done : -1;
}

/* Itens da pasta path (com tamanho e data). -1 se nao e pasta. */
int drives_items(const char* path, struct dir_item* out, int max) {
  char L;
  char name[DRV_PATH];
  int idx;
  path_parse(path, &L, name, (int)sizeof(name));
  idx = letter_idx(L);
  if (idx < 0 || !drv_ready(idx)) return -1;
  return dir_collect(idx, L, name, out, max);
}

int drives_long_name(const char* path, char* out, int max) {
  char L;
  char name[DRV_PATH];
  int idx;
  if (!out || max < 2) return -1;
  path_parse(path, &L, name, (int)sizeof(name));
  idx = letter_idx(L);
  if (!name[0] || idx < 0 || !drv_ready(idx)) return -1;
  if (drv_kind[idx] == DRV_FAT) return fat_long_name(name, out, max);
  {
    int s = -1;
    int i;
    for (i = 0; name[i]; i++)
      if (name[i] == '\\') s = i;
    copy_str(out, max, name + s + 1);
  }
  return file_size(path) >= 0 ? 0 : -1;
}

/* md/rd: os codigos de fat_mkdir/fat_rmdir; -5 se nao e disco FAT. */
static int dir_op(const char* path, int make) {
  char L;
  char name[DRV_PATH];
  int idx;
  path_parse(path, &L, name, (int)sizeof(name));
  idx = letter_idx(L);
  if (!name[0] || idx < 0 || !drv_ready(idx)) return -1;
  if (drv_kind[idx] != DRV_FAT) return -5;
  return make ? fat_mkdir(name) : fat_rmdir(name);
}

int drives_mkdir(const char* path) { return dir_op(path, 1); }
int drives_rmdir(const char* path) { return dir_op(path, 0); }

/* Espaco da unidade em KB. -1 se nao e disco FAT montado. */
int drives_space(char letter, uint32_t* total_kb, uint32_t* free_kb) {
  int idx = letter_idx(letter);
  uint32_t t, f;
  if (idx < 0 || !drv_ready(idx) || drv_kind[idx] != DRV_FAT) return -1;
  if (fat_get_space_info(&t, &f) != 0) return -1;
  if (total_kb) *total_kb = t / 1024u;
  if (free_kb) *free_kb = f / 1024u;
  return 0;
}

void drives_print_dir(const char* spec) {
  drives_dir(spec, 0);
}

int drives_names(char letter, char out[][32], int max) {
  char L = norm_letter(letter);
  int idx = letter_idx(L);
  int n = 0;
  int i;
  if (!out || max <= 0) return 0;
  if (!drv_ready(idx)) return 0;
  if (drv_kind[idx] == DRV_FAT) {
    char tmp[1024];
    int pos = 0;
    int rc = fat_list_dir(cur_dir[idx], tmp, (int)sizeof(tmp));
    if (rc < 0) return 0;
    while (tmp[pos] && n < max) {
      char name[32];
      int k = 0;
      while (tmp[pos] && tmp[pos] != '\n' && k < 31)
        name[k++] = tmp[pos++];
      if (k > 0 && name[k - 1] == '\\') k--;
      name[k] = '\0';
      if (tmp[pos] == '\n') pos++;
      if (k) n = push_name(out, n, max, name);
    }
    return n;
  }
  if (L == 'D') {
    for (i = 0; i < d_ram_count && n < max; i++)
      n = push_name(out, n, max, d_ram[i].name);
    return n;
  }
  for (i = 0; i < file_count && n < max; i++)
    n = push_name(out, n, max, files[i].name);
  for (i = 0; i < mem_file_count && n < max; i++)
    n = push_name(out, n, max, mem_files[i].name);
  return n;
}

void drives_list(char letter) {
  int idx = letter_idx(letter);
  char L = norm_letter(letter);
  if (!drv_ready(idx)) {
    putc(L ? L : '?');
    puts_k(": nao montado. Use: mount D:   ou   mount 1 D:\n");
    return;
  }
  putc(L);
  puts_k(":  ");
  puts_k(drives_kind_name(L));
  puts_k("\n");
  if (drv_kind[idx] == DRV_FAT) {
    char output[2048];
    int n = fat_list_files(output, sizeof(output));
    if (n > 0) puts_k(output);
    else puts_k("(vazio)\n");
    return;
  }
  if (L == 'D') {
    int i;
    if (d_ram_count == 0) {
      puts_k("(vazio)\n");
      return;
    }
    for (i = 0; i < d_ram_count; i++) {
      puts_k("  ");
      puts_k(d_ram[i].name);
      puts_k("\n");
    }
    return;
  }
  {
    int i;
    if (mem_file_count > 0) {
      puts_k("Memoria:\n");
      for (i = 0; i < mem_file_count; i++) {
        puts_k("  ");
        puts_k(mem_files[i].name);
        puts_k("\n");
      }
    }
    if (file_count > 0) {
      puts_k("Embutidos:\n");
      for (i = 0; i < file_count; i++) {
        puts_k("  ");
        puts_k(files[i].name);
        puts_k("\n");
      }
    }
  }
}

static void put_path(char* dst, int max, char letter, const char* name) {
  int i = 0;
  if (!dst || max < 5) return;
  dst[i++] = letter;
  dst[i++] = ':';
  dst[i++] = '\\';
  while (name && *name && i < max - 1)
    dst[i++] = *name++;
  dst[i] = '\0';
}

/* dir vazio lista a raiz. Pastas (nome terminado em \) ficam de fora. */
static int enum_one_dir(char out[][48], int n, int max, const char* dir) {
  char tmp[1024];
  int pos = 0;
  char rel[40];
  int pre = 0;
  if (n >= max) return n;
  if (fat_list_dir(dir ? dir : "", tmp, (int)sizeof(tmp)) < 0) return n;
  if (dir && dir[0]) {
    while (dir[pre] && pre < 36) {
      rel[pre] = dir[pre];
      pre++;
    }
    rel[pre++] = '\\';
    rel[pre] = '\0';
  }
  while (tmp[pos] && n < max) {
    char name[32];
    char full[46];
    int k = 0;
    int i;
    int j;
    while (tmp[pos] && tmp[pos] != '\n' && k < 31)
      name[k++] = tmp[pos++];
    name[k] = '\0';
    if (tmp[pos] == '\n') pos++;
    if (!k || name[k - 1] == '\\') continue;
    /* Nome cortado nao abre: quem nao cabe fica de fora. */
    if (pre + k > 44) continue;
    i = 0;
    for (j = 0; j < pre && i < 44; j++) full[i++] = rel[j];
    for (j = 0; name[j] && i < 44; j++) full[i++] = name[j];
    full[i] = '\0';
    put_path(out[n], 48, 'D', full);
    n++;
  }
  return n;
}

int drives_enum_paths(char out[][48], int max) {
  int n = 0;
  int i;
  if (!out || max <= 0) return 0;
  if (drv_kind['D' - 'A'] == DRV_FAT && fat_use('D' - 'A') == 0) {
    n = enum_one_dir(out, n, max, "user\\default\\desktop");
    n = enum_one_dir(out, n, max, "");
  } else if (drv_kind['D' - 'A'] == DRV_RAM) {
    for (i = 0; i < d_ram_count && n < max; i++)
      put_path(out[n++], 48, 'D', d_ram[i].name);
    if (n < max) put_path(out[n++], 48, 'D', "nota.txt");
  }
  for (i = 0; i < file_count && n < max; i++)
    put_path(out[n++], 48, 'C', files[i].name);
  for (i = 0; i < mem_file_count && n < max; i++)
    put_path(out[n++], 48, 'C', mem_files[i].name);
  return n;
}
