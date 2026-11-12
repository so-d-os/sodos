/* sodos_shm.c - memoria compartilhada POSIX (shm_open) dentro do processo.
 *
 * O Gecko guarda em memoria compartilhada o mapa de preferencias, as tabelas
 * de fontes e os buffers do IPC. No SODOS so existe um processo, entao o
 * "objeto" e um bloco do heap com contador de referencias: cada descritor e
 * cada mmap(MAP_SHARED) aponta para o mesmo bloco, que some quando o ultimo
 * solta. Os descritores comecam em 3000 (os pipes usam 2000..2127).
 *
 * Limites: o bloco nao muda de lugar depois de mapeado (ftruncate so cresce
 * ate a capacidade ja reservada) e nao ha protecao: um mapa "so leitura" e
 * gravavel como todo o resto do SODOS.
 */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sys/mman.h"
#include "sodos.h"

#define SHM_FD_BASE 3000
#define SHM_FD_MAX 256
#define SHM_MAP_MAX 512
#define SHM_NAME_MAX 64
#define SHM_PAGE 4096u

struct shm_obj {
  char name[SHM_NAME_MAX]; /* vazio depois do shm_unlink */
  char* raw;               /* bloco do malloc */
  char* data;              /* raw alinhado a pagina */
  size_t size;
  size_t cap;
  int refs; /* descritores + mapas + 1 enquanto tem nome */
  struct shm_obj* next;
};

struct shm_fd {
  struct shm_obj* o;
  int flags;
  size_t pos;
};

struct shm_map {
  char* addr;
  size_t len;
  struct shm_obj* o;
};

static struct shm_obj* objs;
static struct shm_fd sfd[SHM_FD_MAX];
static struct shm_map smap[SHM_MAP_MAX];

static struct shm_fd* get_fd(int fd) {
  if (fd < SHM_FD_BASE || fd >= SHM_FD_BASE + SHM_FD_MAX) return NULL;
  return sfd[fd - SHM_FD_BASE].o ? &sfd[fd - SHM_FD_BASE] : NULL;
}

int sodos_shm_is(int fd) { return get_fd(fd) != NULL; }

static void obj_unref(struct shm_obj* o) {
  struct shm_obj** pp;
  if (--o->refs > 0) return;
  for (pp = &objs; *pp; pp = &(*pp)->next) {
    if (*pp == o) {
      *pp = o->next;
      break;
    }
  }
  free(o->raw);
  free(o);
}

static struct shm_obj* obj_find(const char* name) {
  struct shm_obj* o;
  for (o = objs; o; o = o->next)
    if (o->name[0] && strcmp(o->name, name) == 0) return o;
  return NULL;
}

static int fd_new(struct shm_obj* o, int flags) {
  int i;
  for (i = 0; i < SHM_FD_MAX; i++) {
    if (!sfd[i].o) {
      sfd[i].o = o;
      sfd[i].flags = flags;
      sfd[i].pos = 0;
      o->refs++;
      return SHM_FD_BASE + i;
    }
  }
  errno = EMFILE;
  return -1;
}

int shm_open(const char* name, int flags, mode_t mode) {
  struct shm_obj* o;
  int fd;
  (void)mode;
  if (!name || !name[0] || strlen(name) >= SHM_NAME_MAX) {
    errno = EINVAL;
    return -1;
  }
  o = obj_find(name);
  if (o && (flags & O_CREAT) && (flags & O_EXCL)) {
    errno = EEXIST;
    return -1;
  }
  if (!o) {
    if (!(flags & O_CREAT)) {
      errno = ENOENT;
      return -1;
    }
    o = (struct shm_obj*)calloc(1, sizeof(*o));
    if (!o) {
      errno = ENOMEM;
      return -1;
    }
    strcpy(o->name, name);
    o->refs = 1; /* o nome */
    o->next = objs;
    objs = o;
    fd = fd_new(o, flags);
    if (fd < 0) {
      o->name[0] = 0;
      obj_unref(o);
    }
    return fd;
  }
  return fd_new(o, flags);
}

int shm_unlink(const char* name) {
  struct shm_obj* o = name ? obj_find(name) : NULL;
  if (!o) {
    errno = ENOENT;
    return -1;
  }
  o->name[0] = 0;
  obj_unref(o);
  return 0;
}

int sodos_shm_close(int fd) {
  struct shm_fd* f = get_fd(fd);
  struct shm_obj* o;
  if (!f) {
    errno = EBADF;
    return -1;
  }
  o = f->o;
  f->o = NULL;
  obj_unref(o);
  return 0;
}

int sodos_shm_dup(int fd) {
  struct shm_fd* f = get_fd(fd);
  if (!f) {
    errno = EBADF;
    return -1;
  }
  return fd_new(f->o, f->flags);
}

int sodos_shm_ftruncate(int fd, off_t len) {
  struct shm_fd* f = get_fd(fd);
  struct shm_obj* o;
  if (!f || len < 0) {
    errno = f ? EINVAL : EBADF;
    return -1;
  }
  if ((f->flags & O_ACCMODE) == O_RDONLY) {
    errno = EINVAL;
    return -1;
  }
  o = f->o;
  if ((size_t)len > o->cap) {
    size_t cap = ((size_t)len + SHM_PAGE - 1) & ~(size_t)(SHM_PAGE - 1);
    char* raw;
    char* data;
    int i;
    /* Mapeado, o bloco nao pode mudar de endereco. */
    for (i = 0; i < SHM_MAP_MAX; i++) {
      if (smap[i].o == o) {
        errno = ENOMEM;
        return -1;
      }
    }
    raw = (char*)malloc(cap + SHM_PAGE);
    if (!raw) {
      errno = ENOMEM;
      return -1;
    }
    data = (char*)(((uintptr_t)raw + SHM_PAGE - 1) & ~(uintptr_t)(SHM_PAGE - 1));
    memset(data, 0, cap);
    if (o->size) memcpy(data, o->data, o->size);
    free(o->raw);
    o->raw = raw;
    o->data = data;
    o->cap = cap;
  } else if ((size_t)len > o->size) {
    memset(o->data + o->size, 0, (size_t)len - o->size);
  }
  o->size = (size_t)len;
  return 0;
}

int sodos_shm_fstat(int fd, struct stat* st) {
  struct shm_fd* f = get_fd(fd);
  if (!f || !st) {
    errno = EBADF;
    return -1;
  }
  memset(st, 0, sizeof(*st));
  st->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
  st->st_size = (off_t)f->o->size;
  st->st_nlink = f->o->name[0] ? 1 : 0;
  st->st_blksize = SHM_PAGE;
  return 0;
}

off_t sodos_shm_lseek(int fd, off_t off, int whence) {
  struct shm_fd* f = get_fd(fd);
  off_t np;
  if (!f) {
    errno = EBADF;
    return -1;
  }
  np = whence == SEEK_SET ? off : whence == SEEK_CUR ? (off_t)f->pos + off : (off_t)f->o->size + off;
  if (np < 0 || (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END)) {
    errno = EINVAL;
    return -1;
  }
  f->pos = (size_t)np;
  return np;
}

ssize_t sodos_shm_read(int fd, void* buf, size_t n) {
  struct shm_fd* f = get_fd(fd);
  if (!f) {
    errno = EBADF;
    return -1;
  }
  if (f->pos >= f->o->size) return 0;
  if (n > f->o->size - f->pos) n = f->o->size - f->pos;
  memcpy(buf, f->o->data + f->pos, n);
  f->pos += n;
  return (ssize_t)n;
}

ssize_t sodos_shm_write(int fd, const void* buf, size_t n) {
  struct shm_fd* f = get_fd(fd);
  if (!f) {
    errno = EBADF;
    return -1;
  }
  if (f->pos + n > f->o->size && sodos_shm_ftruncate(fd, (off_t)(f->pos + n)) != 0) return -1;
  memcpy(f->o->data + f->pos, buf, n);
  f->pos += n;
  return (ssize_t)n;
}

/* mmap de um descritor de memoria compartilhada: o proprio bloco. Um mapa
 * MAP_PRIVATE tambem ve o bloco (sem copia na escrita). */
void* sodos_shm_map(int fd, size_t len, off_t off) {
  struct shm_fd* f = get_fd(fd);
  struct shm_obj* o;
  int i;
  if (!f) {
    errno = EBADF;
    return MAP_FAILED;
  }
  o = f->o;
  if (off < 0 || ((size_t)off & (SHM_PAGE - 1)) || (size_t)off + len > o->cap) {
    errno = (size_t)off + len > o->cap ? ENXIO : EINVAL;
    return MAP_FAILED;
  }
  for (i = 0; i < SHM_MAP_MAX; i++) {
    if (!smap[i].o) {
      smap[i].o = o;
      smap[i].addr = o->data + off;
      smap[i].len = len;
      o->refs++;
      return smap[i].addr;
    }
  }
  errno = ENOMEM;
  return MAP_FAILED;
}

/* 1 se a faixa era um mapa daqui (e foi solta). */
int sodos_shm_unmap(void* addr, size_t len) {
  int i;
  (void)len;
  for (i = 0; i < SHM_MAP_MAX; i++) {
    if (smap[i].o && smap[i].addr == (char*)addr) {
      struct shm_obj* o = smap[i].o;
      smap[i].o = NULL;
      obj_unref(o);
      return 1;
    }
  }
  return 0;
}

#else /* x32: sem shm_open; sodos_mmap.c cai no caminho normal. */

#include <stddef.h>
#include <sys/types.h>

int sodos_shm_is(int fd) { (void)fd; return 0; }
void* sodos_shm_map(int fd, size_t len, off_t off) { (void)fd; (void)len; (void)off; return (void*)-1; }
int sodos_shm_unmap(void* addr, size_t len) { (void)addr; (void)len; return 0; }

#endif
