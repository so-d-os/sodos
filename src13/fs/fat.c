// fat.c - Sistema de arquivos FAT12/FAT16
#include <stdint.h>
#include "disk.h"
#include "sched.h"
#include "drives.h"

// Estrutura do Boot Sector FAT
struct fat_boot_sector {
  uint8_t  jmp[3];
  char     oem_name[8];
  uint16_t bytes_per_sector;
  uint8_t  sectors_per_cluster;
  uint16_t reserved_sectors;
  uint8_t  fat_count;
  uint16_t root_entries;
  uint16_t total_sectors_16;
  uint8_t  media_type;
  uint16_t sectors_per_fat_16;
  uint16_t sectors_per_track;
  uint16_t heads;
  uint32_t hidden_sectors;
  uint32_t total_sectors_32;
  
  // FAT16 extended (offset 36)
  uint8_t  drive_number;
  uint8_t  reserved;
  uint8_t  boot_signature;
  uint32_t volume_id;
  char     volume_label[11];
  char     fs_type[8];
  uint8_t  boot_code[448];
  uint16_t boot_signature_word;
} __attribute__((packed));

// Estrutura de entrada de diretório FAT
struct fat_dir_entry {
  char     name[8];
  char     ext[3];
  uint8_t  attributes;
  uint8_t  reserved[10];
  uint16_t time;
  uint16_t date;
  uint16_t cluster;
  uint32_t size;
} __attribute__((packed));

/* Pedaco de nome longo (VFAT): 13 caracteres UCS-2 por entrada, antes da
 * entrada 8.3. A primeira no disco tem ord | 0x40 e e o fim do nome. */
struct fat_lfn_entry {
  uint8_t  ord;
  uint16_t n1[5];
  uint8_t  attributes;   /* 0x0F */
  uint8_t  type;
  uint8_t  sum;          /* soma do nome 8.3 */
  uint16_t n2[6];
  uint16_t cluster;      /* 0 */
  uint16_t n3[2];
} __attribute__((packed));

#define LFN_CHARS 13
#define LFN_ORD_MAX 20   /* 20 * 13 = 260 >= 255 */
/* reserved[0] do 8.3 (NT): nome ou extensao em minusculas, sem LFN. */
#define NT_LOW_BASE 0x08
#define NT_LOW_EXT  0x10

/* Em 10 MB (0x100000-0x9FFFFF e a tela em memoria do visual.c): a FAT16 de um
 * disco de 100 MB passa de 100 KB. FAT ate 256 KB, raiz ate 64 KB.
 * O disco so le/escreve abaixo de 1 MB, por isso tudo passa por fat_bounce. */
#define FAT_TABLE_MAX 0x40000u
#define ROOT_DIR_MAX  0x10000u
static struct fat_boot_sector* boot_sector = (struct fat_boot_sector*)0xA00000;
static uint8_t* fat_table = (uint8_t*)0xA01000;
static struct fat_dir_entry* root_dir = (struct fat_dir_entry*)0xA41000;
/* Diretorio aberto (\temp\net\ e outros). 64 KB (dois clusters de 32 KB,
 * os de um FAT16 de 2 GB), termina em 0xA61000, antes do kmalloc em 0xA80000.
 * Nao pode ir para o BSS do kernel. */
#define SUBDIR_MAX 0x10000u
static struct fat_dir_entry* sub_dir = (struct fat_dir_entry*)0xA51000;

/* Setores da FAT mudados desde o ultimo flush_fat: so eles vao para o disco
 * (um pendrive grava cada setor devagar e a FAT de 2 GB tem 256). */
static uint8_t fat_dirty[FAT_TABLE_MAX / 512u / 8u];

static void mem_zero(void* p, uint32_t n);

static void fat_mark(uint32_t byte_off) {
  uint32_t s = byte_off / 512u;
  fat_dirty[s >> 3] |= (uint8_t)(1u << (s & 7u));
}

static int fat_type = 0; // 0 = não montado, 12 = FAT12, 16 = FAT16
static uint32_t bytes_per_sector = 512;
static uint32_t sectors_per_cluster = 1;
static uint32_t root_dir_sectors = 0;
static uint32_t first_data_sector = 0;
static uint32_t first_fat_sector = 0;
static uint32_t root_dir_sector = 0;

// Drive atual para operações FAT
static uint32_t current_drive = 1; // Default: segundo disco

// Define o drive atual
void fat_set_drive(uint32_t drive) {
  current_drive = drive;
}

/* Disco so aceita buffer em 0x60000-0x9FFFF, fora da pilha.
 * Arquivo e editor ficam no heap: copia por este setor. */
static uint8_t* const fat_bounce = (uint8_t*)0x7E000u;

static int sector_direct(const void* buffer) {
  uint32_t a = (uint32_t)buffer;
  if (a < 0x60000u || a + 512u > 0x100000u) return 0;
  if (a >= 0x90000u && a < 0xA0000u) return 0;
  return 1;
}

/* A cada poucos setores o desktop volta a ler teclado e rato. */
static void fat_breathe(void) {
  static int n;
  if (++n < 4) return;
  n = 0;
  sched_yield();
}

static int read_sector(uint32_t lba, void* buffer) {
  uint8_t* dst = (uint8_t*)buffer;
  int i;
  if (sector_direct(buffer))
    return disk_read_sector(current_drive, lba, buffer);
  if (disk_read_sector(current_drive, lba, fat_bounce) != 0) return -1;
  for (i = 0; i < 512; i++) dst[i] = fat_bounce[i];
  return 0;
}

static int write_sector(uint32_t lba, const void* buffer) {
  const uint8_t* src = (const uint8_t*)buffer;
  int i;
  if (sector_direct(buffer))
    return disk_write_sector(current_drive, lba, (void*)buffer);
  for (i = 0; i < 512; i++) fat_bounce[i] = src[i];
  return disk_write_sector(current_drive, lba, fat_bounce);
}

static int read_sectors(uint32_t lba, uint32_t count, void* buffer) {
  uint8_t* buf = (uint8_t*)buffer;
  uint32_t i;
  uint32_t step = bytes_per_sector ? bytes_per_sector : 512u;
  for (i = 0; i < count; i++) {
    if (read_sector(lba + i, buf + (i * step)) != 0) return -1;
  }
  return 0;
}

static int write_sectors(uint32_t lba, uint32_t count, const void* buffer) {
  const uint8_t* buf = (const uint8_t*)buffer;
  uint32_t i;
  uint32_t step = bytes_per_sector ? bytes_per_sector : 512u;
  for (i = 0; i < count; i++) {
    if (write_sector(lba + i, buf + (i * step)) != 0) return -1;
  }
  return 0;
}

// Offset LBA da partição (0 = disco inteiro, ou offset da partição)
static uint32_t partition_offset = 0;

// Define offset da partição
void fat_set_partition_offset(uint32_t offset) {
  partition_offset = offset;
}

// Monta o sistema de arquivos FAT
int fat_is_mounted(void) {
  return fat_type != 0;
}

int fat_mount(void) {
  // Lê o boot sector (LBA 0 + offset da partição)
  read_sector(partition_offset, boot_sector);
  
  // Verifica assinatura
  if (boot_sector->jmp[0] != 0xEB && boot_sector->jmp[0] != 0xE9) {
    return -1; // Não é um boot sector válido
  }

  /* Validar antes de alterar globais (evita deixar sectors_per_cluster=0 e causar div/0 em df/copy) */
  if (boot_sector->bytes_per_sector == 0 || boot_sector->sectors_per_cluster == 0)
    return -1; /* boot sector inválido (ex.: partição não formatada) */
  /* O resto do arquivo le e grava de 512 em 512 bytes. */
  if (boot_sector->bytes_per_sector != 512 || boot_sector->fat_count == 0 ||
      boot_sector->reserved_sectors == 0)
    return -1;

  bytes_per_sector = boot_sector->bytes_per_sector;
  sectors_per_cluster = boot_sector->sectors_per_cluster;

  // Calcula setores importantes
  first_fat_sector = boot_sector->reserved_sectors;
  root_dir_sectors = ((boot_sector->root_entries * 32) + (bytes_per_sector - 1)) / bytes_per_sector;

  // Determina tipo de FAT baseado no número de clusters
  uint32_t total_sectors = boot_sector->total_sectors_16;
  if (total_sectors == 0) {
    total_sectors = boot_sector->total_sectors_32;
  }

  uint32_t data_sectors = total_sectors -
                          (boot_sector->reserved_sectors +
                           (boot_sector->fat_count * boot_sector->sectors_per_fat_16) +
                           root_dir_sectors);
  uint32_t total_clusters = data_sectors / sectors_per_cluster;
  
  if (total_clusters < 4085) {
    fat_type = 12;
  } else if (total_clusters < 65525) {
    fat_type = 16;
  } else {
    return -2; // FAT32 não suportado ainda
  }
  
  first_data_sector = partition_offset + boot_sector->reserved_sectors + 
                      (boot_sector->fat_count * boot_sector->sectors_per_fat_16) + 
                      root_dir_sectors;
  root_dir_sector = partition_offset + boot_sector->reserved_sectors + 
                    (boot_sector->fat_count * boot_sector->sectors_per_fat_16);
  first_fat_sector = partition_offset + boot_sector->reserved_sectors;

  if ((uint32_t)boot_sector->sectors_per_fat_16 * bytes_per_sector > FAT_TABLE_MAX ||
      root_dir_sectors * bytes_per_sector > ROOT_DIR_MAX) {
    fat_type = 0;
    return -3;
  }
  
  // Lê a FAT
  mem_zero(fat_dirty, sizeof(fat_dirty));
  if (read_sectors(first_fat_sector, boot_sector->sectors_per_fat_16, fat_table) != 0) {
    fat_type = 0;
    return -4;
  }
  
  // Lê o diretório raiz
  if (read_sectors(root_dir_sector, root_dir_sectors, root_dir) != 0) {
    fat_type = 0;
    return -4;
  }
  
  return 0;
}

static uint32_t cluster_count(void) {
  uint32_t total = boot_sector->total_sectors_16;
  if (total == 0) total = boot_sector->total_sectors_32;
  if (sectors_per_cluster == 0) return 0;
  return (total - (boot_sector->reserved_sectors +
                   (boot_sector->fat_count * boot_sector->sectors_per_fat_16) +
                   root_dir_sectors)) / sectors_per_cluster;
}

static char low_ascii(char c) {
  return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

// Converte nome 8.3 para string normal (com as minusculas do NT)
static void fat_name_to_string(const struct fat_dir_entry* entry, char* output) {
  int i = 0;
  uint8_t nt = entry->reserved[0];
  // Nome
  for (int j = 0; j < 8; j++) {
    if (entry->name[j] != ' ') {
      char c = entry->name[j];
      /* 0x05 no disco e 0xE5 de verdade no primeiro caractere */
      if (j == 0 && (unsigned char)c == 0x05) c = (char)0xE5;
      output[i++] = (nt & NT_LOW_BASE) ? low_ascii(c) : c;
    }
  }
  // Extensão
  if (entry->ext[0] != ' ') {
    output[i++] = '.';
    for (int j = 0; j < 3; j++) {
      if (entry->ext[j] != ' ') {
        output[i++] = (nt & NT_LOW_EXT) ? low_ascii(entry->ext[j]) : entry->ext[j];
      }
    }
  }
  output[i] = '\0';
}

/* O sistema escreve em CP850; o LFN guarda Unicode. Metade de cima do CP850. */
static const uint16_t cp850_hi[128] = {
  0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
  0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
  0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
  0x00FF, 0x00D6, 0x00DC, 0x00F8, 0x00A3, 0x00D8, 0x00D7, 0x0192,
  0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
  0x00BF, 0x00AE, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
  0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x00C1, 0x00C2, 0x00C0,
  0x00A9, 0x2563, 0x2551, 0x2557, 0x255D, 0x00A2, 0x00A5, 0x2510,
  0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x00E3, 0x00C3,
  0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x00A4,
  0x00F0, 0x00D0, 0x00CA, 0x00CB, 0x00C8, 0x0131, 0x00CD, 0x00CE,
  0x00CF, 0x2518, 0x250C, 0x2588, 0x2584, 0x00A6, 0x00CC, 0x2580,
  0x00D3, 0x00DF, 0x00D4, 0x00D2, 0x00F5, 0x00D5, 0x00B5, 0x00FE,
  0x00DE, 0x00DA, 0x00DB, 0x00D9, 0x00FD, 0x00DD, 0x00AF, 0x00B4,
  0x00AD, 0x00B1, 0x2017, 0x00BE, 0x00B6, 0x00A7, 0x00F7, 0x00B8,
  0x00B0, 0x00A8, 0x00B7, 0x00B9, 0x00B3, 0x00B2, 0x25A0, 0x00A0,
};

static uint16_t oem_to_uni(unsigned char c) {
  return c < 0x80 ? c : cp850_hi[c - 0x80];
}

/* Fora do CP850 vira '_'. */
static char uni_to_oem(uint16_t u) {
  int i;
  if (u < 0x80) return (char)u;
  for (i = 0; i < 128; i++)
    if (cp850_hi[i] == u) return (char)(0x80 + i);
  return '_';
}

/* Maiuscula do Latin-1, para comparar nomes sem caixa (ç = Ç). */
static uint16_t uni_fold(uint16_t u) {
  if (u >= 'a' && u <= 'z') return (uint16_t)(u - 32);
  if (u >= 0xE0 && u <= 0xFE && u != 0xF7) return (uint16_t)(u - 32);
  if (u == 0xFF) return 0x178;
  return u;
}

static int name_eq_fold(const char* a, const char* b) {
  while (*a && *b) {
    if (uni_fold(oem_to_uni((unsigned char)*a)) != uni_fold(oem_to_uni((unsigned char)*b)))
      return 0;
    a++;
    b++;
  }
  return *a == *b;
}

// Converte string para nome 8.3
static void string_to_fat_name(const char* input, char* name, char* ext) {
  int i = 0;
  int j = 0;
  
  // Limpa buffers
  for (int k = 0; k < 8; k++) name[k] = ' ';
  for (int k = 0; k < 3; k++) ext[k] = ' ';
  
  // Copia nome
  while (input[i] != '\0' && input[i] != '.' && j < 8) {
    if (input[i] >= 'a' && input[i] <= 'z') {
      name[j++] = input[i] - 32; // Para maiúscula
    } else {
      name[j++] = input[i];
    }
    i++;
  }
  
  // Pula o ponto
  if (input[i] == '.') i++;
  
  // Copia extensão
  j = 0;
  while (input[i] != '\0' && j < 3) {
    if (input[i] >= 'a' && input[i] <= 'z') {
      ext[j++] = input[i] - 32;
    } else {
      ext[j++] = input[i];
    }
    i++;
  }
}

// Busca arquivo no diretório raiz
struct fat_dir_entry* fat_find_file(const char* filename) {
  char search_name[8];
  char search_ext[3];
  string_to_fat_name(filename, search_name, search_ext);
  
  for (int i = 0; i < boot_sector->root_entries; i++) {
    if (root_dir[i].name[0] == 0x00) break; // Fim do diretório
    if (root_dir[i].name[0] == 0xE5) continue; // Entrada deletada
    
    // Verifica se é arquivo normal (não diretório, volume label, etc)
    if ((root_dir[i].attributes & 0x18) == 0) {
      int match = 1;
      for (int j = 0; j < 8; j++) {
        if (root_dir[i].name[j] != search_name[j]) {
          match = 0;
          break;
        }
      }
      if (match) {
        for (int j = 0; j < 3; j++) {
          if (root_dir[i].ext[j] != search_ext[j]) {
            match = 0;
            break;
          }
        }
        if (match) {
          return &root_dir[i];
        }
      }
    }
  }
  return 0;
}

// Lê cluster do FAT16
static uint16_t read_fat16_cluster(uint16_t cluster) {
  uint16_t* fat16 = (uint16_t*)fat_table;
  return fat16[cluster];
}

// Lê cluster do FAT12
static uint16_t read_fat12_cluster(uint16_t cluster) {
  uint32_t fat_offset = cluster + (cluster / 2);
  uint16_t fat_value = *(uint16_t*)(fat_table + fat_offset);
  
  if (cluster & 0x0001) {
    return fat_value >> 4;
  } else {
    return fat_value & 0x0FFF;
  }
}

// Lê próximo cluster
static uint16_t get_next_cluster(uint16_t cluster) {
  if (fat_type == 12) {
    return read_fat12_cluster(cluster);
  } else if (fat_type == 16) {
    return read_fat16_cluster(cluster);
  }
  return 0;
}

/* Caminho com pastas: "temp\\net\\ARQ.EXT". A raiz continua valendo para
 * "ARQ.EXT". So FAT16 cria pasta; a leitura tambem segue a cadeia. */
static void write_fat16_cluster(uint16_t cluster, uint16_t value);
static uint16_t find_free_cluster(void);
static void free_chain(uint16_t cluster);
static void flush_fat(void);
static void flush_root_dir(void);

struct dir_view {
  struct fat_dir_entry* ents;
  int count;
  int bytes;
  uint16_t cluster;
};

static void mem_zero(void* p, uint32_t n) {
  uint8_t* b = (uint8_t*)p;
  uint32_t i;
  for (i = 0; i < n; i++) b[i] = 0;
}

static int cluster_end(uint16_t c) {
  if (c < 2) return 1;
  if (fat_type == 12 && c >= 0xFF8) return 1;
  if (fat_type == 16 && c >= 0xFFF8) return 1;
  return 0;
}

/* 1 leu um nome, 0 acabou, -1 nome maior que max - 1. */
static int next_comp(const char* path, int* pos, char* out, int max) {
  int i = 0;
  int too_long = 0;
  if (!path || max < 2) return 0;
  while (path[*pos] == '\\' || path[*pos] == '/') (*pos)++;
  if (!path[*pos]) return 0;
  while (path[*pos] && path[*pos] != '\\' && path[*pos] != '/') {
    if (i < max - 1) out[i++] = path[*pos];
    else too_long = 1;
    (*pos)++;
  }
  out[i] = 0;
  if (too_long) return -1;
  return i > 0;
}

static int dir_load(uint16_t cluster, struct dir_view* v) {
  uint8_t* dst;
  int guard = 0;
  v->cluster = cluster;
  v->bytes = 0;
  v->count = 0;
  v->ents = 0;
  if (cluster < 2) {
    v->ents = root_dir;
    v->count = boot_sector->root_entries;
    v->bytes = v->count * 32;
    return 0;
  }
  if (bytes_per_sector != 512 || sectors_per_cluster == 0) return -1;
  v->ents = sub_dir;
  dst = (uint8_t*)sub_dir;
  while (!cluster_end(cluster) && guard++ < 64) {
    uint32_t lba = first_data_sector + ((uint32_t)(cluster - 2) * sectors_per_cluster);
    uint32_t s;
    for (s = 0; s < sectors_per_cluster; s++) {
      if ((uint32_t)v->bytes + 512u > SUBDIR_MAX) return -1;
      if (read_sector(lba + s, dst + v->bytes) != 0) return -1;
      v->bytes += 512;
    }
    cluster = get_next_cluster(cluster);
  }
  v->count = v->bytes / 32;
  return 0;
}

static int dir_flush(struct dir_view* v) {
  uint16_t cluster;
  uint32_t off;
  int guard = 0;
  if (!v || !v->ents) return -1;
  if (v->cluster < 2) {
    flush_root_dir();
    return 0;
  }
  cluster = v->cluster;
  off = 0;
  while (!cluster_end(cluster) && off < (uint32_t)v->bytes && guard++ < 64) {
    uint32_t lba = first_data_sector + ((uint32_t)(cluster - 2) * sectors_per_cluster);
    uint32_t s;
    for (s = 0; s < sectors_per_cluster && off < (uint32_t)v->bytes; s++) {
      if (write_sector(lba + s, (uint8_t*)v->ents + off) != 0) return -1;
      off += 512;
    }
    cluster = get_next_cluster(cluster);
  }
  return off == (uint32_t)v->bytes ? 0 : -1;
}

static uint8_t sum11(const uint8_t* p) {
  uint8_t s = 0;
  int i;
  for (i = 0; i < 11; i++) s = (uint8_t)(((s & 1u) << 7) + (s >> 1) + p[i]);
  return s;
}

static uint16_t lfn_char(const struct fat_lfn_entry* l, int k) {
  if (k < 5) return l->n1[k];
  if (k < 11) return l->n2[k - 5];
  return l->n3[k - 11];
}

static void lfn_set(struct fat_lfn_entry* l, int k, uint16_t u) {
  if (k < 5) l->n1[k] = u;
  else if (k < 11) l->n2[k - 5] = u;
  else l->n3[k - 11] = u;
}

/* Poe em name a parte de l (ordem ord); *len encurta no 0 que fecha o nome. */
static void lfn_take(const struct fat_lfn_entry* l, int ord, char* name, int* len) {
  int base = (ord - 1) * LFN_CHARS;
  int k;
  for (k = 0; k < LFN_CHARS; k++) {
    uint16_t u = lfn_char(l, k);
    if (u == 0) {
      if (base + k < *len) *len = base + k;
      return;
    }
    if (base + k < FAT_NAME_MAX - 1) name[base + k] = uni_to_oem(u);
  }
}

/* Proxima entrada de arquivo ou pasta a partir de *i (pula as apagadas, o
 * rotulo e os pedacos LFN). name (FAT_NAME_MAX) recebe o nome longo, ou o
 * 8.3 sem LFN valido; *first e a primeira entrada do nome. 0 no fim. */
static struct fat_dir_entry* dir_next(struct dir_view* v, int* i, char* name, int* first) {
  int need = 0;     /* proxima ordem LFN esperada */
  int done = 0;     /* nome longo completo */
  int len = 0;
  int start = 0;
  uint8_t sum = 0;
  for (; *i < v->count; (*i)++) {
    struct fat_dir_entry* e = &v->ents[*i];
    unsigned char n0 = (unsigned char)e->name[0];
    if (n0 == 0x00) break;
    if (n0 == 0xE5) {
      need = done = 0;
      continue;
    }
    if ((e->attributes & 0x3F) == 0x0F) {
      const struct fat_lfn_entry* l = (const struct fat_lfn_entry*)e;
      int ord = l->ord & 0x1F;
      if (l->ord & 0x40) {
        if (ord < 1 || ord > LFN_ORD_MAX) {
          need = done = 0;
          continue;
        }
        need = ord;
        sum = l->sum;
        start = *i;
        len = ord * LFN_CHARS;
        if (len > FAT_NAME_MAX - 1) len = FAT_NAME_MAX - 1;
      } else if (!need || ord != need || l->sum != sum) {
        need = done = 0;
        continue;
      }
      lfn_take(l, ord, name, &len);
      need--;
      done = need == 0;
      continue;
    }
    if (e->attributes & 0x08) {
      need = done = 0;
      continue;
    }
    if (done && len > 0 && sum11((const uint8_t*)e->name) == sum) {
      name[len] = 0;
      if (first) *first = start;
    } else {
      fat_name_to_string(e, name);
      if (first) *first = *i;
    }
    (*i)++;
    return e;
  }
  return 0;
}

/* Nome longo da ultima entrada achada por dir_find. */
static char found_name[FAT_NAME_MAX];

/* Pelo nome longo ou pelo 8.3, sem caixa. *first = primeira entrada (LFN). */
static struct fat_dir_entry* dir_find(struct dir_view* v, const char* filename, int want_dir,
                                      int* first) {
  char sn[13];
  int i = 0;
  int f;
  struct fat_dir_entry* e;
  while ((e = dir_next(v, &i, found_name, &f)) != 0) {
    int is_dir = (e->attributes & 0x10) != 0;
    if (want_dir != is_dir) continue;
    if (!name_eq_fold(found_name, filename)) {
      fat_name_to_string(e, sn);
      if (!name_eq_fold(sn, filename)) continue;
    }
    if (first) *first = f;
    return e;
  }
  return 0;
}

/* Para os programas (32 bytes): o nome longo se couber, senao o 8.3. */
static void item_name(const struct fat_dir_entry* e, const char* lname, char* out) {
  int n = 0;
  while (lname[n]) n++;
  if (n > 31) {
    fat_name_to_string(e, out);
    return;
  }
  for (n = 0; lname[n]; n++) out[n] = lname[n];
  out[n] = 0;
}

/* Apaga o nome: as entradas LFN de first ate e, e a propria e. */
static void dir_unlink(struct dir_view* v, int first, struct fat_dir_entry* e) {
  int i;
  for (i = first; i <= (int)(e - v->ents); i++) v->ents[i].name[0] = (char)0xE5;
}

static int dir_grow(struct dir_view* v) {
  uint16_t cluster;
  uint16_t prev = 0;
  uint16_t neu;
  uint32_t s;
  uint32_t lba;
  int guard = 0;
  if (v->cluster < 2 || fat_type != 16) return -1;
  if ((uint32_t)v->bytes + sectors_per_cluster * 512u > SUBDIR_MAX) return -1;
  cluster = v->cluster;
  while (!cluster_end(cluster) && guard++ < 64) {
    uint16_t n = get_next_cluster(cluster);
    if (cluster_end(n)) {
      prev = cluster;
      break;
    }
    cluster = n;
  }
  if (!prev) return -1;
  neu = find_free_cluster();
  if (!neu) return -1;
  write_fat16_cluster(neu, 0xFFFF);
  write_fat16_cluster(prev, neu);
  lba = first_data_sector + ((uint32_t)(neu - 2) * sectors_per_cluster);
  for (s = 0; s < sectors_per_cluster; s++) {
    uint32_t i;
    uint8_t* dst = (uint8_t*)v->ents + v->bytes;
    for (i = 0; i < 512; i++) fat_bounce[i] = 0;
    if (disk_write_sector(current_drive, lba + s, fat_bounce) != 0) return -1;
    for (i = 0; i < 512; i++) dst[i] = 0;
    v->bytes += 512;
  }
  v->count = v->bytes / 32;
  return 0;
}

/* n entradas livres seguidas; cresce a pasta se faltar. */
static struct fat_dir_entry* dir_slots(struct dir_view* v, int n) {
  int tries;
  for (tries = 0; tries < 4; tries++) {
    int i;
    int run = 0;
    for (i = 0; i < v->count; i++) {
      unsigned char n0 = (unsigned char)v->ents[i].name[0];
      if (n0 == 0x00 || n0 == 0xE5) {
        if (++run == n) return &v->ents[i - n + 1];
      } else {
        run = 0;
      }
    }
    if (dir_grow(v) != 0) return 0;
  }
  return 0;
}

static int short_bad(unsigned char c) {
  if (c <= 0x20 || c >= 0x80) return 1;
  return c == '"' || c == '*' || c == '+' || c == ',' || c == '.' || c == '/' ||
         c == ':' || c == ';' || c == '<' || c == '=' || c == '>' || c == '?' ||
         c == '[' || c == '\\' || c == ']' || c == '|';
}

static int long_bad(unsigned char c) {
  return c < 0x20 || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
         c == '>' || c == '?' || c == '\\' || c == '|';
}

/* 1 se name cabe em 8.3 sem LFN: cada parte toda maiuscula ou toda
 * minuscula. *nt recebe as minusculas (NT_LOW_*), como o Windows grava. */
static int short_fits(const char* name, uint8_t* nt) {
  int up[2] = {0, 0};
  int low[2] = {0, 0};
  int part = 0;
  int n = 0;
  int i;
  *nt = 0;
  for (i = 0; name[i]; i++) {
    unsigned char c = (unsigned char)name[i];
    if (c == '.') {
      if (part || n == 0) return 0;
      part = 1;
      n = 0;
      continue;
    }
    if (short_bad(c)) return 0;
    if (++n > (part ? 3 : 8)) return 0;
    if (c >= 'a' && c <= 'z') low[part] = 1;
    if (c >= 'A' && c <= 'Z') up[part] = 1;
  }
  if (n == 0) return 0;
  if ((up[0] && low[0]) || (up[1] && low[1])) return 0;
  if (low[0]) *nt |= NT_LOW_BASE;
  if (low[1]) *nt |= NT_LOW_EXT;
  return 1;
}

static int short_taken(struct dir_view* v, const char* n11) {
  int i;
  for (i = 0; i < v->count; i++) {
    const char* p = v->ents[i].name;
    unsigned char n0 = (unsigned char)p[0];
    int j;
    if (n0 == 0x00) break;
    if (n0 == 0xE5 || (v->ents[i].attributes & 0x0F) == 0x0F) continue;
    for (j = 0; j < 11 && p[j] == n11[j]; j++) {
    }
    if (j == 11) return 1;
  }
  return 0;
}

static char up_short(unsigned char c) {
  if (short_bad(c)) return '_';
  return (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
}

/* Apelido 8.3 de um nome longo: "Relatorio final.txt" -> RELATO~1.TXT,
 * com o primeiro numero livre na pasta. */
static int make_alias(struct dir_view* v, const char* lname, char* n11) {
  char base[8];
  char ext[3];
  int bn = 0;
  int en = 0;
  int dot = -1;
  int i;
  uint32_t num;
  for (i = 0; lname[i]; i++)
    if (lname[i] == '.') dot = i;
  if (dot == 0) dot = -1;   /* ".config" fica sem extensao */
  for (i = 0; lname[i] && (dot < 0 || i < dot) && bn < 8; i++) {
    unsigned char c = (unsigned char)lname[i];
    if (c == ' ' || c == '.') continue;
    base[bn++] = up_short(c);
  }
  if (dot > 0)
    for (i = dot + 1; lname[i] && en < 3; i++) {
      unsigned char c = (unsigned char)lname[i];
      if (c == ' ') continue;
      ext[en++] = up_short(c);
    }
  if (bn == 0) base[bn++] = '_';
  for (num = 1; num < 1000000u; num++) {
    char d[7];
    int dn = 0;
    int keep;
    int k;
    uint32_t x = num;
    do {
      d[dn++] = (char)('0' + x % 10u);
      x /= 10u;
    } while (x);
    keep = 7 - dn;
    if (keep > bn) keep = bn;
    for (k = 0; k < 11; k++) n11[k] = ' ';
    for (k = 0; k < keep; k++) n11[k] = base[k];
    n11[keep] = '~';
    for (k = 0; k < dn; k++) n11[keep + 1 + k] = d[dn - 1 - k];
    for (k = 0; k < en; k++) n11[8 + k] = ext[k];
    if (!short_taken(v, n11)) return 0;
  }
  return -1;
}

/* Entrada nova para name em v: so 8.3 quando cabe, senao LFN + apelido.
 * Devolve a entrada 8.3 zerada, com nome e atributo; 0 sem lugar ou nome
 * invalido. Espacos e pontos do fim caem, como no Windows. */
static struct fat_dir_entry* dir_new_entry(struct dir_view* v, const char* name, uint8_t attr) {
  static char clean[FAT_NAME_MAX];
  char n11[11];
  uint8_t nt;
  int len = 0;
  int cnt;
  int i;
  int k;
  struct fat_dir_entry* e;
  while (name[len] && len < FAT_NAME_MAX - 1) {
    clean[len] = name[len];
    len++;
  }
  if (name[len]) return 0;
  while (len > 0 && (clean[len - 1] == ' ' || clean[len - 1] == '.')) len--;
  clean[len] = 0;
  if (len == 0) return 0;
  for (i = 0; i < len; i++)
    if (long_bad((unsigned char)clean[i])) return 0;
  if (short_fits(clean, &nt)) {
    e = dir_slots(v, 1);
    if (!e) return 0;
    mem_zero(e, 32);
    string_to_fat_name(clean, e->name, e->ext);
    e->reserved[0] = nt;
    e->attributes = attr;
    return e;
  }
  if (make_alias(v, clean, n11) != 0) return 0;
  cnt = (len + LFN_CHARS - 1) / LFN_CHARS;
  e = dir_slots(v, cnt + 1);
  if (!e) return 0;
  /* No disco o pedaco do fim vem primeiro. */
  for (k = 0; k < cnt; k++) {
    struct fat_lfn_entry* l = (struct fat_lfn_entry*)&e[k];
    int ord = cnt - k;
    int base = (ord - 1) * LFN_CHARS;
    mem_zero(l, 32);
    l->ord = (uint8_t)(ord | (k == 0 ? 0x40 : 0));
    l->attributes = 0x0F;
    l->sum = sum11((const uint8_t*)n11);
    for (i = 0; i < LFN_CHARS; i++) {
      int p = base + i;
      uint16_t u = 0xFFFF;
      if (p < len) u = oem_to_uni((unsigned char)clean[p]);
      else if (p == len) u = 0;
      lfn_set(l, i, u);
    }
  }
  e += cnt;
  mem_zero(e, 32);
  for (i = 0; i < 8; i++) e->name[i] = n11[i];
  for (i = 0; i < 3; i++) e->ext[i] = n11[8 + i];
  e->attributes = attr;
  return e;
}

static void put_dot(struct fat_dir_entry* e, int dots, uint16_t cluster) {
  int i;
  mem_zero(e, 32);
  e->name[0] = '.';
  if (dots >= 2) e->name[1] = '.';
  for (i = dots; i < 8; i++) e->name[i] = ' ';
  for (i = 0; i < 3; i++) e->ext[i] = ' ';
  e->attributes = 0x10;
  e->cluster = cluster;
}

static int format_new_dir(uint16_t self, uint16_t parent_cluster) {
  uint32_t lba = first_data_sector + ((uint32_t)(self - 2) * sectors_per_cluster);
  uint32_t s;
  struct fat_dir_entry dots[2];
  put_dot(&dots[0], 1, self);
  put_dot(&dots[1], 2, parent_cluster < 2 ? 0 : parent_cluster);
  for (s = 0; s < sectors_per_cluster; s++) {
    uint32_t i;
    uint8_t* raw = (uint8_t*)dots;
    for (i = 0; i < 512; i++) fat_bounce[i] = 0;
    if (s == 0) {
      for (i = 0; i < 64; i++) fat_bounce[i] = raw[i];
    }
    if (disk_write_sector(current_drive, lba + s, fat_bounce) != 0) return -1;
  }
  return 0;
}

static int mkdir_in(struct dir_view* parent, const char* name, uint16_t* outc) {
  struct fat_dir_entry* e;
  uint16_t c;
  if (fat_type != 16) return -1;
  c = find_free_cluster();
  if (!c) return -3;
  write_fat16_cluster(c, 0xFFFF);
  if (format_new_dir(c, parent->cluster) != 0) {
    free_chain(c);
    return -1;
  }
  /* Depois do cluster: a pasta pode crescer e pegar outro. */
  e = dir_new_entry(parent, name, 0x10);
  if (!e) {
    free_chain(c);
    flush_fat();
    return -2;
  }
  e->cluster = c;
  e->size = 0;
  e->date = 0x4A00;
  e->time = 0x8000;
  if (dir_flush(parent) != 0) return -1;
  flush_fat();
  *outc = c;
  return 0;
}

/* parent fica com o diretorio que contem o arquivo e leaf (FAT_NAME_MAX)
 * com o ultimo nome. create cria as pastas do meio. */
static int open_parent(const char* path, struct dir_view* parent, char* leaf, int create) {
  static char next[FAT_NAME_MAX];
  int pos = 0;
  int depth = 0;
  if (!path || !path[0]) return -1;
  if (next_comp(path, &pos, leaf, FAT_NAME_MAX) != 1) return -1;
  if (dir_load(0, parent) != 0) return -1;
  for (;;) {
    int rc = next_comp(path, &pos, next, FAT_NAME_MAX);
    struct fat_dir_entry* e;
    uint16_t child;
    int k;
    if (rc < 0) return -1;
    if (rc == 0) return 0;
    if (++depth > 32) return -1;
    e = dir_find(parent, leaf, 1, 0);
    if (!e) {
      if (!create) return -1;
      if (mkdir_in(parent, leaf, &child) != 0) return -1;
    } else {
      child = e->cluster;
      if (child < 2) return -1;
    }
    if (dir_load(child, parent) != 0) return -1;
    for (k = 0; next[k]; k++) leaf[k] = next[k];
    leaf[k] = 0;
  }
}

static int lookup_file(const char* filename, uint16_t* cluster, uint32_t* size) {
  struct dir_view parent;
  char leaf[FAT_NAME_MAX];
  struct fat_dir_entry* e;
  if (fat_type == 0) return -1;
  if (open_parent(filename, &parent, leaf, 0) != 0) return -1;
  e = dir_find(&parent, leaf, 0, 0);
  if (!e) return -1;
  *cluster = e->cluster;
  *size = e->size;
  return 0;
}

static int write_chain(struct fat_dir_entry* entry, const void* data, uint32_t size) {
  uint32_t cluster_bytes = bytes_per_sector * sectors_per_cluster;
  uint32_t clusters_needed;
  uint16_t first_cluster = 0;
  uint16_t prev_cluster = 0;
  const uint8_t* file_data = (const uint8_t*)data;
  uint32_t bytes_written = 0;
  uint32_t c;
  if (cluster_bytes == 0) return -1;
  clusters_needed = size ? (size + cluster_bytes - 1) / cluster_bytes : 0;
  for (c = 0; c < clusters_needed; c++) {
    uint16_t cluster = find_free_cluster();
    uint32_t lba;
    uint32_t s;
    if (cluster == 0) {
      free_chain(first_cluster);
      entry->cluster = 0;
      entry->size = 0;
      return -3;
    }
    write_fat16_cluster(cluster, 0xFFFF);
    if (first_cluster == 0) {
      first_cluster = cluster;
      entry->cluster = cluster;
    }
    if (prev_cluster != 0) write_fat16_cluster(prev_cluster, cluster);
    lba = first_data_sector + ((uint32_t)(cluster - 2) * sectors_per_cluster);
    for (s = 0; s < sectors_per_cluster; s++) {
      uint32_t left = size - bytes_written;
      uint32_t i;
      if (left > bytes_per_sector) left = bytes_per_sector;
      for (i = 0; i < left; i++) fat_bounce[i] = file_data[bytes_written + i];
      for (; i < 512; i++) fat_bounce[i] = 0;
      if (disk_write_sector(current_drive, lba + s, fat_bounce) != 0) return -1;
      bytes_written += left;
      fat_breathe();
    }
    prev_cluster = cluster;
  }
  if (prev_cluster != 0) write_fat16_cluster(prev_cluster, 0xFFFF);
  entry->size = size;
  entry->date = 0x4A00;
  entry->time = 0x8000;
  return 0;
}

// Lê arquivo completo. filename pode ser "temp\\net\\ARQ.EXT".
int fat_read_file(const char* filename, void* buffer, uint32_t max_size) {
  uint16_t cluster;
  uint32_t file_size;
  if (lookup_file(filename, &cluster, &file_size) != 0) return -1;
  if (file_size > max_size) {
    file_size = max_size;
  }
  uint8_t* buf = (uint8_t*)buffer;
  uint32_t bytes_read = 0;
  
  while (cluster >= 2 && bytes_read < file_size) {
    uint32_t lba = first_data_sector + ((cluster - 2) * sectors_per_cluster);
    uint32_t s;
    /* Setor a setor: o ultimo vai pelo bounce para nao passar de max_size. */
    for (s = 0; s < sectors_per_cluster && bytes_read < file_size; s++) {
      uint32_t left = file_size - bytes_read;
      if (left >= bytes_per_sector) {
        if (read_sector(lba + s, buf + bytes_read) != 0) return -1;
        bytes_read += bytes_per_sector;
      } else {
        uint32_t i;
        if (disk_read_sector(current_drive, lba + s, fat_bounce) != 0) return -1;
        for (i = 0; i < left; i++) buf[bytes_read + i] = fat_bounce[i];
        bytes_read += left;
      }
      fat_breathe();
    }
    
    cluster = get_next_cluster(cluster);
    if (fat_type == 12 && cluster >= 0xFF8) break;
    if (fat_type == 16 && cluster >= 0xFFF8) break;
  }
  
  return bytes_read;
}

/* Le um trecho. O chamador mostra o avanco entre as chamadas. */
int fat_read_span(const char* filename, uint32_t offset, void* buffer, uint32_t len) {
  uint16_t cluster;
  uint32_t file_size;
  uint32_t cluster_bytes;
  uint32_t skip;
  uint32_t so;
  uint8_t* buf;
  uint32_t bytes_read = 0;
  if (!filename || !buffer || len == 0) return 0;
  if (lookup_file(filename, &cluster, &file_size) != 0) return -1;
  if (offset >= file_size) return 0;
  if (offset + len > file_size) len = file_size - offset;
  cluster_bytes = bytes_per_sector * sectors_per_cluster;
  if (cluster_bytes == 0) return -1;
  skip = offset / cluster_bytes;
  so = offset % cluster_bytes;
  while (skip > 0 && cluster >= 2) {
    cluster = get_next_cluster(cluster);
    skip--;
    if (fat_type == 16 && cluster >= 0xFFF8) return -1;
    if (fat_type == 12 && cluster >= 0xFF8) return -1;
  }
  buf = (uint8_t*)buffer;
  while (cluster >= 2 && bytes_read < len) {
    uint32_t lba = first_data_sector + ((uint32_t)(cluster - 2) * sectors_per_cluster);
    uint32_t s0 = so / bytes_per_sector;
    uint32_t off = so % bytes_per_sector;
    uint32_t s;
    so = 0;
    for (s = s0; s < sectors_per_cluster && bytes_read < len; s++) {
      uint32_t left = len - bytes_read;
      uint32_t take;
      uint32_t i;
      if (disk_read_sector(current_drive, lba + s, fat_bounce) != 0) return -1;
      take = bytes_per_sector - off;
      if (take > left) take = left;
      for (i = 0; i < take; i++) buf[bytes_read + i] = fat_bounce[off + i];
      bytes_read += take;
      off = 0;
      fat_breathe();
    }
    cluster = get_next_cluster(cluster);
    if (fat_type == 16 && cluster >= 0xFFF8) break;
    if (fat_type == 12 && cluster >= 0xFF8) break;
  }
  return (int)bytes_read;
}

int fat_file_size(const char* filename) {
  uint16_t cluster;
  uint32_t size;
  if (lookup_file(filename, &cluster, &size) != 0) return -1;
  (void)cluster;
  return (int)size;
}

static int entry_dot(const struct fat_dir_entry* e) {
  if ((unsigned char)e->name[0] != '.') return 0;
  return e->name[1] == ' ' || e->name[1] == '.';
}

static int dir_at(const char* path, struct dir_view* v) {
  struct dir_view parent;
  char leaf[FAT_NAME_MAX];
  struct fat_dir_entry* e;
  if (!path || !path[0]) return dir_load(0, v);
  if (open_parent(path, &parent, leaf, 0) != 0) return -1;
  e = dir_find(&parent, leaf, 1, 0);
  if (!e || e->cluster < 2) return -1;
  return dir_load(e->cluster, v);
}

/* 1 se path e uma pasta. Vazio e a raiz. */
int fat_is_dir(const char* path) {
  struct dir_view v;
  if (fat_type == 0) return 0;
  if (!path || !path[0]) return 1;
  return dir_at(path, &v) == 0;
}

/* 0 criou, 1 ja existia, -1 falhou. Cria as pastas do meio. */
int fat_mkdir(const char* path) {
  struct dir_view parent;
  char leaf[FAT_NAME_MAX];
  uint16_t c;
  if (fat_type != 16 || !path || !path[0]) return -1;
  if (open_parent(path, &parent, leaf, 1) != 0) return -1;
  if (dir_find(&parent, leaf, 1, 0)) return 1;
  if (dir_find(&parent, leaf, 0, 0)) return -1;
  if (mkdir_in(&parent, leaf, &c) != 0) return -1;
  return 0;
}

/* 0 removeu. -1 nao existe. -2 nao esta vazia. -3 e arquivo. */
int fat_rmdir(const char* path) {
  struct dir_view parent;
  struct dir_view child;
  char leaf[FAT_NAME_MAX];
  struct fat_dir_entry* e;
  uint16_t cluster;
  uint16_t parent_cluster;
  int first;
  int i;
  if (fat_type != 16 || !path || !path[0]) return -1;
  if (open_parent(path, &parent, leaf, 0) != 0) return -1;
  if (leaf[0] == '.' && (leaf[1] == 0 || (leaf[1] == '.' && leaf[2] == 0))) return -1;
  e = dir_find(&parent, leaf, 1, 0);
  if (!e) return dir_find(&parent, leaf, 0, 0) ? -3 : -1;
  cluster = e->cluster;
  parent_cluster = parent.cluster;
  if (cluster < 2) return -1;
  if (dir_load(cluster, &child) != 0) return -1;
  for (i = 0; i < child.count; i++) {
    unsigned char n0 = (unsigned char)child.ents[i].name[0];
    unsigned char attr;
    if (n0 == 0x00) break;
    if (n0 == 0xE5) continue;
    attr = child.ents[i].attributes;
    if ((attr & 0x0F) == 0x0F) continue;
    if (attr & 0x08) continue;
    if (entry_dot(&child.ents[i])) continue;
    return -2;
  }
  if (dir_load(parent_cluster, &parent) != 0) return -1;
  e = dir_find(&parent, leaf, 1, &first);
  if (!e) return -1;
  dir_unlink(&parent, first, e);
  if (dir_flush(&parent) != 0) return -1;
  free_chain(cluster);
  flush_fat();
  return 0;
}

/* 0 apagou. -1 nao existe. -3 e pasta. -4 somente leitura.
 * Marca tambem as entradas de nome longo (LFN) que vem antes. */
int fat_delete(const char* path) {
  struct dir_view parent;
  char leaf[FAT_NAME_MAX];
  struct fat_dir_entry* e;
  uint16_t cluster;
  int first;
  if (fat_type != 16 || !path || !path[0]) return -1;
  if (open_parent(path, &parent, leaf, 0) != 0) return -1;
  e = dir_find(&parent, leaf, 0, &first);
  if (!e) return dir_find(&parent, leaf, 1, 0) ? -3 : -1;
  if (e->attributes & 0x01) return -4;
  cluster = e->cluster;
  dir_unlink(&parent, first, e);
  if (dir_flush(&parent) != 0) return -1;
  if (cluster >= 2) free_chain(cluster);
  flush_fat();
  return 0;
}

/* Linhas "NOME" ou "NOME\\" se for pasta. path vazio lista a raiz. -1 se nao e pasta.
 * Nome longo de ate 31 caracteres; mais que isso vem o 8.3 (readdir e k_dirent). */
int fat_list_dir(const char* path, char* output, int max_output) {
  static char lname[FAT_NAME_MAX];
  struct dir_view v;
  struct fat_dir_entry* e;
  int pos = 0;
  int i = 0;
  if (!output || max_output < 2) return -1;
  output[0] = 0;
  if (fat_type == 0) return -1;
  if (dir_at(path, &v) != 0) return -1;
  while (pos < max_output - 36 && (e = dir_next(&v, &i, lname, 0)) != 0) {
    char name[32];
    int j;
    if (entry_dot(e)) continue;
    item_name(e, lname, name);
    if (!name[0]) continue;
    for (j = 0; name[j] && pos < max_output - 3; j++) output[pos++] = name[j];
    if (e->attributes & 0x10) output[pos++] = '\\';
    output[pos++] = '\n';
  }
  output[pos] = 0;
  return pos;
}

/* Como fat_list_dir, mas com tamanho e data. Quantas copiou; -1 se nao e pasta. */
int fat_dir_items(const char* path, struct dir_item* out, int max) {
  static char lname[FAT_NAME_MAX];
  struct dir_view v;
  struct fat_dir_entry* e;
  int n = 0;
  int i = 0;
  if (!out || max <= 0 || fat_type == 0) return -1;
  if (dir_at(path, &v) != 0) return -1;
  while (n < max && (e = dir_next(&v, &i, lname, 0)) != 0) {
    if (entry_dot(e)) continue;
    item_name(e, lname, out[n].name);
    if (!out[n].name[0]) continue;
    out[n].dir = (e->attributes & 0x10) ? 1 : 0;
    out[n].size = out[n].dir ? 0 : e->size;
    out[n].date = e->date;
    out[n].time = e->time;
    n++;
  }
  return n;
}

/* Nome longo inteiro do ultimo nome de path (arquivo ou pasta). */
int fat_long_name(const char* path, char* out, int max) {
  struct dir_view parent;
  char leaf[FAT_NAME_MAX];
  int i;
  if (fat_type == 0 || !out || max < 2) return -1;
  if (open_parent(path, &parent, leaf, 0) != 0) return -1;
  if (!dir_find(&parent, leaf, 0, 0) && !dir_find(&parent, leaf, 1, 0)) return -1;
  for (i = 0; found_name[i] && i < max - 1; i++) out[i] = found_name[i];
  out[i] = 0;
  return 0;
}

// Lista arquivos do diretório raiz
int fat_list_files(char* output, int max_output) {
  int pos = 0;
  if (fat_type == 0 || max_output < 2) {
    if (max_output > 0) output[0] = '\0';
    return 0;
  }
  
  for (int i = 0; i < boot_sector->root_entries && pos < max_output - 32; i++) {
    if (root_dir[i].name[0] == 0x00) break;
    if (root_dir[i].name[0] == 0xE5) continue;
    
    // Verifica se é arquivo normal
    if ((root_dir[i].attributes & 0x18) == 0) {
      char name[13];
      fat_name_to_string(&root_dir[i], name);
      
      // Copia nome
      int j = 0;
      while (name[j] != '\0' && pos < max_output - 1) {
        output[pos++] = name[j++];
      }
      output[pos++] = '\n';
    }
  }
  
  output[pos] = '\0';
  return pos;
}

// Escreve cluster no FAT16
static void write_fat16_cluster(uint16_t cluster, uint16_t value) {
  uint16_t* fat16 = (uint16_t*)fat_table;
  fat16[cluster] = value;
  fat_mark((uint32_t)cluster * 2u);
}

// Encontra cluster livre
static uint16_t find_free_cluster(void) {
  if (fat_type == 16) {
    uint16_t* fat16 = (uint16_t*)fat_table;
    uint32_t n = cluster_count() + 2u;
    uint32_t i;
    if (n > 0xFFF0u) n = 0xFFF0u;
    for (i = 2; i < n; i++) {
      if (fat16[i] == 0) {
        return (uint16_t)i;
      }
    }
  }
  return 0; // Sem clusters livres
}

static void free_chain(uint16_t cluster) {
  uint16_t* fat16 = (uint16_t*)fat_table;
  int guard = 0;
  while (cluster >= 2 && cluster < 0xFFF0u && guard++ < 0x10000) {
    uint16_t next = fat16[cluster];
    fat16[cluster] = 0;
    fat_mark((uint32_t)cluster * 2u);
    cluster = next;
  }
}

// Escreve de volta no disco os setores mudados da FAT, em todas as copias
static void flush_fat(void) {
  uint8_t* fat_buf = (uint8_t*)fat_table;
  uint32_t n = boot_sector->sectors_per_fat_16;
  for (uint32_t i = 0; i < n; i++) {
    if (!(fat_dirty[i >> 3] & (1u << (i & 7u)))) continue;
    for (uint32_t f = 0; f < boot_sector->fat_count; f++)
      write_sector(first_fat_sector + f * n + i, fat_buf + (i * bytes_per_sector));
  }
  mem_zero(fat_dirty, sizeof(fat_dirty));
}

// Escreve diretório raiz de volta no disco
static void flush_root_dir(void) {
  uint8_t* root_buf = (uint8_t*)root_dir;
  for (uint32_t i = 0; i < root_dir_sectors; i++) {
    write_sector(root_dir_sector + i, root_buf + (i * bytes_per_sector));
  }
}

/* Grava na raiz ou em pastas ("temp\\net\\ARQ.EXT" cria temp e net). */
int fat_write_file(const char* filename, const void* data, uint32_t size) {
  struct dir_view parent;
  char leaf[FAT_NAME_MAX];
  struct fat_dir_entry* entry;
  int rc;
  if (fat_type != 16) return -1;
  if (sectors_per_cluster == 0 || bytes_per_sector == 0) return -1;
  if (!filename || (!data && size)) return -1;
  if (open_parent(filename, &parent, leaf, 1) != 0) return -1;
  entry = dir_find(&parent, leaf, 0, 0);
  if (!entry) {
    if (dir_find(&parent, leaf, 1, 0)) return -1;   /* ja e uma pasta */
    entry = dir_new_entry(&parent, leaf, 0x20);
    if (!entry) return -2;
  } else {
    free_chain(entry->cluster);
    entry->cluster = 0;
    entry->size = 0;
  }
  rc = write_chain(entry, data ? data : (const void*)"", size);
  if (dir_flush(&parent) != 0 && rc == 0) rc = -1;
  flush_fat();
  return rc;
}

// Obtém informações de espaço do sistema de arquivos
int fat_get_space_info(uint32_t* total_bytes, uint32_t* free_bytes) {
  if (fat_type == 0) {
    return -1; // Sistema não montado
  }
  if (sectors_per_cluster == 0 || bytes_per_sector == 0) return -1; /* evita div/0 */

  // Calcula total de clusters de dados
  uint32_t total_clusters = 0;
  uint32_t free_clusters = 0;
  
  if (fat_type == 16) {
    uint16_t* fat16 = (uint16_t*)fat_table;
    
    // Conta clusters totais (começa do cluster 2)
    // Precisa saber quantos clusters existem
    // Vamos usar uma estimativa baseada no tamanho da FAT
    uint32_t fat_size_sectors = boot_sector->sectors_per_fat_16;
    uint32_t fat_size_bytes = fat_size_sectors * bytes_per_sector;
    
    // FAT16: cada entrada é 2 bytes
    // Total de clusters = (tamanho FAT em bytes) / 2
    total_clusters = fat_size_bytes / 2;
    
    // Conta clusters livres (valor 0)
    for (uint32_t i = 2; i < total_clusters && i < 0xFFF8; i++) {
      if (fat16[i] == 0) {
        free_clusters++;
      }
    }
  } else if (fat_type == 12) {
    // FAT12 é mais complexo, por enquanto retorna erro
    return -2;
  }
  
  // Converte para bytes
  *total_bytes = total_clusters * sectors_per_cluster * bytes_per_sector;
  *free_bytes = free_clusters * sectors_per_cluster * bytes_per_sector;
  
  return 0;
}
