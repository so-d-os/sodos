/* main.c - WSL.EXE: o "WSL2" do SODOS.
 *
 * Le o Ubuntu do WSL2 direto do SSD do Windows (NTFS -> ext4.vhdx -> ext4),
 * so leitura, e sobe o mesmo Ubuntu numa VM AMD-V com o kernel do proprio
 * WSL (C:\Program Files\WSL\tools\kernel). Ver wsl/README.md.
 *
 *   WSL list | info | ls /x | cat /x | cp /x D:\DEST
 *   WSL run [-m MB] [-w MB] [-i INIT] [-k KERNEL] [-c "params"] [-t s] [-x "texto"] [-v] [-D PASTA]
 *
 * Sem Windows no PC (notebook so com Linux): a imagem propria em D:\WSL
 * (SET WSLDIR muda a pasta, -D so numa VM) - DEBIAN.IMG ou DEBIAN.000,
 * .001... (ext4), VMLINUZ e INITRD.IMG, feitos por wsl/scripts/build-image.sh
 * ou nova-imagem.sh - e a distro "SODOS-Debian", a padrao quando existe. -d
 * escolhe uma distro do Windows pelo nome. SET WSLOPT da opcoes ao servico. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sodos_api.h"
#include "wslcore.h"
#include "wslcli.h"
#include "vmm/vmm.h"
#include "vmm/nat.h"
#include "wslbox.h"

/* ---------- imagem propria (D:\WSL\DEBIAN.IMG) ---------- */
/* A imagem e um arquivo so (DEBIAN.IMG) ou partes DEBIAN.000, DEBIAN.001...
 * (as partes ganham, se existirem). Partes porque o k_fsize do SODOS devolve
 * int (arquivo < 2 GB) e porque o 9p grava o arquivo inteiro de uma vez: a
 * VM consegue gravar uma imagem nova em pedacos (wsl/scripts/nova-imagem.sh).
 * Cada parte, menos a ultima, tem tamanho multiplo de 2 MB (IMG_CHUNK): um
 * bloco nunca fica entre duas partes. */
#define IMG_NAME "SODOS-Debian"
#define IMG_CHUNK (2u << 20) /* le a imagem em blocos de 2 MB, guardados em RAM */
#define IMG_PARTS 64
static char img_dir[96];

static void img_path(char* out, int max, const char* file) {
  if (!img_dir[0] && k_getenv("WSLDIR", img_dir, (int)sizeof img_dir) <= 0)
    strcpy(img_dir, "D:\\WSL");
  snprintf(out, (size_t)max, "%s\\%s", img_dir, file);
}

static void img_part_name(char* out, int max, int i) {
  char f[16];
  snprintf(f, sizeof f, "DEBIAN.%03d", i);
  img_path(out, max, f);
}

/* 1 se a imagem esta em partes (DEBIAN.000 existe) */
static int img_split(void) {
  char p[128];
  img_part_name(p, sizeof p, 0);
  return k_fsize(p) > 0;
}

static int img_present(void) {
  char p[128];
  if (img_split()) return 1;
  img_path(p, sizeof p, "DEBIAN.IMG");
  return k_fsize(p) > 0;
}

/* a imagem e a distro padrao; -d escolhe outra pelo prefixo do nome */
static int img_wanted(const char* which) {
  const char* n = IMG_NAME;
  int i;
  if (!img_present()) return 0;
  if (!which || !which[0]) return 1;
  for (i = 0; which[i]; i++) {
    char a = which[i], b = n[i];
    if (!b) return 0;
    if (a >= 'a' && a <= 'z') a = (char)(a - 32);
    if (b >= 'a' && b <= 'z') b = (char)(b - 32);
    if (a != b) return 0;
  }
  return 1;
}

typedef struct {
  char path[128];              /* DEBIAN.IMG ou DEBIAN.000 (mensagens) */
  uint64_t size;               /* total */
  uint8_t** chunk;
  uint32_t n;
  int nparts;                  /* 1 = arquivo so */
  uint64_t part_off[IMG_PARTS];
  uint32_t part_size[IMG_PARTS];
} img_file;
static img_file g_img;

/* k_fread acha o arquivo a cada chamada: um bloco de 2 MB por vez e o bloco
 * fica (o ext4 le o mesmo lugar varias vezes no boot) */
static int img_read(void* ctx, uint64_t off, void* buf, uint32_t len) {
  img_file* f = (img_file*)ctx;
  uint8_t* o = (uint8_t*)buf;
  if (off + len > f->size) {
    wsl_seterr("imagem: leitura alem do fim de ", f->path);
    return -1;
  }
  while (len) {
    uint32_t ci = (uint32_t)(off / IMG_CHUNK), co = (uint32_t)(off % IMG_CHUNK);
    uint32_t k = IMG_CHUNK - co;
    if (k > len) k = len;
    if (!f->chunk[ci]) {
      uint64_t base = (uint64_t)ci * IMG_CHUNK;
      uint32_t want = f->size - base < IMG_CHUNK ? (uint32_t)(f->size - base) : IMG_CHUNK;
      char pp[128];
      const char* path = f->path;
      uint64_t poff = base;
      uint8_t* p;
      if (f->nparts > 1) {
        int i = f->nparts - 1;
        while (i > 0 && f->part_off[i] > base) i--;
        img_part_name(pp, sizeof pp, i);
        path = pp;
        poff = base - f->part_off[i];
      }
      p = (uint8_t*)k_hmalloc(IMG_CHUNK, 4096);
      if (!p) {
        wsl_seterr("imagem: sem memoria (SET HIMEM) para ", path);
        return -1;
      }
      if (k_fread(path, (unsigned)poff, (char*)p, (int)want) != (int)want) {
        k_hfree(p);
        wsl_seterr("imagem: erro lendo ", path);
        return -1;
      }
      f->chunk[ci] = p;
    }
    memcpy(o, f->chunk[ci] + co, k);
    o += k;
    off += k;
    len -= k;
  }
  return 0;
}

/* acha as partes DEBIAN.000... ou o DEBIAN.IMG e soma o tamanho */
static int img_scan(img_file* f) {
  int n;
  f->nparts = 0;
  f->size = 0;
  if (img_split()) {
    char p[128];
    int i;
    for (i = 0; i < IMG_PARTS; i++) {
      img_part_name(p, sizeof p, i);
      n = k_fsize(p);
      if (n <= 0) break;
      if (i > 0 && f->part_size[i - 1] % IMG_CHUNK) {
        img_part_name(p, sizeof p, i - 1);
        wsl_seterr("imagem: parte do meio com tamanho que nao e multiplo de 2 MB: ", p);
        return -1;
      }
      f->part_off[i] = f->size;
      f->part_size[i] = (uint32_t)n;
      f->size += (uint64_t)(uint32_t)n;
      f->nparts = i + 1;
    }
    img_part_name(f->path, sizeof f->path, 0);
    return 0;
  }
  img_path(f->path, sizeof f->path, "DEBIAN.IMG");
  n = k_fsize(f->path);
  if (n <= 0) {
    wsl_seterr("imagem nao encontrada: ", f->path);
    return -1;
  }
  f->nparts = 1;
  f->part_off[0] = 0;
  f->part_size[0] = (uint32_t)n;
  f->size = (uint64_t)(uint32_t)n;
  return 0;
}

static int img_open(wsl_io* out) {
  if (img_scan(&g_img)) return -1;
  g_img.n = (uint32_t)((g_img.size + IMG_CHUNK - 1) / IMG_CHUNK);
  if (!g_img.chunk) g_img.chunk = (uint8_t**)calloc(g_img.n, sizeof(uint8_t*));
  if (!g_img.chunk) {
    wsl_seterr("imagem: sem memoria", "");
    return -1;
  }
  out->read = img_read;
  out->ctx = &g_img;
  out->size = g_img.size;
  return 0;
}

/* Arquivo inteiro da pasta da imagem em RAM alta. 0 se faltar. */
static uint8_t* img_load(const char* file, uint32_t* len) {
  char p[128];
  int n;
  uint8_t* b;
  img_path(p, sizeof p, file);
  n = k_fsize(p);
  if (n <= 0) {
    printf("WSL: %s nao encontrado\n", p);
    return 0;
  }
  /* o k_fload guarda 1 byte para o NUL: com max == tamanho le um a menos */
  if (!(b = (uint8_t*)k_hmalloc((uint64_t)n + 1, 4096))) {
    printf("WSL: sem memoria alta para %s (%d KB; SET HIMEM)\n", p, n >> 10);
    return 0;
  }
  {
    int got = k_fload(p, (char*)b, n + 1);
    if (got != n) {
      printf("WSL: erro lendo %s (%d de %d bytes)\n", p, got, n);
      k_hfree(b);
      return 0;
    }
  }
  *len = (uint32_t)n;
  return b;
}

/* "wsl --list [--verbose]" como o do Windows: UTF-16LE (o Claude Desktop e
 * outros programas do Windows leem assim). */
static void put16(const char* s) {
  while (*s) {
    putchar(*s == '\n' ? '\r' : *s);
    putchar(0);
    if (*s == '\n') {
      putchar('\n');
      putchar(0);
    }
    s++;
  }
}
static int cmd_winlist(int verbose) {
  if (!img_present()) {
    put16("O Subsistema do Windows para Linux nao tem distribuicoes instaladas.\n");
    return 1;
  }
  if (verbose) {
    put16("  NAME            STATE           VERSION\n");
    put16("* " IMG_NAME "    Stopped         2\n");
  } else {
    put16("Windows Subsystem for Linux Distributions:\n");
    put16(IMG_NAME " (Default)\n");
  }
  fflush(stdout);
  return 0;
}

static int img_list(void) {
  char p[128];
  img_file f;
  if (!img_present()) return 1;
  memset(&f, 0, sizeof f);
  if (img_scan(&f)) {
    printf("Imagem propria: %s\n", wsl_err);
    return 1;
  }
  if (f.nparts > 1)
    printf("Imagem propria: %s (padrao)\n  %s e mais %d partes, %llu MB\n", IMG_NAME, f.path, f.nparts - 1,
           (unsigned long long)(f.size >> 20));
  else
    printf("Imagem propria: %s (padrao)\n  %s, %llu MB\n", IMG_NAME, f.path, (unsigned long long)(f.size >> 20));
  img_path(p, sizeof p, "VMLINUZ");
  printf("  kernel %s %s\n", p, k_fsize(p) > 0 ? "" : "(FALTA)");
  img_path(p, sizeof p, "INITRD.IMG");
  printf("  initrd %s %s\n", p, k_fsize(p) > 0 ? "" : "(FALTA)");
  return 0;
}

/* info/ls/cat/cp na imagem */
static int img_read_cmd(int argc, char** argv, wsl_save_fn save) {
  wsl_io io;
  ext4_fs* fs;
  if (img_open(&io)) {
    printf("WSL: %s\n", wsl_err);
    return 1;
  }
  fs = ext4_mount(&io);
  if (!fs) {
    printf("WSL: ext4 da imagem ilegivel: %s\n", wsl_err);
    return 1;
  }
  if (!strcmp(argv[1], "info")) {
    ext4_inode in;
    printf("Distro %s\n  %s, ext4 \"%s\", %llu MB\n", IMG_NAME, g_img.path, ext4_label(fs),
           (unsigned long long)(ext4_bytes(fs) >> 20));
    if (ext4_lookup(fs, "/etc/os-release", 1, &in) == 0 && in.size < 4096) {
      char* b = (char*)malloc((size_t)in.size + 1);
      if (b && ext4_pread(fs, &in, 0, b, (uint32_t)in.size) == 0) {
        char* q;
        b[in.size] = 0;
        q = strstr(b, "PRETTY_NAME=");
        if (q) {
          char* e = strchr(q, '\n');
          if (e) *e = 0;
          printf("  %s\n", q);
        }
      }
      free(b);
    }
    return 0;
  }
  return wsl_cli_ext4(argc, argv, fs, save);
}

/* ---------- caixa de comandos (SET WSL=1) ---------- */
/* A caixa do servico: a mesma memoria a cada volta do servico (k_malloc_shared
 * nao tem dono); publicada no kernel como "WSL". */
static struct wslbox* box_get(int create) {
  struct wslbox* b = (struct wslbox*)k_svc_get("WSL");
  if (b && b->magic == WSLB_MAGIC && b->version == WSLB_VERSION) return b;
  if (!create) return 0;
  b = (struct wslbox*)k_malloc_shared((unsigned)sizeof *b);
  if (!b) return 0;
  memset(b, 0, sizeof *b);
  b->magic = WSLB_MAGIC;
  b->version = WSLB_VERSION;
  k_svc_set("WSL", b);
  return b;
}

static void ring_drain(struct wslb_ring* r, FILE* f) {
  uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE), t = r->tail;
  if (h == t) return;
  while (t != h) {
    uint32_t off = t % WSLB_RING, n = h - t;
    if (n > WSLB_RING - off) n = WSLB_RING - off;
    fwrite(r->buf + off, 1, n, f);
    t += n;
  }
  __atomic_store_n(&r->tail, t, __ATOMIC_RELEASE);
  fflush(f);
}

/* Pasta do SODOS -> Linux: X:\a\b -> /x/a/b, como no Git Bash (o Linux da
 * VM monta cada unidade em /x; o agente usa /root se nao existir). */
static void cwd_linux(char* out, int max) {
  char w[260];
  int i, o = 0;
  out[0] = 0;
  k_pwd(w, (int)sizeof w);
  if (!(w[0] && w[1] == ':')) return;
  o = snprintf(out, (size_t)max, "/%c", (char)(w[0] | 0x20));
  for (i = 2; w[i] && o < max - 1; i++) out[o++] = w[i] == '\\' ? '/' : w[i];
  out[o] = 0;
  if (o > 1 && out[o - 1] == '/') out[o - 1] = 0;
}

/* Caminhos do Windows no comando -> Linux: o Claude Code passa
 * 'D:\\WINE\\USERS\\...\\snapshot.sh' e TEMP='D:\\WINE\\TEMP' (o Git Bash entende).
 * X:\\a\\b (ou X:/a/b), sem letra/numero colado antes, vira /x/a/b; as
 * barras invertidas do caminho viram / ate espaco, aspas, ; | & ) ou o fim. */
static char* win2lin(const char* in, uint32_t* len) {
  uint32_t n = *len, i, o = 0;
  char* out = (char*)malloc((size_t)n * 2 + 16);
  if (!out) return 0;
  for (i = 0; i < n; i++) {
    char c = in[i];
    int start = i + 2 < n && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) && in[i + 1] == ':' &&
                (in[i + 2] == '\\' || in[i + 2] == '/');
    if (start && i > 0) {
      char p = in[i - 1];
      if ((p >= 'A' && p <= 'Z') || (p >= 'a' && p <= 'z') || (p >= '0' && p <= '9') || p == '_' || p == '\\' || p == '/')
        start = 0;
    }
    if (!start) {
      out[o++] = c;
      continue;
    }
    out[o++] = '/'; /* /d/..., como no Git Bash (o Claude Code tambem usa) */
    out[o++] = (char)(c | 0x20);
    out[o++] = '/';
    for (i += 3; i < n; i++) {
      char d = in[i];
      if (d == ' ' || d == '\'' || d == '"' || d == ';' || d == '|' || d == '&' || d == ')' || d == '\n' ||
          d == '\t') {
        i--;
        break;
      }
      out[o++] = d == '\\' ? '/' : d;
    }
  }
  out[o] = 0;
  *len = o;
  return out;
}

/* Roda cmd no Linux da VM e devolve o codigo de saida (255: a VM caiu). */
static int client_run(const char* cmd, uint32_t len, int flags) {
  static volatile int nap;
  struct wslbox* b;
  struct wslb_slot* s = 0;
  unsigned t0 = k_ms();
  int i, code;
  char v[8];
  if (len > WSLB_CMD_MAX) {
    fprintf(stderr, "WSL: comando grande demais (%u bytes)\n", len);
    return 255;
  }
  b = box_get(0);
  if ((!b || !b->alive) && (k_getenv("WSL", v, (int)sizeof v) <= 0 || v[0] != '1')) {
    /* liga o servico (tela Ctrl+F10) e espera o Linux subir */
    k_setenv("WSL", "1");
  }
  while (!(b = box_get(0)) || !b->alive || !b->ready) {
    if (k_ms() - t0 > 120000u) {
      fprintf(stderr, "WSL: a VM nao ficou pronta em 120 s (veja a tela Ctrl+F10)\n");
      return 255;
    }
    k_futex_wait(&nap, 0, 50);
  }
  while (!s) {
    for (i = 0; i < WSLB_SLOTS && !s; i++)
      if (__sync_bool_compare_and_swap(&b->slot[i].state, SL_FREE, SL_CLAIM)) s = &b->slot[i];
    if (!s) {
      if (k_ms() - t0 > 120000u) {
        fprintf(stderr, "WSL: todas as vagas da VM ocupadas\n");
        return 255;
      }
      k_futex_wait(&nap, 0, 20);
    }
  }
  s->cmd = (char*)k_malloc_shared(len + 1);
  if (!s->cmd) {
    s->state = SL_FREE;
    fprintf(stderr, "WSL: sem memoria para o comando\n");
    return 255;
  }
  memcpy(s->cmd, cmd, len);
  s->cmd[len] = 0;
  s->cmd_len = len;
  s->flags = flags;
  cwd_linux(s->cwd, WSLB_CWD);
  s->out.head = s->out.tail = s->err.head = s->err.tail = 0;
  s->kill = s->killed = 0;
  s->exit_code = 0;
  s->gen++;
  __atomic_store_n(&s->state, SL_POSTED, __ATOMIC_RELEASE);
  if (b->wake) k_futex_wake(b->wake, 1);
  for (;;) {
    int seq = s->seq;
    ring_drain(&s->out, stdout);
    ring_drain(&s->err, stderr);
    if (s->state == SL_DONE) break;
    if (!b->alive) {
      s->exit_code = 255;
      break;
    }
    k_futex_wait(&s->seq, seq, 50);
  }
  ring_drain(&s->out, stdout);
  ring_drain(&s->err, stderr);
  code = s->exit_code;
  k_free_shared(s->cmd);
  s->cmd = 0;
  __atomic_store_n(&s->state, SL_FREE, __ATOMIC_RELEASE);
  return code;
}

/* Junta argv[from..] num comando so (com aspas simples quando precisa). */
static char* join_args(int argc, char** argv, int from, uint32_t* len) {
  size_t n = 1;
  int i;
  char* out;
  for (i = from; i < argc; i++) n += strlen(argv[i]) * 4 + 3;
  out = (char*)malloc(n);
  if (!out) return 0;
  out[0] = 0;
  for (i = from; i < argc; i++) {
    const char* a = argv[i];
    int plain = *a != 0;
    const char* q;
    for (q = a; *q; q++)
      if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || strchr("-_./=:,+@%", *q)))
        plain = 0;
    if (i > from) strcat(out, " ");
    if (plain) {
      strcat(out, a);
    } else {
      char* o = out + strlen(out);
      *o++ = '\'';
      for (q = a; *q; q++) {
        if (*q == '\'') {
          memcpy(o, "'\\''", 4);
          o += 4;
        } else {
          *o++ = *q;
        }
      }
      *o++ = '\'';
      *o = 0;
    }
  }
  *len = (uint32_t)strlen(out);
  return out;
}

/* BASH.EXE (o WSL.EXE copiado com esse nome, para o Claude Code):
 * bash [-l|--login] [-i] -c "comando" [$0 ...] */
static int bash_main(int argc, char** argv) {
  int i, flags = 0;
  const char* cmd = 0;
  for (i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (!strcmp(a, "--login")) {
      flags |= WSLB_LOGIN;
    } else if (a[0] == '-' && a[1] && a[1] != '-') {
      const char* q;
      int want_c = 0;
      for (q = a + 1; *q; q++) {
        if (*q == 'l') flags |= WSLB_LOGIN;
        else if (*q == 'c') want_c = 1;
      }
      if (want_c && !cmd) {
        /* o comando e o proximo argumento que nao e opcao */
        int j;
        for (j = i + 1; j < argc; j++) {
          if (argv[j][0] == '-' && argv[j][1] && !cmd && (!strcmp(argv[j], "-l") || !strcmp(argv[j], "--login") || !strcmp(argv[j], "-i"))) {
            if (strcmp(argv[j], "-i")) flags |= WSLB_LOGIN;
            continue;
          }
          cmd = argv[j];
          i = j;
          break;
        }
      }
    } else if (!cmd) {
      break; /* bash script.sh: nao suportado */
    }
  }
  if (!cmd) {
    fprintf(stderr, "bash (SODOS): use bash -c \"comando\"; o console do Linux fica no Ctrl+F10 (SET WSL=1)\n");
    return 2;
  }
  {
    uint32_t n = (uint32_t)strlen(cmd);
    char* t = win2lin(cmd, &n);
    int rc = client_run(t ? t : cmd, t ? n : (uint32_t)strlen(cmd), flags);
    free(t);
    return rc;
  }
}

static int is_bash_exe(const char* p) {
  size_t n = p ? strlen(p) : 0;
  const char* t = "BASH.EXE";
  size_t i;
  if (n < 8 || (n > 8 && p[n - 9] != '\\' && p[n - 9] != '/' && p[n - 9] != ':')) return 0;
  for (i = 0; i < 8; i++) {
    char c = p[n - 8 + i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c != t[i]) return 0;
  }
  return 1;
}

static int nvme_rd(void* ctx, uint64_t lba, uint32_t count, void* buf) {
  (void)ctx;
  return k_nvme_raw_read(lba, count, buf) ? -1 : 0;
}

static int save_local(const char* path, const void* data, uint32_t len) {
  return k_fsave(path, (const char*)data, (int)len) < 0 ? -1 : 0;
}

static int exists_ext4(ext4_fs* fs, const char* p) {
  ext4_inode in;
  return fs && ext4_lookup(fs, p, 1, &in) == 0;
}

/* 1 se o arquivo e um script (#!). O /init das distros WSL e o binario da
 * Microsoft (precisa de Hyper-V/vsock): so usamos /init se for script. */
static int is_script(ext4_fs* fs, const char* p) {
  ext4_inode in;
  char b[2];
  return fs && ext4_lookup(fs, p, 1, &in) == 0 && in.size > 2 &&
         ext4_pread(fs, &in, 0, b, 2) == 0 && b[0] == '#' && b[1] == '!';
}

/* SET WSLOPT=-m 2048 -w 1024: opcoes a mais para o "run --service" (o kernel
 * sobe o servico com a linha fixa "WSL.EXE run --service"). Vao depois das
 * da linha; aspas juntam um argumento com espacos (-c "a b"). */
static int run_wslopt(int argc, char*** argvp) {
  static char buf[512];
  static char* nv[96];
  char** argv = *argvp;
  char* s;
  int i, n = 0, service = 0;
  for (i = 2; i < argc; i++)
    if (!strcmp(argv[i], "--service")) service = 1;
  if (!service || argc >= 64 || k_getenv("WSLOPT", buf, (int)sizeof buf) <= 0) return argc;
  for (i = 0; i < argc; i++) nv[n++] = argv[i];
  s = buf;
  while (*s && n < (int)(sizeof nv / sizeof nv[0]) - 1) {
    char* t;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) break;
    if (*s == '"') {
      t = ++s;
      while (*s && *s != '"') s++;
    } else {
      t = s;
      while (*s && *s != ' ' && *s != '\t') s++;
    }
    if (*s) *s++ = 0;
    nv[n++] = t;
  }
  nv[n] = 0;
  if (n > argc) {
    printf("WSL: SET WSLOPT=%s\n", buf);
    *argvp = nv;
  }
  return n;
}

static int cmd_run(int argc, char** argv, const wsl_io* disk) {
  vmm_config cfg;
  wsl_stack st;
  wsl_distro info;
  const char* which = 0;
  const char* init = 0;
  const char* kpath = 0;
  const char* extra = "";
  static char cmdline[2048];
  uint8_t* kbuf = 0;
  uint64_t ksize = 0;
  int i, rc, nonet = 0;
  static char ipp[160];
  uint32_t dns = 0;
  memset(&cfg, 0, sizeof cfg);
  cfg.ram_mb = 1024;
  cfg.cow_mb = 512;
  cfg.cpu = -1;
  argc = run_wslopt(argc, &argv);
  for (i = 2; i < argc; i++) {
    const char* a = argv[i];
    const char* v = i + 1 < argc ? argv[i + 1] : 0;
    if (!strcmp(a, "-m") && v) cfg.ram_mb = (uint32_t)atoi(v), i++;
    else if (!strcmp(a, "-D") && v) {
      /* pasta da imagem so nesta VM (testar uma imagem nova sem SET WSLDIR) */
      size_t l;
      snprintf(img_dir, sizeof img_dir, "%s", v);
      l = strlen(img_dir);
      while (l > 3 && img_dir[l - 1] == '\\') img_dir[--l] = 0;
      i++;
    }
    else if (!strcmp(a, "-w") && v) cfg.cow_mb = (uint32_t)atoi(v), i++;
    else if (!strcmp(a, "-i") && v) init = v, i++;
    else if (!strcmp(a, "-k") && v) kpath = v, i++;
    else if (!strcmp(a, "-c") && v) extra = v, i++;
    else if (!strcmp(a, "-t") && v) cfg.timeout_s = (uint32_t)atoi(v), i++;
    else if (!strcmp(a, "-x") && v) {
      /* \n \r \t \\ como no C (a linha do shell nao tem quebra) */
      static char inp[1024];
      int o = 0;
      const char* s = v;
      while (*s && o < (int)sizeof inp - 1) {
        if (s[0] == '\\' && s[1]) {
          char e = s[1];
          inp[o++] = e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e;
          s += 2;
        } else {
          inp[o++] = *s++;
        }
      }
      inp[o] = 0;
      cfg.input = inp;
      i++;
    }
    else if (!strcmp(a, "-a") && v) cfg.input_after = v, i++;
    else if (!strcmp(a, "-d") && v) which = v, i++;
    else if (!strcmp(a, "-p") && v) cfg.cpu = atoi(v), i++;
    else if (!strcmp(a, "-v")) cfg.verbose = 1;
    else if (!strcmp(a, "-n")) nonet = 1;
    else if (!strcmp(a, "--service")) cfg.service = 1;
    else {
      printf("WSL run: opcao desconhecida %s\n", a);
      return 1;
    }
  }
  {
    char why[128];
    if (vmm_check(why, sizeof why)) {
      printf("WSL: %s\n", why);
      return 1;
    }
  }
  if (img_wanted(which)) {
    /* imagem propria: kernel e initrd da pasta, init /sbin/sodos-init (monta
     * o basico, rede pelo sodos.ip=, bash no ttyS0, desliga ao sair) */
    int cols = 80, rows = 25;
    if (img_open(&cfg.disk)) {
      printf("WSL: %s\n", wsl_err);
      return 1;
    }
    if (kpath && kpath[0] && kpath[1] == ':') {
      int n = k_fsize(kpath);
      if (n <= 0 || !(kbuf = (uint8_t*)k_hmalloc((uint64_t)n + 1, 4096)) || k_fload(kpath, (char*)kbuf, n + 1) != n) {
        printf("WSL: nao consegui ler %s\n", kpath);
        return 1;
      }
      ksize = (uint64_t)n;
    } else {
      uint32_t kl = 0;
      kbuf = img_load("VMLINUZ", &kl);
      ksize = kl;
    }
    cfg.initrd = img_load("INITRD.IMG", &cfg.initrd_len);
    if (!kbuf || !cfg.initrd) {
      printf("WSL: falta VMLINUZ ou INITRD.IMG em %s\n", img_dir);
      return 1;
    }
    printf("WSL: distro %s (%s, %llu MB), kernel %llu KB, initrd %u KB\n", IMG_NAME, g_img.path,
           (unsigned long long)(g_img.size >> 20), (unsigned long long)(ksize >> 10), cfg.initrd_len >> 10);
    ipp[0] = 0;
    if (!nonet) {
      if (nat_init(&dns) == 0) {
        if (!dns) dns = 0x08080808u;
        cfg.net = 1;
        snprintf(ipp, sizeof ipp, "sodos.ip=10.0.3.15/24,10.0.3.2,%u.%u.%u.%u", dns >> 24, (dns >> 16) & 255,
                 (dns >> 8) & 255, dns & 255);
      } else {
        printf("WSL: o SODOS esta sem rede (sem placa ou sem IP): a VM sobe sem rede\n");
      }
    }
    k_textsize(&cols, &rows);
    if (cfg.service) {
      /* WSL=1: caixa de comandos pela segunda serial; o bash do console volta
       * quando sai (sodos.service=1) e a VM so para com SET WSL=0 */
      cfg.box = box_get(1);
      if (cfg.box && cfg.box->alive) {
        printf("WSL: o servico ja esta rodando (Ctrl+F10)\n");
        return 1;
      }
      cfg.timeout_s = 0;
    }
    snprintf(cmdline, sizeof cmdline,
             "console=ttyS0,115200 nolapic noapic acpi=off nokaslr tsc=reliable mitigations=off nosmp "
             "maxcpus=1 nr_cpus=1 panic=-1 pci=conf1,nomsi root=/dev/vda rw rootfstype=ext4 quiet "
             "sodos.rows=%d sodos.cols=%d %s%s %s init=%s",
             rows, cols, ipp, cfg.box ? " sodos.service=1" : "", extra, init ? init : "/sbin/sodos-init");
    cfg.kernel = kbuf;
    cfg.kernel_len = (uint32_t)ksize;
    cfg.cmdline = cmdline;
    return vmm_run(&cfg);
  }
  if (!disk) {
    printf("WSL: sem a imagem %s\\DEBIAN.IMG e sem SSD NVMe com o Windows\n", img_dir[0] ? img_dir : "D:\\WSL");
    return 1;
  }
  if (wsl_open_stack(disk, which, &st, &info)) {
    printf("WSL: %s\n", wsl_err);
    return 1;
  }
  printf("WSL: distro %s (usuario %s), disco virtual de %llu MB\n", info.name, info.user,
         (unsigned long long)(vhdx_size(st.vd) >> 20));
  if (vhdx_dirty(st.vd))
    printf("WSL: aviso: o vhdx tem log pendente (o Windows nao fechou o WSL); o ext4 pode ter\n"
           "     journal por reaplicar, o Linux faz isso na camada de escrita em RAM.\n");

  /* kernel: arquivo local (-k D:\...) ou o do WSL no NTFS */
  if (kpath && kpath[0] && kpath[1] == ':') {
    int n = k_fsize(kpath);
    if (n <= 0 || !(kbuf = (uint8_t*)k_hmalloc((uint64_t)n + 1, 4096)) ||
        k_fload(kpath, (char*)kbuf, n + 1) != n) {
      printf("WSL: nao consegui ler %s\n", kpath);
      return 1;
    }
    ksize = (uint64_t)n;
  } else {
    const char* np = kpath ? kpath : wsl_find_kernel(st.ntfs);
    ntfs_file* kf = np ? ntfs_open(st.ntfs, np) : 0;
    if (!kf) {
      printf("WSL: kernel do WSL nao encontrado (%s); use -k\n", np ? wsl_err : "Program Files\\WSL\\tools\\kernel");
      return 1;
    }
    ksize = ntfs_fsize(kf);
    if (ksize > (128u << 20) || !(kbuf = (uint8_t*)k_hmalloc(ksize, 4096)) ||
        ntfs_pread(kf, 0, kbuf, (uint32_t)ksize)) {
      printf("WSL: erro lendo o kernel: %s\n", wsl_err);
      return 1;
    }
    printf("WSL: kernel %s (%llu KB)\n", np, (unsigned long long)(ksize >> 10));
    ntfs_close(kf);
  }

  /* Rede: NAT para o IP do SODOS. A VM e 10.0.3.15 (ip= do kernel, sem
   * cliente DHCP), o roteador 10.0.3.2 e o DNS o mesmo do SODOS. */
  ipp[0] = 0;
  if (!nonet) {
    if (nat_init(&dns) == 0) {
      if (!dns) dns = 0x08080808u;
      cfg.net = 1;
      snprintf(ipp, sizeof ipp, "ip=10.0.3.15::10.0.3.2:255.255.255.0:sodos-wsl:eth0:off:%u.%u.%u.%u",
               dns >> 24, (dns >> 16) & 255, (dns >> 8) & 255, dns & 255);
    } else {
      printf("WSL: o SODOS esta sem rede (sem placa ou sem IP): a VM sobe sem rede\n");
    }
  }

  if (!init) {
    if (is_script(st.fs, "/init")) init = "/init";
    else if (exists_ext4(st.fs, "/bin/bash")) init = "/bin/bash";
    else init = "/bin/sh";
  }
  {
    /* Shell como init: monta o basico antes (o "login" do WSL e direto como
     * root). Os argumentos depois de "--" vao para o init. */
    static char sh_args[1024];
    size_t il = strlen(init);
    int cols = 80, rows = 25;
    k_textsize(&cols, &rows);
    sh_args[0] = 0;
    /* O shell nao e o PID 1: roda como filho, com o ttyS0 como terminal de
     * controle (Ctrl+C, job control), e quando ele sai a VM desliga. Se o
     * init morresse o Linux daria "Attempted to kill init" (panic). */
    if ((il >= 5 && !strcmp(init + il - 5, "/bash")) || (il >= 3 && !strcmp(init + il - 3, "/sh")))
      snprintf(sh_args, sizeof sh_args,
               " -- -c \"mount -t proc proc /proc;mount -t sysfs sys /sys;"
               "mount -t devtmpfs dev /dev 2>/dev/null;mkdir -p /dev/pts /run;"
               "mount -t devpts devpts /dev/pts;mount -t tmpfs tmpfs /run;mount -t tmpfs tmpfs /tmp;"
               "hostname sodos-wsl;export HOME=/root TERM=linux;cd /root;"
               "rm -f /etc/resolv.conf;grep nameserver /proc/net/pnp >/etc/resolv.conf 2>/dev/null;"
               "stty rows %d cols %d </dev/ttyS0 2>/dev/null;"
               "if setsid --help 2>&1 | grep -q -- --wait;"
               "then setsid -w -c $0 -l <>/dev/ttyS0 >&0 2>&0;else $0 -l;fi;"
               "sync;poweroff -ff 2>/dev/null;poweroff -f\"",
               rows, cols);
    snprintf(cmdline, sizeof cmdline,
             "console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 nolapic noapic acpi=off "
             "nokaslr tsc=reliable mitigations=off nosmp maxcpus=1 nr_cpus=1 panic=-1 "
             "pci=conf1,nomsi root=/dev/vda rw rootfstype=ext4 rootwait %s %s init=%s%s",
             ipp, extra, init, sh_args);
  }
  cfg.kernel = kbuf;
  cfg.kernel_len = (uint32_t)ksize;
  cfg.cmdline = cmdline;
  cfg.disk = st.disk;
  rc = vmm_run(&cfg);
  wsl_close_stack(&st);
  return rc;
}

int main(int argc, char** argv) {
  wsl_io disk;
  uint64_t secs;
  const char* which = 0;
  int i, verbose = 0, winlist = 0;
  if (argc >= 1 && is_bash_exe(argv[0])) return bash_main(argc, argv);
  /* WSL exec cmd... | WSL -e cmd... | WSL -- cmd... (como o wsl.exe do Windows) */
  if (argc >= 3 && (!strcmp(argv[1], "exec") || !strcmp(argv[1], "-e") || !strcmp(argv[1], "--exec") ||
                    !strcmp(argv[1], "--"))) {
    uint32_t n = 0;
    char* c = join_args(argc, argv, 2, &n);
    int rc = c ? client_run(c, n, 0) : 255;
    free(c);
    return rc;
  }
  if (argc >= 2 && !strcmp(argv[1], "status")) {
    struct wslbox* b = box_get(0);
    int i;
    static const char* const st[] = {"livre", "pegando", "na fila", "rodando", "pronto"};
    if (!b) {
      printf("WSL: servico nunca rodou neste boot (SET WSL=1)\n");
      return 1;
    }
    printf("WSL: servico %s, agente %s, ultima volta ha %u ms\n", b->alive ? "ligado" : "parado",
           b->ready ? "pronto" : "esperando", k_ms() - b->beat);
    printf("  serial ttyS1: %u bytes para o Linux, %u do Linux; %u cabecalhos, IRQ %u, IER/MCR %x, enviando vaga %d\n",
           b->d_rx, b->d_tx, b->d_frames, b->d_irq, b->d_ier, b->d_feeding);
    printf("  ultimo do agente: %s\n", b->d_last);
    for (i = 0; i < WSLB_SLOTS; i++)
      if (b->slot[i].state != SL_FREE)
        printf("  vaga %d (geracao %u): %s, saida %u/%u bytes, codigo %d\n", i, b->slot[i].gen,
               b->slot[i].state >= 0 && b->slot[i].state <= 4 ? st[b->slot[i].state] : "?",
               wslb_used(&b->slot[i].out), wslb_used(&b->slot[i].err), b->slot[i].exit_code);
    return 0;
  }
  if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "/?")) {
    wsl_usage();
    if (img_present()) printf("  (imagem propria %s em D:\\WSL: e a distro padrao)\n", IMG_NAME);
    return argc < 2;
  }
  /* sintaxe do wsl.exe do Windows: --list/-l [--verbose/-v] */
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--list") || !strcmp(argv[i], "-l")) winlist = 1;
    else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) verbose = 1;
    else if (!strcmp(argv[i], "-d") && i + 1 < argc) which = argv[++i];
  }
  if (winlist) return cmd_winlist(verbose);
  secs = k_nvme_raw_sectors();
  if (secs) wsl_disk_io(&disk, nvme_rd, 0, secs);
  if (!strcmp(argv[1], "run")) return cmd_run(argc, argv, secs ? &disk : 0);
  if (!strcmp(argv[1], "list")) {
    int rc = img_list();
    if (secs) rc = wsl_cli_read(argc, argv, &disk, save_local) && rc;
    return rc;
  }
  if (img_wanted(which)) {
    int j;
    for (i = 1, j = 1; i < argc; i++) { /* tira -d NOME: o caminho fica em argv[2] */
      if (!strcmp(argv[i], "-d") && i + 1 < argc) i++;
      else argv[j++] = argv[i];
    }
    return img_read_cmd(j, argv, save_local);
  }
  if (!secs) {
    printf("WSL: sem a imagem D:\\WSL\\DEBIAN.IMG e sem SSD NVMe com o Windows\n");
    return 1;
  }
  return wsl_cli_read(argc, argv, &disk, save_local);
}
