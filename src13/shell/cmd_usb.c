#include "shell.h"
#include "xhci.h"
#include "drives.h"

/* Liga os controladores xHCI, procura o pendrive e lista o que achou. */
int cmd_usb(int argc, char** argv) {
  static char out[1536];
  (void)argc;
  (void)argv;
  int L = drives_usb_letter();
  if (L) (void)xhci_disk_init();
  xhci_describe(out, (int)sizeof(out));
  puts_k(out);
  if (!L)
    puts_k("Pendrive desligado (SET PENDRIVE=0).\n");
  else if (xhci_disk_sectors()) {
    char m[32] = "Pendrive em X:  (dir X:)\n";
    m[12] = (char)L;
    m[21] = (char)L;
    puts_k(m);
  } else
    puts_k("Nenhum pendrive (porta do PC ou hub USB 2; FAT12/16/32).\n");
  return 0;
}
