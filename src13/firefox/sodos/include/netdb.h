/* netdb.h - resolucao de nomes minima (o kernel so tem IPv4/DNS). */
#ifndef _NETDB_H
#define _NETDB_H

#include <netinet/in.h>
#include <sys/socket.h>

struct hostent {
  char* h_name;
  char** h_aliases;
  int h_addrtype;
  int h_length;
  char** h_addr_list;
};
#define h_addr h_addr_list[0]

struct servent {
  char* s_name;
  char** s_aliases;
  int s_port;
  char* s_proto;
};

struct protoent {
  char* p_name;
  char** p_aliases;
  int p_proto;
};

struct protoent* getprotobyname(const char* name);
struct protoent* getprotobynumber(int proto);
struct servent* getservbyport(int port, const char* proto);

extern int h_errno;
#define HOST_NOT_FOUND 1
#define TRY_AGAIN 2
#define NO_RECOVERY 3
#define NO_DATA 4
#define NO_ADDRESS NO_DATA

struct hostent* gethostbyname(const char* name);
struct hostent* gethostbyaddr(const void* addr, socklen_t len, int type);
struct servent* getservbyname(const char* name, const char* proto);
int gethostname(char* name, size_t len);

#define NI_MAXHOST 1025
#define NI_MAXSERV 32
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_NAMEREQD 4
#define NI_DGRAM 16

int getnameinfo(const struct sockaddr* addr, socklen_t addrlen, char* host,
                socklen_t hostlen, char* serv, socklen_t servlen, int flags);

#endif
