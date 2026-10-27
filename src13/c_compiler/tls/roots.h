/* roots.h - raizes confiaveis (ca-certificates), geradas em roots.c */
#ifndef ROOTS_H
#define ROOTS_H
typedef struct {
  const unsigned char* der;
  int len;
} ca_root;
extern const ca_root ca_roots[];
extern const int ca_root_n;
#endif
