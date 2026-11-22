#ifndef _DIRENT_H
#define _DIRENT_H

#ifdef __cplusplus
extern "C" {
#endif

#define DT_UNKNOWN 0
#define DT_REG     8
#define DT_DIR     4

/* Nome longo inteiro (FAT LFN, ate 255). Programas compilados com o
 * dirent.h antigo (d_name[32], d_type no byte 32) continuam valendo: o
 * readdir do posix-lite tambem poe o d_type no byte 32 quando o nome tem ate
 * 31 letras. */
struct dirent {
  char d_name[256];
  unsigned char d_type;
};

typedef struct __so_DIR DIR;

DIR* opendir(const char* path);
struct dirent* readdir(DIR* d);
int closedir(DIR* d);
void rewinddir(DIR* d);


#ifdef __cplusplus
}
#endif
#endif
