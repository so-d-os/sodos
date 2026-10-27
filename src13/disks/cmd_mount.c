#include "shell.h"
#include "drives.h"

static int is_drive_letter(const char* s) {
  char c;
  if (!s || !s[0]) return 0;
  c = s[0];
  if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  if (c < 'A' || c > 'Z') return 0;
  if (s[1] == '\0') return 1;
  if (s[1] == ':' && s[2] == '\0') return 1;
  return 0;
}

static int is_ram_word(const char* s) {
  char a, b, c;
  if (!s || !s[0] || !s[1] || !s[2] || s[3] != '\0') return 0;
  a = s[0];
  b = s[1];
  c = s[2];
  if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
  if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
  if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
  return a == 'r' && b == 'a' && c == 'm';
}

int cmd_mount(int argc, char** argv) {
  char letter = 'D';
  uint32_t drive;
  uint32_t part = 0;
  int result;

  if (argc < 2) {
    drives_print_status();
    puts_k("Uso:\n");
    puts_k("  mount D:        disco RAM em D:\n");
    puts_k("  mount 1 D:      disco ATA 1 (disk.img) em D:\n");
    puts_k("  mount 1 D: 0    particao 0 do ATA 1 em D:\n");
    return 0;
  }

  if (is_drive_letter(argv[1]) || is_ram_word(argv[1])) {
    if (is_drive_letter(argv[1]))
      letter = argv[1][0];
    else if (argc >= 3 && is_drive_letter(argv[2]))
      letter = argv[2][0];
    if (letter == 'C' || letter == 'c') {
      puts_k("C: ja e RAM.\n");
      return 0;
    }
    result = drives_mount_ram(letter);
    if (result == 0) {
      putc(letter >= 'a' ? (char)(letter - 32) : letter);
      puts_k(": montado como disco RAM. Ex.: edit D:\\nota.txt\n");
    }
    return result;
  }

  drive = str_to_uint32(argv[1], 10);
  if (argc >= 3 && is_drive_letter(argv[2])) {
    letter = argv[2][0];
    if (argc >= 4) part = str_to_uint32(argv[3], 10);
  } else {
    /* Compat: "mount 0" = particao 0 do ATA 1 em D: */
    if (argc == 2 && drive < 4) {
      part = drive;
      drive = 1;
    }
    if (argc >= 3) part = str_to_uint32(argv[2], 10);
  }

  puts_k("Montando FAT em ");
  putc(letter >= 'a' ? (char)(letter - 32) : letter);
  puts_k(": ...\n");
  result = drives_mount_fat(drive, letter, part);
  if (result == 0) {
    puts_k("FAT montado. Ex.: dir D:   edit D:\\nota.txt\n");
    return 0;
  }
  if (result == -1)
    puts_k("Erro: boot sector invalido. Tente: mount D:  (RAM)\n");
  else if (result == -2)
    puts_k("Erro: FAT nao suportado (so FAT12/FAT16).\n");
  else
    puts_k("Erro ao montar. Tente: mount D:  (disco RAM)\n");
  return result;
}
