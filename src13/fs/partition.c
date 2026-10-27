// partition.c - Gerenciamento de partições MBR
#include <stdint.h>
#include "disk.h"
#include "trace.h"

extern void fdisk_log(const char*);

// Estrutura de entrada de partição MBR
struct partition_entry {
  uint8_t  status;
  uint8_t  chs_start[3];
  uint8_t  type;
  uint8_t  chs_end[3];
  uint32_t lba_start;
  uint32_t sector_count;
} __attribute__((packed));

// Estrutura MBR
struct mbr {
  uint8_t boot_code[446];
  struct partition_entry partitions[4];
  uint16_t signature;
} __attribute__((packed));

/*
 * Mapa de memória (evitar conflitos):
 *   0x10000+   kernel (.text/.data/.bss), tem de acabar antes de 0x58000
 *   0x58000    programa do asm/run (32KB) ate 0x60000; o boot larga 2KB de app aqui
 *   0x7C000    porta NET1 (20 bytes): connect, send, recv, close
 *   0x7E000    FAT bounce (512 bytes)
 *   0x80000    MBR buffer (I/O disco), 512 bytes
 *   0x80200    buffer saída fdisk list, 512 bytes
 *   0x80400    buffer verificação write_mbr, 512 bytes
 *   0x80600    buffer string drive (fdisk list), 32 bytes
 *   0x9FC00    stack
 *   0xB8000    VGA texto
 *   0x100000   tela em memoria do visual.c (ate 9 MB)
 *   0xA00000   FAT boot_sector, 0xA01000 fat_table (256 KB), 0xA41000 root_dir
 *   0xA51000   diretorio aberto (32 KB, fat.c)
 *   0xA80000   heap do kernel (kmalloc), 2 MB
 *   0x1000000  heap alto (umalloc): programas .EXE, .DLL e seus dados
 */
#define MBR_BUFFER_ADDR      0x00080000u
#define FDISK_LIST_OUT_BUF   0x00080200u
#define WRITE_MBR_VERIFY_BUF 0x00080400u
static struct mbr* const mbr_buf = (struct mbr*)MBR_BUFFER_ADDR;

// Lê MBR do disco para mbr_buf (0x80000). Sempre passa buffer válido, nunca NULL.
// Retorna 0 se MBR válido, -1 se MBR inválido ou falha de leitura.
int read_mbr(uint32_t drive, struct mbr* mbr) {
  TRACE_FUNC("read_mbr");
  (void)mbr;
  fdisk_log("[read_mbr] start\n");
  trace_event("read_mbr_args", drive, (uint32_t)MBR_BUFFER_ADDR, 0);
  fdisk_log("[read_mbr] calling disk_read_sector\n");
  if (disk_read_sector(drive, 0, mbr_buf) != 0) {
    fdisk_log("[read_mbr] disk_read_sector failed\n");
    return -1; /* falha de I/O ou buffer inválido */
  }
  fdisk_log("[read_mbr] disk_read_sector ok, check signature\n");
  if (mbr_buf->signature != 0xAA55) {
    fdisk_log("[read_mbr] bad signature\n");
    return -1; /* MBR inválido ou não existe */
  }
  fdisk_log("[read_mbr] ok\n");
  return 0;
}

// Cria MBR vazio
static void init_empty_mbr(struct mbr* mbr) {
  for (int i = 0; i < 446; i++) mbr->boot_code[i] = 0;
  for (int i = 0; i < 4; i++) {
    mbr->partitions[i].status = 0;
    mbr->partitions[i].type = 0;
    mbr->partitions[i].lba_start = 0;
    mbr->partitions[i].sector_count = 0;
    for (int j = 0; j < 3; j++) {
      mbr->partitions[i].chs_start[j] = 0;
      mbr->partitions[i].chs_end[j] = 0;
    }
  }
  mbr->signature = 0xAA55;
}

// Buffer fixo para verificação (evita I/O para stack)
static struct mbr* const verify_buf = (struct mbr*)WRITE_MBR_VERIFY_BUF;

// Escreve MBR no disco (mbr e verify_buf nunca NULL)
int write_mbr(uint32_t drive, struct mbr* mbr) {
  TRACE_FUNC("write_mbr");
  trace_event("write_mbr_args", drive, (uint32_t)mbr, 0);
  if (mbr == 0) return -1;
  mbr->signature = 0xAA55;
  if (disk_write_sector(drive, 0, mbr) != 0) return -1;
  for (volatile int i = 0; i < 10000; i++) { }
  if (disk_read_sector(drive, 0, verify_buf) != 0) return -1;
  if (verify_buf->signature != 0xAA55) {
    return -1; // Falha na escrita
  }
  if (mbr->partitions[0].type != 0 && verify_buf->partitions[0].type != mbr->partitions[0].type) {
    return -2; // Partição não foi escrita corretamente
  }
  return 0;
}

// Cria partição primária
int create_partition(uint32_t drive, uint8_t partition_num, uint32_t lba_start, uint32_t sector_count, uint8_t type) {
  TRACE_FUNC("create_part");
  trace_event("create_part_args", ((uint32_t)partition_num << 16) | type, lba_start, sector_count);
  if (partition_num >= 4) return -1;
  
  struct mbr* mbr = mbr_buf;
  if (read_mbr(drive, mbr) < 0) {
    init_empty_mbr(mbr);
  }
  if (mbr->partitions[partition_num].type != 0) {
    return -2; // Slot já ocupado
  }
  
  mbr->partitions[partition_num].status = 0x80;
  mbr->partitions[partition_num].type = type;
  mbr->partitions[partition_num].lba_start = lba_start;
  mbr->partitions[partition_num].sector_count = sector_count;
  uint32_t cylinder = lba_start / (63 * 16);
  uint32_t temp = lba_start % (63 * 16);
  uint32_t head = temp / 63;
  uint32_t sector = (temp % 63) + 1;
  mbr->partitions[partition_num].chs_start[0] = head;
  mbr->partitions[partition_num].chs_start[1] = ((cylinder >> 2) & 0xC0) | sector;
  mbr->partitions[partition_num].chs_start[2] = cylinder & 0xFF;
  uint32_t end_lba = lba_start + sector_count - 1;
  cylinder = end_lba / (63 * 16);
  temp = end_lba % (63 * 16);
  head = temp / 63;
  sector = (temp % 63) + 1;
  mbr->partitions[partition_num].chs_end[0] = head;
  mbr->partitions[partition_num].chs_end[1] = ((cylinder >> 2) & 0xC0) | sector;
  mbr->partitions[partition_num].chs_end[2] = cylinder & 0xFF;
  int result = write_mbr(drive, mbr);
  if (result < 0) {
    return -3; // Erro ao escrever MBR
  }
  
  return 0;
}

// Lista partições
int list_partitions(uint32_t drive, char* output, int max_output) {
  TRACE_FUNC("list_part");
  fdisk_log("[list_partitions] start\n");
  trace_event("list_part_args", drive, (uint32_t)max_output, (uint32_t)output);
  struct mbr* mbr = mbr_buf;
  fdisk_log("[list_partitions] calling read_mbr\n");
  if (read_mbr(drive, mbr) < 0) {
    fdisk_log("[list_partitions] read_mbr failed\n");
    // MBR não existe - mostra mensagem
    const char* msg = "No partitions found.\n";
    int j = 0;
    int pos = 0;
    while (msg[j] != '\0' && pos < max_output - 1) {
      output[pos++] = msg[j++];
    }
    output[pos] = '\0';
    return pos;
  }
  fdisk_log("[list_partitions] read_mbr ok, loop partitions\n");
  int pos = 0;
  int found = 0;
  for (int i = 0; i < 4 && pos < max_output - 64; i++) {
    if (mbr->partitions[i].type != 0) {
      found = 1;
      // Formata informação da partição
      static char line[64];
      int len = 0;

      // helpers locais para evitar overflow
      #define LINE_APPEND_CH(ch) do { if (len < (int)sizeof(line) - 1) line[len++] = (ch); } while (0)
      #define LINE_APPEND_STR(s) do { const char* _p = (s); while (*_p && len < (int)sizeof(line) - 1) line[len++] = *_p++; } while (0)
      
      // Número da partição
      LINE_APPEND_CH('0' + i);
      LINE_APPEND_CH(':');
      LINE_APPEND_CH(' ');
      
      // Tipo
      if (mbr->partitions[i].type == 0x01) {
        LINE_APPEND_STR("FAT12");
      } else if (mbr->partitions[i].type == 0x04 || mbr->partitions[i].type == 0x06) {
        LINE_APPEND_STR("FAT16");
      } else {
        LINE_APPEND_CH('0');
        LINE_APPEND_CH('x');
        uint8_t type = mbr->partitions[i].type;
        static const char hex[] = "0123456789ABCDEF";
        LINE_APPEND_CH(hex[(type >> 4) & 0xF]);
        LINE_APPEND_CH(hex[type & 0xF]);
      }
      
      LINE_APPEND_CH(' ');
      LINE_APPEND_CH('L');
      LINE_APPEND_CH('B');
      LINE_APPEND_CH('A');
      LINE_APPEND_CH(':');
      LINE_APPEND_CH(' ');
      
      // LBA start
      uint32_t lba = mbr->partitions[i].lba_start;
      static char num[16];
      int num_len = 0;
      if (lba == 0) {
        if (num_len < (int)sizeof(num) - 1) num[num_len++] = '0';
      } else {
        static char temp[16];
        int k = 0;
        while (lba > 0 && k < (int)sizeof(temp) - 1) {
          temp[k++] = (char)('0' + (lba % 10));
          lba /= 10;
        }
        for (int l = k - 1; l >= 0 && num_len < (int)sizeof(num) - 1; l--) {
          num[num_len++] = temp[l];
        }
      }
      num[num_len] = '\0';
      LINE_APPEND_STR(num);
      
      LINE_APPEND_CH('\n');
      line[len] = '\0';
      
      for (int j = 0; j < len && pos < max_output - 1; j++) {
        output[pos++] = line[j];
      }

      #undef LINE_APPEND_CH
      #undef LINE_APPEND_STR
    }
  }
  
  if (!found && pos < max_output - 32) {
    const char* msg = "No partitions found.\n";
    int j = 0;
    while (msg[j] != '\0' && pos < max_output - 1) {
      output[pos++] = msg[j++];
    }
  }
  
  if (pos >= max_output) pos = max_output - 1;
  output[pos] = '\0';
  fdisk_log("[list_partitions] done\n");
  return pos;
}

// Obtém informações de uma partição
int get_partition_info(uint32_t drive, uint8_t partition_num, uint32_t* lba_start, uint32_t* sector_count, uint8_t* type) {
  if (partition_num >= 4) return -1;
  
  struct mbr* mbr = mbr_buf;
  if (read_mbr(drive, mbr) < 0) return -1;
  if (mbr->partitions[partition_num].type == 0) return -2; // Partição não existe
  
  *lba_start = mbr->partitions[partition_num].lba_start;
  *sector_count = mbr->partitions[partition_num].sector_count;
  *type = mbr->partitions[partition_num].type;
  
  return 0;
}
