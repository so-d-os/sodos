/* dirent.h - opendir do SODOS + scandir/dirfd */
#ifndef _SODOS_COMPAT_DIRENT_H
#define _SODOS_COMPAT_DIRENT_H
#include_next <dirent.h>
int dirfd(DIR* d);
int scandir(const char* dir, struct dirent*** list, int (*sel)(const struct dirent*),
            int (*cmp)(const struct dirent**, const struct dirent**));
int alphasort(const struct dirent** a, const struct dirent** b);
#endif
