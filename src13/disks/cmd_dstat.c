#include "shell.h"
#include "disk.h"

int cmd_dstat(int argc, char** argv) {
  (void)argc;
  (void)argv;
  puts_k("ATA last: drive=");
  char num[16];
  uint32_to_string(last_ata_drive, num, sizeof(num));
  puts_k(num);
  puts_k(" lba=");
  uint32_to_string(last_ata_lba, num, sizeof(num));
  puts_k(num);
  puts_k(" status=0x");
  {
    uint8_t v = last_ata_status;
    char h[3];
    h[0] = (char)((v >> 4) > 9 ? 'A' + (v >> 4) - 10 : '0' + (v >> 4));
    h[1] = (char)((v & 15) > 9 ? 'A' + (v & 15) - 10 : '0' + (v & 15));
    h[2] = '\0';
    puts_k(h);
  }
  puts_k(" error=0x");
  {
    uint8_t v = last_ata_error;
    char h[3];
    h[0] = (char)((v >> 4) > 9 ? 'A' + (v >> 4) - 10 : '0' + (v >> 4));
    h[1] = (char)((v & 15) > 9 ? 'A' + (v & 15) - 10 : '0' + (v & 15));
    h[2] = '\0';
    puts_k(h);
  }
  puts_k("\n");
  return 0;
}
