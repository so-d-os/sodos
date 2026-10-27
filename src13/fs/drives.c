#include "drives.h"
#include "shell.h"
#include "mm.h"

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
static char cur_dir[26][48];

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

void drives_init(void) {
  int i;
  for (i = 0; i < 26; i++) {
    drv_kind[i] = DRV_NONE;
    drv_ata[i] = 0;
  }
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
  if (i < 0 || drv_kind[i] == DRV_NONE) return -1;
  cur_letter = L;
  return 0;
}

int drives_is_mounted(char letter) {
  int i = letter_idx(letter);
  if (i < 0) return 0;
  return drv_kind[i] != DRV_NONE;
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
  fat_set_drive(ata_drive);
  result = get_partition_info(ata_drive, (uint8_t)partition_num, &lba_start, &sector_count, &type);
  if (result != 0) {
    puts_k("Particao nao encontrada. Usando disco inteiro.\n");
    fat_set_partition_offset(0);
  } else {
    fat_set_partition_offset(lba_start);
  }
  result = fat_mount();
  if (result != 0) return result;
  drv_kind[i] = DRV_FAT;
  drv_ata[i] = ata_drive;
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
  fat_set_drive(1);
  fat_set_partition_offset(lba);
  if (fat_mount() != 0) return;
  drv_kind[idx] = DRV_FAT;
  drv_ata[idx] = 1;
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
    if (drv_kind[i] == DRV_FAT) {
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

/* Junta . e .. . No maximo 8 componentes de 12 caracteres. */
static void path_norm(char* s, int max) {
  char comp[8][13];
  int n = 0;
  int pos = 0;
  int i = 0;
  int k;
  if (!s || max < 2) return;
  while (n < 8) {
    char one[13];
    int m = 0;
    while (s[pos] == '\\' || s[pos] == '/') pos++;
    if (!s[pos]) break;
    while (s[pos] && s[pos] != '\\' && s[pos] != '/' && m < 12) one[m++] = s[pos++];
    while (s[pos] && s[pos] != '\\' && s[pos] != '/') pos++;
    one[m] = 0;
    if (m == 1 && one[0] == '.') continue;
    if (m == 2 && one[0] == '.' && one[1] == '.') {
      if (n > 0) n--;
      continue;
    }
    for (k = 0; k < m; k++) comp[n][k] = one[k];
    comp[n][m] = 0;
    n++;
  }
  for (k = 0; k < n; k++) {
    int j;
    if (k && i < max - 1) s[i++] = '\\';
    for (j = 0; comp[k][j] && i < max - 1; j++) s[i++] = comp[k][j];
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
  char name[64];
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
  char name[64];
  int idx;
  if (!path || !buf || len < 1) return -1;
  path_parse(path, &L, name, sizeof(name));
  if (name[0] == '\0') return -1;
  idx = letter_idx(L);
  if (idx < 0 || drv_kind[idx] == DRV_NONE) return -2;
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
  return -1;
}

int file_load(const char* path, char* buf, int max_size) {
  char L;
  char name[64];
  int idx;
  int n;
  if (!path || !buf || max_size < 2) return -1;
  path_parse(path, &L, name, sizeof(name));
  if (name[0] == '\0') return -1;
  idx = letter_idx(L);
  if (idx < 0 || drv_kind[idx] == DRV_NONE) return -2;
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
  char name[64];
  int idx;
  if (!path) return -1;
  path_parse(path, &L, name, sizeof(name));
  if (name[0] == '\0') return -1;
  idx = letter_idx(L);
  if (idx < 0 || drv_kind[idx] == DRV_NONE) return -2;
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
  char name[64];
  int idx;
  if (!path || !data) return -1;
  if (size < 0) size = 0;
  path_parse(path, &L, name, sizeof(name));
  if (name[0] == '\0') return -1;
  idx = letter_idx(L);
  if (idx < 0 || drv_kind[idx] == DRV_NONE) return -2;
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
  char name[48];
  int idx;
  if (!spec || !spec[0]) {
    char pwd[64];
    drives_pwd(pwd, (int)sizeof(pwd));
    puts_k(pwd);
    puts_k("\n");
    return 0;
  }
  path_parse(spec, &L, name, (int)sizeof(name));
  idx = letter_idx(L);
  if (idx < 0 || drv_kind[idx] == DRV_NONE) {
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
  char name[64];
  int idx;
  if (!out || max < 2) return -1;
  out[0] = 0;
  path_parse(path ? path : "", &L, name, (int)sizeof(name));
  idx = letter_idx(L);
  if (idx < 0 || drv_kind[idx] != DRV_FAT) return -1;
  return fat_list_dir(name, out, max);
}

static void print_where(char L, const char* name) {
  putc(L);
  puts_k(":\\");
  if (name && name[0]) puts_k(name);
  puts_k("\n");
}

void drives_print_dir(const char* spec) {
  char L;
  char name[64];
  int idx;
  path_parse(spec ? spec : "", &L, name, (int)sizeof(name));
  idx = letter_idx(L);
  if (idx < 0 || drv_kind[idx] == DRV_NONE) {
    putc(L ? L : '?');
    puts_k(": nao montado\n");
    return;
  }
  puts_k(" Diretorio de ");
  print_where(L, name);
  if (drv_kind[idx] == DRV_FAT) {
    char output[2048];
    int n = fat_list_dir(name, output, (int)sizeof(output));
    int p = 0;
    if (n < 0) {
      puts_k("pasta nao encontrada\n");
      return;
    }
    if (n == 0) {
      puts_k("(vazio)\n");
      return;
    }
    while (output[p]) {
      char line[16];
      int k = 0;
      int dir = 0;
      while (output[p] && output[p] != '\n' && k < 14) line[k++] = output[p++];
      line[k] = 0;
      if (output[p] == '\n') p++;
      if (k > 0 && line[k - 1] == '\\') {
        line[k - 1] = 0;
        dir = 1;
      }
      if (dir) puts_k("<DIR>  ");
      else puts_k("       ");
      puts_k(line);
      puts_k("\n");
    }
    return;
  }
  if (name[0]) {
    puts_k("esta unidade nao tem pastas\n");
    return;
  }
  {
    char names[16][32];
    int n = drives_names(L, names, 16);
    int i;
    if (n <= 0) puts_k("(vazio)\n");
    for (i = 0; i < n; i++) {
      puts_k("       ");
      puts_k(names[i]);
      puts_k("\n");
    }
  }
}

int drives_names(char letter, char out[][32], int max) {
  char L = norm_letter(letter);
  int idx = letter_idx(L);
  int n = 0;
  int i;
  if (!out || max <= 0) return 0;
  if (idx < 0 || drv_kind[idx] == DRV_NONE) return 0;
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
  if (idx < 0 || drv_kind[idx] == DRV_NONE) {
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
    char name[16];
    char full[46];
    int k = 0;
    int i;
    int j;
    while (tmp[pos] && tmp[pos] != '\n' && k < 15)
      name[k++] = tmp[pos++];
    name[k] = '\0';
    if (tmp[pos] == '\n') pos++;
    if (!k || name[k - 1] == '\\') continue;
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
  if (drv_kind['D' - 'A'] == DRV_FAT) {
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
