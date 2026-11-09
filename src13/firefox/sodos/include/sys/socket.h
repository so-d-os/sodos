/* sys/socket.h - soquetes BSD sobre a conexao TCP unica do SODOS. */
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <stdint.h>
#include <sys/types.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int socklen_t;

#ifndef _SA_FAMILY_T_DEFINED
#define _SA_FAMILY_T_DEFINED
typedef unsigned short sa_family_t;
#endif

struct sockaddr {
  sa_family_t sa_family;
  char sa_data[14];
};

struct sockaddr_storage {
  sa_family_t ss_family;
  char __data[126];
};

struct linger {
  int l_onoff;
  int l_linger;
};

#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_INET 2
#define AF_INET6 10
#define PF_UNIX AF_UNIX
#define PF_INET AF_INET
#define PF_INET6 AF_INET6

#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3

#define SOL_SOCKET 1
#define SOMAXCONN 4096
#define SO_DEBUG 1
#define SO_REUSEADDR 2
#define SO_TYPE 3
#define SO_ERROR 4
#define SO_DONTROUTE 5
#define SO_BROADCAST 6
#define SO_SNDBUF 7
#define SO_RCVBUF 8
#define SO_KEEPALIVE 9
#define SO_OOBINLINE 10
#define SO_LINGER 13
#define SO_RCVTIMEO 20
#define SO_SNDTIMEO 21

#define TCP_NODELAY 1

#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2

#define MSG_OOB 0x1
#define MSG_PEEK 0x2
#define MSG_DONTROUTE 0x4
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0x4000

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr* addr, socklen_t len);
int connect(int fd, const struct sockaddr* addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr* addr, socklen_t* len);
int send(int fd, const void* buf, size_t n, int flags);
int recv(int fd, void* buf, size_t n, int flags);
int sendto(int fd, const void* buf, size_t n, int flags,
           const struct sockaddr* to, socklen_t tolen);
int recvfrom(int fd, void* buf, size_t n, int flags, struct sockaddr* from,
             socklen_t* fromlen);
int shutdown(int fd, int how);
int setsockopt(int fd, int level, int opt, const void* val, socklen_t len);
int getsockopt(int fd, int level, int opt, void* val, socklen_t* len);
int getsockname(int fd, struct sockaddr* addr, socklen_t* len);
int getpeername(int fd, struct sockaddr* addr, socklen_t* len);
int socketpair(int domain, int type, int protocol, int sv[2]);
ssize_t sendfile(int out_fd, int in_fd, off_t* offset, size_t count);

/* lote2 */
#include <sys/uio.h>
#define PF_UNSPEC AF_UNSPEC
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK 04000
#define SOCK_CLOEXEC 02000000
#define MSG_CTRUNC 0x8
#define MSG_TRUNC 0x20
#define MSG_WAITALL 0x100
#define SCM_RIGHTS 1
#define SCM_CREDENTIALS 2
#define SO_PASSCRED 16
#define SO_PEERCRED 17
struct ucred {
  pid_t pid;
  uid_t uid;
  gid_t gid;
};
struct msghdr {
  void* msg_name;
  socklen_t msg_namelen;
  struct iovec* msg_iov;
  size_t msg_iovlen;
  void* msg_control;
  size_t msg_controllen;
  int msg_flags;
};
struct cmsghdr {
  size_t cmsg_len;
  int cmsg_level;
  int cmsg_type;
};
#define CMSG_ALIGN(n) (((n) + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1))
#define CMSG_LEN(n) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (n))
#define CMSG_SPACE(n) (CMSG_ALIGN(sizeof(struct cmsghdr)) + CMSG_ALIGN(n))
#define CMSG_DATA(c) ((unsigned char*)(c) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_FIRSTHDR(m) \
  ((m)->msg_controllen >= sizeof(struct cmsghdr) ? (struct cmsghdr*)(m)->msg_control : (struct cmsghdr*)0)
#define CMSG_NXTHDR(m, c) ((struct cmsghdr*)0)
ssize_t sendmsg(int fd, const struct msghdr* msg, int flags);
ssize_t recvmsg(int fd, struct msghdr* msg, int flags);
int accept4(int fd, struct sockaddr* addr, socklen_t* len, int flags);

#ifdef __cplusplus
}
#endif

#endif
