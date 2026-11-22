/* pendrive.c - PENDRIVE.EXE: ver, montar e copiar do pendrive USB.
 *
 * O kernel so carrega este programa quando o comando PENDRIVE e digitado;
 * com SET PENDRIVE=0 nem isso (nem procura o pendrive). Comandos:
 *
 *   PENDRIVE                  estado e ajuda
 *   PENDRIVE SCAN             procura o pendrive: disco, particoes, rotulo, letra
 *   PENDRIVE MOUNT G:         monta (ou muda) o pendrive em G:
 *   PENDRIVE EJECT            solta o pendrive (nao monta sozinho de novo)
 *   PENDRIVE INFO             letra, espaco livre e total
 *   PENDRIVE DIR [pasta]      lista uma pasta do pendrive (padrao: a raiz)
 *   PENDRIVE COPY orig [dest] copia arquivo ou pasta inteira
 *
 * Em DIR e COPY um caminho sem letra e do pendrive: "PENDRIVE COPY FOTOS
 * D:\FOTOS" copia a pasta FOTOS do pendrive para D:. Sem dest, um caminho de
 * outra unidade vai para a raiz do pendrive e um do pendrive vai para a pasta
 * atual. A copia confere o tamanho gravado de cada arquivo.
 */
#include <stdio.h>
#include <string.h>
#include <so.h>

#define PATH_LEN 260
#define MAX_DIRENT 1024
#define MAX_DEPTH 16

static int letter;          /* letra do pendrive ('U'...), 0 sem ele */
static unsigned n_files, n_dirs, n_err;
static unsigned long long n_bytes;

static int up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static int same(const char* a, const char* b) {
  while (*a && *b && up(*a) == up(*b)) { a++; b++; }
  return *a == 0 && *b == 0;
}

static int has_drive(const char* p) {
  return p[0] && p[1] == ':' && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z'));
}

/* Letra atual; monta sozinho se puder. 0 sem pendrive montado. */
static int find_letter(int quiet) {
  int l = k_pendrive(0, 0, 0, 0);
  if (l > 0) return l;
  if (!quiet) {
    if (l == 0) printf("Pendrive desligado (SET PENDRIVE=0).\n");
    else printf("Nenhum pendrive montado em %c:. Tente: PENDRIVE SCAN\n", -l);
  }
  return 0;
}

/* "FOTOS" -> "U:\FOTOS"; "\X" -> "U:\X"; "D:\Y" fica. */
static void on_pen(const char* in, char* out) {
  if (has_drive(in)) {
    snprintf(out, PATH_LEN, "%s", in);
    return;
  }
  while (*in == '\\' || *in == '/') in++;
  snprintf(out, PATH_LEN, "%c:\\%s", letter, in);
}

static const char* leaf(const char* p) {
  const char* l = p;
  for (; *p; p++)
    if ((*p == '\\' || *p == '/' || *p == ':') && p[1]) l = p + 1;
  return l;
}

static void strip_slash(char* p) {
  int n = (int)strlen(p);
  while (n > 3 && (p[n - 1] == '\\' || p[n - 1] == '/')) p[--n] = 0;
}

static int is_dir(const char* p) {
  struct k_dirent one;
  if (strlen(p) == 2 && p[1] == ':') return 1;
  if (strlen(p) == 3 && p[1] == ':' && p[2] == '\\') return 1;
  return k_dir(p, &one, 1) >= 0;
}

static void show_size(unsigned long long b) {
  if (b >= 1024ull * 1024 * 1024) printf("%llu.%llu GB", b >> 30, ((b >> 20) & 1023) * 10 / 1024);
  else if (b >= 1024ull * 1024) printf("%llu.%llu MB", b >> 20, ((b >> 10) & 1023) * 10 / 1024);
  else if (b >= 1024) printf("%llu KB", b >> 10);
  else printf("%llu bytes", b);
}

static int cmd_scan(void) {
  static char out[4096];
  int l = k_pendrive(1, 0, out, (int)sizeof(out));
  printf("%s", out);
  return l > 0 ? 0 : 1;
}

static int cmd_mount(const char* arg) {
  int r;
  if (!arg || !arg[0] || (arg[1] && arg[1] != ':')) {
    printf("Uso: PENDRIVE MOUNT G:\n");
    return 1;
  }
  r = k_pendrive(2, up(arg[0]), 0, 0);
  if (r == 0) {
    printf("Pendrive em %c:  (PENDRIVE DIR, DIR %c:)\n", up(arg[0]), up(arg[0]));
    return 0;
  }
  if (r == -1) printf("Nenhum pendrive USB.\n");
  else if (r == -2) printf("O pendrive nao tem FAT12/16/32.\n");
  else if (r == -4) printf("Pendrive desligado (SET PENDRIVE=0).\n");
  else if (r == -5) printf("%c: ja e de outra unidade.\n", up(arg[0]));
  else if (r == -6) printf("Letra invalida (C: e Z: sao do sistema).\n");
  else printf("Nao montou (%d).\n", r);
  return 1;
}

static int cmd_info(void) {
  unsigned total = 0, freek = 0;
  if (!(letter = find_letter(0))) return 1;
  printf("Pendrive em %c:\n", letter);
  if (k_dfree(letter, &total, &freek) == 0) {
    printf("  livre ");
    show_size((unsigned long long)freek * 1024);
    printf(" de ");
    show_size((unsigned long long)total * 1024);
    printf("\n");
  }
  return 0;
}

static int cmd_dir(const char* arg) {
  char p[PATH_LEN];
  struct k_direntl* e; /* nome longo inteiro (k_dir_long) */
  int n, i;
  unsigned long long sum = 0;
  if (!(letter = find_letter(0))) return 1;
  on_pen(arg ? arg : "", p);
  strip_slash(p);
  e = (struct k_direntl*)k_malloc(MAX_DIRENT * (unsigned)sizeof(struct k_direntl));
  if (!e) return 1;
  n = k_dir_long(p, e, MAX_DIRENT);
  if (n < 0) {
    printf("Nao e pasta: %s\n", p);
    k_free(e);
    return 1;
  }
  printf("Pasta %s\n", p);
  for (i = 0; i < n; i++) {
    if (e[i].dir) {
      printf("  %-32s   <PASTA>\n", e[i].name);
    } else {
      printf("  %-32s %10u", e[i].name, e[i].size);
      if (e[i].date)
        printf("  %02u/%02u/%04u %02u:%02u", e[i].date & 31, (e[i].date >> 5) & 15,
               1980 + (e[i].date >> 9), e[i].time >> 11, (e[i].time >> 5) & 63);
      printf("\n");
      sum += e[i].size;
    }
  }
  printf("  %d itens, ", n);
  show_size(sum);
  printf("\n");
  k_free(e);
  return 0;
}

static int copy_file(const char* src, const char* dst) {
  int size = k_fsize(src), n;
  char* buf;
  if (size < 0) {
    printf("  nao achou %s\n", src);
    n_err++;
    return -1;
  }
  buf = (char*)k_malloc(size ? (unsigned)size : 1u);
  if (!buf) {
    printf("  sem memoria para %s (", src);
    show_size((unsigned long long)size);
    printf(")\n");
    n_err++;
    return -1;
  }
  n = size ? k_fread(src, 0, buf, size) : 0;
  if (n != size) {
    printf("  erro lendo %s\n", src);
    k_free(buf);
    n_err++;
    return -1;
  }
  if (k_fsave(dst, buf, size) != 0 || k_fsize(dst) != size) {
    printf("  erro gravando %s (disco cheio?)\n", dst);
    k_free(buf);
    n_err++;
    return -1;
  }
  k_free(buf);
  printf("  %s -> %s\n", src, dst);
  n_files++;
  n_bytes += (unsigned)size;
  return 0;
}

static void copy_tree(const char* src, const char* dst, int depth) {
  struct k_direntl* e; /* nome longo inteiro (k_dir_long) */
  int n, i;
  if (depth > MAX_DEPTH) return;
  if (k_mkdir(dst) < 0) {
    printf("  nao criou a pasta %s\n", dst);
    n_err++;
    return;
  }
  n_dirs++;
  e = (struct k_direntl*)k_malloc(MAX_DIRENT * (unsigned)sizeof(struct k_direntl));
  if (!e) return;
  n = k_dir_long(src, e, MAX_DIRENT);
  for (i = 0; i < n; i++) {
    char s[PATH_LEN], d[PATH_LEN];
    if (strlen(src) + strlen(e[i].name) + 2 >= PATH_LEN ||
        strlen(dst) + strlen(e[i].name) + 2 >= PATH_LEN) {
      n_err++;
      continue;
    }
    snprintf(s, PATH_LEN, "%s\\%s", src, e[i].name);
    snprintf(d, PATH_LEN, "%s\\%s", dst, e[i].name);
    if (e[i].dir) copy_tree(s, d, depth + 1);
    else copy_file(s, d);
    if (k_trygetc() == 27) {
      printf("parado (Esc)\n");
      break;
    }
  }
  k_free(e);
}

static int cmd_copy(const char* a, const char* b) {
  char src[PATH_LEN], dst[PATH_LEN];
  if (!(letter = find_letter(0))) return 1;
  if (!a) {
    printf("Uso: PENDRIVE COPY orig [dest]   (sem letra = no pendrive)\n");
    return 1;
  }
  on_pen(a, src);
  strip_slash(src);
  if (b) {
    on_pen(b, dst);
  } else if (up(src[0]) == letter) {
    char cwd[PATH_LEN];
    k_pwd(cwd, PATH_LEN);
    snprintf(dst, PATH_LEN, "%s", cwd);
  } else {
    snprintf(dst, PATH_LEN, "%c:\\", letter);
  }
  strip_slash(dst);
  /* destino e uma pasta que existe: vai para dentro dela com o mesmo nome */
  if (is_dir(dst)) {
    char t[PATH_LEN];
    int n = (int)strlen(dst);
    snprintf(t, PATH_LEN, "%s%s%s", dst, (dst[n - 1] == '\\' || dst[n - 1] == ':') ? "" : "\\", leaf(src));
    strcpy(dst, t);
  }
  if (same(src, dst)) {
    printf("Origem e destino iguais.\n");
    return 1;
  }
  n_files = n_dirs = n_err = 0;
  n_bytes = 0;
  if (is_dir(src)) copy_tree(src, dst, 0);
  else copy_file(src, dst);
  printf("%u arquivo(s), %u pasta(s), ", n_files, n_dirs);
  show_size(n_bytes);
  if (n_err) printf(", %u erro(s)", n_err);
  printf("\n");
  return n_err ? 1 : 0;
}

static void help(void) {
  int l = k_pendrive(0, 0, 0, 0);
  if (l > 0) printf("Pendrive montado em %c:\n", l);
  else if (l < 0) printf("Pendrive: letra %c:, nao montado agora\n", -l);
  else printf("Pendrive desligado (SET PENDRIVE=0)\n");
  printf("Uso:\n"
         "  PENDRIVE SCAN             procura: disco, particoes, rotulo, letra\n"
         "  PENDRIVE MOUNT G:         monta (ou muda) o pendrive em G:\n"
         "  PENDRIVE EJECT            solta o pendrive\n"
         "  PENDRIVE INFO             espaco livre e total\n"
         "  PENDRIVE DIR [pasta]      lista uma pasta do pendrive\n"
         "  PENDRIVE COPY orig [dest] copia arquivo ou pasta (sem letra = pendrive)\n"
         "  SET PENDRIVE=0            desliga tudo (nem procura o pendrive)\n");
}

int main(int argc, char** argv) {
  const char* c = argc > 1 ? argv[1] : "";
  if (!c[0] || same(c, "HELP") || same(c, "/?")) {
    help();
    return 0;
  }
  if (same(c, "SCAN")) return cmd_scan();
  if (same(c, "MOUNT")) return cmd_mount(argc > 2 ? argv[2] : 0);
  if (same(c, "EJECT") || same(c, "UMOUNT")) {
    if (k_pendrive(3, 0, 0, 0) == 0) {
      printf("Pendrive solto. Pode tirar. (PENDRIVE SCAN monta de novo)\n");
      return 0;
    }
    printf("Nenhum pendrive montado.\n");
    return 1;
  }
  if (same(c, "INFO")) return cmd_info();
  if (same(c, "DIR") || same(c, "LS")) return cmd_dir(argc > 2 ? argv[2] : 0);
  if (same(c, "COPY") || same(c, "CP"))
    return cmd_copy(argc > 2 ? argv[2] : 0, argc > 3 ? argv[3] : 0);
  printf("Comando desconhecido: %s\n", c);
  help();
  return 1;
}
