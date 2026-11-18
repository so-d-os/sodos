/*
 * fsdrv.h - Driver de sistema de arquivos carregado sob demanda (DLL SOX2 em
 * X:\SODOS\DRV, como os drivers de video de docs/GPU.md).
 *
 * O kernel so entende FAT12/FAT16 (fs/fat.c). Quando acha outro formato
 * (hoje: FAT32, tipo 0x0B/0x0C ou BPB com FATSz16 = 0) ele carrega o driver
 * (FAT32.DLL), pede fs_driver() e monta a unidade com ops->mount. Sem
 * particao FAT32 nada daqui roda e nenhum DLL e lido.
 *
 * O driver roda no contexto do kernel (gcc -m64 -mno-sse -mno-red-zone, sem
 * libc) e nao importa nada de KERNEL: tudo que precisa vem em fsdrv_host.
 * Chamadas ao driver ja vem com o BKL do kernel; o driver so cede a CPU em
 * host->yield, e so com o estado dele consistente (outra chamada pode entrar
 * nesse meio tempo).
 *
 * Caminhos chegam sem letra e sem barra no comeco, separados por '\'
 * ("SRC\\MC\\MC.C"; "" e a raiz), em CP850. Nomes sem diferenca de caixa.
 */
#pragma once
#include <stdint.h>

#define FSDRV_ABI 1      /* fsdrv_host e as ops ate space */
#define FSDRV_ABI_MKDIR 2 /* ops->abi 2: tem mkdir (DLL antigo com 1 continua valendo) */
#define FSDRV_ABI_WRITE 3 /* ops->abi 3: tambem grava e apaga arquivos */
#define FSDRV_ABI_RENAME 4 /* ops->abi 4: tambem renomeia/move sem copiar */
#define FSDRV_SYM "fs_driver"

/* Igual a struct dir_item de drives.h (k_dirent dos programas). */
struct fsdrv_item {
  char name[32];      /* nome longo se cabe em 31; senao o apelido 8.3 */
  uint32_t size;
  uint16_t date;      /* formato FAT; 0 = sem data */
  uint16_t time;
  uint8_t dir;
};

struct fsdrv_host {
  uint32_t abi;
  /* n setores de 512 bytes a partir do LBA absoluto do disco; buf em
   * qualquer endereco. 0 ok. */
  int (*read)(uint32_t disk, uint32_t lba, uint32_t n, void* buf);
  int (*write)(uint32_t disk, uint32_t lba, uint32_t n, const void* buf);
  void* (*alloc)(uint32_t bytes);   /* memoria do kernel (nao some com programas) */
  void (*free)(void* p);
  void (*yield)(void);              /* deixa o desktop andar */
  void (*log)(const char* s);       /* serial */
};

struct fsdrv_ops {
  uint32_t abi;                     /* FSDRV_ABI */
  const char* name;                 /* "FAT32" */
  /* 1 se o setor de boot (512 bytes) e deste formato. */
  int (*probe)(const uint8_t* boot);
  /* Volume que comeca em lba no disco; 0 se nao deu. */
  void* (*mount)(const struct fsdrv_host* h, uint32_t disk, uint32_t lba);
  void (*umount)(void* vol);
  /* 0 arquivo, 1 pasta, -1 nao existe. size so para arquivo (pode ser 0). */
  int (*stat)(void* vol, const char* path, uint32_t* size);
  /* Ate len bytes a partir de off. Quantos leu (0 no fim); <0 erro. */
  int (*read)(void* vol, const char* path, uint32_t off, void* buf, uint32_t len);
  /* Itens da pasta (sem . e ..). Quantos; -1 se nao e pasta. */
  int (*items)(void* vol, const char* path, struct fsdrv_item* out, int max);
  /* Nome longo inteiro do ultimo nome de path. 0 achou. */
  int (*long_name)(void* vol, const char* path, char* out, int max);
  /* Espaco em KB. 0 ok. */
  int (*space)(void* vol, uint32_t* total_kb, uint32_t* free_kb);
  /* So com abi >= FSDRV_ABI_MKDIR. Cria a pasta (date/time no formato FAT,
   * 0 = sem data). 0 ok; 1 ja existe; -1 erro (pai nao existe, nome invalido,
   * existe arquivo com o nome); -2 disco cheio; -3 erro de disco. */
  int (*mkdir)(void* vol, const char* path, uint16_t date, uint16_t time);
  /* So com abi >= FSDRV_ABI_WRITE. Grava o arquivo inteiro (cria ou troca o
   * conteudo; os dados novos vao para clusters novos e so depois os antigos
   * sao liberados). 0 ok; -1 invalido (pai nao existe, e pasta, nome ruim);
   * -2 disco cheio; -3 erro de disco. */
  int (*write)(void* vol, const char* path, const void* data, uint32_t len, uint16_t date,
               uint16_t time);
  /* Apaga um arquivo (nao pasta). 0 ok; -1 nao existe; -3 e pasta; -4
   * somente leitura; -5 erro de disco. */
  int (*remove)(void* vol, const char* path);
  /* So com abi >= FSDRV_ABI_RENAME. Troca o nome e/ou a pasta sem copiar os
   * dados. Um to que ja existe (arquivo) e substituido; pasta so muda de nome
   * dentro da mesma pasta-mae. Mesmos codigos de drives_rename: 0 ok; -1 nao
   * existe (from ou a pasta de to); -2 nome invalido ou pasta/disco cheio;
   * -3 to e pasta (ou from e pasta e to existe); -4 somente leitura ou erro
   * de disco; -7 pasta para outra pasta. */
  int (*rename)(void* vol, const char* from, const char* to);
};

typedef const struct fsdrv_ops* (*fsdrv_entry_fn)(void);

/* ---- lado do kernel (fs/fsdrv.c) ---- */
/* 1 se o setor de boot e FAT32 (FATSz16 = 0 e FATSz32 != 0). */
int fsdrv_is_fat32(const uint8_t* boot);
/* Driver do formato (carrega o DLL na primeira vez). Ordem: a variavel de
 * ambiente (FAT32.DLL -> %FAT32DLL%, caminho completo), depois
 * X:\SODOS\DRV\<dll> em Z: (SODOS.PAK), D:, ... e U:. skip = unidade onde
 * nao procurar (a que esta sendo montada). 0 se nao ha DLL. Quem monta deve
 * chamar de novo quando env_gen() muda: o driver pode ter sido trocado. */
const struct fsdrv_ops* fsdrv_get(const char* dll, char skip);
/* Caminho do ultimo driver devolvido por fsdrv_get. */
const char* fsdrv_path(void);
const struct fsdrv_host* fsdrv_host(void);
