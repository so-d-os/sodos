#ifndef _DIRENT_H
#define _DIRENT_H

#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4

struct dirent {
  char d_name[32];
  unsigned char d_type;
};

typedef struct __so_DIR DIR;

DIR* opendir(const char* path);
struct dirent* readdir(DIR* d);
int closedir(DIR* d);

#endif
