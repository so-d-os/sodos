/* mntent.h (SODOS) - declaracoes. */
#ifndef _MNTENT_H
#define _MNTENT_H
#include <stdio.h>
#ifdef __cplusplus
extern "C" {
#endif
struct mntent {
  char* mnt_fsname;
  char* mnt_dir;
  char* mnt_type;
  char* mnt_opts;
  int mnt_freq;
  int mnt_passno;
};
FILE* setmntent(const char* file, const char* type);
struct mntent* getmntent(FILE* f);
int endmntent(FILE* f);
/* lote3 */
struct mntent* getmntent_r(FILE* f, struct mntent* m, char* buf, int n);
char* hasmntopt(const struct mntent* m, const char* opt);

#ifdef __cplusplus
}
#endif
#endif
