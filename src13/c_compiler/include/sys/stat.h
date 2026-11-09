#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>

#define S_IFMT  0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000
#define S_IFCHR 0020000
#define S_IFBLK 0060000
#define S_IFIFO 0010000
#define S_IFSOCK 0140000
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXU 0700

#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

#if __SIZEOF_POINTER__ == 8
#include <time.h>
#endif
struct stat {
  mode_t st_mode;
  off_t st_size;
  time_t st_mtime;
  dev_t st_dev;
  ino_t st_ino;
  /* Campos extras usados pelo NSPR (port do Firefox). Ficam no fim para
   * nao mudar o layout que o resto do SO ja usa. */
  time_t st_atime;
  time_t st_ctime;
#if __SIZEOF_POINTER__ == 8
  /* So em LP64 (estatico): nao muda o layout das DLLs x32. Zerados por stat. */
  unsigned long st_nlink;
  unsigned int st_uid;
  unsigned int st_gid;
  long st_blksize;
  long st_blocks;
  struct timespec st_atim;
  struct timespec st_mtim;
  struct timespec st_ctim;
  dev_t st_rdev; /* sem dispositivos de caractere/bloco: sempre 0 */
#endif
};

#if __SIZEOF_POINTER__ == 8 && !defined(SODOS_NO_STAT64_ALIAS)
/* LP64: stat e stat64 sao o mesmo tipo para quem compila contra estes headers. */
#define stat64 stat
#define lstat64 lstat
#define fstat64 fstat
#endif
int stat(const char* path, struct stat* st);
int fstat(int fd, struct stat* st);
int mkdir(const char* path, mode_t mode);
int lstat(const char* path, struct stat* st);
int chmod(const char* path, mode_t mode);
int fchmod(int fd, mode_t mode);
int mkfifo(const char* path, mode_t mode);
mode_t umask(mode_t mask);

#define S_IRWXG 070
#define S_IRGRP 040
#define S_IWGRP 020
#define S_IXGRP 010
#define S_IRWXO 07
#define S_IROTH 04
#define S_IWOTH 02
#define S_IXOTH 01
#define S_IFLNK 0120000
#define S_ISLNK(m) 0
#define S_ISCHR(m) 0
#define S_ISBLK(m) 0
#define S_ISSOCK(m) 0
#define S_ISFIFO(m) 0


#ifdef __cplusplus
}
#endif
#endif
