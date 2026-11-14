/* drives.h - Letras de unidade estilo DOS (C: RAM, D: disco/RAM) */
#pragma once
#include <stdint.h>

/* Nome longo (LFN) do FAT: ate 255 caracteres; caminho inteiro sem a letra. */
#define FAT_NAME_MAX 256
#define DRV_PATH     260

void drives_init(void);
/* Unidade e pastas atuais: cada tela do DOS (vt.c) tem as suas. */
struct drives_state {
  char letter;
  char dir[26][DRV_PATH];
};
void drives_state_save(struct drives_state* s);
void drives_state_load(const struct drives_state* s);
char drives_current(void);
int drives_set_current(char letter);
int drives_is_mounted(char letter);
const char* drives_kind_name(char letter);
int drives_mount_ram(char letter);
int drives_mount_fat(uint32_t ata_drive, char letter, uint32_t partition_num);
/* Pendrive USB (FAT12/16) em U:. U: tambem monta sozinho no primeiro uso. */
int drives_mount_usb(void);
/* Z: na RAM (FAT16, ate 1 GB), criado vazio no boot. */
int drives_mount_ramdisk(void);
/* 0 com a unidade montada e o FAT dela carregado (para quem chama fat_* direto). */
int drives_use(char letter);
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

/* Opcoes do dir: /s subpastas, /w largo, /d so pastas. */
#define DIR_SUB  1
#define DIR_WIDE 2
#define DIR_DIRS 4
/* spec aceita * e ? no ultimo nome (*.exe, ???.txt). */
void drives_dir(const char* spec, int flags);
/* del: 0 apagou, >0 quantos (com * e ?), <0 erro (ver drives.c). */
int drives_del(const char* spec);
/* Renomeia/move no mesmo disco FAT: 0 ok, -1 nao existe, -2 invalido,
 * -3 destino e pasta, -5 nao e FAT, -6 outra unidade, -7 pasta entre pastas. */
int drives_rename(const char* from, const char* to);
/* md/rd por caminho completo: 0 ok, 1 ja existe (md), -2 nao vazia (rd),
 * -3 nao e pasta (rd), -5 nao e disco FAT, -1 outro erro. */
int drives_mkdir(const char* path);
int drives_rmdir(const char* path);
/* Espaco em KB de uma unidade FAT. -1 se nao for. */
int drives_space(char letter, uint32_t* total_kb, uint32_t* free_kb);

/* Uma entrada de pasta. date/time no formato FAT (0 = sem data).
 * name e o nome longo quando cabe; senao o apelido 8.3 (NOME~1.EXT), que
 * tambem abre o arquivo. E o k_dirent dos programas: nao muda de tamanho. */
struct dir_item {
  char name[32];
  uint32_t size;
  uint16_t date;
  uint16_t time;
  uint8_t dir;
};
/* Arquivos que casam com spec (* e ? no ultimo nome), um por vez em fn:
 * path completo e shown (pasta como foi escrita + nome achado). Sem * e ?
 * fn recebe spec como esta. Quantos; -1 nenhum, -2 pasta/unidade invalida.
 * fn devolvendo nao zero para. Usado por type, cat, find, grep e tail. */
typedef int (*drives_glob_fn)(const char* path, const char* shown, void* ctx);
int drives_glob(const char* spec, drives_glob_fn fn, void* ctx);
/* Itens de uma pasta ("U:\\SRC"). Quantos; -1 se nao e pasta. */
int drives_items(const char* path, struct dir_item* out, int max);
/* Nome longo inteiro de path (o ultimo nome). 0 achou; -1 nao. */
int drives_long_name(const char* path, char* out, int max);
void drives_list(char letter);
int drives_names(char letter, char out[][32], int max);
char path_letter(const char* path);
int drives_enum_paths(char out[][48], int max);
