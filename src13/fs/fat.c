// fat.c - Sistema de arquivos FAT12/FAT16
#include <stdint.h>
#include "disk.h"
#include "sched.h"

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

/* Em 10 MB (0x100000-0x9FFFFF e a tela em memoria do visual.c): a FAT16 de um
 * disco de 100 MB passa de 100 KB. FAT ate 256 KB, raiz ate 64 KB.
 * O disco so le/escreve abaixo de 1 MB, por isso tudo passa por fat_bounce. */
#define FAT_TABLE_MAX 0x40000u
#define ROOT_DIR_MAX  0x10000u
static struct fat_boot_sector* boot_sector = (struct fat_boot_sector*)0xA00000;
static uint8_t* fat_table = (uint8_t*)0xA01000;
static struct fat_dir_entry* root_dir = (struct fat_dir_entry*)0xA41000;
/* Diretorio aberto (\temp\net\ e outros). 32 KB, termina em 0xA59000,
 * antes do kmalloc em 0xA80000. Nao pode ir para o BSS do kernel. */
#define SUBDIR_MAX 0x8000u
static struct fat_dir_entry* sub_dir = (struct fat_dir_entry*)0xA51000;

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

// Converte nome 8.3 para string normal
static void fat_name_to_string(const struct fat_dir_entry* entry, char* output) {
  int i = 0;
  // Nome
  for (int j = 0; j < 8; j++) {
    if (entry->name[j] != ' ') {
      output[i++] = entry->name[j];
    }
  }
  // Extensão
  if (entry->ext[0] != ' ') {
    output[i++] = '.';
    for (int j = 0; j < 3; j++) {
      if (entry->ext[j] != ' ') {
        output[i++] = entry->ext[j];
      }
    }
  }
  output[i] = '\0';
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

static int next_comp(const char* path, int* pos, char* out, int max) {
  int i = 0;
  if (!path || max < 2) return 0;
  while (path[*pos] == '\\' || path[*pos] == '/') (*pos)++;
  if (!path[*pos]) return 0;
  while (path[*pos] && path[*pos] != '\\' && path[*pos] != '/') {
    if (i < max - 1) out[i++] = path[*pos];
    (*pos)++;
  }
  out[i] = 0;
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

static struct fat_dir_entry* dir_find(struct dir_view* v, const char* filename, int want_dir) {
  char search_name[8];
  char search_ext[3];
  int i;
  string_to_fat_name(filename, search_name, search_ext);
  for (i = 0; i < v->count; i++) {
    unsigned char n0 = (unsigned char)v->ents[i].name[0];
    unsigned char attr;
    int match = 1;
    int j;
    if (n0 == 0x00) break;
    if (n0 == 0xE5) continue;
    attr = v->ents[i].attributes;
    if ((attr & 0x0F) == 0x0F) continue;
    if (attr & 0x08) continue;
    if (want_dir) {
      if ((attr & 0x10) == 0) continue;
    } else if (attr & 0x10) {
      continue;
    }
    for (j = 0; j < 8; j++) {
      if (v->ents[i].name[j] != search_name[j]) {
        match = 0;
        break;
      }
    }
    if (!match) continue;
    for (j = 0; j < 3; j++) {
      if (v->ents[i].ext[j] != search_ext[j]) {
        match = 0;
        break;
      }
    }
    if (match) return &v->ents[i];
  }
  return 0;
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

static struct fat_dir_entry* dir_slot(struct dir_view* v) {
  int i;
  for (i = 0; i < v->count; i++) {
    unsigned char n0 = (unsigned char)v->ents[i].name[0];
    if (n0 == 0x00 || n0 == 0xE5) return &v->ents[i];
  }
  if (dir_grow(v) != 0) return 0;
  for (i = 0; i < v->count; i++) {
    unsigned char n0 = (unsigned char)v->ents[i].name[0];
    if (n0 == 0x00 || n0 == 0xE5) return &v->ents[i];
  }
  return 0;
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
  uint16_t parent_cluster;
  if (fat_type != 16) return -1;
  e = dir_slot(parent);
  if (!e) return -2;
  c = find_free_cluster();
  if (!c) return -3;
  write_fat16_cluster(c, 0xFFFF);
  parent_cluster = parent->cluster;
  if (format_new_dir(c, parent_cluster) != 0) {
    free_chain(c);
    return -1;
  }
  mem_zero(e, 32);
  string_to_fat_name(name, e->name, e->ext);
  e->attributes = 0x10;
  e->cluster = c;
  e->size = 0;
  e->date = 0x4A00;
  e->time = 0x8000;
  if (dir_flush(parent) != 0) return -1;
  flush_fat();
  *outc = c;
  return 0;
}

/* parent fica com o diretorio que contem o arquivo. create cria as pastas. */
static int open_parent(const char* path, struct dir_view* parent, char* leaf, int create) {
  char comps[8][13];
  int n = 0;
  int pos = 0;
  int i;
  uint16_t cluster = 0;
  char extra[4];
  int p2;
  if (!path || !path[0]) return -1;
  while (n < 8 && next_comp(path, &pos, comps[n], 13)) n++;
  p2 = pos;
  if (n < 1 || next_comp(path, &p2, extra, 4)) return -1;
  for (i = 0; comps[n - 1][i] && i < 12; i++) leaf[i] = comps[n - 1][i];
  leaf[i] = 0;
  if (dir_load(0, parent) != 0) return -1;
  for (i = 0; i < n - 1; i++) {
    struct fat_dir_entry* e = dir_find(parent, comps[i], 1);
    uint16_t child;
    if (!e) {
      if (!create) return -1;
      if (mkdir_in(parent, comps[i], &child) != 0) return -1;
    } else {
      child = e->cluster;
      if (child < 2) return -1;
    }
    cluster = child;
    if (dir_load(cluster, parent) != 0) return -1;
  }
  (void)cluster;
  return 0;
}

static int lookup_file(const char* filename, uint16_t* cluster, uint32_t* size) {
  struct dir_view parent;
  char leaf[13];
  struct fat_dir_entry* e;
  if (fat_type == 0) return -1;
  if (open_parent(filename, &parent, leaf, 0) != 0) return -1;
  e = dir_find(&parent, leaf, 0);
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
  char leaf[13];
  struct fat_dir_entry* e;
  if (!path || !path[0]) return dir_load(0, v);
  if (open_parent(path, &parent, leaf, 0) != 0) return -1;
  e = dir_find(&parent, leaf, 1);
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
  char leaf[13];
  uint16_t c;
  if (fat_type != 16 || !path || !path[0]) return -1;
  if (open_parent(path, &parent, leaf, 1) != 0) return -1;
  if (dir_find(&parent, leaf, 1)) return 1;
  if (dir_find(&parent, leaf, 0)) return -1;
  if (mkdir_in(&parent, leaf, &c) != 0) return -1;
  return 0;
}

/* 0 removeu. -1 nao existe. -2 nao esta vazia. -3 e arquivo. */
int fat_rmdir(const char* path) {
  struct dir_view parent;
  struct dir_view child;
  char leaf[13];
  struct fat_dir_entry* e;
  uint16_t cluster;
  uint16_t parent_cluster;
  int i;
  if (fat_type != 16 || !path || !path[0]) return -1;
  if (open_parent(path, &parent, leaf, 0) != 0) return -1;
  if (leaf[0] == '.' && (leaf[1] == 0 || (leaf[1] == '.' && leaf[2] == 0))) return -1;
  e = dir_find(&parent, leaf, 1);
  if (!e) return dir_find(&parent, leaf, 0) ? -3 : -1;
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
  e = dir_find(&parent, leaf, 1);
  if (!e) return -1;
  e->name[0] = (char)0xE5;
  if (dir_flush(&parent) != 0) return -1;
  free_chain(cluster);
  flush_fat();
  return 0;
}

/* Linhas "NOME" ou "NOME\\" se for pasta. path vazio lista a raiz. -1 se nao e pasta. */
int fat_list_dir(const char* path, char* output, int max_output) {
  struct dir_view v;
  int pos = 0;
  int i;
  if (!output || max_output < 2) return -1;
  output[0] = 0;
  if (fat_type == 0) return -1;
  if (dir_at(path, &v) != 0) return -1;
  for (i = 0; i < v.count && pos < max_output - 16; i++) {
    unsigned char n0 = (unsigned char)v.ents[i].name[0];
    unsigned char attr;
    char name[13];
    int j;
    if (n0 == 0x00) break;
    if (n0 == 0xE5) continue;
    attr = v.ents[i].attributes;
    if ((attr & 0x0F) == 0x0F) continue;
    if (attr & 0x08) continue;
    if (entry_dot(&v.ents[i])) continue;
    fat_name_to_string(&v.ents[i], name);
    if (!name[0]) continue;
    for (j = 0; name[j] && pos < max_output - 3; j++) output[pos++] = name[j];
    if (attr & 0x10) output[pos++] = '\\';
    output[pos++] = '\n';
  }
  output[pos] = 0;
  return pos;
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
    cluster = next;
  }
}

// Escreve FAT de volta no disco
static void flush_fat(void) {
  uint8_t* fat_buf = (uint8_t*)fat_table;
  // Escreve primeira FAT
  for (uint32_t i = 0; i < boot_sector->sectors_per_fat_16; i++) {
    write_sector(first_fat_sector + i, fat_buf + (i * bytes_per_sector));
  }
  // Escreve segunda FAT também (cópia)
  uint32_t second_fat = first_fat_sector + boot_sector->sectors_per_fat_16;
  for (uint32_t i = 0; i < boot_sector->sectors_per_fat_16; i++) {
    write_sector(second_fat + i, fat_buf + (i * bytes_per_sector));
  }
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
  char leaf[13];
  struct fat_dir_entry* entry;
  int rc;
  if (fat_type != 16) return -1;
  if (sectors_per_cluster == 0 || bytes_per_sector == 0) return -1;
  if (!filename || (!data && size)) return -1;
  if (open_parent(filename, &parent, leaf, 1) != 0) return -1;
  entry = dir_find(&parent, leaf, 0);
  if (!entry) {
    entry = dir_slot(&parent);
    if (!entry) return -2;
    mem_zero(entry, 32);
    string_to_fat_name(leaf, entry->name, entry->ext);
    entry->attributes = 0x20;
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
