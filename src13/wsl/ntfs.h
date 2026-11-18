/* ntfs.h - Leitor NTFS so leitura (para achar o ext4.vhdx e o kernel do WSL
 * na particao do Windows). Sem compressao nem arquivos WOF; com runlists
 * esparsas e $ATTRIBUTE_LIST (arquivos muito fragmentados, como o vhdx). */
#ifndef WSL_NTFS_H
#define WSL_NTFS_H
#include "wslio.h"

typedef struct ntfs_vol ntfs_vol;
typedef struct ntfs_file ntfs_file;

/* 1 se o setor 0 da particao e NTFS. */
int ntfs_probe(const uint8_t* boot512);
ntfs_vol* ntfs_mount(const wsl_io* part);
void ntfs_umount(ntfs_vol* v);
/* Rotulo do volume em UTF-8 ("" se nao tem). */
const char* ntfs_label(ntfs_vol* v);

/* Caminho com '\' ou '/', sem letra; caixa ignorada. "" = raiz. */
ntfs_file* ntfs_open(ntfs_vol* v, const char* path);
void ntfs_close(ntfs_file* f);
int ntfs_isdir(const ntfs_file* f);
uint64_t ntfs_fsize(const ntfs_file* f);
int ntfs_pread(ntfs_file* f, uint64_t off, void* buf, uint32_t len);
/* wsl_io que le o arquivo (vale enquanto f estiver aberto). */
void ntfs_file_io(ntfs_file* f, wsl_io* out);

typedef int (*ntfs_dir_cb)(void* arg, const char* name, int isdir, uint64_t size);
/* Chama cb para cada item (sem nomes DOS 8.3); cb != 0 para. <0 erro. */
int ntfs_list(ntfs_vol* v, const char* path, ntfs_dir_cb cb, void* arg);

#endif
