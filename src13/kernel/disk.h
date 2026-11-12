#pragma once
#include <stdint.h>

/* Drives 0..3 sao os ATA; DISK_USB e o pendrive do xHCI (kernel/xhci.c),
 * que aceita buffer em qualquer endereco. */
#define DISK_USB 0x80u

/* Assinatura única em todo o projeto: (drive, lba, buffer). Nunca passar buffer NULL. */
int disk_read_sector(uint32_t drive, uint32_t lba, void* buffer);
int disk_write_sector(uint32_t drive, uint32_t lba, void* buffer);
/* count setores seguidos (1..256) num so comando; o buffer pode ficar no heap. */
int disk_read_sectors(uint32_t drive, uint32_t lba, uint32_t count, void* buffer);
int disk_write_sectors(uint32_t drive, uint32_t lba, uint32_t count, const void* buffer);

/* Debug ATA: preenchidos em falha (timeout/ERR/DF). Comando dstat imprime. */
extern uint32_t last_ata_drive;
extern uint32_t last_ata_lba;
extern uint8_t last_ata_status;
extern uint8_t last_ata_error;
