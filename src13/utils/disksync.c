/* disksync.c - DISKSYNC.EXE: servico que leva Z:\SAVE para o pendrive.
 *
 * Ligado com SET DISKSYNC=1 (no AUTOEXEC.BAT ou no prompt): o kernel abre
 * este programa numa tela de texto ao fundo (Ctrl+F12 mostra o log). Sem a
 * variavel nada roda. SET DISKSYNC=0 faz o programa sair em ate 1 segundo.
 *
 * A cada segundo varre Z:\SAVE (com as subpastas). Um arquivo que ficou 10
 * segundos sem mudar (tamanho e data iguais) e copiado para a mesma pasta no
 * pendrive, sem o SAVE:
 *     Z:\SAVE\SCREENSHOT\FILE.PNG -> U:\SCREENSHOT\FILE.PNG
 * Depois de gravar, o arquivo do pendrive e lido de volta e comparado; so
 * entao o de Z: e apagado. Pastas ficam. Sem pendrive (U: nao montado) os
 * arquivos esperam em Z:.
 *
 * A pasta e lida com k_dir_long: o nome longo inteiro (ate 255) vai para o
 * pendrive. Antes, nome maior que 31 ia como o apelido 8.3 (NOME~1).
 *
 * Tecla Q (com a tela Ctrl+F12 visivel) tambem para o servico.
 */
#include <stdio.h>
#include <string.h>
#include <so.h>

#define SRC_ROOT "Z:\\SAVE"
#define STABLE_MS 10000u
#define SCAN_MS 1000u
#define MAX_TRACK 256
#define MAX_DIRENT 512
#define MAX_DEPTH 8
#define PATH_LEN 260

struct track {
  char path[PATH_LEN];   /* caminho em Z: */
  unsigned size;
  unsigned short date, time;
  unsigned since;        /* k_ms() em que ficou assim */
  int seen;              /* visto na varredura atual */
  int failed;            /* copias que falharam seguidas */
  unsigned retry_at;     /* depois de falhar, espera ate aqui */
};

static struct track trk[MAX_TRACK];
static int ntrk;
static unsigned now;

static int wanted(void) {
  char v[16];
  int n = k_getenv("DISKSYNC", v, (int)sizeof(v));
  return n > 0 && !(v[0] == '0' && v[1] == 0);
}

static void stamp(void) {
  int t = k_time();
  if (t < 0) t = 0;
  printf("%02d:%02d:%02d ", t / 10000, (t / 100) % 100, t % 100);
}

/* Pendrive na letra atual (PENDRIVE MOUNT G: muda; SET PENDRIVE=0 desliga). */
static char dst_root[3] = "U:";
static int u_mounted(void) {
  int l = k_pendrive(0, 0, 0, 0);
  if (l == 0) return 0;
  dst_root[0] = (char)(l < 0 ? -l : l);
  return l > 0;
}
#define DST_ROOT dst_root

static struct track* find(const char* path) {
  int i;
  for (i = 0; i < ntrk; i++)
    if (strcmp(trk[i].path, path) == 0) return &trk[i];
  return 0;
}

static void forget(struct track* t) {
  int i = (int)(t - trk);
  if (i < 0 || i >= ntrk) return;
  trk[i] = trk[ntrk - 1];
  ntrk--;
}

/* Cria as pastas de dst (U:\A\B\ARQ -> U:\A e U:\A\B). 0 ok. */
static int make_dirs(const char* dst) {
  char p[PATH_LEN];
  int i, n = (int)strlen(dst);
  if (n >= PATH_LEN) return -1;
  memcpy(p, dst, (size_t)n + 1);
  for (i = 3; i < n; i++) {
    if (p[i] != '\\') continue;
    p[i] = 0;
    if (k_mkdir(p) < 0) {
      p[i] = '\\';
      return -1;
    }
    p[i] = '\\';
  }
  return 0;
}

/* Copia, confere e apaga a origem. 0 ok; 1 sem pendrive (espera); -1 erro. */
static int sync_one(const struct track* t) {
  char dst[PATH_LEN];
  char* buf;
  char* back;
  int n, rc = -1;
  unsigned size = t->size;
  const char* rel = t->path + strlen(SRC_ROOT);   /* "\SCREENSHOT\X.PNG" */
  if (!u_mounted()) return 1;
  if (strlen(DST_ROOT) + strlen(rel) >= PATH_LEN) return -1;
  strcpy(dst, DST_ROOT);
  strcat(dst, rel);
  if (make_dirs(dst) != 0) {
    stamp();
    printf("ERRO: nao criou as pastas de %s\n", dst);
    return -1;
  }
  buf = (char*)k_malloc(size ? size : 1);
  back = (char*)k_malloc(size ? size : 1);
  if (!buf || !back) {
    stamp();
    printf("ERRO: sem memoria para %s (%u bytes)\n", t->path, size);
    goto out;
  }
  n = size ? k_fread(t->path, 0, buf, (int)size) : 0;
  if (n != (int)size || k_fsize(t->path) != (int)size) {
    stamp();
    printf("mudou durante a leitura, depois: %s\n", t->path);
    goto out;
  }
  if (k_fsave(dst, buf, (int)size) != 0) {
    stamp();
    printf("ERRO ao gravar %s (pendrive cheio ou FAT32.DLL antigo?)\n", dst);
    goto out;
  }
  n = size ? k_fread(dst, 0, back, (int)size) : 0;
  if (k_fsize(dst) != (int)size || n != (int)size || memcmp(buf, back, size) != 0) {
    stamp();
    printf("ERRO: %s gravado diferente; %s fica em Z:\n", dst, t->path);
    goto out;
  }
  if (k_fdelete(t->path) != 0) {
    stamp();
    printf("copiado, mas nao apagou %s\n", t->path);
  }
  stamp();
  printf("%s -> %s (%u bytes)\n", t->path, dst, size);
  rc = 0;
out:
  if (buf) k_free(buf);
  if (back) k_free(back);
  return rc;
}

/* Marca/atualiza os arquivos de dir (recursivo). */
static void scan(const char* dir, int depth) {
  struct k_direntl* ents;
  int n, i;
  if (depth > MAX_DEPTH) return;
  ents = (struct k_direntl*)k_malloc(MAX_DIRENT * (unsigned)sizeof(struct k_direntl));
  if (!ents) return;
  n = k_dir_long(dir, ents, MAX_DIRENT);
  for (i = 0; i < n; i++) {
    char p[PATH_LEN];
    struct track* t;
    if (strlen(dir) + 1 + strlen(ents[i].name) >= PATH_LEN) continue;
    sprintf(p, "%s\\%s", dir, ents[i].name);
    if (ents[i].dir) {
      scan(p, depth + 1);
      continue;
    }
    t = find(p);
    if (!t) {
      if (ntrk >= MAX_TRACK) continue;
      t = &trk[ntrk++];
      memset(t, 0, sizeof(*t));
      strcpy(t->path, p);
      t->size = ents[i].size;
      t->date = ents[i].date;
      t->time = ents[i].time;
      t->since = now;
    } else if (t->size != ents[i].size || t->date != ents[i].date || t->time != ents[i].time) {
      t->size = ents[i].size;
      t->date = ents[i].date;
      t->time = ents[i].time;
      t->since = now;   /* ainda sendo escrito: conta de novo */
    }
    t->seen = 1;
  }
  k_free(ents);
}

static void pass(void) {
  int i;
  now = k_ms();
  for (i = 0; i < ntrk; i++) trk[i].seen = 0;
  scan(SRC_ROOT, 0);
  for (i = 0; i < ntrk;) {
    struct track* t = &trk[i];
    int r;
    if (!t->seen) {   /* apagado ou renomeado por outro */
      forget(t);
      continue;
    }
    if (now - t->since < STABLE_MS || (t->failed && (int)(now - t->retry_at) < 0)) {
      i++;
      continue;
    }
    r = sync_one(t);
    if (r == 0) {
      forget(t);
      continue;
    }
    if (r < 0) {
      t->failed++;
      /* 10 s, 20 s, ... ate 5 min entre tentativas */
      t->retry_at = now + (t->failed < 30 ? (unsigned)t->failed * 10000u : 300000u);
    }
    i++;
  }
}

int main(void) {
  int was_u = -1;
  if (!wanted()) {
    printf("DISKSYNC: use SET DISKSYNC=1 (o kernel abre o servico sozinho).\n");
    return 1;
  }
  k_mkdir(SRC_ROOT);
  printf("DISKSYNC: %s\\... -> %s\\... depois de %u s parado. Q para sair.\n", SRC_ROOT,
         DST_ROOT, STABLE_MS / 1000u);
  while (wanted()) {
    unsigned t0;
    int u = u_mounted();
    if (u != was_u) {
      stamp();
      if (u) printf("pendrive %s presente\n", DST_ROOT);
      else printf("sem pendrive em %s, os arquivos esperam em Z:\n", DST_ROOT);
      was_u = u;
    }
    pass();
    /* espera cedendo a vez: ao fundo o k_trygetc devolve a CPU a tela visivel */
    t0 = k_ms();
    while (k_ms() - t0 < SCAN_MS) {
      int c = k_trygetc();
      if (c == 'q' || c == 'Q' || c == 27) {
        stamp();
        printf("parado pelo teclado (SET DISKSYNC=1 de novo para voltar)\n");
        return 0;
      }
    }
  }
  stamp();
  printf("DISKSYNC desligado (SET DISKSYNC=0)\n");
  return 0;
}
