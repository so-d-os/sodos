/* ffperfil.c - cache do Firefox entre uma abertura e outra e entre boots.
 *
 * O Firefox roda com HOME=Z:\USER\DEFAULT (ramdisk, vazio a cada boot):
 *   .mozilla\firefox\profiles.ini, installs.ini   qual perfil usar
 *   .mozilla\firefox\<perfil>\...                 cookies, historico, bancos
 *   mozilla\firefox\<perfil>\cache2, startupCache cache HTTP e de scripts
 * O cache e achado pelo NOME do perfil. Este port trava ("Nova aba" parada,
 * fork de processo de aba) ao reabrir um perfil ja usado, entao cada abertura
 * comecava com perfil novo, de nome sorteado, e o cache nunca era reusado.
 *
 * - O Firefox fechou (ffp_program_exit) ou o WIN vai salvar: z_reset esvazia
 *   o conteudo do perfil e deixa so os .ini; a proxima abertura cria um perfil
 *   novo com o mesmo nome e encontra o cache. Caches de perfis que o
 *   profiles.ini nao cita saem. Cookies e logins nao ficam (como antes).
 * - Ao fechar o WIN (ffp_save): copia .mozilla (so os .ini) e mozilla para
 *   X:\USER\DEFAULT\FFPERFIL (X = unidade do FIREFOX\FIREFOX.EXE, D: ou U:),
 *   so o que mudou: arquivo novo, de outro tamanho ou gravado depois da ultima
 *   copia (data do FAT). O que sumiu do Z: sai da copia.
 * - Ao abrir o WIN (ffp_win_start + ffp_poll): traz a copia de volta para o
 *   Z: aos poucos, de dentro do laco do WIN, poucos ms por volta e so sem
 *   tecla ou mouse esperando; a barra de tarefas mostra a porcentagem.
 *   Abrir o Firefox antes do fim termina a copia na hora (ffp_finish).
 *
 * FFPERFIL\SALVANDO.TXT existe enquanto a copia e gravada: se o micro
 * desligar no meio, a copia pela metade nao volta para o Z:.
 */
#include <stdint.h>
#include "drives.h"
#include "../kernel/mm.h"
#include "../kernel/irq.h"
#include "../kernel/serial.h"
#include "../kernel/kexports.h"

#define FP_Z        "Z:\\USER\\DEFAULT"
#define FP_RUNMARK  "Z:\\USER\\DEFAULT\\SODOSRUN.TXT"
#define FP_DEPTH    12
#define FP_FILE_MAX (16u << 20)   /* maior arquivo copiado (video no cache fica de fora) */
#define FP_CHUNK    (64 * 1024)   /* leitura por passo no fundo */
#define FP_BUDGET   3             /* ms por volta do WIN */
#define FP_DELAY    2000          /* ms depois de abrir o WIN: a interface primeiro */
#define FP_MARGIN   (32u << 20)   /* folga que fica livre na unidade */

static const char* const fp_roots[] = {".mozilla", "mozilla"};
#define FP_NROOTS 2

struct fp_lvl {
  char src[DRV_PATH];
  char dst[DRV_PATH];
  struct dir_item_long* it;   /* pasta de origem */
  int n, i;
  struct dir_item_long* dit;  /* pasta de destino (so ao salvar) */
  int dn;
  uint8_t* seen;              /* dit[k] tambem existe na origem */
  int* hash;                  /* indice de dit por nome */
  int hmask;
};

#define M_IDLE    0
#define M_WAIT    1   /* restaura depois de FP_DELAY */
#define M_RESTORE 2
#define M_SAVE    3

static struct fp_lvl lv[FP_DEPTH];
static int depth = -1;
static int root_i;
static int mode;
static uint64_t wait_until;
static char save_dir[DRV_PATH];
static uint8_t* buf;
static int buf_high;
static char cur_src[DRV_PATH], cur_dst[DRV_PATH];
static uint32_t cur_size, cur_off;
static int cur_on;
static uint32_t total_bytes, done_bytes;
static uint32_t files_copied, files_skipped, files_deleted;
static uint32_t space_left;    /* bytes livres no destino (ao salvar) */
static int last_pct = -1;
static uint32_t stamp0;        /* data<<16|hora da ultima copia completa */

/* ---- pequenos utilitarios ---- */

static int s_len(const char* s) {
  int n = 0;
  while (s[n]) n++;
  return n;
}

static void s_cpy(char* d, int max, const char* s) {
  int i;
  for (i = 0; s[i] && i < max - 1; i++) d[i] = s[i];
  d[i] = 0;
}

static int s_join(char* d, const char* a, const char* b) {
  int la = s_len(a), lb = s_len(b);
  if (la + 1 + lb >= DRV_PATH) return -1;
  s_cpy(d, DRV_PATH, a);
  d[la] = '\\';
  s_cpy(d + la + 1, DRV_PATH - la - 1, b);
  return 0;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static int s_ieq(const char* a, const char* b) {
  while (*a && *b && lower(*a) == lower(*b)) {
    a++;
    b++;
  }
  return *a == 0 && *b == 0;
}

static int s_iprefix(const char* s, const char* p) {
  while (*p) {
    if (lower(*s) != lower(*p)) return 0;
    s++;
    p++;
  }
  return 1;
}

static uint32_t s_hash(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) h = (h ^ (uint8_t)lower(*s++)) * 16777619u;
  return h;
}

static void say(const char* a, uint32_t v) {
  serial_puts("[ffperfil] ");
  serial_puts(a);
  serial_hex32(v);
  serial_puts("\n");
}

static void* fp_alloc(uint32_t n, int* high) {
  void* p = 0;
  if (hm_ready()) p = hmalloc(n);
  if (p) {
    if (high) *high = 1;
    return p;
  }
  if (high) *high = 0;
  return umalloc(n);
}

static void fp_free(void* p, int high) {
  if (!p) return;
  if (high) hfree(p);
  else ufree(p);
}

/* As listas e tabelas sempre do mesmo lugar (alto se houver). */
static int lists_high;
static void* l_alloc(uint32_t n) { return fp_alloc(n, &lists_high); }
static void l_free(void* p) { fp_free(p, lists_high); }

static uint32_t stamp_now(void) {
  int d = rtc_date(), t = rtc_time();
  uint32_t fd, ft = 0;
  if (d <= 19800000 || d >= 21080000) return 0;
  fd = (uint32_t)(((d / 10000 - 1980) << 9) | ((d / 100 % 100) << 5) | (d % 100));
  if (t >= 0) ft = (uint32_t)(((t / 10000) << 11) | ((t / 100 % 100) << 5) | (t % 100 / 2));
  return (fd << 16) | ft;
}

static int skip_name(const char* n) {
  return s_ieq(n, "doomed") || s_ieq(n, "Crash Reports") || s_ieq(n, "minidumps") ||
         s_ieq(n, ".parentlock") || s_ieq(n, "lock") || s_ieq(n, "SODOSRUN.TXT") ||
         s_iprefix(n, "trash");
}

/* Unidade da copia: a do FIREFOX\FIREFOX.EXE (D: no QEMU, U: no pendrive). */
static int pick_save_dir(void) {
  static const char drv[2] = {'D', 'U'};
  int i;
  for (i = 0; i < 2; i++) {
    char p[] = "?:\\FIREFOX\\FIREFOX.EXE";
    p[0] = drv[i];
    if (!drives_is_mounted(drv[i]) || file_size(p) <= 0) continue;
    s_cpy(save_dir, DRV_PATH, "?:\\USER\\DEFAULT\\FFPERFIL");
    save_dir[0] = drv[i];
    return 0;
  }
  return -1;
}

static int is_dir(const char* path) { return drives_items_long(path, 0, 0) >= 0; }

/* Apaga a pasta e tudo dentro (destino da copia). */
static void rm_tree(const char* path, int d) {
  struct dir_item_long* it;
  char sub[DRV_PATH];
  int n, i;
  if (d > FP_DEPTH) return;
  n = drives_items_long(path, 0, 0);
  if (n < 0) return;
  it = n > 0 ? (struct dir_item_long*)l_alloc((uint32_t)n * sizeof(*it)) : 0;
  if (n > 0 && !it) return;
  n = n > 0 ? drives_items_long(path, it, n) : 0;
  for (i = 0; i < n; i++) {
    if (s_join(sub, path, it[i].name) != 0) continue;
    if (it[i].dir) rm_tree(sub, d + 1);
    else if (drives_del(sub) >= 0) files_deleted++;
  }
  l_free(it);
  drives_rmdir(path);
}

/* ---- pilha de pastas ---- */

static void lvl_free(struct fp_lvl* l) {
  l_free(l->it);
  l_free(l->dit);
  l_free(l->seen);
  l_free(l->hash);
  l->it = l->dit = 0;
  l->seen = 0;
  l->hash = 0;
}

static struct dir_item_long* list_dir(const char* path, int* count) {
  struct dir_item_long* it;
  int n = drives_items_long(path, 0, 0);
  *count = 0;
  if (n <= 0) return 0;
  it = (struct dir_item_long*)l_alloc((uint32_t)n * sizeof(*it));
  if (!it) return 0;
  *count = drives_items_long(path, it, n);
  if (*count < 0) *count = 0;
  return it;
}

static int push(const char* src, const char* dst) {
  struct fp_lvl* l;
  if (depth + 1 >= FP_DEPTH) return -1;
  l = &lv[depth + 1];
  s_cpy(l->src, DRV_PATH, src);
  s_cpy(l->dst, DRV_PATH, dst);
  l->it = l->dit = 0;
  l->seen = 0;
  l->hash = 0;
  l->n = l->i = l->dn = 0;
  if (drives_mkdir(dst) < 0) return -1;
  l->it = list_dir(src, &l->n);
  if (mode == M_SAVE) {
    l->dit = list_dir(dst, &l->dn);
    if (l->dn > 0) {
      int sz = 1, k;
      while (sz < 2 * l->dn) sz <<= 1;
      l->seen = (uint8_t*)l_alloc((uint32_t)l->dn);
      l->hash = (int*)l_alloc((uint32_t)sz * sizeof(int));
      if (!l->seen || !l->hash) {
        lvl_free(l);
        return -1;
      }
      l->hmask = sz - 1;
      for (k = 0; k < l->dn; k++) l->seen[k] = 0;
      for (k = 0; k < sz; k++) l->hash[k] = -1;
      for (k = 0; k < l->dn; k++) {
        uint32_t h = s_hash(l->dit[k].name) & (uint32_t)l->hmask;
        while (l->hash[h] >= 0) h = (h + 1) & (uint32_t)l->hmask;
        l->hash[h] = k;
      }
    }
  }
  depth++;
  return 0;
}

static int dst_find(struct fp_lvl* l, const char* name) {
  uint32_t h;
  if (!l->hash) return -1;
  h = s_hash(name) & (uint32_t)l->hmask;
  while (l->hash[h] >= 0) {
    if (s_ieq(l->dit[l->hash[h]].name, name)) return l->hash[h];
    h = (h + 1) & (uint32_t)l->hmask;
  }
  return -1;
}

static void pop(void) {
  struct fp_lvl* l = &lv[depth];
  int k;
  /* Ao salvar: o que esta na copia e nao esta mais no Z: sai. */
  if (mode == M_SAVE)
    for (k = 0; k < l->dn; k++) {
      char p[DRV_PATH];
      if (l->seen[k] || s_join(p, l->dst, l->dit[k].name) != 0) continue;
      if (l->dit[k].dir) rm_tree(p, depth + 1);
      else if (drives_del(p) >= 0) files_deleted++;
    }
  lvl_free(l);
  depth--;
}

/* ---- um passo de trabalho ---- */

/* Termina o arquivo atual (ou anda um pedaco). 1 = ainda ha o que ler. */
static int file_step(void) {
  uint32_t want = cur_size - cur_off;
  if (want > FP_CHUNK && mode != M_SAVE) want = FP_CHUNK;
  while (want > 0) {
    int got = file_read_at(cur_src, cur_off, (char*)buf + cur_off, (int)want);
    if (got <= 0) {
      say("leitura falhou, pulado ", cur_off);
      cur_on = 0;
      files_skipped++;
      return 0;
    }
    cur_off += (uint32_t)got;
    done_bytes += (uint32_t)got;
    want -= (uint32_t)got;
    if (mode != M_SAVE) break;
  }
  if (cur_off < cur_size) return 1;
  if (file_save(cur_dst, (const char*)buf, (int)cur_size) < 0) {
    serial_puts("[ffperfil] nao gravou ");
    serial_puts(cur_dst);
    serial_puts("\n");
    files_skipped++;
  } else {
    files_copied++;
    if (mode == M_SAVE) space_left = space_left > cur_size ? space_left - cur_size : 0;
  }
  cur_on = 0;
  return 0;
}

/* Proximo item da pasta de cima (ou proxima raiz). 0 quando acabou tudo. */
static int walk_step(void) {
  struct fp_lvl* l;
  struct dir_item_long* e;
  char src[DRV_PATH], dst[DRV_PATH];
  if (cur_on) {
    file_step();
    return 1;
  }
  if (depth < 0) {
    char base_src[DRV_PATH], base_dst[DRV_PATH];
    const char* from = mode == M_SAVE ? FP_Z : save_dir;
    const char* to = mode == M_SAVE ? save_dir : FP_Z;
    while (root_i < FP_NROOTS) {
      int r = root_i++;
      if (s_join(base_src, from, fp_roots[r]) != 0 || s_join(base_dst, to, fp_roots[r]) != 0)
        continue;
      if (!is_dir(base_src)) continue;
      if (push(base_src, base_dst) == 0) return 1;
    }
    return 0;
  }
  l = &lv[depth];
  if (l->i >= l->n) {
    pop();
    return 1;
  }
  e = &l->it[l->i++];
  if (skip_name(e->name)) return 1;
  if (s_join(src, l->src, e->name) != 0 || s_join(dst, l->dst, e->name) != 0) return 1;
  if (mode == M_SAVE) {
    int k = dst_find(l, e->name);
    if (k >= 0 && l->dit[k].dir == e->dir) {
      /* fica na copia; um arquivo grande demais ou sem lugar sai (fora de seen) */
      if (e->dir || e->size <= FP_FILE_MAX) l->seen[k] = 1;
      if (!e->dir && e->size <= FP_FILE_MAX) {
        uint32_t st = ((uint32_t)e->date << 16) | e->time;
        int changed = stamp0 ? st >= stamp0 : !s_ieq(l->src + s_len(l->src) - 7, "entries");
        if (l->dit[k].size == e->size && !changed) {
          done_bytes += e->size;
          return 1;
        }
      }
    }
  }
  if (e->dir) {
    if (push(src, dst) != 0) say("pasta pulada, nivel ", (uint32_t)depth);
    return 1;
  }
  if (e->size > FP_FILE_MAX) {
    files_skipped++;
    return 1;
  }
  if (mode == M_SAVE && e->size + FP_MARGIN > space_left) {
    files_skipped++;
    return 1;
  }
  s_cpy(cur_src, DRV_PATH, src);
  s_cpy(cur_dst, DRV_PATH, dst);
  cur_size = e->size;
  cur_off = 0;
  cur_on = 1;
  if (cur_size == 0) file_step();
  return 1;
}

static int begin(int m) {
  int i;
  buf = (uint8_t*)fp_alloc(FP_FILE_MAX, &buf_high);
  if (!buf) {
    serial_puts("[ffperfil] sem memoria para a copia\n");
    return -1;
  }
  mode = m;
  depth = -1;
  root_i = 0;
  cur_on = 0;
  done_bytes = 0;
  files_copied = files_skipped = files_deleted = 0;
  last_pct = -1;
  for (i = 0; i < FP_DEPTH; i++) lv[i].it = 0;
  return 0;
}

static void end(void) {
  while (depth >= 0) {
    lvl_free(&lv[depth]);
    depth--;
  }
  fp_free(buf, buf_high);
  buf = 0;
  mode = M_IDLE;
  last_pct = -1;
}

static void write_total(void) {
  char t[24], p[DRV_PATH];
  uint32_t v = done_bytes;
  int n = 0, i;
  char r[12];
  do {
    r[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v && n < 11);
  for (i = 0; i < n; i++) t[i] = r[n - 1 - i];
  t[n++] = '\n';
  if (s_join(p, save_dir, "TOTAL.TXT") == 0) file_save(p, t, n);
}

static uint32_t read_total(void) {
  char t[16], p[DRV_PATH];
  uint32_t v = 0;
  int n, i;
  if (s_join(p, save_dir, "TOTAL.TXT") != 0) return 0;
  n = file_load(p, t, (int)sizeof(t));
  for (i = 0; i < n && t[i] >= '0' && t[i] <= '9'; i++) v = v * 10 + (uint32_t)(t[i] - '0');
  return v;
}

/* ---- perfil reaproveitavel ---- */

#define FP_FFDIR  FP_Z "\\.mozilla\\firefox"
#define FP_CACHED FP_Z "\\mozilla\\firefox"
#define FP_MAXPROF 8

/* Path= do profiles.ini (perfis relativos). Quantos. */
static int ini_paths(char out[][DIR_LONG_NAME], int max) {
  static char ini[4096];
  int n = file_load(FP_FFDIR "\\profiles.ini", ini, (int)sizeof(ini));
  int i = 0, k = 0;
  if (n <= 0) return 0;
  while (i < n && k < max) {
    if ((i == 0 || ini[i - 1] == '\n') && s_iprefix(ini + i, "Path=")) {
      int j = 0;
      i += 5;
      while (i < n && ini[i] != '\r' && ini[i] != '\n' && j < DIR_LONG_NAME - 1)
        out[k][j++] = ini[i++];
      out[k][j] = 0;
      if (j > 0) k++;
    }
    i++;
  }
  return k;
}

/* Esvazia o perfil (fica a pasta, com o mesmo nome) e apaga o marcador. */
static void z_reset(void) {
  static char keep[FP_MAXPROF][DIR_LONG_NAME];
  struct dir_item_long* it;
  char p[DRV_PATH];
  int np, n, i, k;
  if (!drives_is_mounted('Z') || !is_dir(FP_FFDIR)) return;
  np = ini_paths(keep, FP_MAXPROF);
  if (np <= 0) return;
  it = list_dir(FP_FFDIR, &n);
  for (i = 0; i < n; i++) {
    if (!it[i].dir || s_join(p, FP_FFDIR, it[i].name) != 0) continue;
    rm_tree(p, 0);
    for (k = 0; k < np; k++)
      if (s_ieq(it[i].name, keep[k])) drives_mkdir(p);
  }
  l_free(it);
  /* caches de perfis que nao existem mais */
  it = list_dir(FP_CACHED, &n);
  for (i = 0; i < n; i++) {
    int used = 0;
    for (k = 0; k < np; k++)
      if (s_ieq(it[i].name, keep[k])) used = 1;
    if (!used && it[i].dir && s_join(p, FP_CACHED, it[i].name) == 0) rm_tree(p, 0);
  }
  l_free(it);
  if (file_size(FP_RUNMARK) >= 0) drives_del(FP_RUNMARK);
  serial_puts("[ffperfil] perfil esvaziado, cache fica: ");
  serial_puts(keep[0]);
  serial_puts("\n");
}

/* ---- API ---- */

int ffp_pct(void) {
  uint32_t p;
  if (mode != M_RESTORE) return -1;
  if (!total_bytes) return 0;
  p = (uint32_t)(((uint64_t)done_bytes * 100u) / total_bytes);
  return p > 99 ? 99 : (int)p;
}

void ffp_win_start(void) {
  char p[DRV_PATH];
  if (mode != M_IDLE) return;
  if (!drives_is_mounted('Z') || pick_save_dir() != 0) return;
  /* Ja ha perfil no Z: (o WIN abriu de novo neste boot) ou o Firefox esta
   * aberto/caiu: nada a trazer. */
  if (is_dir(FP_Z "\\.mozilla") || file_size(FP_RUNMARK) >= 0) return;
  if (s_join(p, save_dir, ".mozilla") != 0 || !is_dir(p)) return;
  if (s_join(p, save_dir, "SALVANDO.TXT") == 0 && file_size(p) >= 0) {
    serial_puts("[ffperfil] copia incompleta (SALVANDO.TXT), nao restaura\n");
    return;
  }
  total_bytes = read_total();
  mode = M_WAIT;
  wait_until = irq_ticks() + FP_DELAY;
  serial_puts("[ffperfil] restaura de ");
  serial_puts(save_dir);
  serial_puts(" em segundo plano\n");
}

static void restore_done(void) {
  stamp0 = stamp_now();
  say("restaurado, arquivos ", files_copied);
  if (files_skipped) say("pulados ", files_skipped);
  end();
}

/* Do laco do WIN, sem entrada esperando. 1 = a porcentagem mudou. */
int ffp_poll(void) {
  uint64_t t0;
  int pct;
  if (mode == M_IDLE) return 0;
  t0 = irq_ticks();
  if (mode == M_WAIT) {
    if (t0 < wait_until) return 0;
    if (begin(M_RESTORE) != 0) {
      mode = M_IDLE;
      return 0;
    }
    return 1;
  }
  if (mode != M_RESTORE) return 0;
  do {
    if (!walk_step()) {
      restore_done();
      return 1;
    }
  } while (irq_ticks() - t0 < FP_BUDGET);
  pct = ffp_pct();
  if (pct != last_pct) {
    last_pct = pct;
    return 1;
  }
  return 0;
}

int ffp_busy(void) { return mode == M_WAIT || mode == M_RESTORE; }

/* O Firefox vai abrir: o perfil tem de estar inteiro antes. */
void ffp_finish(void) {
  if (mode == M_WAIT && begin(M_RESTORE) != 0) {
    mode = M_IDLE;
    return;
  }
  if (mode != M_RESTORE) return;
  serial_puts("[ffperfil] Firefox abrindo: termina a restauracao agora\n");
  while (walk_step()) {}
  restore_done();
}

/* Fim do WIN, com o Firefox ja fechado. show(pct) desenha o progresso. */
void ffp_save(void (*show)(int pct)) {
  char mark[DRV_PATH];
  uint32_t total_kb = 0, free_kb = 0, est, t_start, t0;
  int last = -1;
  if (mode == M_WAIT || mode == M_RESTORE) {
    /* fechou antes de acabar: o Z: nao tem o perfil inteiro */
    end();
    serial_puts("[ffperfil] WIN fechou durante a restauracao, nao salva\n");
    return;
  }
  if (!drives_is_mounted('Z') || pick_save_dir() != 0) return;
  if (!is_dir(FP_Z "\\.mozilla")) return;
  z_reset();   /* o Firefox ja fechou (ou foi encerrado): so .ini e cache */
  if (drives_space(save_dir[0], &total_kb, &free_kb) < 0) return;
  if (begin(M_SAVE) != 0) return;
  t0 = (uint32_t)irq_ticks();
  space_left = free_kb > 0x3FFFFFu ? 0xFFFFFFFFu : free_kb * 1024u;
  est = read_total();
  total_bytes = est;
  t_start = stamp_now();
  /* a pasta FFPERFIL e o marcador antes de tudo */
  drives_mkdir(save_dir);
  if (s_join(mark, save_dir, "SALVANDO.TXT") != 0 || file_save(mark, "1", 1) < 0) {
    end();
    return;
  }
  while (walk_step()) {
    if (show && total_bytes) {
      int p = (int)(((uint64_t)done_bytes * 100u) / total_bytes);
      if (p > 99) p = 99;
      if (p != last) {
        last = p;
        show(p);
      }
    }
  }
  write_total();
  drives_del(mark);
  stamp0 = t_start;
  say("salvo, copiados ", files_copied);
  say("apagados ", files_deleted);
  if (files_skipped) say("pulados ", files_skipped);
  say("ms ", (uint32_t)irq_ticks() - t0);
  end();
}

/* Fim do FIREFOX.EXE (mod_exec voltou): o perfil fica vazio com o mesmo
 * nome para a proxima abertura reusar o cache. O Firefox sai com _exit (fast
 * shutdown), entao o atexit do port que apagaria o SODOSRUN.TXT nao roda;
 * z_reset apaga o marcador tambem. */
void ffp_program_exit(const char* name, int rc) {
  int n;
  (void)rc;
  if (!name) return;
  n = s_len(name);
  if (n < 11 || !s_ieq(name + n - 11, "FIREFOX.EXE")) return;
  z_reset();
}
