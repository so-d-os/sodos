#pragma once
#include <stdint.h>

/* Assinatura única em todo o projeto: (drive, lba, buffer). Nunca passar buffer NULL. */
int disk_read_sector(uint32_t drive, uint32_t lba, void* buffer);
int disk_write_sector(uint32_t drive, uint32_t lba, void* buffer);

/* Debug ATA: preenchidos em falha (timeout/ERR/DF). Comando dstat imprime. */
extern uint32_t last_ata_drive;
extern uint32_t last_ata_lba;
extern uint8_t last_ata_status;
extern uint8_t last_ata_error;
