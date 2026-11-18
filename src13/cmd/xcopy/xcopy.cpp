/* xcopy.cpp - XCOPY do SODOS: copia arquivos e arvores de pastas.
 *
 *   XCOPY origem [destino] [/S] [/E] [/I] [/Y] [/-Y] [/D] [/L] [/Q] [/F] [/?]
 *
 * origem   arquivo, pasta ou mascara com * e ? ("D:\SRC\*.C").
 *          Pasta sem mascara = tudo o que tem dentro dela.
 * destino  pasta ou arquivo; sem destino, a pasta atual.
 *
 * Compila com:  cc -o XCOPY.EXE xcopy.cpp   (ou o BUILD.BAT desta pasta)
 *
 * O sistema de arquivos do SODOS le e grava arquivos inteiros (k_fload e
 * k_fsave), entao cada arquivo e copiado de uma vez, como no MC.
 * Codigos de saida (os do XCOPY do DOS): 0 copiou, 1 nada para copiar,
 * 2 cancelado pelo usuario, 4 erro. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>

#define XPATH 260
#define DEPTH_MAX 16

/* ---------- caminhos ---------- */

static int up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

static int has_wild(const char* s) {
  for (; *s; s++)
    if (*s == '*' || *s == '?') return 1;
  return 0;
}

/* Mascara do DOS sem diferenca de maiusculas; "*.*" e "*" casam com tudo. */
static int wild_match(const char* p, const char* s) {
  if (strcmp(p, "*.*") == 0) return 1;
  while (*p) {
    if (*p == '*') {
      p++;
      if (!*p) return 1;
      for (; *s; s++)
        if (wild_match(p, s)) return 1;
      return wild_match(p, s);
    }
    if (!*s) {
      /* "NOME.*" casa com "NOME" (sem extensao) */
      return p[0] == '.' && p[1] == '*' && !p[2];
    }
    if (*p != '?' && up(*p) != up(*s)) return 0;
    p++;
    s++;
  }
  return *s == 0;
}

static int path_join(char* out, const char* dir, const char* name) {
  int n = (int)strlen(dir);
  int need = n + (int)strlen(name) + 2;
  if (need > XPATH) return -1;
  strcpy(out, dir);
  if (n > 0 && out[n - 1] != '\\') strcat(out, "\\");
  strcat(out, name);
  return 0;
}

/* "SRC\..\X" ou "\SRC" -> "D:\X", "D:\SRC". Junta com a pasta atual,
 * troca / por \, resolve . e ..; o resultado nunca termina em \ (so a raiz). */
static int full_path(const char* in, char* out) {
  char tmp[XPATH * 2];
  char* seg[64];
  int nseg = 0, i;
  char* p;
  char drive;
  if (!in[0]) {
    k_pwd(out, XPATH);
    return 0;
  }
  if (in[1] == ':') {
    drive = (char)up(in[0]);
    in += 2;
    if (in[0] != '\\' && in[0] != '/') {
      /* "E:NOME": relativo a pasta atual so se for a unidade atual */
      char cwd[XPATH];
      k_pwd(cwd, XPATH);
      if (up(cwd[0]) == drive) snprintf(tmp, sizeof(tmp), "%s\\%s", cwd + 2, in);
      else snprintf(tmp, sizeof(tmp), "\\%s", in);
    } else {
      snprintf(tmp, sizeof(tmp), "%s", in);
    }
  } else {
    char cwd[XPATH];
    k_pwd(cwd, XPATH);
    drive = (char)up(cwd[0]);
    if (in[0] == '\\' || in[0] == '/') snprintf(tmp, sizeof(tmp), "%s", in);
    else snprintf(tmp, sizeof(tmp), "%s\\%s", cwd + 2, in);
  }
  for (p = tmp; *p; p++)
    if (*p == '/') *p = '\\';
  p = tmp;
  while (*p) {
    char* s;
    while (*p == '\\') p++;
    if (!*p) break;
    s = p;
    while (*p && *p != '\\') p++;
    if (*p) *p++ = 0;
    if (strcmp(s, ".") == 0) continue;
    if (strcmp(s, "..") == 0) {
      if (nseg > 0) nseg--;
      continue;
    }
    if (nseg >= 64) return -1;
    seg[nseg++] = s;
  }
  out[0] = drive;
  out[1] = ':';
  out[2] = '\\';
  out[3] = 0;
  for (i = 0; i < nseg; i++) {
    if ((int)(strlen(out) + strlen(seg[i]) + 2) > XPATH) return -1;
    if (i > 0) strcat(out, "\\");
    strcat(out, seg[i]);
  }
  return 0;
}

/* Separa "D:\SRC\*.C" em pasta "D:\SRC" e nome "*.C". */
static void split_path(const char* full, char* dir, char* name) {
  int i, cut = -1;
  for (i = 0; full[i]; i++)
    if (full[i] == '\\') cut = i;
  if (cut < 0) {
    strcpy(dir, full);
    name[0] = 0;
    return;
  }
  strcpy(name, full + cut + 1);
  memcpy(dir, full, (size_t)cut);
  dir[cut] = 0;
  if (cut == 2) strcat(dir, "\\");   /* "D:" -> "D:\" */
}

/* b fica dentro de a? ("D:\SRC" e "D:\SRC\BAK") */
static int inside(const char* a, const char* b) {
  int n = (int)strlen(a), i;
  for (i = 0; i < n; i++)
    if (up(a[i]) != up(b[i])) return 0;
  return b[n] == 0 || b[n] == '\\' || a[n - 1] == '\\';
}

/* ---------- pastas ---------- */

/* Lista uma pasta inteira (cresce o buffer ate caber). */
class Listing {
public:
  struct k_dirent* ent;
  int n;

  Listing(void) {
    ent = 0;
    n = -1;
  }
  ~Listing(void) {
    if (ent) free(ent);
  }
  int load(const char* path) {
    int cap = 256;
    for (;;) {
      if (ent) free(ent);
      ent = (struct k_dirent*)malloc(sizeof(struct k_dirent) * (size_t)cap);
      if (!ent) {
        n = -1;
        return -1;
      }
      n = k_dir(path, ent, cap);
      if (n < cap || cap >= 65536) return n;
      cap = cap * 2;
    }
  }
  struct k_dirent* find(const char* name) {
    int i, j;
    for (i = 0; i < n; i++) {
      for (j = 0; ent[i].name[j] && name[j]; j++)
        if (up(ent[i].name[j]) != up(name[j])) break;
      if (!ent[i].name[j] && !name[j]) return &ent[i];
    }
    return 0;
  }
};

static int is_dir(const char* path) {
  struct k_dirent one;
  return k_dir(path, &one, 1) >= 0;
}

/* Entrada de um arquivo (tamanho e data) lendo a pasta dele. */
static int stat_file(const char* full, struct k_dirent* out) {
  char dir[XPATH], name[XPATH];
  Listing l;
  struct k_dirent* e;
  split_path(full, dir, name);
  if (!name[0] || l.load(dir) < 0) return -1;
  e = l.find(name);
  if (!e) return -1;
  *out = *e;
  return 0;
}

static unsigned stamp(struct k_dirent* e) {
  return ((unsigned)e->date << 16) | e->time;
}

/* ---------- a copia ---------- */

class XCopy {
public:
  int sub;          /* /S */
  int empty;        /* /E */
  int as_dir;       /* /I */
  int ask;          /* pergunta antes de sobrescrever (/-Y; /Y desliga) */
  int newer;        /* /D */
  int list_only;    /* /L */
  int quiet;        /* /Q */
  int show_full;    /* /F */
  int all;          /* respondeu "Todos" */
  int stop;         /* respondeu "Cancelar" */
  int copied;
  int errors;
  int dirs_made;

  XCopy(void) {
    sub = 0;
    empty = 0;
    as_dir = 0;
    ask = 1;
    newer = 0;
    list_only = 0;
    quiet = 0;
    show_full = 0;
    all = 0;
    stop = 0;
    copied = 0;
    errors = 0;
    dirs_made = 0;
  }

  void fail(const char* what, const char* path) {
    printf("XCOPY: %s: %s\n", what, path);
    errors++;
  }

  /* 1 = sobrescreve, 0 = pula. */
  int confirm(const char* dst) {
    char line[16];
    if (!ask || all) return 1;
    for (;;) {
      printf("Sobrescrever %s? (Sim/Nao/Todos/Cancelar) ", dst);
      if (k_readline(line, sizeof(line)) < 0) line[0] = 'C';
      printf("\n");
      switch (up(line[0])) {
        case 'S': case 'Y': return 1;
        case 'N': return 0;
        case 'T': case 'A': all = 1; return 1;
        case 'C': case 'Q': stop = 1; return 0;
      }
    }
  }

  int make_dir(const char* path) {
    int rc;
    if (list_only || is_dir(path)) return 0;
    rc = k_mkdir(path);
    if (rc < 0) {
      fail("nao criei a pasta", path);
      return -1;
    }
    if (rc == 0) dirs_made++;
    return 0;
  }

  void copy_file(const char* src, const char* dst, struct k_dirent* se) {
    struct k_dirent de;
    int exists = stat_file(dst, &de) == 0;
    int size, got;
    char* buf;
    if (exists && de.dir) {
      fail("o destino e uma pasta", dst);
      return;
    }
    if (exists && newer && se && stamp(se) <= stamp(&de)) return;
    if (exists && !list_only && !confirm(dst)) return;
    if (stop) return;
    if (!quiet) {
      if (show_full) printf("%s -> %s\n", src, dst);
      else printf("%s\n", src);
    }
    if (list_only) {
      copied++;
      return;
    }
    size = k_fsize(src);
    if (size < 0) {
      fail("nao achei", src);
      return;
    }
    buf = (char*)malloc((size_t)size + 1);
    if (!buf) {
      fail("sem memoria para", src);
      return;
    }
    got = size > 0 ? k_fload(src, buf, size + 1) : 0;
    if (got < 0) {
      free(buf);
      fail("nao consegui ler", src);
      return;
    }
    if (k_fsave(dst, buf, got) != 0) {
      free(buf);
      fail("nao consegui gravar", dst);
      return;
    }
    free(buf);
    copied++;
  }

  /* Copia de src_dir os itens que casam com mask para dst_dir.
   * Devolve quantos arquivos havia para copiar (mesmo os pulados). */
  int copy_dir(const char* src_dir, const char* mask, const char* dst_dir, const char* stop_at, int depth) {
    Listing l;
    char a[XPATH], b[XPATH];
    int i, found = 0, made = 0;
    if (depth > DEPTH_MAX) {
      fail("pastas fundas demais", src_dir);
      return 0;
    }
    if (l.load(src_dir) < 0) {
      fail("nao abri a pasta", src_dir);
      return 0;
    }
    /* /E: cria a pasta mesmo vazia; sem /E so quando tiver arquivo. */
    if (empty || depth == 0) {
      if (make_dir(dst_dir) < 0) return 0;
      made = 1;
    }
    for (i = 0; i < l.n && !stop; i++) {
      struct k_dirent* e = &l.ent[i];
      if (e->dir) continue;
      if (!wild_match(mask, e->name)) continue;
      if (path_join(a, src_dir, e->name) < 0 || path_join(b, dst_dir, e->name) < 0) {
        fail("caminho longo demais", e->name);
        continue;
      }
      if (!made) {
        if (make_dir(dst_dir) < 0) return found;
        made = 1;
      }
      found++;
      copy_file(a, b, e);
    }
    if (!sub) return found;
    for (i = 0; i < l.n && !stop; i++) {
      struct k_dirent* e = &l.ent[i];
      if (!e->dir) continue;
      if (path_join(a, src_dir, e->name) < 0 || path_join(b, dst_dir, e->name) < 0) {
        fail("caminho longo demais", e->name);
        continue;
      }
      /* nao entra no destino quando ele fica dentro da origem */
      if (inside(stop_at, a)) continue;
      found += copy_dir(a, mask, b, stop_at, depth + 1);
    }
    return found;
  }
};

/* ---------- linha de comando ---------- */

static void usage(void) {
  printf("Copia arquivos e arvores de pastas.\n\n");
  printf("XCOPY origem [destino] [/S] [/E] [/I] [/Y] [/-Y] [/D] [/L] [/Q] [/F]\n\n");
  printf("  origem   arquivo, pasta ou mascara (* e ?)\n");
  printf("  destino  pasta ou arquivo (sem ele: a pasta atual)\n");
  printf("  /S  inclui as subpastas (menos as vazias)\n");
  printf("  /E  inclui as subpastas, ate as vazias (liga o /S)\n");
  printf("  /I  destino que nao existe e uma pasta\n");
  printf("  /Y  sobrescreve sem perguntar     /-Y  pergunta (padrao)\n");
  printf("  /D  so copia o que e mais novo que o destino\n");
  printf("  /L  so mostra o que seria copiado\n");
  printf("  /Q  nao mostra os nomes           /F  mostra origem -> destino\n\n");
  printf("Exemplos:\n");
  printf("  xcopy D:\\SRC U:\\BAK\\SRC /E /I\n");
  printf("  xcopy *.C U:\\FONTES /Y\n");
}

int main(int argc, char** argv) {
  XCopy x;
  const char* arg_src = 0;
  const char* arg_dst = 0;
  char src[XPATH], dst[XPATH], dir[XPATH], mask[XPATH], target[XPATH];
  int i, found;

  for (i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (a[0] == '/' && a[1] && (a[2] == 0 || (a[1] == '-' && a[3] == 0))) {
      int neg = a[1] == '-';
      int c = up(neg ? a[2] : a[1]);
      if (c == '?') {
        usage();
        return 0;
      }
      if (neg && c != 'Y') {
        printf("XCOPY: opcao invalida: %s\n", a);
        return 4;
      }
      switch (c) {
        case 'S': x.sub = 1; break;
        case 'E': x.sub = 1; x.empty = 1; break;
        case 'I': x.as_dir = 1; break;
        case 'Y': x.ask = neg; break;
        case 'D': x.newer = 1; break;
        case 'L': x.list_only = 1; break;
        case 'Q': x.quiet = 1; break;
        case 'F': x.show_full = 1; break;
        default:
          printf("XCOPY: opcao invalida: %s\n", a);
          return 4;
      }
    } else if (!arg_src) {
      arg_src = a;
    } else if (!arg_dst) {
      arg_dst = a;
    } else {
      printf("XCOPY: parametros demais: %s\n", a);
      return 4;
    }
  }
  if (!arg_src) {
    usage();
    return 4;
  }
  {
    /* SET COPYCMD=/Y, como no DOS (a linha de comando ainda vale mais) */
    char env[32];
    int asked = 0;
    for (i = 1; i < argc; i++)
      if (argv[i][0] == '/' && (up(argv[i][1]) == 'Y' || (argv[i][1] == '-' && up(argv[i][2]) == 'Y'))) asked = 1;
    if (!asked && k_getenv("COPYCMD", env, sizeof(env)) > 0 && env[0] == '/') {
      if (env[1] == '-' && up(env[2]) == 'Y') x.ask = 1;
      else if (up(env[1]) == 'Y') x.ask = 0;
    }
  }

  if (full_path(arg_src, src) < 0 || full_path(arg_dst ? arg_dst : "", dst) < 0) {
    printf("XCOPY: caminho longo demais\n");
    return 4;
  }

  /* origem: pasta (copia o conteudo) ou pasta + mascara/arquivo */
  if (!has_wild(src) && is_dir(src)) {
    strcpy(dir, src);
    strcpy(mask, "*");
  } else {
    split_path(src, dir, mask);
    if (!mask[0]) strcpy(mask, "*");
    if (!is_dir(dir)) {
      printf("XCOPY: pasta nao encontrada: %s\n", dir);
      return 4;
    }
  }

  /* um arquivo so, para um destino que nao e pasta: copia com o novo nome */
  if (!has_wild(mask) && strcmp(mask, "*") != 0 && !x.sub && !x.as_dir && !is_dir(dst) &&
      arg_dst && arg_dst[strlen(arg_dst) - 1] != '\\' && arg_dst[strlen(arg_dst) - 1] != '/') {
    struct k_dirent se;
    char d2[XPATH], n2[XPATH];
    if (stat_file(src, &se) < 0 || se.dir) {
      printf("XCOPY: arquivo nao encontrado: %s\n", src);
      return 4;
    }
    if (strcmp(src, dst) == 0) {
      printf("XCOPY: o arquivo nao pode ser copiado sobre ele mesmo\n");
      return 4;
    }
    split_path(dst, d2, n2);
    if (!is_dir(d2) && x.make_dir(d2) < 0) return 4;
    x.copy_file(src, dst, &se);
  } else {
    int n = (int)strlen(dir), m = (int)strlen(dst);
    if (n == m && inside(dir, dst)) {
      printf("XCOPY: origem e destino sao a mesma pasta\n");
      return 4;
    }
    /* sem /S nao precisa evitar o destino dentro da origem */
    strcpy(target, dst);
    found = x.copy_dir(dir, mask, dst, target, 0);
    if (!found && !x.errors && !x.stop) {
      printf("Nenhum arquivo encontrado: %s\n", src);
      return 1;
    }
  }

  if (x.stop) {
    printf("Cancelado. ");
  }
  if (x.list_only) printf("%d arquivo(s) a copiar\n", x.copied);
  else printf("%d arquivo(s) copiado(s)\n", x.copied);
  if (x.dirs_made && !x.quiet) printf("%d pasta(s) criada(s)\n", x.dirs_made);
  if (x.errors) {
    printf("%d erro(s)\n", x.errors);
    return 4;
  }
  if (x.stop) return 2;
  return 0;
}
