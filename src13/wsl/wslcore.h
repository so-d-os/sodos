/* wslcore.h - Acha o WSL num disco do Windows: particoes NTFS (GPT/MBR),
 * distros (ext4.vhdx) e o kernel do WSL. Igual no SODOS e no host de teste. */
#ifndef WSLCORE_H
#define WSLCORE_H
#include "wslio.h"
#include "ntfs.h"
#include "vhdx.h"
#include "ext4.h"

/* Disco cru com setores de 512 bytes: le qualquer faixa de bytes. */
typedef int (*sector_read_fn)(void* ctx, uint64_t lba, uint32_t count, void* buf);
void wsl_disk_io(wsl_io* out, sector_read_fn fn, void* ctx, uint64_t sectors);

/* Faixa [off, off+size) de outro wsl_io. */
typedef struct {
  wsl_io base;
  uint64_t off;
} wsl_sub;
void wsl_sub_io(wsl_sub* s, const wsl_io* base, uint64_t off, uint64_t size, wsl_io* out);

#define WSL_MAX_PARTS 16
typedef struct {
  uint64_t off, size; /* bytes no disco */
  int index;          /* 1.. */
} wsl_part;
/* Particoes NTFS do disco. Quantas (<= max). */
int wsl_ntfs_parts(const wsl_io* disk, wsl_part* out, int max);

#define WSL_MAX_DISTROS 16
typedef struct {
  char name[64];   /* nome da pasta (guid ou pacote) */
  char user[64];
  char path[400];  /* caminho NTFS do ext4.vhdx */
  uint64_t size;
} wsl_distro;
int wsl_find_distros(ntfs_vol* v, wsl_distro* out, int max);
/* Caminho NTFS do kernel do WSL, ou 0. */
const char* wsl_find_kernel(ntfs_vol* v);

/* Pilha aberta: disco -> NTFS -> vhdx -> ext4 (ext4 opcional). */
typedef struct {
  wsl_sub part_sub;
  wsl_io part;
  ntfs_vol* ntfs;
  ntfs_file* file;
  wsl_io file_io;
  vhdx* vd;
  wsl_io disk; /* disco virtual (ext4 inteiro, sem tabela de particoes) */
  ext4_fs* fs;
} wsl_stack;

/* Abre a primeira distro achada em qualquer particao NTFS (ou a de nome
 * which, se != 0). 0 ok. */
int wsl_open_stack(const wsl_io* disk, const char* which, wsl_stack* st, wsl_distro* info);
void wsl_close_stack(wsl_stack* st);

#endif
