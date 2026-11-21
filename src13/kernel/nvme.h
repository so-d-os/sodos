/* nvme.h - Particao FAT do SODOS num SSD NVMe, por polling.
 *
 * O driver liga o primeiro controlador NVMe do PCI (classe 01 08 02), le a
 * GPT do namespace 1 e procura uma particao "Microsoft basic data" cujo
 * setor de boot seja FAT com o rotulo SODOSHD. So essa particao aparece:
 * o disco DISK_NVME comeca no setor 0 dela e termina no fim dela, entao o
 * SODOS nao consegue ler nem gravar as outras particoes do SSD (o Linux,
 * o /home, a ESP...). */
#pragma once
#include <stdint.h>

/* 0 com a particao pronta (chamar de novo nao refaz nada). <0 sem NVMe,
 * sem GPT ou sem a particao SODOSHD; o motivo vai para a serial. */
int nvme_init(void);

/* Setores de 512 bytes da particao; 0 sem ela. */
uint32_t nvme_sectors(void);

/* Setor inicial da particao no SSD (so para mostrar). */
uint64_t nvme_part_lba(void);

/* Modelo do SSD, ex.: "NVMe SSD Controller MAP1202". */
const char* nvme_name(void);

/* count setores (1..256) a partir de lba, relativo a particao. 0 = ok.
 * Fora da particao devolve -1 sem tocar no SSD. */
int nvme_read(uint32_t lba, uint32_t count, void* buf);
int nvme_write(uint32_t lba, uint32_t count, const void* buf);

/* Leitura crua do namespace 1 inteiro (lba absoluto no SSD), so leitura.
 * Funciona mesmo sem particao SODOSHD (usa o primeiro SSD). E o que o
 * WSL.EXE usa para achar o NTFS do Windows. 0 ok; <0 erro. */
int nvme_raw_read(uint64_t lba, uint32_t count, void* buf);
/* Setores do namespace 1; 0 sem SSD. */
uint64_t nvme_raw_sectors(void);

/* Temperatura do SSD em graus C (log SMART); -1000 sem leitura. Com o BKL. */
int nvme_temp_c(void);
