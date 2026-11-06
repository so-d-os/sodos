/* net/if.h (SODOS) - declaracoes. */
#ifndef _NET_IF_H
#define _NET_IF_H
#include <sys/socket.h>
#ifdef __cplusplus
extern "C" {
#endif
#define IFNAMSIZ 16
#define IF_NAMESIZE IFNAMSIZ
#define IFF_UP 0x1
#define IFF_LOOPBACK 0x8
#define IFF_RUNNING 0x40
struct ifreq {
  char ifr_name[IFNAMSIZ];
  union {
    struct sockaddr ifr_addr;
    short ifr_flags;
    int ifr_ifindex;
    int ifr_mtu;
    char __pad[24];
  };
};
unsigned int if_nametoindex(const char* name);
char* if_indextoname(unsigned int index, char* name);
#ifdef __cplusplus
}
#endif
#endif
