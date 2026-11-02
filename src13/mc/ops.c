/* ops.c - F5 copiar, F6 mover/renomear, F7 criar pasta, F8 apagar.
 * Valem para os arquivos marcados (Insert) ou, sem marca, para o do cursor.
 * Pastas sao copiadas e apagadas com tudo o que tem dentro. */
#include "mc.h"

#define DEPTH_MAX 8

static int over_all;     /* "Todos" na pergunta de sobrescrever */
static int stop_all;     /* "Cancelar" */
static int del_all;      /* "Todos" em apagar pasta cheia */

static int is_dir_path(const char* path) {
  struct k_dirent one;
  return k_dir(path, &one, 1) >= 0;
}

static const char* base_name(const char* path) {
  const char* b = path;
  for (; *path; path++)
    if (*path == '\\' || *path == ':') b = path + 1;
  return b;
}

static int ask_error(const char* what, const char* name) {
  char m[200];
  sprintf(m, "%s\n%s\n\nContinuar?", what, name);
  if (!dlg_yesno("Erro", m, 1)) stop_all = 1;
  return -1;
}

/* Lista uma pasta em memoria nova (a recursao nao pode usar buffer fixo). */
static struct k_dirent* list_dir(const char* path, int* n) {
  struct k_dirent* d = (struct k_dirent*)malloc(sizeof(struct k_dirent) * ENT_MAX);
  if (!d) {
    *n = -1;
    return 0;
  }
  *n = k_dir(path, d, ENT_MAX);
  return d;
}

static int copy_file(const char* src, const char* dst) {
  int size = k_fsize(src), got;
  char* buf;
  char m[200];
  if (size < 0) return ask_error("Nao achei", src);
  if (k_fsize(dst) >= 0 && !over_all) {
    int r;
    sprintf(m, "O arquivo ja existe:\n%s\n\nSobrescrever?", dst);
    r = dlg_ask4("Sobrescrever", m);
    if (r < 0) {
      stop_all = 1;
      return -1;
    }
    if (r == 0) return 1;     /* pulou */
    if (r == 2) over_all = 1;
  }
  sprintf(m, "%s\n%s", src, dst);
  dlg_note("Copiando", m);
  buf = (char*)malloc((size_t)size + 1);
  if (!buf) return ask_error("Sem memoria para", src);
  got = k_fload(src, buf, size + 1);
  if (got < 0) {
    free(buf);
    return ask_error("Nao consegui ler", src);
  }
  if (k_fsave(dst, buf, got) != 0) {
    free(buf);
    return ask_error("Nao consegui gravar", dst);
  }
  free(buf);
  return 0;
}

static int copy_tree(const char* src, const char* dst, int depth) {
  struct k_dirent* d;
  int n, i, rc = 0;
  char a[PATH_MAX_MC], b[PATH_MAX_MC];
  if (depth > DEPTH_MAX) return ask_error("Pastas fundas demais", src);
  if (k_mkdir(dst) < 0) return ask_error("Nao criei a pasta", dst);
  d = list_dir(src, &n);
  if (n < 0) {
    free(d);
    return ask_error("Nao abri a pasta", src);
  }
  for (i = 0; i < n && !stop_all; i++) {
    if (path_join(a, sizeof(a), src, d[i].name) != 0 || path_join(b, sizeof(b), dst, d[i].name) != 0) {
      rc = ask_error("Caminho longo demais", d[i].name);
      continue;
    }
    if (d[i].dir) {
      if (copy_tree(a, b, depth + 1) != 0) rc = -1;
    } else if (copy_file(a, b) < 0) {
      rc = -1;
    }
  }
  free(d);
  return rc;
}

static int delete_tree(const char* path, int depth, int ask) {
  struct k_dirent* d;
  int n, i, rc = 0;
  char a[PATH_MAX_MC];
  char m[200];
  if (depth > DEPTH_MAX) return ask_error("Pastas fundas demais", path);
  d = list_dir(path, &n);
  if (n < 0) {
    free(d);
    return ask_error("Nao abri a pasta", path);
  }
  if (n > 0 && ask && !del_all) {
    int r;
    sprintf(m, "A pasta nao esta vazia:\n%s\n\nApagar tudo o que tem dentro?", path);
    r = dlg_ask4("Apagar", m);
    if (r < 0) stop_all = 1;
    if (r == 2) del_all = 1;
    if (r <= 0) {
      free(d);
      return 1;
    }
  }
  for (i = 0; i < n && !stop_all; i++) {
    if (path_join(a, sizeof(a), path, d[i].name) != 0) continue;
    if (d[i].dir) {
      if (delete_tree(a, depth + 1, 0) != 0) rc = -1;
    } else {
      dlg_note("Apagando", a);
      if (k_fdelete(a) < 0) rc = ask_error("Nao apaguei", a);
    }
  }
  free(d);
  if (rc == 0 && !stop_all && k_rmdir(path) != 0) rc = ask_error("Nao removi a pasta", path);
  return rc;
}

/* Itens marcados, ou o do cursor. Devolve quantos e o primeiro em *one. */
static int targets(struct panel* p, struct ent** one) {
  int n = panel_marked(p, 0);
  struct ent* e = panel_cur(p);
  int i;
  *one = 0;
  if (n) {
    for (i = 0; i < p->n; i++)
      if (p->e[i].mark) {
        *one = &p->e[i];
        break;
      }
    return n;
  }
  if (!e || e->up) return 0;
  *one = e;
  return 1;
}

static void after_op(void) {
  panel_reload(&pan[0]);
  panel_reload(&pan[1]);
  draw_all();
}

void op_copy(int move) {
  struct panel* p = &pan[act];
  struct ent* one;
  int n = targets(p, &one), i, dst_dir;
  char dest[PATH_MAX_MC], prompt[120], src[PATH_MAX_MC], to[PATH_MAX_MC];
  const char* verb = move ? "Mover" : "Copiar";
  if (!n) return;
  strcpy(dest, pan[1 - act].path);
  if (n == 1) sprintf(prompt, "%s \"%s\" para:", verb, one->name);
  else sprintf(prompt, "%s %d itens marcados para:", verb, n);
  if (!dlg_input(move ? "Mover / Renomear" : "Copiar", prompt, dest, sizeof(dest))) return;
  if (!dest[0]) return;
  /* destino relativo: dentro da pasta deste painel */
  if (!(dest[0] && dest[1] == ':')) {
    char t[PATH_MAX_MC];
    if (path_join(t, sizeof(t), p->path, dest) != 0) return;
    strcpy(dest, t);
  }
  if (dest[0] >= 'a' && dest[0] <= 'z') dest[0] = (char)(dest[0] - 32);
  if (strlen(dest) == 2) strcat(dest, "\\");
  dst_dir = is_dir_path(dest);
  if (!dst_dir && n > 1) {
    dlg_msg("Erro", "Com varios itens o destino precisa ser uma pasta.", 1);
    return;
  }
  over_all = stop_all = 0;
  for (i = 0; i < p->n && !stop_all; i++) {
    struct ent* e = &p->e[i];
    int rc;
    if (n > 1 ? !e->mark : e != one) continue;
    if (path_join(src, sizeof(src), p->path, e->name) != 0) continue;
    if (dst_dir) {
      if (path_join(to, sizeof(to), dest, e->name) != 0) continue;
    } else {
      strcpy(to, dest);
    }
    if (name_eq_path(src, to)) {
      dlg_msg("Erro", "Origem e destino sao o mesmo arquivo.", 1);
      continue;
    }
    if (e->dir) {
      int l = (int)strlen(src);
      if (strncmp(to, src, (size_t)l) == 0 && to[l] == '\\') {
        dlg_msg("Erro", "Nao da para copiar uma pasta para dentro dela.", 1);
        continue;
      }
      rc = copy_tree(src, to, 0);
      if (rc == 0 && move && !stop_all) {
        del_all = 1;
        rc = delete_tree(src, 0, 0);
      }
    } else {
      rc = copy_file(src, to);
      if (rc == 0 && move) {
        if (k_fdelete(src) < 0) ask_error("Copiei, mas nao apaguei", src);
      }
    }
    if (rc == 0) e->mark = 0;
  }
  after_op();
  if (!dst_dir && n == 1) {
    /* renomeou na mesma pasta: cursor no nome novo */
    int k;
    for (k = 0; k < p->n; k++)
      if (name_eq_path(p->e[k].name, base_name(dest))) p->cur = k;
  }
}

void op_delete(void) {
  struct panel* p = &pan[act];
  struct ent* one;
  int n = targets(p, &one), i;
  char m[160], path[PATH_MAX_MC];
  if (!n) return;
  if (n == 1) sprintf(m, "Apagar %s \"%s\"?", one->dir ? "a pasta" : "o arquivo", one->name);
  else sprintf(m, "Apagar os %d itens marcados?", n);
  if (!dlg_yesno("Apagar", m, 1)) return;
  stop_all = del_all = 0;
  for (i = 0; i < p->n && !stop_all; i++) {
    struct ent* e = &p->e[i];
    if (n > 1 ? !e->mark : e != one) continue;
    if (path_join(path, sizeof(path), p->path, e->name) != 0) continue;
    if (e->dir) {
      delete_tree(path, 0, 1);
    } else {
      int rc = k_fdelete(path);
      if (rc == -4) ask_error("Arquivo somente leitura", path);
      else if (rc < 0) ask_error("Nao apaguei", path);
    }
  }
  after_op();
}

void op_mkdir(void) {
  struct panel* p = &pan[act];
  char name[64], path[PATH_MAX_MC];
  int rc, k;
  name[0] = 0;
  if (!dlg_input("Criar pasta", "Nome da nova pasta:", name, sizeof(name)) || !name[0]) return;
  if (path_join(path, sizeof(path), p->path, name) != 0) return;
  rc = k_mkdir(path);
  if (rc == 1) dlg_msg("Erro", "Ja existe.", 1);
  else if (rc == -5) dlg_msg("Erro", "So da para criar pasta em disco FAT.", 1);
  else if (rc != 0) dlg_msg("Erro", "Nao criei a pasta.", 1);
  panel_reload(p);
  for (k = 0; k < p->n; k++)
    if (name_eq_path(p->e[k].name, name)) p->cur = k;
  draw_all();
}
