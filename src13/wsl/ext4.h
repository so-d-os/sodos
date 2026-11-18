/* ext4.h - Leitor ext2/3/4 so leitura (extents, 64bit, flex_bg, htree lido
 * como pasta linear, dados inline, links simbolicos). Para olhar o disco do
 * Ubuntu do WSL sem subir a VM: WSL LS / WSL CAT / WSL CP. */
#ifndef WSL_EXT4_H
#define WSL_EXT4_H
#include "wslio.h"

typedef struct ext4_fs ext4_fs;

typedef struct ext4_inode {
  uint32_t ino;
  uint16_t mode;  /* S_IFMT | permissoes */
  uint32_t uid, gid;
  uint64_t size;
  uint32_t mtime;
  uint32_t flags;
  uint16_t raw_len;
  uint8_t raw[512]; /* inode cru (i_block em raw+0x28) */
} ext4_inode;

#define EXT4_S_IFMT 0xF000
#define EXT4_S_IFDIR 0x4000
#define EXT4_S_IFREG 0x8000
#define EXT4_S_IFLNK 0xA000

ext4_fs* ext4_mount(const wsl_io* dev);
void ext4_umount(ext4_fs* fs);
const char* ext4_label(ext4_fs* fs);
uint64_t ext4_bytes(ext4_fs* fs); /* tamanho do sistema de arquivos */

int ext4_iget(ext4_fs* fs, uint32_t ino, ext4_inode* out);
/* Caminho absoluto Unix; segue links simbolicos (no fim tambem se follow). */
int ext4_lookup(ext4_fs* fs, const char* path, int follow, ext4_inode* out);
int ext4_pread(ext4_fs* fs, const ext4_inode* in, uint64_t off, void* buf, uint32_t len);
/* Alvo de um link simbolico. */
int ext4_readlink(ext4_fs* fs, const ext4_inode* in, char* out, int max);

typedef int (*ext4_dir_cb)(void* arg, const char* name, uint32_t ino, int type);
int ext4_readdir(ext4_fs* fs, const ext4_inode* dir, ext4_dir_cb cb, void* arg);

#endif
