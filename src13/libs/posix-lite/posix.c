/* posix-lite - subset POSIX sobre k_* (Fase 1 do port MC).
 * Compila como POSIX.DLL pelo cc do SODOS. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <so.h>

#define FD_MAX 16
#define DIR_MAX 4
#define PATH_MAX 96
#define LIST_MAX 4096
#define FILE_CAP (256 * 1024)

struct fd_ent {
  int used;
  int flags;
  int pos;
  int size;
  int dirty;
  char path[PATH_MAX];
  char* data;
};

struct __so_DIR {
  int used;
  int pos;
  char list[LIST_MAX];
  struct dirent cur;
};

static struct fd_ent fds[FD_MAX];
static struct __so_DIR dirs[DIR_MAX];

static int is_abs_dos(const char* p) {
  return p && p[0] && p[1] == ':' && (p[2] == '\\' || p[2] == '/' || p[2] == 0);
}

static void norm_sep(char* s) {
  for (; *s; s++)
    if (*s == '/') *s = '\\';
}

static void join_cwd(const char* path, char* out, int max) {
  char cwd[64];
  int i = 0, j = 0;
  if (!path) path = "";
  if (is_abs_dos(path) || path[0] == '\\' || path[0] == '/') {
    while (path[i] && j < max - 1) out[j++] = path[i++];
    out[j] = 0;
    norm_sep(out);
    return;
  }
  k_pwd(cwd, (int)sizeof(cwd));
  while (cwd[i] && j < max - 1) out[j++] = cwd[i++];
  if (j > 0 && out[j - 1] != '\\' && j < max - 1) out[j++] = '\\';
  i = 0;
  while (path[i] && j < max - 1) out[j++] = path[i++];
  out[j] = 0;
  norm_sep(out);
}

static int path_is_dir(const char* path) {
  char list[8];
  int n;
  if (!path || !path[0]) return 0;
  n = (int)strlen(path);
  if (n >= 2 && path[n - 1] == '\\') return 1;
  if (is_abs_dos(path) && path[2] == 0) return 1;
  n = k_dirlist(path, list, (int)sizeof(list));
  return n >= 0;
}

__export int chdir(const char* path) {
  char full[PATH_MAX];
  if (!path) {
    errno = EINVAL;
    return -1;
  }
  join_cwd(path, full, PATH_MAX);
  if (k_chdir(full) != 0) {
    errno = ENOENT;
    return -1;
  }
  errno = 0;
  return 0;
}

__export char* getcwd(char* buf, size_t size) {
  char tmp[64];
  if (!buf || size < 2) {
    errno = EINVAL;
    return 0;
  }
  k_pwd(tmp, (int)sizeof(tmp));
  if (strlen(tmp) >= size) {
    errno = ERANGE;
    return 0;
  }
  strcpy(buf, tmp);
  return buf;
}

__export int isatty(int fd) {
  return fd == 0 || fd == 1 || fd == 2;
}

__export int unlink(const char* path) {
  (void)path;
  errno = ENOSYS;
  return -1;
}

__export int rmdir(const char* path) {
  (void)path;
  errno = ENOSYS;
  return -1;
}

__export int mkdir(const char* path, mode_t mode) {
  (void)path;
  (void)mode;
  errno = ENOSYS;
  return -1;
}

static int fd_alloc(void) {
  int i;
  for (i = 3; i < FD_MAX; i++)
    if (!fds[i].used) return i;
  return -1;
}

__export int open(const char* path, int flags, ...) {
  char full[PATH_MAX];
  int fd, sz, mode = 0;
  struct fd_ent* e;
  if (!path) {
    errno = EINVAL;
    return -1;
  }
  if (flags & O_CREAT) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int);
    va_end(ap);
    (void)mode;
  }
  join_cwd(path, full, PATH_MAX);
  if (path_is_dir(full)) {
    errno = EISDIR;
    return -1;
  }
  sz = k_fsize(full);
  if (sz < 0) {
    if (!(flags & O_CREAT)) {
      errno = ENOENT;
      return -1;
    }
    if (k_fsave(full, "", 0) < 0) {
      errno = EIO;
      return -1;
    }
    sz = 0;
  }
  fd = fd_alloc();
  if (fd < 0) {
    errno = ENOMEM;
    return -1;
  }
  e = &fds[fd];
  memset(e, 0, sizeof(*e));
  e->used = 1;
  e->flags = flags;
  e->size = sz;
  e->pos = (flags & O_APPEND) ? sz : 0;
  strncpy(e->path, full, PATH_MAX - 1);
  if ((flags & O_TRUNC) && (flags & (O_WRONLY | O_RDWR))) {
    e->size = 0;
    e->pos = 0;
    e->dirty = 1;
    sz = 0;
  }
  if (sz > 0) {
    if (sz > FILE_CAP) {
      e->used = 0;
      errno = EIO;
      return -1;
    }
    e->data = (char*)malloc((size_t)sz + 1);
    if (!e->data) {
      e->used = 0;
      errno = ENOMEM;
      return -1;
    }
    if (k_fload(full, e->data, sz + 1) != sz) {
      free(e->data);
      e->used = 0;
      errno = EIO;
      return -1;
    }
    e->data[sz] = 0;
  } else {
    e->data = (char*)malloc(1);
    if (!e->data) {
      e->used = 0;
      errno = ENOMEM;
      return -1;
    }
    e->data[0] = 0;
  }
  errno = 0;
  return fd;
}

__export int creat(const char* path, mode_t mode) {
  return open(path, O_WRONLY | O_CREAT | O_TRUNC, (int)mode);
}

__export int close(int fd) {
  struct fd_ent* e;
  if (fd < 3 || fd >= FD_MAX || !fds[fd].used) {
    errno = EBADF;
    return -1;
  }
  e = &fds[fd];
  if (e->dirty && (e->flags & (O_WRONLY | O_RDWR))) {
    if (k_fsave(e->path, e->data ? e->data : "", e->size) < 0) {
      errno = EIO;
      free(e->data);
      e->used = 0;
      return -1;
    }
  }
  free(e->data);
  e->used = 0;
  errno = 0;
  return 0;
}

__export ssize_t read(int fd, void* buf, size_t n) {
  struct fd_ent* e;
  size_t left;
  if (fd == 0) {
    errno = ENOSYS;
    return -1;
  }
  if (fd < 3 || fd >= FD_MAX || !fds[fd].used) {
    errno = EBADF;
    return -1;
  }
  e = &fds[fd];
  if (e->pos >= e->size) return 0;
  left = (size_t)(e->size - e->pos);
  if (n > left) n = left;
  memcpy(buf, e->data + e->pos, n);
  e->pos += (int)n;
  return (ssize_t)n;
}

__export ssize_t write(int fd, const void* buf, size_t n) {
  struct fd_ent* e;
  char* nb;
  int need;
  if (fd == 1 || fd == 2) {
    k_write((const char*)buf, (int)n);
    return (ssize_t)n;
  }
  if (fd < 3 || fd >= FD_MAX || !fds[fd].used) {
    errno = EBADF;
    return -1;
  }
  e = &fds[fd];
  if (!(e->flags & (O_WRONLY | O_RDWR))) {
    errno = EACCES;
    return -1;
  }
  need = e->pos + (int)n;
  if (need > FILE_CAP) {
    errno = EIO;
    return -1;
  }
  if (!e->data || need > e->size) {
    nb = (char*)realloc(e->data, (size_t)need + 1);
    if (!nb) {
      errno = ENOMEM;
      return -1;
    }
    e->data = nb;
  }
  memcpy(e->data + e->pos, buf, n);
  e->pos += (int)n;
  if (e->pos > e->size) e->size = e->pos;
  e->data[e->size] = 0;
  e->dirty = 1;
  return (ssize_t)n;
}

__export off_t lseek(int fd, off_t off, int whence) {
  struct fd_ent* e;
  int np;
  if (fd < 3 || fd >= FD_MAX || !fds[fd].used) {
    errno = EBADF;
    return -1;
  }
  e = &fds[fd];
  if (whence == SEEK_SET) np = off;
  else if (whence == SEEK_CUR) np = e->pos + off;
  else if (whence == SEEK_END) np = e->size + off;
  else {
    errno = EINVAL;
    return -1;
  }
  if (np < 0) {
    errno = EINVAL;
    return -1;
  }
  e->pos = np;
  return e->pos;
}

__export int fstat(int fd, struct stat* st) {
  struct fd_ent* e;
  if (!st) {
    errno = EINVAL;
    return -1;
  }
  if (fd < 3 || fd >= FD_MAX || !fds[fd].used) {
    errno = EBADF;
    return -1;
  }
  e = &fds[fd];
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
  st->st_size = e->size;
  return 0;
}

__export int stat(const char* path, struct stat* st) {
  char full[PATH_MAX];
  int sz;
  if (!path || !st) {
    errno = EINVAL;
    return -1;
  }
  join_cwd(path, full, PATH_MAX);
  memset(st, 0, sizeof(*st));
  if (path_is_dir(full)) {
    st->st_mode = S_IFDIR | S_IRWXU;
    st->st_size = 0;
    return 0;
  }
  sz = k_fsize(full);
  if (sz < 0) {
    errno = ENOENT;
    return -1;
  }
  st->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
  st->st_size = sz;
  return 0;
}

__export DIR* opendir(const char* path) {
  char full[PATH_MAX];
  int i, n;
  struct __so_DIR* d;
  if (!path) {
    errno = EINVAL;
    return 0;
  }
  join_cwd(path, full, PATH_MAX);
  for (i = 0; i < DIR_MAX; i++)
    if (!dirs[i].used) break;
  if (i >= DIR_MAX) {
    errno = ENOMEM;
    return 0;
  }
  d = &dirs[i];
  memset(d, 0, sizeof(*d));
  n = k_dirlist(full, d->list, LIST_MAX);
  if (n < 0) {
    errno = ENOTDIR;
    return 0;
  }
  d->used = 1;
  d->pos = 0;
  return d;
}

__export struct dirent* readdir(DIR* dp) {
  struct __so_DIR* d = (struct __so_DIR*)dp;
  int i = 0;
  char* p;
  if (!d || !d->used) {
    errno = EBADF;
    return 0;
  }
  p = d->list + d->pos;
  while (*p == '\n') {
    p++;
    d->pos++;
  }
  if (!*p) return 0;
  memset(&d->cur, 0, sizeof(d->cur));
  while (*p && *p != '\n' && i < 31) d->cur.d_name[i++] = *p++;
  d->cur.d_name[i] = 0;
  if (i > 0 && d->cur.d_name[i - 1] == '\\') {
    d->cur.d_name[i - 1] = 0;
    d->cur.d_type = DT_DIR;
  } else {
    d->cur.d_type = DT_REG;
  }
  if (*p == '\n') p++;
  d->pos = (int)(p - d->list);
  return &d->cur;
}

__export int closedir(DIR* dp) {
  struct __so_DIR* d = (struct __so_DIR*)dp;
  if (!d || !d->used) {
    errno = EBADF;
    return -1;
  }
  d->used = 0;
  return 0;
}
