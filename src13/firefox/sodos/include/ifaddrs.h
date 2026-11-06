/* ifaddrs.h (SODOS) - declaracoes. */
#ifndef _IFADDRS_H
#define _IFADDRS_H
#include <sys/socket.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ifaddrs {
  struct ifaddrs* ifa_next;
  char* ifa_name;
  unsigned int ifa_flags;
  struct sockaddr* ifa_addr;
  struct sockaddr* ifa_netmask;
  struct sockaddr* ifa_dstaddr;
  void* ifa_data;
};
int getifaddrs(struct ifaddrs** out);
void freeifaddrs(struct ifaddrs* ifa);
#ifdef __cplusplus
}
#endif
#endif
