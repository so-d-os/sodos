/* sodos_dart_compat.h - o que a Dart VM 3.13 espera e o port ainda nao tem.
 *
 * Entra em toda compilacao do Dart pelo wrapper
 * dart/toolchain/bin/x86_64-sodos64-g++ (-include); o Firefox nao o ve.
 *
 * - std::atomic_ref (C++20): chegou na libc++ 19; o port usa a 18.1.8. Aqui
 *   sobre os __atomic do gcc, que aceitam qualquer tipo trivialmente copiavel
 *   (os ponteiros "tagged" da VM, como ObjectPtr, sao uma palavra).
 */
#ifndef SODOS_DART_COMPAT_H
#define SODOS_DART_COMPAT_H

/* ---- libc: funcoes *at e *64 -------------------------------------------- */
/* O namespace padrao do dart:io passa AT_FDCWD; os caminhos vao direto para as
 * funcoes de caminho. openat64/lseek64/unlinkat/mkdirat vem de
 * firefox/sodos/port/sodos_linux_abi.c; fstatat/renameat/utimensat/readlinkat
 * de dart/port/sodos_dart_libc.c. O fstatat64 do port usa o layout do glibc
 * (para o Rust); aqui stat64 == stat, entao o Dart usa o fstatat do SODOS. */
/* Os nomes do Dart viram sodos_* por macro: o port do Firefox ja tem open64,
 * openat64, unlinkat... que ignoram o dirfd, e o Firefox nao muda.
 * sodos_dart_libc.c (que implementa os sodos_*) compila sem os renomes. */
#ifndef SODOS_DART_NO_RENAME
#define open64 sodos_open64
#define openat64 sodos_openat64
#define openat sodos_openat64
#define fstatat sodos_fstatat
#define fstatat64 sodos_fstatat
#define unlinkat sodos_unlinkat
#define mkdirat sodos_mkdirat
#define renameat sodos_renameat
#define utimensat sodos_utimensat
#define readlinkat sodos_readlinkat
#define symlinkat sodos_symlinkat
#define fdopendir sodos_fdopendir
#define syscall sodos_dart_syscall
/* Limites da pilha por thread (os_thread_linux.cc: GetCurrentStackBounds):
 * o pthread_attr_getstack do port so conhece a pilha do processo. */
#define pthread_getattr_np sodos_dart_getattr_np
#define pthread_attr_getstack sodos_dart_attr_getstack
/* O Dart acha que esta no Linux: so "/..." e absoluto. getcwd devolve o estilo
 * MSYS que o posix-lite aceita ("D:\X\Y" -> "/d/X/Y"), e Uri.base fica certo. */
#define getcwd sodos_dart_getcwd
#endif

#ifdef __cplusplus
extern "C" {
#endif
struct stat;
struct timespec;
struct __so_DIR;
int sodos_open64(const char* path, int flags, ...);
int sodos_openat64(int dirfd, const char* path, int flags, ...);
int sodos_fstatat(int dirfd, const char* path, struct stat* st, int flags);
int sodos_unlinkat(int dirfd, const char* path, int flags);
int sodos_mkdirat(int dirfd, const char* path, unsigned int mode);
int sodos_renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath);
int sodos_utimensat(int dirfd, const char* path, const struct timespec times[2], int flags);
long sodos_readlinkat(int dirfd, const char* path, char* buf, unsigned long n);
int sodos_symlinkat(const char* target, int newdirfd, const char* linkpath);
struct __so_DIR* sodos_fdopendir(int fd);
long sodos_dart_syscall(long number, ...);
char* sodos_dart_getcwd(char* buf, unsigned long size);
/* sodos_dart_getattr_np/sodos_dart_attr_getstack: o proprio pthread.h os
 * declara (com os tipos certos) depois das macros acima. */
/* ja existem no port (sodos_linux_abi.c / sodos_mesa.c), sem declaracao nos headers */
long lseek64(int fd, long off, int whence);
long pread64(int fd, void* buf, unsigned long n, long off);
long pwrite64(int fd, const void* buf, unsigned long n, long off);
int ftruncate64(int fd, long len);
long sendfile64(int out_fd, int in_fd, long* off, unsigned long n);
int setsid(void);
char* strsignal(int sig);
/* novas (sodos_dart_libc.c) */
char* strcasestr(const char* hay, const char* needle);
void bzero(void* p, unsigned long n);
/* gcc avisa "noreturn function does return" sem isto (-Werror no Dart) */
void abort(void) __attribute__((noreturn));
void exit(int code) __attribute__((noreturn));
void _exit(int code) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif

/* ---- constantes e tipos do glibc x86_64 que faltam nos headers ---------- */
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif
#ifndef UTIME_NOW
#define UTIME_NOW ((1l << 30) - 1l)
#define UTIME_OMIT ((1l << 30) - 2l)
#endif
typedef unsigned long ino64_t;
/* dirent.h tem DT_UNKNOWN/DT_REG/DT_DIR */
#define DT_FIFO 1
#define DT_CHR 2
#define DT_BLK 6
#define DT_LNK 10
#define DT_SOCK 12
/* limits.h */
#define PIPE_BUF 4096
#define _POSIX_OPEN_MAX 20
/* signal.h: si_code */
#define ILL_ILLOPC 1
#define ILL_ILLADR 3
#define ILL_ILLTRP 4
#define ILL_PRVOPC 5
#define ILL_PRVREG 6
#define ILL_COPROC 7
#define ILL_BADSTK 8
#define BUS_ADRERR 2
#define BUS_OBJERR 3
#define BUS_MCEERR_AR 4
#define BUS_MCEERR_AO 5
#define TRAP_BRKPT 1
#define TRAP_TRACE 2
/* netinet/in.h: IPv6 e multicast por grupo (o kernel nao tem; setsockopt falha) */
#define IPV6_MULTICAST_IF 17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_V6ONLY 26
#define MCAST_JOIN_GROUP 42
#define MCAST_LEAVE_GROUP 45
/* sys/syscall.h */
#ifndef __NR_getrandom
#define __NR_getrandom 318
#endif
#ifndef SYS_getrandom
#define SYS_getrandom __NR_getrandom
#endif
/* sys/ioctl.h tem TIOCGWINSZ mas nao a struct */
struct winsize {
  unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel;
};

/* group_req (MCAST_JOIN_GROUP) precisa de sockaddr_storage */
#ifdef __cplusplus
#include <stdint.h>
#include <sys/socket.h>
struct group_req {
  uint32_t gr_interface;
  struct sockaddr_storage gr_group;
};
#endif

#if defined(__cplusplus) && __cplusplus >= 202002L
#include <atomic>
#include <cstddef>
#include <type_traits>

#if !defined(_LIBCPP_VERSION) || _LIBCPP_VERSION < 190000
_LIBCPP_BEGIN_NAMESPACE_STD

template <class _Tp>
class atomic_ref {
  static_assert(is_trivially_copyable<_Tp>::value, "atomic_ref<T> exige T trivialmente copiavel");
  _Tp* __p_;

  static constexpr int __mo(memory_order __o) { return static_cast<int>(__o); }
  /* load/compare_exchange no caso de falha nao aceitam release/acq_rel */
  static constexpr int __mo_fail(memory_order __o) {
    return __o == memory_order_acq_rel ? __ATOMIC_ACQUIRE
           : __o == memory_order_release ? __ATOMIC_RELAXED
                                         : static_cast<int>(__o);
  }

 public:
  using value_type = _Tp;
  using difference_type = _Tp;  /* so faz sentido para aritmeticos */
  static constexpr size_t required_alignment = alignof(_Tp) > sizeof(_Tp) ? alignof(_Tp) : sizeof(_Tp);
  static constexpr bool is_always_lock_free = __atomic_always_lock_free(sizeof(_Tp), 0);

  explicit atomic_ref(_Tp& __r) noexcept : __p_(std::addressof(__r)) {}
  atomic_ref(const atomic_ref&) noexcept = default;
  atomic_ref& operator=(const atomic_ref&) = delete;

  bool is_lock_free() const noexcept { return __atomic_is_lock_free(sizeof(_Tp), __p_); }

  void store(_Tp __v, memory_order __o = memory_order_seq_cst) const noexcept {
    __atomic_store(__p_, std::addressof(__v), __mo(__o));
  }
  _Tp operator=(_Tp __v) const noexcept {
    store(__v);
    return __v;
  }
  _Tp load(memory_order __o = memory_order_seq_cst) const noexcept {
    alignas(_Tp) unsigned char __buf[sizeof(_Tp)];
    _Tp* __r = reinterpret_cast<_Tp*>(__buf);
    __atomic_load(__p_, __r, __mo(__o));
    return *__r;
  }
  operator _Tp() const noexcept { return load(); }

  _Tp exchange(_Tp __v, memory_order __o = memory_order_seq_cst) const noexcept {
    alignas(_Tp) unsigned char __buf[sizeof(_Tp)];
    _Tp* __r = reinterpret_cast<_Tp*>(__buf);
    __atomic_exchange(__p_, std::addressof(__v), __r, __mo(__o));
    return *__r;
  }
  bool compare_exchange_weak(_Tp& __e, _Tp __d, memory_order __s, memory_order __f) const noexcept {
    return __atomic_compare_exchange(__p_, std::addressof(__e), std::addressof(__d), true,
                                     __mo(__s), __mo_fail(__f));
  }
  bool compare_exchange_strong(_Tp& __e, _Tp __d, memory_order __s, memory_order __f) const noexcept {
    return __atomic_compare_exchange(__p_, std::addressof(__e), std::addressof(__d), false,
                                     __mo(__s), __mo_fail(__f));
  }
  bool compare_exchange_weak(_Tp& __e, _Tp __d, memory_order __o = memory_order_seq_cst) const noexcept {
    return compare_exchange_weak(__e, __d, __o, __o);
  }
  bool compare_exchange_strong(_Tp& __e, _Tp __d, memory_order __o = memory_order_seq_cst) const noexcept {
    return compare_exchange_strong(__e, __d, __o, __o);
  }

  /* Aritmetica: so para inteiros (os ponteiros "tagged" da VM nao usam). */
  template <class _Up = _Tp, class = enable_if_t<is_integral<_Up>::value>>
  _Tp fetch_add(_Tp __v, memory_order __o = memory_order_seq_cst) const noexcept {
    return __atomic_fetch_add(__p_, __v, __mo(__o));
  }
  template <class _Up = _Tp, class = enable_if_t<is_integral<_Up>::value>>
  _Tp fetch_sub(_Tp __v, memory_order __o = memory_order_seq_cst) const noexcept {
    return __atomic_fetch_sub(__p_, __v, __mo(__o));
  }
  template <class _Up = _Tp, class = enable_if_t<is_integral<_Up>::value>>
  _Tp fetch_and(_Tp __v, memory_order __o = memory_order_seq_cst) const noexcept {
    return __atomic_fetch_and(__p_, __v, __mo(__o));
  }
  template <class _Up = _Tp, class = enable_if_t<is_integral<_Up>::value>>
  _Tp fetch_or(_Tp __v, memory_order __o = memory_order_seq_cst) const noexcept {
    return __atomic_fetch_or(__p_, __v, __mo(__o));
  }
  template <class _Up = _Tp, class = enable_if_t<is_integral<_Up>::value>>
  _Tp fetch_xor(_Tp __v, memory_order __o = memory_order_seq_cst) const noexcept {
    return __atomic_fetch_xor(__p_, __v, __mo(__o));
  }
};

template <class _Tp>
atomic_ref(_Tp&) -> atomic_ref<_Tp>;

_LIBCPP_END_NAMESPACE_STD
#endif /* libc++ < 19 */
#endif /* C++20 */

#endif
