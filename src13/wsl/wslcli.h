#ifndef WSLCLI_H
#define WSLCLI_H
#include "wslio.h"

/* Grava um arquivo local inteiro. 0 ok. */
typedef int (*wsl_save_fn)(const char* path, const void* data, uint32_t len);
void wsl_usage(void);
/* list/info/ls/cat/cp. Codigo de saida. */
int wsl_cli_read(int argc, char** argv, const wsl_io* disk, wsl_save_fn save);

#endif
