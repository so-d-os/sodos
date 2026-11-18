/* ntfsput.c - (host, teste) Copia um arquivo para dentro de uma imagem NTFS,
 * criando as pastas do caminho. Usa a libntfs-3g sem montar (sem root):
 *   ntfsput IMAGEM 'Users\Teste\AppData\Local\wsl\{guid}\ext4.vhdx' arquivo
 * Compilado por wsl/scripts/make-testdisk.sh. */
#define HAVE_STDINT_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_ERRNO_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_LIMITS_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDIO_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_UNISTD_H 1
#include <time.h>
#include <ntfs-3g/types.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/unistr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static ntfs_inode* child(ntfs_volume* vol, ntfs_inode* dir, const char* name, int mkdir_) {
  ntfschar* u = NULL;
  int ul = ntfs_mbstoucs(name, &u);
  u64 ref;
  ntfs_inode* ni;
  if (ul < 0) return NULL;
  ref = ntfs_inode_lookup_by_name(dir, u, ul);
  if (ref != (u64)-1) {
    free(u);
    return ntfs_inode_open(vol, MREF(ref));
  }
  ni = ntfs_create(dir, const_cpu_to_le32(0), u, (u8)ul, mkdir_ ? S_IFDIR : S_IFREG);
  free(u);
  return ni;
}

int main(int argc, char** argv) {
  ntfs_volume* vol;
  ntfs_inode *dir, *ni;
  ntfs_attr* na;
  char path[4096], *p, *s;
  FILE* f;
  static char buf[1 << 20];
  s64 off = 0;
  size_t n;
  if (argc != 4) {
    fprintf(stderr, "uso: ntfsput IMAGEM CAMINHO\\NO\\NTFS ARQUIVO\n");
    return 2;
  }
  vol = ntfs_mount(argv[1], 0);
  if (!vol) {
    perror("ntfs_mount");
    return 1;
  }
  snprintf(path, sizeof path, "%s", argv[2]);
  for (p = path; *p; p++)
    if (*p == '/') *p = '\\';
  dir = ntfs_inode_open(vol, FILE_root);
  s = path;
  for (;;) {
    char* sep = strchr(s, '\\');
    if (!sep) break;
    *sep = 0;
    if (*s) {
      ni = child(vol, dir, s, 1);
      if (!ni) {
        perror(s);
        return 1;
      }
      ntfs_inode_close(dir);
      dir = ni;
    }
    s = sep + 1;
  }
  ni = child(vol, dir, s, 0);
  if (!ni) {
    perror(s);
    return 1;
  }
  na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
  f = fopen(argv[3], "rb");
  if (!na || !f) {
    perror("abrir");
    return 1;
  }
  ntfs_attr_truncate(na, 0);
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
    if (ntfs_attr_pwrite(na, off, (s64)n, buf) != (s64)n) {
      perror("pwrite");
      return 1;
    }
    off += (s64)n;
  }
  fclose(f);
  ntfs_attr_close(na);
  ntfs_inode_close(ni);
  ntfs_inode_close(dir);
  if (ntfs_umount(vol, FALSE)) {
    perror("umount");
    return 1;
  }
  return 0;
}
