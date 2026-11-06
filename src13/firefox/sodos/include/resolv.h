/* resolv.h (SODOS) - declaracoes; a resolucao passa pelo k_dns do kernel. */
#ifndef _RESOLV_H
#define _RESOLV_H
#include <netinet/in.h>
#ifdef __cplusplus
extern "C" {
#endif
struct __res_state {
  int options;
};
typedef struct __res_state* res_state;
enum { ns_c_in = 1 };
enum { ns_t_a = 1, ns_t_aaaa = 28, ns_t_https = 65 };
int res_init(void);
int res_ninit(res_state s);
void res_nclose(res_state s);
int res_query(const char* name, int cls, int type, unsigned char* ans, int len);
int res_nquery(res_state s, const char* name, int cls, int type, unsigned char* ans, int len);
#ifdef __cplusplus
}
#endif
#endif
