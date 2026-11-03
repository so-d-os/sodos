#ifndef _SODOS_ARPA_INET_H
#define _SODOS_ARPA_INET_H
#include <netinet/in.h>
int inet_aton(const char* cp, struct in_addr* a);
int inet_pton(int af, const char* src, void* dst);
#endif
