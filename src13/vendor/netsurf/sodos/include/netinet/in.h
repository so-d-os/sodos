#ifndef _SODOS_NETINET_IN_H
#define _SODOS_NETINET_IN_H
#include <stdint.h>
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;
struct in_addr { in_addr_t s_addr; };
struct in6_addr { unsigned char s6_addr[16]; };
#define AF_INET 2
#define AF_INET6 10
#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46
#endif
