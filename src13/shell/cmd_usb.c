#include "shell.h"
#include "xhci.h"

/* Liga os controladores xHCI, procura o pendrive e lista o que achou. */
int cmd_usb(int argc, char** argv) {
  static char out[1536];
  (void)argc;
  (void)argv;
  (void)xhci_disk_init();
  xhci_describe(out, (int)sizeof(out));
  puts_k(out);
  if (xhci_disk_sectors())
    puts_k("Pendrive em U:  (dir U:)\n");
  else
    puts_k("Nenhum pendrive (porta do PC ou hub USB 2; FAT12/FAT16).\n");
  return 0;
}
