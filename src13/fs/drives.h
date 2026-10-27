/* drives.h - Letras de unidade estilo DOS (C: RAM, D: disco/RAM) */
#pragma once
#include <stdint.h>

void drives_init(void);
char drives_current(void);
int drives_set_current(char letter);
int drives_is_mounted(char letter);
const char* drives_kind_name(char letter);
int drives_mount_ram(char letter);
int drives_mount_fat(uint32_t ata_drive, char letter, uint32_t partition_num);
void drives_print_status(void);
void drives_automount(void);

/* Relativo a pasta atual da unidade. Comeca com \ para ir da raiz. */
void path_parse(const char* path, char* letter_out, char* name_out, int name_max);
int file_load(const char* path, char* buf, int max_size);
/* Trecho binario a partir de offset. Nao acrescenta NUL. */
int file_read_at(const char* path, uint32_t offset, char* buf, int len);
int file_save(const char* path, const char* data, int size);
/* Tamanho em bytes; <0 se nao existe. */
int file_size(const char* path);
/* Lista a pasta. Linha terminada em \ e diretorio. -1 se nao e pasta. */
int file_list(const char* path, char* out, int max);
void drives_pwd(char* out, int max);
void drives_prompt(char* out, int max);
/* 0 entrou. Sem argumento so imprime a pasta atual. */
int drives_chdir(const char* spec);
void drives_print_dir(const char* spec);
void drives_list(char letter);
int drives_names(char letter, char out[][32], int max);
char path_letter(const char* path);
int drives_enum_paths(char out[][48], int max);
