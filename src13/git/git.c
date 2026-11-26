/* git.c - comandos do git do SODOS.
 *   git clone https://github.com/dono/projeto [pasta]
 *   git pull [pasta]       busca o commit novo e atualiza os arquivos
 *   git pull --rebase [--autostash]   refaz os commits daqui por cima do servidor
 *   git status [pasta]     o que mudou (staged, nao staged, nao rastreado)
 *   git add -u | -A | arquivo...   prepara o proximo commit
 *   git commit [-a] -m mensagem    grava o commit (objetos soltos em .git\objects)
 *   git push               manda os commits locais (sem login, chama o git login)
 *   git login | logout     entra no GitHub pelo navegador (codigo) | apaga o token
 *   git diff [--cached] [--numstat|--name-only|--name-status] [HEAD] [arquivo...]
 *   git log [--oneline|--format=...] [-n N]
 *   git config [--global] [--get] chave [valor]
 *   consulta: rev-parse, branch, symbolic-ref, remote, worktree list, ls-files
 * O clone e raso (so o ultimo commit). Os arquivos ficam com o nome de
 * verdade (o FAT do SODOS tem nomes longos); so um nome que o FAT nao aceita
 * (com \ : * ? " < > |) vai com _ no lugar. A lista fica em .git\sodos\NAMES.TXT.
 *
 * Versoes (GIT_SODOS_VER; aparece em git --version e no git sem argumentos):
 *   1.0.12  git login/logout: entra no GitHub pelo navegador com um codigo,
 *           sem criar token a mao; o primeiro push sem login chama o login;
 *           clone/pull de repositorio privado do GitHub com esse token.
 *           Nomes longos em tudo: acabou o nome 8.3 de reserva (LEIAME~1.MD)
 *           e o aviso de 8.3 da ajuda.
 *   1.0.13  git login: /user com User-Agent proprio (a API do GitHub recusa
 *           "git/..." com 403), entao o login volta a saber o usuario; aviso
 *           quando o clientid e de um GitHub App (Iv..., token ghu_) em vez
 *           de OAuth App. Push recusado (403) mostra o motivo do GitHub.
 *   1.0.14  Client ID do OAuth App "SODOS git" (Ov23liW7Qf9Rc1K7lI4h) dentro
 *           do GIT.EXE: git login e o primeiro push funcionam sem configurar
 *           nada (git config --global clientid ... ainda troca o app).
 *   1.0.15  Arquivo de 0 bytes nao aparece mais como "apagado" no status/add:
 *           o load_file confirma pela entrada da pasta (k_dir_entry).
 *           Prototipo do git_file no topo: acabou o aviso "funcao 'git_file'
 *           sem declaracao" na compilacao.
 *   1.0.16  git pull --rebase (-r, --rebase=..., git config pull.rebase true):
 *           os commits daqui sao refeitos por cima do commit novo do servidor;
 *           arquivo mudado dos dois lados e juntado por linha (diff3), e
 *           conflito cancela tudo sem mexer na pasta nem no .git. --autostash
 *           (ou rebase.autoStash) junta as mudancas sem commit; conflito ali
 *           fica marcado com <<<<<<< no arquivo. --no-rebase, --ff-only.
 *           git pull com commits daqui e o servidor parado dizia "o servidor
 *           mudou". git reset origin gravava a versao do servidor por cima do
 *           que o commit local tinha mudado: agora a pasta nao muda mesmo.
 *   1.0.17  Convive com outro git no mesmo .git (o da VM do WSL, o Git for
 *           Windows). Antes, depois de QUALQUER comando (ate o rev-parse e o
 *           config que o Claude Desktop chama sem parar) o .git era regravado
 *           do STATE.TXT: refs, config inteiro, logs\HEAD e .git\shallow.
 *           Commit, fetch ou rebase do outro git sumiam, o config perdia
 *           user.name, e o shallow regravado cortava o historico (merge sem
 *           base comum). Agora: (1) o .git so e escrito depois de um comando
 *           que mudou o estado (commit, add, pull, push, reset, restore); o
 *           config ganha so o que falta e o logs\HEAD so ganha linha; o
 *           shallow so recebe o commit que o clone/pull baixou raso, e so se
 *           o pai dele nao estiver em nenhum objeto/pack. (2) Se o outro git
 *           mudou o .git\refs\heads\<ramo> ou o origin, o GIT.EXE adota:
 *           refaz o NAMES.TXT da arvore nova (a pasta nao muda) e o remote=
 *           vira o origin dele. (3) push: anda por todos os pais (merge) e
 *           aceita quando o commit do servidor ja esta no historico daqui
 *           (o outro git fez merge/rebase); o "servidor mudou" so sai quando
 *           falta mesmo, e manda rodar git pull. (4) git pull refaz os
 *           commits daqui por cima do servidor por padrao (pull.rebase=false
 *           volta ao antigo). (5) for-each-ref (o Claude Desktop pede).
 *           (6) .gitignore de cada pasta (o do SODOS esta em src13): o
 *           status listava os .o/.obj e o add -A os punha no commit; ** casa
 *           com nenhuma pasta. (7) commit -F arquivo (mensagem de varias
 *           linhas) e -m repetido vira paragrafo. (8) git status colorido
 *           (verde no stage, vermelho fora e nao rastreado) quando a saida
 *           vai para a tela (k_con_color do kernel; color.status/color.ui,
 *           --color, --no-color); nao rastreado sai com /.
 *
 * Repositorio no formato padrao, em .git:
 *   HEAD, config, refs\heads\<ramo>, refs\remotes\origin\<ramo>, logs\HEAD
 *   objects\pack\pack-<sha>.pack + .idx   o pack do clone/pull
 *   objects\xx\<38 hexa>                  objetos criados aqui (zlib)
 *   index                                 DIRC v2 do que vai no commit
 * e o que e so do git do SODOS em .git\sodos:
 *   STATE.TXT  url, ref, commit (o HEAD local) e remote (o ultimo do servidor)
 *   NAMES.TXT  arquivos do HEAD: nome no disco TAB nome no git TAB sha TAB modo
 *   INDEX.TXT  mesmo formato, o que vai no proximo commit (sem ele = NAMES)
 *   COMMIT.TXT o commit do clone/pull; PACK.TXT o caminho do pack
 *   CONFIG.TXT e X:\GITCONF.TXT (X = disco do GIT.EXE): chave=valor
 * Repositorios antigos (_GIT) passam para .git sozinhos no primeiro comando. */
#include "git.h"

/* O repositorio fica em .git, como em qualquer git: HEAD, config, refs,
 * logs, objects (pack do clone com .idx e objetos soltos com zlib) e index.
 * O que e so do git do SODOS (estado, lista de nomes, o que vai no
 * commit) fica em .git\sodos. Repositorios antigos (_GIT) sao migrados
 * sozinhos no primeiro comando. */
#define GIT_SODOS_VER "1.0.17"
#define GITDIR ".git\\sodos"
#define OLDDIR "_GIT"
#define PATHMAX 260

static char err[320];
/* 1 depois de gravar STATE.TXT, NAMES.TXT ou INDEX.TXT: so ai o .git padrao
 * e posto em dia (sync_dotgit). Consulta nao escreve no .git. */
static int st_dirty;
/* adopt_dotgit sem mensagem: comandos de maquina (rev-parse, config...) */
static int adopt_quiet;
static void adopt_dotgit(const char* root);
static void sync_dotgit(const char* root);
static int gh_auth(const char* url);
static void git_file(const char* root, const char* name, char* out);

/* ---------- utilidades ---------- */

static void path_join(char* out, const char* a, const char* b) {
  int n = (int)strlen(a);
  strcpy(out, a);
  if (n && out[n - 1] != '\\') out[n++] = '\\';
  strcpy(out + n, b);
}

static int is_dir(const char* path) {
  struct k_dirent one;
  return k_dir(path, &one, 1) >= 0;
}

static int dir_entries(const char* path) {
  struct k_dirent one;
  return k_dir(path, &one, 1);
}

/* Arquivo inteiro (malloc, com 0 no fim). 0 se nao existe.
 * Arquivo de 0 bytes nao tem cluster no FAT, e o k_fsize/k_fload podem dar
 * erro nele: quem decide se existe e a entrada da pasta (k_dir_entry). Sem
 * isso o git status dava "apagado" para um arquivo vazio que esta ali. */
static char* load_file(const char* path, int* len) {
  int n = k_fsize(path);
  char* b;
  if (n <= 0) {
    struct k_direntl d;
    if (n < 0 && (k_dir_entry(path, &d) != 0 || d.dir || d.size != 0)) return 0;
    b = (char*)malloc(1);
    if (!b) return 0;
    b[0] = 0;
    *len = 0;
    return b;
  }
  b = (char*)malloc((size_t)n + 1);
  if (!b) return 0;
  n = k_fload(path, b, n + 1);
  if (n < 0) {
    free(b);
    return 0;
  }
  b[n] = 0;
  *len = n;
  return b;
}

static int file_sha(const char* path, unsigned char* sha) {
  int n;
  char* b = load_file(path, &n);
  if (!b) return -1;
  git_hash("blob", (unsigned char*)b, n, sha);
  free(b);
  return 0;
}

static char* xstrdup(const char* s) {
  char* d = (char*)malloc(strlen(s) + 1);
  strcpy(d, s);
  return d;
}

static int up(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
static int lowc(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int ci_cmp(const char* a, const char* b) {
  for (;; a++, b++) {
    int x = up((unsigned char)*a), y = up((unsigned char)*b);
    if (x != y || !x) return x - y;
  }
}

static int ci_start(const char* s, const char* p) {
  for (; *p; s++, p++)
    if (up((unsigned char)*s) != up((unsigned char)*p)) return 0;
  return 1;
}

static int sha_zero(const unsigned char* s) {
  int i;
  for (i = 0; i < 20; i++)
    if (s[i]) return 0;
  return 1;
}

/* sha em hexa no buffer out (41 bytes), para usar dentro de um printf */
static const char* hex_of(const unsigned char* sha, char* out) {
  sha_hex(sha, out);
  return out;
}

struct sbuf {
  char* p;
  int len;
  int cap;
};

static void sb_add(struct sbuf* s, const char* t, int n) {
  if (n < 0) n = (int)strlen(t);
  if (s->len + n + 1 > s->cap) {
    int nc = s->cap ? s->cap * 2 : 4096;
    while (nc < s->len + n + 1) nc *= 2;
    s->p = (char*)realloc(s->p, (size_t)nc);
    s->cap = nc;
  }
  memcpy(s->p + s->len, t, (size_t)n);
  s->len += n;
  s->p[s->len] = 0;
}

static void sb_byte(struct sbuf* s, int c) {
  char ch = (char)c;
  sb_add(s, &ch, 1);
}

static void pkt_n(struct sbuf* s, const char* line, int n) {
  char h[8];
  sprintf(h, "%04x", n + 4);
  sb_add(s, h, 4);
  sb_add(s, line, n);
}

static void pkt(struct sbuf* s, const char* line) { pkt_n(s, line, (int)strlen(line)); }

static int pkt_len(const char* p) {
  int i, v = 0;
  for (i = 0; i < 4; i++) {
    int c = (unsigned char)p[i], d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return -1;
    v = v * 16 + d;
  }
  return v;
}

/* ---------- rede: refs e pack ---------- */

struct remote {
  char url[300];
  char ref[160];
  unsigned char head[20];
};

static void url_clean(char* u) {
  int n = (int)strlen(u);
  while (n > 0 && u[n - 1] == '/') u[--n] = 0;
}

/* push = 0: info/refs do upload-pack, acha HEAD e o ramo dele.
 * push = 1: info/refs do receive-pack, r->head = sha de r->ref (zeros se nao existe). */
static int get_refs(struct remote* r, int push) {
  char url[400];
  char* body;
  int len, st, p = 0, have = 0;
  unsigned char main_sha[20];
  int have_main = 0;
  sprintf(url, "%s/info/refs?service=%s", r->url, push ? "git-receive-pack" : "git-upload-pack");
  printf("buscando refs de %s\n", r->url);
  st = http_req(url, 0, 0, 0, &body, &len, err);
  /* repositorio privado do GitHub: de novo com o token do git login */
  if (!push && (st == 401 || st == 403 || st == 404) && !http_auth[0] && gh_auth(r->url)) {
    free(body);
    st = http_req(url, 0, 0, 0, &body, &len, err);
  }
  if (st < 0) return -1;
  if (st == 401 || st == 403) {
    if (push) {
      /* o GitHub diz o motivo no corpo: "Permission to dono/repo.git denied to fulano." */
      char why[200];
      int k = 0;
      while (body && k < len && k < (int)sizeof(why) - 1 && body[k] != '\n' && body[k] != '\r') {
        why[k] = body[k];
        k++;
      }
      why[k] = 0;
      if (st == 401) sprintf(err, "HTTP 401: o servidor recusou o login (rode git login de novo)");
      else sprintf(err, "HTTP 403: %s\n  (o token nao pode escrever aqui; veja git login)", why[0] ? why : "sem permissao");
    } else {
      sprintf(err, "repositorio nao encontrado (se for privado: git login): HTTP %d", st);
    }
    free(body);
    return -1;
  }
  if (st == 404) {
    sprintf(err, "repositorio nao encontrado: HTTP 404");
    free(body);
    return -1;
  }
  if (st != 200 || len < 4) {
    sprintf(err, "HTTP %d em info/refs (o servidor fala o protocolo do git?)", st);
    free(body);
    return -1;
  }
  if (push) memset(r->head, 0, 20);
  else r->ref[0] = 0;
  while (p + 4 <= len) {
    int n = pkt_len(body + p);
    char line[512];
    int l;
    if (n < 0) break;
    if (n == 0) {
      p += 4;
      continue;
    }
    if (p + n > len || n < 4) break;
    l = n - 4 < (int)sizeof(line) - 1 ? n - 4 : (int)sizeof(line) - 1;
    memcpy(line, body + p + 4, (size_t)l);
    line[l] = 0;
    p += n;
    if (line[0] == '#') continue;
    if (l > 41 && line[40] == ' ') {
      char* name = line + 41;
      char* caps = name + strlen(name) + 1;
      int nl = (int)strlen(name);
      if (nl && name[nl - 1] == '\n') name[--nl] = 0;
      if (push) {
        if (strcmp(name, r->ref) == 0) hex_sha(line, r->head);
        continue;
      }
      if (strcmp(name, "HEAD") == 0) {
        hex_sha(line, r->head);
        have = 1;
        if (caps < line + l) {
          char* s = strstr(caps, "symref=HEAD:");
          if (s) {
            int k = 0;
            s += 12;
            while (*s && *s != ' ' && *s != '\n' && k < (int)sizeof(r->ref) - 1) r->ref[k++] = *s++;
            r->ref[k] = 0;
          }
        }
      } else if (!have_main && (strcmp(name, "refs/heads/main") == 0 || strcmp(name, "refs/heads/master") == 0)) {
        hex_sha(line, main_sha);
        have_main = 1;
        if (!r->ref[0]) strcpy(r->ref, name);
      }
    }
  }
  free(body);
  if (push) return 0;
  if (!have && have_main) {
    memcpy(r->head, main_sha, 20);
    have = 1;
  }
  if (!have) {
    /* repositorio vazio (recem-criado no GitHub): sem commit; o primeiro
     * commit daqui cria o ramo no push */
    memset(r->head, 0, 20);
    if (!r->ref[0]) strcpy(r->ref, "refs/heads/main");
    return 1;
  }
  if (!r->ref[0]) strcpy(r->ref, "HEAD");
  return 0;
}

/* Pede so o commit r->head (deepen 1). *buf e a resposta; *pack aponta dentro. */
static int get_pack(struct remote* r, char** buf, unsigned char** pack, int* plen) {
  struct sbuf req;
  char line[200], hex[41], url[400];
  int len, st, p = 0;
  memset(&req, 0, sizeof(req));
  sha_hex(r->head, hex);
  sprintf(line, "want %s ofs-delta shallow no-progress agent=git/sodos\n", hex);
  pkt(&req, line);
  pkt(&req, "deepen 1\n");
  sb_add(&req, "0000", 4);
  pkt(&req, "done\n");
  sprintf(url, "%s/git-upload-pack", r->url);
  printf("baixando o pack\n");
  st = http_req(url, "application/x-git-upload-pack-request", req.p, req.len, buf, &len, err);
  free(req.p);
  if (st < 0) return -1;
  if (st != 200) {
    sprintf(err, "HTTP %d em git-upload-pack", st);
    free(*buf);
    return -1;
  }
  /* shallow ..., 0000, NAK, depois o PACK cru */
  while (p + 4 <= len) {
    int n;
    if (memcmp(*buf + p, "PACK", 4) == 0) {
      *pack = (unsigned char*)*buf + p;
      *plen = len - p;
      return 0;
    }
    n = pkt_len(*buf + p);
    if (n < 0) break;
    if (n == 0) {
      p += 4;
      continue;
    }
    if (n >= 7 && memcmp(*buf + p + 4, "ERR", 3) == 0) {
      int l = n - 4 < 200 ? n - 4 : 200;
      memcpy(err, *buf + p + 4, (size_t)l);
      err[l] = 0;
      free(*buf);
      return -1;
    }
    p += n;
  }
  sprintf(err, "a resposta nao trouxe o pack");
  free(*buf);
  return -1;
}

/* ---------- nomes no disco ----------
 * O FAT do SODOS tem nomes longos: cada arquivo fica com o nome do git.
 * is83 so serve ao git add, para nomes que o FAT antigo gravava em
 * maiusculas (NOVO.TXT vira novo.txt). */

static int ok83(int c) {
  if (c >= 'A' && c <= 'Z') return 1;
  if (c >= '0' && c <= '9') return 1;
  return strchr("!#$%&'()-@^_`{}~", c) != 0 && c != 0;
}

/* 1 se o nome ja e 8.3 (sem contar maiusculas). */
static int is83(const char* s) {
  int b = 0, e = -1;
  if (s[0] == '.') return 0;
  for (; *s; s++) {
    int c = up((unsigned char)*s);
    if (c == '.') {
      if (e >= 0) return 0;
      e = 0;
      continue;
    }
    if (!ok83(c)) return 0;
    if (e >= 0) {
      if (++e > 3) return 0;
    } else if (++b > 8) {
      return 0;
    }
  }
  return b > 0 && e != 0;
}


/* Nome que o FAT com nomes longos aceita como esta: sem \ / : * ? " < > |,
 * sem controle, ate 200 caracteres, sem terminar em ponto ou espaco. */
static int long_ok(const char* n) {
  int l = (int)strlen(n), i;
  if (!l || l > 200 || n[l - 1] == '.' || n[l - 1] == ' ') return 0;
  if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) return 0;
  for (i = 0; i < l; i++) {
    unsigned char c = (unsigned char)n[i];
    if (c < 32 || strchr("\\/:*?\"<>|", c)) return 0;
  }
  return 1;
}

/* 1 se s ja e o nome de outra entrada da arvore ou de um nome ja trocado
 * nesta pasta (o FAT nao diferencia maiusculas). */
static int name_taken(const char* s, const unsigned char* tree, int size, char (*used)[256], int nused) {
  int p = 0, i;
  for (i = 0; i < nused; i++)
    if (ci_cmp(used[i], s) == 0) return 1;
  while (tree && p < size) {
    while (p < size && tree[p] != ' ') p++;
    p++;
    if (p < size && ci_cmp((const char*)tree + p, s) == 0) return 1;
    while (p < size && tree[p]) p++;
    p += 21;
  }
  return 0;
}

/* Nome no disco para um nome do git que o FAT nao aceita (\ / : * ? " < > |,
 * controle, ponto ou espaco no fim): continua longo, com _ no lugar do que
 * nao pode; se colidir na pasta, ganha ~N antes da extensao. O nome de
 * verdade fica no NAMES.TXT. out: 256 bytes. */
static void disk_name(const char* name, const unsigned char* tree, int size, char (*used)[256], int nused, char* out) {
  char base[210], ext[210];
  char* dot;
  const char* p;
  int l = 0, k;
  for (p = name; *p && l < 200; p++) {
    unsigned char c = (unsigned char)*p;
    base[l++] = (char)((c < 32 || strchr("\\/:*?\"<>|", c)) ? '_' : c);
  }
  if (!l) base[l++] = '_';
  base[l] = 0;
  if (base[l - 1] == '.' || base[l - 1] == ' ') base[l - 1] = '_';
  strcpy(out, base);
  if (!name_taken(out, tree, size, used, nused)) return;
  dot = strrchr(base, '.');
  ext[0] = 0;
  if (dot && dot != base) {
    strcpy(ext, dot);
    *dot = 0;
  }
  for (k = 1; k < 100000; k++) {
    sprintf(out, "%s~%d%s", base, k, ext);
    if (!name_taken(out, tree, size, used, nused)) return;
  }
}

/* ---------- listas de arquivos (NAMES.TXT, INDEX.TXT) ----------
 * linha: nome no disco TAB nome no git TAB sha [TAB modo]. Disco "*" = nao
 * esta no disco (caminho longo demais ou submodulo), mas faz parte da arvore. */

struct nent {
  char* dos;
  char* lng;
  unsigned char sha[20];
  char mode[8];
  int hasmode;
  int seen;
  int gone;
};

struct list {
  struct nent* e;
  int n;
  int cap;
  char* text;
};

static int on_disk(const struct nent* e) { return !e->gone && e->dos[0] != '*'; }

static int nent_cmp(const void* a, const void* b) {
  return strcmp(((const struct nent*)a)->lng, ((const struct nent*)b)->lng);
}

static void list_sort(struct list* l) {
  if (l->n > 1) qsort(l->e, (size_t)l->n, sizeof(struct nent), nent_cmp);
}

static struct nent* list_push(struct list* l) {
  struct nent* e;
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 256;
    l->e = (struct nent*)realloc(l->e, sizeof(struct nent) * (size_t)l->cap);
  }
  e = &l->e[l->n++];
  memset(e, 0, sizeof(*e));
  strcpy(e->mode, "100644");
  return e;
}

/* le _GIT\<file>; -1 se nao existe */
static int list_load(const char* root, const char* file, struct list* l) {
  char path[PATHMAX];
  int len;
  char* p;
  memset(l, 0, sizeof(*l));
  sprintf(path, "%s\\%s\\%s", root, GITDIR, file);
  l->text = load_file(path, &len);
  if (!l->text) return -1;
  p = l->text;
  while (*p) {
    char* f[4];
    int nf = 1;
    char* nl = strchr(p, '\n');
    struct nent* e;
    if (!nl) break;
    *nl = 0;
    if (nl > p && nl[-1] == '\r') nl[-1] = 0;
    f[0] = p;
    p = nl + 1;
    while (nf < 4 && (f[nf] = strchr(f[nf - 1], '\t')) != 0) {
      *f[nf]++ = 0;
      nf++;
    }
    if (nf < 3 || strlen(f[2]) < 40) continue;
    e = list_push(l);
    e->dos = f[0];
    e->lng = f[1];
    if (hex_sha(f[2], e->sha) != 0) {
      l->n--;
      continue;
    }
    if (nf > 3 && f[3][0]) {
      strncpy(e->mode, f[3], sizeof(e->mode) - 1);
      e->hasmode = 1;
    }
  }
  list_sort(l);
  return l->n;
}

static int list_save(const char* root, const char* file, struct list* l) {
  struct sbuf s;
  char path[PATHMAX], hex[41];
  int i, r;
  memset(&s, 0, sizeof(s));
  list_sort(l);
  for (i = 0; i < l->n; i++) {
    struct nent* e = &l->e[i];
    if (e->gone) continue;
    sha_hex(e->sha, hex);
    sb_add(&s, e->dos, -1);
    sb_add(&s, "\t", 1);
    sb_add(&s, e->lng, -1);
    sb_add(&s, "\t", 1);
    sb_add(&s, hex, 40);
    /* sem modo conhecido (NAMES.TXT antigo) fica sem: o commit pega o do HEAD */
    if (e->hasmode) {
      sb_add(&s, "\t", 1);
      sb_add(&s, e->mode, -1);
    }
    sb_add(&s, "\n", 1);
  }
  sprintf(path, "%s\\%s\\%s", root, GITDIR, file);
  r = k_fsave(path, s.p ? s.p : "", s.len);
  free(s.p);
  st_dirty = 1;
  return r;
}

/* procura pelo nome longo (lista ordenada); ignora as apagadas */
static struct nent* list_find(struct list* l, const char* lng) {
  int lo = 0, hi = l->n - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2, c = strcmp(l->e[m].lng, lng);
    if (!c) return l->e[m].gone ? 0 : &l->e[m];
    if (c < 0) lo = m + 1;
    else hi = m - 1;
  }
  return 0;
}

static struct nent* list_find_dos(struct list* l, const char* dos) {
  int i;
  for (i = 0; i < l->n; i++)
    if (!l->e[i].gone && ci_cmp(l->e[i].dos, dos) == 0) return &l->e[i];
  return 0;
}

/* INDEX.TXT, ou NAMES.TXT se nao tem nada staged. 1 = veio do INDEX */
static int index_load(const char* root, struct list* l) {
  if (list_load(root, "INDEX.TXT", l) >= 0) return 1;
  list_load(root, "NAMES.TXT", l);
  return 0;
}

static struct list olds;   /* NAMES.TXT antes do pull */

/* ---------- checkout ---------- */

struct co {
  const char* root;      /* "D:\\SODOS" */
  struct sbuf names;
  int keep;              /* so refaz a lista: nao grava nada que ja existia */
  int loose;             /* pull --rebase: arvores e blobs tambem de .git\objects */
  int files;
  int written;
  int skipped;           /* de proposito: caminho longo demais */
  int failed;            /* erro: objeto sem pack ou k_fsave falhou */
  int kept;
  int quiet;             /* adopt_dotgit: sem mensagem por arquivo */
};

static unsigned char* obj_read(const char* root, const unsigned char* sha, int* type, int* len);

/* objeto para o checkout: o pack carregado; com loose tambem os soltos.
 * *own = 1: e malloc (quem chamou libera) */
static unsigned char* co_get(struct co* c, const unsigned char* sha, int* type, int* size, int* own) {
  unsigned char* d = pack_get(sha, type, size);
  *own = 0;
  if (d || !c->loose) return d;
  d = obj_read(c->root, sha, type, size);
  if (d) *own = 1;
  return d;
}

static int depth_of(const char* path) {
  int n = 0;
  for (; *path; path++)
    if (*path == '\\') n++;
  return n;
}

static void names_line(struct co* c, const char* dos, const char* lng, const unsigned char* sha, const char* mode) {
  char hex[41];
  sha_hex(sha, hex);
  sb_add(&c->names, dos, -1);
  sb_add(&c->names, "\t", 1);
  sb_add(&c->names, lng, -1);
  sb_add(&c->names, "\t", 1);
  sb_add(&c->names, hex, 40);
  sb_add(&c->names, "\t", 1);
  sb_add(&c->names, mode, -1);
  sb_add(&c->names, "\n", 1);
}

/* Cria as pastas do meio de full (abaixo de root, que ja existe): o k_fsave
 * nao cria, e sem isso cobra\BUILD.BAT nao e gravado. */
static void make_parents(const char* root, const char* full) {
  char d[PATHMAX + 40];
  int i;
  strncpy(d, full, sizeof(d) - 1);
  d[sizeof(d) - 1] = 0;
  for (i = (int)strlen(root); d[i]; i++) {
    if (d[i] != '\\') continue;
    d[i] = 0;
    if (!is_dir(d) && k_mkdir(d) != 0) printf("  nao criei a pasta %s\n", d);
    d[i] = '\\';
  }
}

static void put_file(struct co* c, const char* dosrel, const char* lng, const unsigned char* sha, const char* mode) {
  char full[PATHMAX + 40], hex[41];
  struct nent* o = olds.n ? list_find(&olds, lng) : 0;
  unsigned char* data;
  int type, size, own;
  c->files++;
  sha_hex(sha, hex);
  path_join(full, c->root, dosrel);
  /* caminho do FAT: ate 259 caracteres (DRV_PATH do kernel) */
  if ((int)strlen(full) > 250 || depth_of(full) > 40) {
    if (c->skipped < 8 && !c->quiet) printf("  caminho longo demais, pulado: %s\n", lng);
    c->skipped++;
    names_line(c, "*", lng, sha, mode);
    return;
  }
  names_line(c, dosrel, lng, sha, mode);
  /* keep (git reset origin, NAMES.TXT antigo): so a lista; a pasta nao muda.
   * Antes o reset gravava a versao do servidor por cima do que o commit
   * local tinha mudado (e dizia que a pasta nao mudava). */
  if (c->keep) {
    if (o) o->seen = 1;
    return;
  }
  if (o) {
    unsigned char now[20];
    o->seen = 1;
    if (memcmp(o->sha, sha, 20) == 0 && strcmp(o->dos, dosrel) == 0 && (c->keep || k_fsize(full) >= 0)) return;
    if (file_sha(full, now) == 0 && memcmp(now, o->sha, 20) != 0 && memcmp(now, sha, 20) != 0) {
      /* no rebase so chega aqui o que o --autostash junta depois */
      if (!c->loose) printf("  mudado aqui, mantido: %s\n", lng);
      c->kept++;
      return;
    }
  }
  data = co_get(c, sha, &type, &size, &own);
  if (!data || type != OBJ_BLOB) {
    printf("  objeto %s nao veio no pack: %s\n", hex, lng);
    if (own) free(data);
    c->failed++;
    return;
  }
  make_parents(c->root, full);
  if (k_fsave(full, (const char*)data, size) != 0) {
    printf("  nao gravou %s (%s)\n", full, lng);
    if (own) free(data);
    c->failed++;
    return;
  }
  if (own) free(data);
  c->written++;
  if ((c->written & 15) == 0) printf("\r  %d arquivos", c->written);
}

static int checkout_tree(struct co* c, const unsigned char* tsha, const char* dosrel, const char* lngrel) {
  unsigned char* t;
  unsigned char* tree;
  int type, size, p, n = 0, nused = 0, own;
  char (*used)[256];
  t = co_get(c, tsha, &type, &size, &own);
  if (!t || type != OBJ_TREE) {
    if (own) free(t);
    sprintf(err, "arvore que nao veio no pack");
    return -1;
  }
  tree = (unsigned char*)malloc((size_t)size + 1);
  if (!tree) {
    if (own) free(t);
    sprintf(err, "sem memoria");
    return -1;
  }
  memcpy(tree, t, (size_t)size);
  tree[size] = 0;
  if (own) free(t);
  for (p = 0; p < size; p++)
    if (tree[p] == 0) n++;
  used = 0; /* nomes trocados nesta pasta: so se algum nome nao servir */
  p = 0;
  while (p < size) {
    char mode[8], name[256], fixed[256], dos[PATHMAX], lng[PATHMAX + 256];
    const unsigned char* sha;
    int k = 0;
    while (p < size && tree[p] != ' ' && k < 7) mode[k++] = (char)tree[p++];
    mode[k] = 0;
    p++;
    k = 0;
    while (p < size && tree[p] && k < 255) name[k++] = (char)tree[p++];
    name[k] = 0;
    p++;
    sha = tree + p;
    p += 20;
    if (lngrel[0]) sprintf(lng, "%s/%s", lngrel, name);
    else strcpy(lng, name);
    if (strcmp(mode, "160000") == 0) {
      if (!c->quiet) printf("  submodulo ignorado: %s\n", lng);
      names_line(c, "*", lng, sha, mode);
      continue;
    }
    /* o nome de verdade (o FAT do SODOS tem nomes longos); so um nome que o
     * FAT nao aceita vai com _ no lugar do caractere proibido */
    if (long_ok(name)) {
      strcpy(fixed, name);
    } else {
      if (!used) used = (char (*)[256])malloc(sizeof(*used) * (size_t)(n + 1));
      if (!used) {
        sprintf(err, "sem memoria");
        free(tree);
        return -1;
      }
      disk_name(name, tree, size, used, nused, fixed);
      strcpy(used[nused++], fixed);
      if (!c->quiet) printf("  nome que o FAT nao aceita: %s gravado como %s\n", lng, fixed);
    }
    if (dosrel[0]) snprintf(dos, sizeof(dos), "%s\\%s", dosrel, fixed);
    else snprintf(dos, sizeof(dos), "%s", fixed);
    if (strcmp(mode, "40000") == 0) {
      if (checkout_tree(c, sha, dos, lng) != 0) {
        free(tree);
        free(used);
        return -1;
      }
    } else {
      put_file(c, dos, lng, sha, mode);
    }
  }
  free(tree);
  free(used);
  return 0;
}

/* ---------- estado ---------- */

/* commit = HEAD local; base = o ultimo commit conhecido do servidor */
static int state_read(const char* root, struct remote* r, unsigned char* commit, unsigned char* base) {
  char path[PATHMAX];
  int len, have_base = 0;
  char* t;
  char* p;
  sprintf(path, "%s\\%s\\STATE.TXT", root, GITDIR);
  memset(r, 0, sizeof(*r));
  memset(commit, 0, 20);
  t = load_file(path, &len);
  if (!t) return -1;
  for (p = t; *p;) {
    char* nl = strchr(p, '\n');
    if (nl) *nl = 0;
    if (nl && nl > p && nl[-1] == '\r') nl[-1] = 0;
    if (strncmp(p, "url=", 4) == 0) strncpy(r->url, p + 4, sizeof(r->url) - 1);
    else if (strncmp(p, "ref=", 4) == 0) strncpy(r->ref, p + 4, sizeof(r->ref) - 1);
    else if (strncmp(p, "commit=", 7) == 0) hex_sha(p + 7, commit);
    else if (strncmp(p, "remote=", 7) == 0 && base) have_base = hex_sha(p + 7, base) == 0;
    if (!nl) break;
    p = nl + 1;
  }
  free(t);
  if (base && !have_base) memcpy(base, commit, 20);
  return r->url[0] ? 0 : -1;
}

static int state_write(const char* root, const struct remote* r, const unsigned char* commit,
                       const unsigned char* base) {
  char path[PATHMAX], text[700], hex[41], bhex[41];
  sha_hex(commit, hex);
  sha_hex(base, bhex);
  sprintf(text, "url=%s\nref=%s\ncommit=%s\nremote=%s\n", r->url, r->ref, hex, bhex);
  sprintf(path, "%s\\%s\\STATE.TXT", root, GITDIR);
  st_dirty = 1;
  return k_fsave(path, text, (int)strlen(text));
}

/* .git com as pastas de um repositorio padrao, mais .git\sodos */
static void dotgit_dirs(const char* root) {
  static const char* sub[] = {"", "\\sodos", "\\objects", "\\objects\\pack", "\\objects\\info", "\\refs",
                              "\\refs\\heads", "\\refs\\tags", "\\refs\\remotes", "\\refs\\remotes\\origin",
                              "\\logs", "\\info"};
  char d[PATHMAX + 40];
  int i;
  for (i = 0; i < (int)(sizeof(sub) / sizeof(sub[0])); i++) {
    sprintf(d, "%s\\.git%s", root, sub[i]);
    if (!is_dir(d)) k_mkdir(d);
  }
}

static int obj_write(const char* root, int type, const void* data, int len, unsigned char* sha);

/* _GIT (formato antigo do git do SODOS) -> .git. 0 ok. */
static int migrate_old(const char* root) {
  static const char* keep[] = {"STATE.TXT", "NAMES.TXT", "INDEX.TXT", "COMMIT.TXT", "CONFIG.TXT"};
  char from[PATHMAX + 40], to[PATHMAX + 40];
  int i, n, len;
  char* t;
  struct k_dirent* ents;
  printf("git: passando %s\\%s para %s\\.git (formato padrao)\n", root, OLDDIR, root);
  dotgit_dirs(root);
  for (i = 0; i < 5; i++) {
    sprintf(from, "%s\\%s\\%s", root, OLDDIR, keep[i]);
    t = load_file(from, &len);
    if (!t) continue;
    git_file(root, keep[i], to);
    if (k_fsave(to, t, len) != 0) {
      free(t);
      sprintf(err, "nao gravou %s", to);
      return -1;
    }
    free(t);
  }
  /* o pack do clone vira .git\objects\pack\pack-<sha>.pack + .idx */
  sprintf(from, "%s\\%s\\HEAD.PCK", root, OLDDIR);
  t = load_file(from, &len);
  if (t && len > 32 && pack_load((unsigned char*)t, len, err) >= 0) {
    char rel[120], phex[41];
    sha_hex((unsigned char*)t + len - 20, phex);
    sprintf(rel, ".git\\objects\\pack\\pack-%s.pack", phex);
    sprintf(to, "%s\\%s", root, rel);
    if (k_fsave(to, t, len) == 0) {
      strcpy(to + strlen(to) - 5, ".idx");
      pack_write_idx(to);
      strcat(rel, "\n");
      git_file(root, "PACK.TXT", to);
      k_fsave(to, rel, (int)strlen(rel));
      k_fdelete(from);
    }
    pack_free();
  }
  free(t);
  /* objetos locais: _GIT\OBJ\*.OBJ ("tipo tam\0dados") viram objetos soltos */
  ents = (struct k_dirent*)malloc(sizeof(struct k_dirent) * 1024);
  sprintf(from, "%s\\%s\\OBJ", root, OLDDIR);
  n = ents ? k_dir(from, ents, 1024) : -1;
  for (i = 0; i < n; i++) {
    char* z;
    unsigned char sha[20];
    int type;
    if (ents[i].dir) continue;
    sprintf(to, "%s\\%s", from, ents[i].name);
    t = load_file(to, &len);
    if (!t) continue;
    z = (char*)memchr(t, 0, (size_t)len);
    type = strncmp(t, "commit ", 7) == 0 ? OBJ_COMMIT : strncmp(t, "tree ", 5) == 0 ? OBJ_TREE : OBJ_BLOB;
    if (z && obj_write(root, type, z + 1, len - (int)(z + 1 - t), sha) == 0) k_fdelete(to);
    free(t);
  }
  free(ents);
  k_rmdir(from);
  for (i = 0; i < 5; i++) {
    sprintf(from, "%s\\%s\\%s", root, OLDDIR, keep[i]);
    k_fdelete(from);
  }
  sprintf(from, "%s\\%s", root, OLDDIR);
  if (k_rmdir(from) != 0) printf("git: %s ficou (tem outros arquivos); pode apagar\n", from);
  /* o .git de fachada da versao anterior */
  sprintf(from, "%s\\.git\\SODOS.TXT", root);
  k_fdelete(from);
  return 0;
}

/* Acha a pasta do repositorio: arg, ou a atual e as de cima. */
static int find_root(const char* arg, char* root) {
  char path[PATHMAX];
  if (arg) {
    if (arg[0] && arg[1] == ':') strcpy(root, arg);
    else {
      k_pwd(path, sizeof(path));
      path_join(root, path, arg);
    }
  } else {
    k_pwd(root, PATHMAX);
  }
  for (;;) {
    int n = (int)strlen(root);
    while (n > 3 && root[n - 1] == '\\') root[--n] = 0;
    sprintf(path, "%s%s%s\\STATE.TXT", root, n > 3 ? "\\" : "", OLDDIR);
    if (k_fsize(path) >= 0) {
      char r2[PATHMAX];
      strcpy(r2, root);
      if (n == 3) r2[2] = 0;
      if (migrate_old(r2) != 0) printf("git: migracao falhou: %s\n", err);
    }
    sprintf(path, "%s%s%s\\STATE.TXT", root, n > 3 ? "\\" : "", GITDIR);
    if (k_fsize(path) >= 0) {
      static char adopted[PATHMAX];
      if (n == 3) root[2] = 0;   /* "D:" para juntar com "\\_GIT" */
      /* outro git mexeu no .git? uma vez por processo e por pasta */
      if (strcmp(adopted, root) != 0) {
        strcpy(adopted, root);
        adopt_dotgit(root);
      }
      return 0;
    }
    if (arg || n <= 3) break;
    while (n > 3 && root[n - 1] != '\\') n--;
    root[n > 3 ? n - 1 : 3] = 0;
  }
  sprintf(err, "nao e uma pasta de git do SODOS (sem .git\\sodos\\STATE.TXT)");
  return -1;
}

static void git_file(const char* root, const char* name, char* out) {
  sprintf(out, "%s\\%s\\%s", root, GITDIR, name);
}

/* ---------- objetos locais (_GIT\OBJ) ---------- */

static const char* type_str(int t) {
  if (t == OBJ_COMMIT) return "commit";
  if (t == OBJ_TREE) return "tree";
  return "blob";
}

static void obj_path(const char* root, const unsigned char* sha, char* out) {
  char hex[41];
  sha_hex(sha, hex);
  sprintf(out, "%s\\.git\\objects\\%.2s\\%s", root, hex, hex + 2);
}

/* "tipo tamanho\0dados" de um objeto solto (zlib): malloc, *n = tamanho */
static unsigned char* loose_inflate(const unsigned char* z, int zn, int* n) {
  int cap = zn * 4 + 4096, used;
  while (cap <= 256 << 20) {
    unsigned char* b = (unsigned char*)malloc((size_t)cap + 1);
    int r;
    if (!b) return 0;
    r = zinflate(z, zn, b, cap, &used);
    if (r >= 0 && r < cap) {
      b[r] = 0;
      *n = r;
      return b;
    }
    free(b);
    cap *= 4;
  }
  return 0;
}

static int obj_exists(const char* root, const unsigned char* sha) {
  char path[PATHMAX];
  obj_path(root, sha, path);
  return k_fsize(path) >= 0;
}

/* Conteudo (malloc) do objeto sha guardado aqui, ou 0. */
static unsigned char* obj_read(const char* root, const unsigned char* sha, int* type, int* len) {
  char path[PATHMAX];
  unsigned char now[20];
  sha1_ctx c;
  char* b;
  char* z;
  int n;
  unsigned char* out;
  obj_path(root, sha, path);
  {
    int zn;
    char* zb = load_file(path, &zn);
    if (!zb) return 0;
    b = (char*)loose_inflate((unsigned char*)zb, zn, &n);
    free(zb);
    if (!b) return 0;
  }
  sha1_init(&c);
  sha1_update(&c, b, n);
  sha1_final(&c, now);
  z = (char*)memchr(b, 0, (size_t)n);
  if (memcmp(now, sha, 20) != 0 || !z) {
    free(b);
    return 0;
  }
  if (strncmp(b, "commit ", 7) == 0) *type = OBJ_COMMIT;
  else if (strncmp(b, "tree ", 5) == 0) *type = OBJ_TREE;
  else *type = OBJ_BLOB;
  *len = n - (int)(z + 1 - b);
  out = (unsigned char*)malloc((size_t)*len + 1);
  memcpy(out, z + 1, (size_t)*len);
  out[*len] = 0;
  free(b);
  return out;
}

/* objetos novos que o obj_write gravou (wr_on): o pull --rebase cancelado apaga */
static int wr_on, wr_n, wr_cap;
static unsigned char (*wr_sha)[20];

static int obj_write(const char* root, int type, const void* data, int len, unsigned char* sha) {
  char path[PATHMAX], hdr[32];
  char* b;
  int hn, r, t2, l2;
  unsigned char* old;
  git_hash(type_str(type), (const unsigned char*)data, len, sha);
  obj_path(root, sha, path);
  if (k_fsize(path) >= 0) {
    old = obj_read(root, sha, &t2, &l2);
    if (old) {
      free(old);
      return 0;
    }
  }
  {
    /* .git\objects\xx (as pastas de cima ja existem: dotgit_dirs) */
    char d[PATHMAX];
    int k = (int)(strrchr(path, '\\') - path);
    memcpy(d, path, (size_t)k);
    d[k] = 0;
    k_mkdir(d);
  }
  hn = sprintf(hdr, "%s %d", type_str(type), len) + 1;
  b = (char*)malloc((size_t)(hn + len) + 1);
  if (!b) {
    sprintf(err, "sem memoria");
    return -1;
  }
  memcpy(b, hdr, (size_t)hn);
  memcpy(b + hn, data, (size_t)len);
  {
    int zl = hn + len;
    unsigned char* z = (unsigned char*)malloc((size_t)(zl + 16 + 5 * (zl / 65535 + 1)));
    if (!z) {
      free(b);
      sprintf(err, "sem memoria");
      return -1;
    }
    zl = zstore((unsigned char*)b, hn + len, z);
    r = k_fsave(path, (const char*)z, zl);
    free(z);
  }
  free(b);
  if (r != 0) {
    sprintf(err, "nao gravou %s", path);
    return -1;
  }
  if (wr_on) {
    if (wr_n == wr_cap) {
      wr_cap = wr_cap ? wr_cap * 2 : 64;
      wr_sha = (unsigned char (*)[20])realloc(wr_sha, 20 * (size_t)wr_cap);
    }
    memcpy(wr_sha[wr_n++], sha, 20);
  }
  return 0;
}

/* pack do clone/pull, carregado so quando precisa (diff) */
static int quiet_pack; /* saida de maquina (--numstat, -z): sem "lendo ..." */
static char* headpack;
static int headpack_state;

static unsigned char* headpack_get(const char* root, const unsigned char* sha, int* type, int* len) {
  unsigned char* d;
  unsigned char* out;
  if (headpack_state == 0) {
    char path[PATHMAX];
    int n;
    headpack_state = -1;
    /* o pack do clone/pull: o caminho em .git\sodos\PACK.TXT (o pack fica
     * em .git\objects\pack); repositorio antigo: HEAD.PCK */
    {
      char* t;
      git_file(root, "PACK.TXT", path);
      t = load_file(path, &n);
      if (t) {
        char* nl = strchr(t, '\n');
        if (nl) *nl = 0;
        sprintf(path, "%s\\%s", root, t);
        free(t);
      } else {
        git_file(root, "HEAD.PCK", path);
      }
    }
    headpack = load_file(path, &n);
    if (headpack) {
      if (!quiet_pack) printf("lendo %s\n", path);
      if (pack_load((unsigned char*)headpack, n, err) >= 0) headpack_state = 1;
      else printf("aviso: %s\n", err);
    }
  }
  if (headpack_state != 1) return 0;
  d = pack_get(sha, type, len);
  if (!d) return 0;
  out = (unsigned char*)malloc((size_t)*len + 1);
  memcpy(out, d, (size_t)*len);
  out[*len] = 0;
  return out;
}

/* conteudo de um blob: _GIT\OBJ, ou o pack do clone/pull */
static unsigned char* blob_get(const char* root, const unsigned char* sha, int* len) {
  int type;
  unsigned char* b = obj_read(root, sha, &type, len);
  if (b) return b;
  return headpack_get(root, sha, &type, len);
}

/* Commit sha: de _GIT\OBJ ou o COMMIT.TXT (o do clone/pull). malloc, ou 0. */
static char* commit_get(const char* root, const unsigned char* sha, int* len) {
  char path[PATHMAX];
  unsigned char now[20];
  int type;
  char* t = (char*)obj_read(root, sha, &type, len);
  if (t) {
    if (type == OBJ_COMMIT) return t;
    free(t);
    return 0;
  }
  git_file(root, "COMMIT.TXT", path);
  t = load_file(path, len);
  if (!t) return 0;
  git_hash("commit", (unsigned char*)t, *len, now);
  if (memcmp(now, sha, 20) == 0) return t;
  free(t);
  return 0;
}

/* "tree X" e "parent Y" de um commit */
static int commit_parse(const char* t, unsigned char* tree, unsigned char* parent) {
  const char* p = t;
  int have_tree = 0;
  if (parent) memset(parent, 0, 20);
  while (*p && *p != '\n') {
    if (strncmp(p, "tree ", 5) == 0) have_tree = hex_sha(p + 5, tree) == 0;
    else if (parent && strncmp(p, "parent ", 7) == 0) hex_sha(p + 7, parent);
    p = strchr(p, '\n');
    if (!p) break;
    p++;
  }
  return have_tree ? 0 : -1;
}

/* todos os "parent X" de um commit (merge tem mais de um). Devolve quantos. */
static int commit_parents(const char* t, unsigned char (*par)[20], int max) {
  const char* p = t;
  int n = 0;
  while (*p && *p != '\n') {
    if (strncmp(p, "parent ", 7) == 0 && n < max && hex_sha(p + 7, par[n]) == 0) n++;
    p = strchr(p, '\n');
    if (!p) break;
    p++;
  }
  return n;
}

/* ---------- outro git no mesmo .git ----------
 * O .git e o formato padrao: o git da VM do WSL (e o Git for Windows) usam
 * a mesma pasta. O que eles fazem (commit, rebase, merge, fetch) fica nos
 * refs e em objetos soltos ou packs novos em .git\objects\pack. */

/* sha de um ref do .git padrao: .git\refs\... ou .git\packed-refs. 0 ok. */
static int ref_read(const char* root, const char* ref, unsigned char* sha) {
  char path[PATHMAX + 200];
  int len, i, found = -1;
  char* t;
  char* p;
  sprintf(path, "%s\\.git\\%s", root, ref);
  for (i = 0; path[i]; i++)
    if (path[i] == '/') path[i] = '\\';
  t = load_file(path, &len);
  if (t) {
    int ok = len >= 40 && hex_sha(t, sha) == 0;
    free(t);
    return ok ? 0 : -1;
  }
  sprintf(path, "%s\\.git\\packed-refs", root);
  t = load_file(path, &len);
  if (!t) return -1;
  for (p = t; *p;) {
    char* nl = strchr(p, '\n');
    int rl = (int)strlen(ref);
    if (nl) *nl = 0;
    if ((int)strlen(p) >= 41 + rl && p[40] == ' ' && strncmp(p + 41, ref, (size_t)rl) == 0 &&
        (p[41 + rl] == 0 || p[41 + rl] == '\r'))
      found = hex_sha(p, sha);
    if (!nl) break;
    p = nl + 1;
  }
  free(t);
  return found == 0 ? 0 : -1;
}

static unsigned be32(const unsigned char* b) {
  return ((unsigned)b[0] << 24) | ((unsigned)b[1] << 16) | ((unsigned)b[2] << 8) | b[3];
}

/* 1 se o .idx (versao 2) tem o objeto: fanout e busca binaria, lendo so
 * os pedacos (k_fread), sem carregar o pack. */
static int idx_has(const char* path, const unsigned char* sha) {
  unsigned char h[8], f[4], s[20];
  unsigned lo = 0, hi;
  if (k_fread(path, 0, (char*)h, 8) != 8) return 0;
  if (memcmp(h, "\377tOc", 4) != 0 || be32(h + 4) != 2) return 0;
  if (sha[0] > 0) {
    if (k_fread(path, 8 + 4 * (unsigned)(sha[0] - 1), (char*)f, 4) != 4) return 0;
    lo = be32(f);
  }
  if (k_fread(path, 8 + 4 * (unsigned)sha[0], (char*)f, 4) != 4) return 0;
  hi = be32(f);
  while (lo < hi) {
    unsigned m = (lo + hi) / 2;
    int c;
    if (k_fread(path, 8 + 1024 + 20 * m, (char*)s, 20) != 20) return 0;
    c = memcmp(s, sha, 20);
    if (!c) return 1;
    if (c < 0) lo = m + 1;
    else hi = m;
  }
  return 0;
}

/* 1 se o objeto esta aqui: solto, ou em qualquer pack de .git\objects\pack
 * (o do clone/pull ou os do outro git). Nao le o conteudo. */
static int obj_anywhere(const char* root, const unsigned char* sha) {
  char d[PATHMAX + 40], p[PATHMAX + 300];
  struct k_direntl* e;
  int i, n;
  if (obj_exists(root, sha)) return 1;
  sprintf(d, "%s\\.git\\objects\\pack", root);
  e = (struct k_direntl*)malloc(sizeof(*e) * 64);
  if (!e) return 0;
  n = k_dir_long(d, e, 64);
  for (i = 0; i < n; i++) {
    int l = (int)strlen(e[i].name);
    if (e[i].dir || l < 5 || ci_cmp(e[i].name + l - 4, ".idx") != 0) continue;
    sprintf(p, "%s\\%s", d, e[i].name);
    if (idx_has(p, sha)) {
      free(e);
      return 1;
    }
  }
  free(e);
  return 0;
}

/* .git\shallow: o commit que o clone/pull baixou raso (deepen 1) entra so
 * se algum pai dele nao esta aqui. As linhas que ja estao ficam: antes o
 * arquivo era regravado com uma linha so a cada comando, e se o outro git
 * tinha o historico mais fundo, ele passava a achar que o historico acabava
 * ali (merge sem base comum). */
static void shallow_note(const char* root) {
  char f[PATHMAX + 40], chex[41];
  unsigned char cs[20], par[8][20];
  int cl, sl = 0, np, i, need = 0;
  char* ct;
  char* old;
  git_file(root, "COMMIT.TXT", f);
  ct = load_file(f, &cl);
  if (!ct) return;
  git_hash("commit", (unsigned char*)ct, cl, cs);
  np = commit_parents(ct, par, 8);
  free(ct);
  for (i = 0; i < np && !need; i++)
    if (!obj_anywhere(root, par[i])) need = 1;
  if (!need) return;
  sha_hex(cs, chex);
  sprintf(f, "%s\\.git\\shallow", root);
  old = load_file(f, &sl);
  if (!old || !strstr(old, chex)) {
    struct sbuf s;
    memset(&s, 0, sizeof(s));
    sb_add(&s, "", 0);
    if (old && sl) {
      sb_add(&s, old, sl);
      if (old[sl - 1] != '\n') sb_add(&s, "\n", 1);
    }
    sb_add(&s, chex, 40);
    sb_add(&s, "\n", 1);
    k_fsave(f, s.p, s.len);
    free(s.p);
  }
  free(old);
}

/* Outro git mexeu no .git (commit, rebase, merge, fetch)? O ramo dele vale.
 * Se .git\refs\heads\<ramo> nao e o commit= do STATE.TXT, o GIT.EXE passa a
 * usar o commit de la: refaz o NAMES.TXT da arvore (a pasta nao muda) e o
 * INDEX.TXT, que era do commit antigo, sai. O refs\remotes\origin\<ramo>
 * vira o remote=. Antes era o contrario: o STATE.TXT regravava os refs e o
 * trabalho do outro git sumia. Sem conseguir ler a arvore (objeto num pack
 * que o GIT.EXE nao le), so avisa e nao muda nada. */
static void adopt_dotgit(const char* root) {
  struct remote r;
  unsigned char cur[20], base[20], h[20], o[20], tree[20];
  char ref[200], hex[41];
  int changed = 0, dirty = st_dirty, have_o = 0, qp = quiet_pack;
  if (state_read(root, &r, cur, base) != 0) return;
  if (strncmp(r.ref, "refs/heads/", 11) != 0) return;
  sprintf(ref, "refs/remotes/origin/%s", r.ref + 11);
  if (ref_read(root, ref, o) == 0 && !sha_zero(o)) have_o = 1;
  if (ref_read(root, r.ref, h) == 0 && !sha_zero(h) && memcmp(h, cur, 20) != 0) {
    int len;
    char* t = commit_get(root, h, &len);
    sha_hex(h, hex);
    if (!t || commit_parse(t, tree, 0) != 0) {
      free(t);
      if (!adopt_quiet)
        printf("git: %s foi para %.12s por outro git, e o GIT.EXE nao le esse commit\n"
               "     (esta num pack do outro git): continue com o mesmo git.\n",
               r.ref, hex);
      st_dirty = dirty;
      return;
    }
    free(t);
    {
      struct co c;
      char path[PATHMAX];
      int type, l2;
      unsigned char* x;
      memset(&c, 0, sizeof(c));
      c.root = root;
      c.keep = 1;
      c.loose = 1;
      c.quiet = 1;
      {
        /* carrega o pack do clone/pull sem o "lendo"/"indexando" na saida */
        int pq = pack_quiet;
        quiet_pack = 1;
        pack_quiet = 1;
        x = headpack_get(root, tree, &type, &l2);
        free(x);
        quiet_pack = qp;
        pack_quiet = pq;
      }
      if (checkout_tree(&c, tree, "", "") != 0) {
        free(c.names.p);
        if (!adopt_quiet)
          printf("git: %s foi para %.12s por outro git; falta objeto da arvore (%s): nada mudou\n", r.ref,
                 hex, err);
        st_dirty = dirty;
        return;
      }
      git_file(root, "NAMES.TXT", path);
      k_fsave(path, c.names.p ? c.names.p : "", c.names.len);
      free(c.names.p);
      git_file(root, "INDEX.TXT", path);
      if (k_fsize(path) >= 0) k_fdelete(path);
    }
    memcpy(cur, h, 20);
    changed = 1;
    if (!adopt_quiet) printf("git: %s mudou por outro git (commit, rebase ou merge): agora %.12s\n", r.ref, hex);
  }
  if (have_o && memcmp(o, base, 20) != 0) {
    memcpy(base, o, 20);
    changed = 1;
  }
  if (changed) state_write(root, &r, cur, base);
  /* adotar nao e motivo para regravar o .git (o index do outro git fica) */
  st_dirty = dirty;
}

/* ---------- arvores a partir de uma lista ---------- */

struct tctx {
  const char* root;
  int write;                 /* grava as arvores que nao estao em known */
  unsigned char (*known)[20];
  int nknown;
  int capknown;
  int written;
};

static int known_has(struct tctx* t, const unsigned char* sha) {
  int i;
  for (i = 0; i < t->nknown; i++)
    if (memcmp(t->known[i], sha, 20) == 0) return 1;
  return 0;
}

static void known_add(struct tctx* t, const unsigned char* sha) {
  if (t->nknown == t->capknown) {
    t->capknown = t->capknown ? t->capknown * 2 : 64;
    t->known = (unsigned char (*)[20])realloc(t->known, 20 * (size_t)t->capknown);
  }
  memcpy(t->known[t->nknown++], sha, 20);
}

/* v[lo..hi) tem o prefixo de pl letras ("src/"). A ordem por strcmp do
 * caminho inteiro e a mesma ordem das arvores do git. */
static int build_tree(struct nent** v, int lo, int hi, int pl, struct tctx* t, unsigned char* out) {
  struct sbuf s;
  int i = lo;
  memset(&s, 0, sizeof(s));
  sb_add(&s, "", 0);
  while (i < hi) {
    const char* name = v[i]->lng + pl;
    const char* sl = strchr(name, '/');
    if (sl) {
      int dl = (int)(sl - name), j = i;
      unsigned char sub[20];
      while (j < hi && strncmp(v[j]->lng + pl, name, (size_t)dl) == 0 && v[j]->lng[pl + dl] == '/') j++;
      if (build_tree(v, i, j, pl + dl + 1, t, sub) != 0) {
        free(s.p);
        return -1;
      }
      sb_add(&s, "40000 ", 6);
      sb_add(&s, name, dl);
      sb_byte(&s, 0);
      sb_add(&s, (const char*)sub, 20);
      i = j;
    } else {
      sb_add(&s, v[i]->mode, -1);
      sb_byte(&s, ' ');
      sb_add(&s, name, -1);
      sb_byte(&s, 0);
      sb_add(&s, (const char*)v[i]->sha, 20);
      i++;
    }
  }
  git_hash("tree", (unsigned char*)s.p, s.len, out);
  if (t->write) {
    if (!known_has(t, out)) {
      unsigned char chk[20];
      if (obj_write(t->root, OBJ_TREE, s.p, s.len, chk) != 0) {
        free(s.p);
        return -1;
      }
      t->written++;
      known_add(t, out);
    }
  } else {
    known_add(t, out);
  }
  free(s.p);
  return 0;
}

static int list_tree(struct list* l, struct tctx* t, unsigned char* out) {
  struct nent** v;
  int i, n = 0, r;
  list_sort(l);
  v = (struct nent**)malloc(sizeof(*v) * (size_t)(l->n + 1));
  for (i = 0; i < l->n; i++)
    if (!l->e[i].gone) v[n++] = &l->e[i];
  r = build_tree(v, 0, n, 0, t, out);
  free(v);
  return r;
}

/* ---------- configuracao ---------- */

static void cfg_path(const char* root, int global, char* out) {
  if (global || !root) {
    int d = k_exe_drive();
    sprintf(out, "%c:\\GITCONF.TXT", d ? d : 'D');
  } else {
    git_file(root, "CONFIG.TXT", out);
  }
}

static int cfg_read(const char* path, const char* key, char* out, int max) {
  int len, kl = (int)strlen(key), found = 0;
  char* t = load_file(path, &len);
  char* p;
  if (!t) return 0;
  for (p = t; *p;) {
    char* nl = strchr(p, '\n');
    if (nl) *nl = 0;
    if (nl && nl > p && nl[-1] == '\r') nl[-1] = 0;
    if (strncmp(p, key, (size_t)kl) == 0 && p[kl] == '=') {
      strncpy(out, p + kl + 1, (size_t)max - 1);
      out[max - 1] = 0;
      found = 1;
    }
    if (!nl) break;
    p = nl + 1;
  }
  free(t);
  return found;
}

/* repositorio, depois o global; o token tambem vem de SET GIT_TOKEN=... */
static int cfg_get(const char* root, const char* key, char* out, int max) {
  char path[PATHMAX];
  out[0] = 0;
  if (strcmp(key, "token") == 0 && k_getenv("GIT_TOKEN", out, max) > 0) return 1;
  if (root) {
    cfg_path(root, 0, path);
    if (cfg_read(path, key, out, max)) return 1;
  }
  cfg_path(root, 1, path);
  return cfg_read(path, key, out, max);
}

static int cfg_set(const char* path, const char* key, const char* val) {
  struct sbuf s;
  int len, kl = (int)strlen(key), r, done = 0;
  char* t = load_file(path, &len);
  char* p;
  memset(&s, 0, sizeof(s));
  sb_add(&s, "", 0);
  for (p = t; p && *p;) {
    char* nl = strchr(p, '\n');
    if (nl) *nl = 0;
    if (nl && nl > p && nl[-1] == '\r') nl[-1] = 0;
    if (strncmp(p, key, (size_t)kl) == 0 && p[kl] == '=') {
      if (val[0] && !done) {
        sb_add(&s, key, -1);
        sb_add(&s, "=", 1);
        sb_add(&s, val, -1);
        sb_add(&s, "\n", 1);
      }
      done = 1;
    } else if (*p) {
      sb_add(&s, p, -1);
      sb_add(&s, "\n", 1);
    }
    if (!nl) break;
    p = nl + 1;
  }
  if (!done && val[0]) {
    sb_add(&s, key, -1);
    sb_add(&s, "=", 1);
    sb_add(&s, val, -1);
    sb_add(&s, "\n", 1);
  }
  r = k_fsave(path, s.p, s.len);
  free(s.p);
  free(t);
  return r;
}

/* ---------- comandos ---------- */

/* Com o pack de r->head carregado (resp e a resposta que o contem): grava a
 * arvore tsha na pasta, guarda o pack em .git\objects\pack (o anterior sai),
 * NAMES.TXT e STATE.TXT (commit = local, remote = r->head).
 * rebase = 1: a arvore e de commits daqui (objetos soltos tambem valem).
 * keep = 1: so refaz NAMES.TXT do commit atual (nao mexe no INDEX nem em OBJ).
 * Libera o pack e resp. Devolve 0 ok, -1 erro (nada atualizado), 1 = estado
 * gravado mas arquivos ficaram sem gravar (err diz quantos). */
static int checkout_save(const char* root, struct remote* r, char* resp, unsigned char* pack, int plen,
                         const unsigned char* tsha, const unsigned char* local, int keep, int rebase,
                         unsigned t0) {
  int i, removed = 0;
  char path[PATHMAX], hex[41];
  struct co c;
  memset(&c, 0, sizeof(c));
  c.root = root;
  c.keep = keep;
  c.loose = rebase;
  printf(keep ? "refazendo a lista de arquivos\n" : "gravando arquivos\n");
  if (checkout_tree(&c, tsha, "", "") != 0) {
    free(resp);
    pack_free();
    return -1;
  }
  /* o pack fica em .git\objects\pack, com o .idx, como o de qualquer git
   * (e o diff le dele); o anterior (pull) sai */
  {
    char rel[120], phex[41], old[PATHMAX];
    int on;
    char* ot;
    dotgit_dirs(root);
    git_file(root, "PACK.TXT", path);
    ot = load_file(path, &on);
    if (ot) {
      char* nl = strchr(ot, '\n');
      if (nl) *nl = 0;
      sprintf(old, "%s\\%s", root, ot);
      k_fdelete(old);
      on = (int)strlen(old);
      if (on > 5) {
        strcpy(old + on - 5, ".idx");
        k_fdelete(old);
      }
      free(ot);
    }
    sha_hex(pack + plen - 20, phex);
    sprintf(rel, ".git\\objects\\pack\\pack-%s.pack", phex);
    sprintf(old, "%s\\%s", root, rel);
    if (k_fsave(old, (const char*)pack, plen) != 0) printf("aviso: nao gravou %s\n", old);
    strcpy(old + strlen(old) - 5, ".idx");
    if (pack_write_idx(old) != 0) printf("aviso: nao gravou %s\n", old);
    strcat(rel, "\n");
    k_fsave(path, rel, (int)strlen(rel));
    git_file(root, "HEAD.PCK", path);
    if (k_fsize(path) >= 0) k_fdelete(path);
  }
  pack_free();
  free(resp);
  /* o que saiu do repositorio sai da pasta, se nao foi mudado aqui */
  for (i = 0; i < olds.n && !keep; i++) {
    unsigned char now[20];
    char full[PATHMAX + 40];
    if (olds.e[i].seen || olds.e[i].dos[0] == '*') continue;
    path_join(full, root, olds.e[i].dos);
    if (file_sha(full, now) != 0) continue;
    if (memcmp(now, olds.e[i].sha, 20) != 0) {
      printf("  saiu do repositorio mas foi mudado aqui, mantido: %s\n", olds.e[i].lng);
      continue;
    }
    if (k_fdelete(full) == 0) removed++;
  }
  git_file(root, "NAMES.TXT", path);
  if (k_fsave(path, c.names.p ? c.names.p : "", c.names.len) != 0) printf("aviso: nao gravou %s\n", path);
  free(c.names.p);
  if (!keep) {
    git_file(root, "INDEX.TXT", path);
    if (k_fsize(path) >= 0) {
      printf("  o que estava staged (git add) foi descartado; rode git add de novo\n");
      k_fdelete(path);
    }
  }
  state_write(root, r, local, r->head);
  if (!keep) shallow_note(root);
  if (c.failed) sprintf(err, "%d arquivo(s) nao foram gravados (veja acima)", c.failed);
  if (keep) return c.failed ? 1 : 0;
  sha_hex(local, hex);
  printf("\r%d arquivos (%d gravados", c.files, c.written);
  if (removed) printf(", %d apagados", removed);
  if (c.kept && !rebase) printf(", %d mantidos", c.kept);
  if (c.skipped) printf(", %d pulados", c.skipped);
  if (c.failed) printf(", %d com erro", c.failed);
  printf(") em %u s\ncommit %.12s (%s)\n", (k_ms() - t0) / 1000, hex, r->ref);
  return c.failed ? 1 : 0;
}

/* Busca o commit de r e grava os arquivos em root (keep: veja checkout_save). */
static int fetch_checkout(const char* root, struct remote* r, int keep) {
  char* resp;
  unsigned char* pack;
  int plen, type, size;
  unsigned char* cm;
  unsigned char tsha[20];
  char path[PATHMAX];
  unsigned t0 = k_ms();
  dotgit_dirs(root);
  if (get_pack(r, &resp, &pack, &plen) != 0) return -1;
  printf("  pack de %d KB\n", plen / 1024);
  if (pack_load(pack, plen, err) < 0) {
    free(resp);
    return -1;
  }
  cm = pack_get(r->head, &type, &size);
  if (!cm || type != OBJ_COMMIT || size < 46 || memcmp(cm, "tree ", 5) != 0 || hex_sha((char*)cm + 5, tsha) != 0) {
    sprintf(err, "o commit nao veio no pack");
    free(resp);
    return -1;
  }
  git_file(root, "COMMIT.TXT", path);
  k_fsave(path, (char*)cm, size);
  return checkout_save(root, r, resp, pack, plen, tsha, r->head, keep, 0, t0);
}

static int cmd_clone(int argc, char** argv) {
  struct remote r;
  char root[PATHMAX], name[PATHMAX], fixed[256], cwd[PATHMAX];
  const char* p;
  const char* last;
  int n;
  if (argc < 3) {
    printf("uso: git clone https://servidor/dono/projeto [pasta]\n");
    return 1;
  }
  memset(&r, 0, sizeof(r));
  strncpy(r.url, argv[2], sizeof(r.url) - 1);
  url_clean(r.url);
  if (argc > 3) {
    strncpy(name, argv[3], sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    if (!strchr(name, ':') && !strchr(name, '\\') && !long_ok(name)) {
      disk_name(name, 0, 0, 0, 0, fixed);
      strcpy(name, fixed);
    }
  } else {
    last = r.url;
    for (p = r.url; *p; p++)
      if (*p == '/') last = p + 1;
    strncpy(name, last, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    n = (int)strlen(name);
    if (n > 4 && strcmp(name + n - 4, ".git") == 0) name[n - 4] = 0;
    /* a pasta com o nome do projeto (o FAT tem nomes longos) */
    if (!long_ok(name)) {
      disk_name(name, 0, 0, 0, 0, fixed);
      strcpy(name, fixed);
    }
  }
  if (name[0] && name[1] == ':') strcpy(root, name);
  else {
    k_pwd(cwd, sizeof(cwd));
    path_join(root, cwd, name);
  }
  if (dir_entries(root) > 0) {
    printf("%s ja existe e nao esta vazia\n", root);
    return 1;
  }
  if (!is_dir(root) && k_mkdir(root) != 0) {
    printf("nao criei a pasta %s\n", root);
    return 1;
  }
  if (!k_net_ready()) {
    printf("rede nao pronta\n");
    return 1;
  }
  n = get_refs(&r, 0);
  if (n < 0) {
    printf("erro: %s\n", err);
    return 1;
  }
  if (n == 1) {
    /* vazio: so o .git, pronto para o primeiro commit */
    unsigned char z[20];
    char path[PATHMAX + 40];
    memset(z, 0, 20);
    dotgit_dirs(root);
    git_file(root, "NAMES.TXT", path);
    k_fsave(path, "", 0);
    state_write(root, &r, z, z);
    sync_dotgit(root);
    printf("clonado %s em %s: o repositorio esta vazio (sem commits ainda).\n", r.url, root);
    printf("Crie arquivos, git add -A, git commit -m ... e git push (cria o ramo %s).\n", r.ref + 11);
    return 0;
  }
  printf("clonando %s em %s\n", r.ref, root);
  memset(&olds, 0, sizeof(olds));
  n = fetch_checkout(root, &r, 0);
  if (n < 0) {
    printf("\nerro: %s\n", err);
    return 1;
  }
  sync_dotgit(root);
  if (n > 0) {
    printf("erro: %s\n", err);
    return 1;
  }
  return 0;
}

static const char* path_arg(int argc, char** argv);
static int count_ahead(const char* root, const unsigned char* cur, const unsigned char* base);
static int pull_rebase(const char* root, struct remote* r, const unsigned char* cur, const unsigned char* base,
                       int autostash);

/* valor de config ligado: true, yes, on, 1, merges, interactive... */
static int cfg_on(const char* root, const char* key) {
  char v[32];
  if (!cfg_get(root, key, v, sizeof(v)) || !v[0]) return -1;
  return !(ci_cmp(v, "false") == 0 || ci_cmp(v, "no") == 0 || ci_cmp(v, "off") == 0 || strcmp(v, "0") == 0);
}

/* git pull [--rebase|-r|--no-rebase] [--ff-only] [--autostash] [origin [ramo]] [pasta]
 * Sem commits daqui: so baixa o commit novo. Com commits daqui e o servidor
 * mudado: refaz os commits daqui em cima do servidor (o padrao desde a
 * 1.0.17; --no-rebase ou git config pull.rebase false: nada muda). */
static int cmd_pull(int argc, char** argv) {
  struct remote r;
  char root[PATHMAX], hex[41];
  unsigned char cur[20], base[20];
  int gr, i, rebase = -1, ffonly = 0, autostash = -1;
  for (i = 2; i < argc; i++) {
    const char* a = argv[i];
    if (strcmp(a, "--rebase") == 0 || strcmp(a, "-r") == 0) rebase = 1;
    else if (strncmp(a, "--rebase=", 9) == 0) rebase = !(strcmp(a + 9, "false") == 0 || strcmp(a + 9, "no") == 0);
    else if (strcmp(a, "--no-rebase") == 0) rebase = 0;
    else if (strcmp(a, "--ff-only") == 0) ffonly = 1;
    else if (strcmp(a, "--autostash") == 0) autostash = 1;
    else if (strcmp(a, "--no-autostash") == 0) autostash = 0;
    /* -q, -v, --ff, --no-ff, --prune, --tags...: ignorados */
  }
  if (find_root(path_arg(argc, argv), root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  if (state_read(root, &r, cur, base) != 0) {
    printf("%s\\%s\\STATE.TXT invalido\n", root, GITDIR);
    return 1;
  }
  if (rebase < 0) {
    /* este git nao faz commit de merge: refazer os commits daqui por cima
     * do servidor e o jeito de juntar, entao e o padrao. pull.rebase=false
     * (ou --no-rebase) volta ao antigo: com os dois lados mudados, nada. */
    int pr = cfg_on(root, "pull.rebase");
    rebase = pr < 0 ? 1 : pr;
  }
  if (autostash < 0) {
    autostash = cfg_on(root, "rebase.autoStash");
    if (autostash < 0) autostash = cfg_on(root, "rebase.autostash");
    autostash = autostash > 0;
  }
  if (!k_net_ready()) {
    printf("rede nao pronta\n");
    return 1;
  }
  gr = get_refs(&r, 0);
  if (gr == 1) {
    printf("o servidor ainda esta vazio: nada para baixar\n");
    return 0;
  }
  if (gr != 0) {
    printf("erro: %s\n", err);
    return 1;
  }
  if (memcmp(cur, r.head, 20) == 0) {
    /* os commits daqui ja estao no servidor (push de outro jeito) */
    if (memcmp(base, cur, 20) != 0) state_write(root, &r, cur, cur);
    sha_hex(cur, hex);
    printf("Ja esta atualizado (%.12s).\n", hex);
    return 0;
  }
  if (memcmp(base, r.head, 20) == 0) {
    sha_hex(cur, hex);
    printf("Ja esta atualizado: o servidor nao mudou (%.12s). %d commit(s) daqui para o git push.\n", hex,
           count_ahead(root, cur, base));
    return 0;
  }
  if (rebase && !ffonly) return pull_rebase(root, &r, cur, base, autostash);
  if (memcmp(cur, base, 20) != 0) {
    printf("Ha commits locais que nao foram para o servidor e o servidor mudou.\n");
    if (ffonly) {
      printf("--ff-only: nada foi mudado.\n");
    } else {
      printf("Rode  git pull --rebase  para refazer os commits daqui em cima dos do servidor\n");
      printf("(ou  git config pull.rebase true  para o git pull fazer sempre assim).\n");
    }
    return 1;
  }
  list_load(root, "NAMES.TXT", &olds);
  gr = fetch_checkout(root, &r, 0);
  if (gr != 0) {
    printf(gr < 0 ? "\nerro: %s\n" : "erro: %s\n", err);
    return 1;
  }
  return 0;
}

/* NAMES.TXT antigo (sem modos) nao refaz a arvore do HEAD: baixa o commit de
 * novo so para refazer a lista. Os arquivos da pasta nao mudam. */
static int reindex(const char* root, struct remote* r, const unsigned char* cur) {
  printf("NAMES.TXT sem os modos dos arquivos: baixando o commit para refazer a lista\n");
  if (!k_net_ready()) {
    sprintf(err, "rede nao pronta");
    return -1;
  }
  memcpy(r->head, cur, 20);
  list_load(root, "NAMES.TXT", &olds);
  return fetch_checkout(root, r, 1);
}

/* ---------- caminhos dos argumentos ---------- */

/* arquivo do argumento (relativo a pasta atual) -> relativo a root, com \ */
static int rel_path(const char* root, const char* arg, char* rel) {
  char cwd[PATHMAX], full[PATHMAX * 2];
  int rl = (int)strlen(root);
  if (arg[0] && arg[1] == ':') strcpy(full, arg);
  else {
    k_pwd(cwd, sizeof(cwd));
    path_join(full, cwd, arg);
  }
  {
    /* tira "." e ".." */
    char* parts[32];
    int np = 0, i;
    char tmp[PATHMAX * 2];
    char* p;
    strcpy(tmp, full + 2);
    for (p = strtok(tmp, "\\/"); p; p = strtok(0, "\\/")) {
      if (strcmp(p, ".") == 0) continue;
      if (strcmp(p, "..") == 0) {
        if (np) np--;
        continue;
      }
      if (np < 32) parts[np++] = p;
    }
    full[2] = 0;
    for (i = 0; i < np; i++) {
      strcat(full, "\\");
      strcat(full, parts[i]);
    }
    if (!np) strcat(full, "\\");
  }
  if (!ci_start(full, root) || (full[rl] != '\\' && full[rl] != 0)) {
    sprintf(err, "%s esta fora do repositorio %s", arg, root);
    return -1;
  }
  strcpy(rel, full[rl] ? full + rl + 1 : "");
  return 0;
}

/* Nome longo de um arquivo novo: as pastas que ja existem no repositorio
 * ganham o nome longo delas; o resto fica como foi digitado (lower = minusculas). */
static void long_for(struct list* idx, const char* rel, int lower, char* out) {
  char comp[16][PATHMAX];
  int nc = 0, d, i, k;
  const char* p = rel;
  out[0] = 0;
  while (*p && nc < 16) {
    k = 0;
    while (*p && *p != '\\' && k < PATHMAX - 1) comp[nc][k++] = *p++;
    comp[nc][k] = 0;
    nc++;
    if (*p == '\\') p++;
  }
  for (d = nc - 1; d > 0; d--) {
    char pre[PATHMAX * 2];
    pre[0] = 0;
    for (i = 0; i < d; i++) {
      strcat(pre, comp[i]);
      strcat(pre, "\\");
    }
    for (i = 0; i < idx->n; i++) {
      struct nent* e = &idx->e[i];
      if (e->dos[0] != '*' && ci_start(e->dos, pre)) {
        const char* q = e->lng;
        int c = 0;
        while (*q && c < d) {
          if (*q == '/') c++;
          if (c < d) {
            int L = (int)strlen(out);
            out[L] = *q;
            out[L + 1] = 0;
          }
          q++;
        }
        break;
      }
    }
    if (i < idx->n) break;
  }
  for (i = d > 0 ? d : 0; i < nc; i++) {
    char* q;
    if (out[0]) strcat(out, "/");
    q = out + strlen(out);
    strcat(out, comp[i]);
    /* NOVO.TXT (8.3 todo maiusculo, como o FAT antigo guardava) vira
     * novo.txt; nome longo ou com minusculas fica como esta no disco */
    {
      const char* c;
      int has_low = 0;
      for (c = comp[i]; *c; c++)
        if (*c >= 'a' && *c <= 'z') has_low = 1;
      if (!has_low && is83(comp[i]))
        for (; *q; q++) *q = (char)lowc((unsigned char)*q);
    }
    (void)lower;
  }
}

/* ---------- add ---------- */

struct addst {
  const char* root;
  struct list* idx;
  struct nent** bydos;    /* rastreados, por nome curto (para a busca do -A) */
  int nbydos;
  int changed;
  int added;
  int removed;
  int quiet;
};

static int dos_cmp(const void* a, const void* b) {
  return ci_cmp((*(struct nent* const*)a)->dos, (*(struct nent* const*)b)->dos);
}

static void bydos_build(struct addst* a) {
  int i;
  a->bydos = (struct nent**)malloc(sizeof(struct nent*) * (size_t)(a->idx->n + 1));
  a->nbydos = 0;
  for (i = 0; i < a->idx->n; i++)
    if (on_disk(&a->idx->e[i])) a->bydos[a->nbydos++] = &a->idx->e[i];
  qsort(a->bydos, (size_t)a->nbydos, sizeof(struct nent*), dos_cmp);
}

static int bydos_has(struct addst* a, const char* dos) {
  int lo = 0, hi = a->nbydos - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2, c = ci_cmp(a->bydos[m]->dos, dos);
    if (!c) return 1;
    if (c < 0) lo = m + 1;
    else hi = m - 1;
  }
  return 0;
}

/* Poe o arquivo e (rastreado) no index como esta no disco. */
static int add_tracked(struct addst* a, struct nent* e) {
  char full[PATHMAX * 2];
  unsigned char sha[20];
  int n;
  char* b;
  path_join(full, a->root, e->dos);
  b = load_file(full, &n);
  if (!b) {
    e->gone = 1;
    a->removed++;
    if (!a->quiet) printf("  apagado:    %s\n", e->lng);
    return 0;
  }
  git_hash("blob", (unsigned char*)b, n, sha);
  if (memcmp(sha, e->sha, 20) != 0) {
    if (obj_write(a->root, OBJ_BLOB, b, n, sha) != 0) {
      free(b);
      return -1;
    }
    memcpy(e->sha, sha, 20);
    a->changed++;
    if (!a->quiet) printf("  modificado: %s\n", e->lng);
  }
  free(b);
  return 0;
}

static int add_new(struct addst* a, const char* rel, int lower) {
  char full[PATHMAX * 2], lng[PATHMAX * 2];
  unsigned char sha[20];
  int n, k;
  char* b;
  struct nent* e;
  path_join(full, a->root, rel);
  b = load_file(full, &n);
  if (!b) {
    sprintf(err, "nao li %s", full);
    return -1;
  }
  if (obj_write(a->root, OBJ_BLOB, b, n, sha) != 0) {
    free(b);
    return -1;
  }
  free(b);
  long_for(a->idx, rel, lower, lng);
  e = list_find(a->idx, lng);
  if (!e) {
    e = list_push(a->idx);
    e->lng = xstrdup(lng);
    e->hasmode = 1;
  }
  e->dos = xstrdup(rel);
  for (k = 0; e->dos[k]; k++) e->dos[k] = (char)up((unsigned char)e->dos[k]);
  memcpy(e->sha, sha, 20);
  a->added++;
  if (!a->quiet) printf("  novo:       %s\n", lng);
  return 0;
}

/* Arquivos que nao estao no index, em rel (recursivo). cb = 0 so conta. */
typedef int (*untracked_cb)(struct addst* a, const char* rel);

/* ---------- .gitignore ----------
 * Da raiz, de .git\info\exclude e de cada pasta (o .gitignore de uma
 * subpasta vale para o que esta dentro dela e manda mais que o de cima; o
 * do SODOS esta quase todo em src13\.gitignore, e antes so o da raiz era
 * lido: o status listava os .o e .obj e o git add -A punha tudo no commit).
 * Um padrao por linha (# comentario), com * ? [..] e **; "nome/" so pastas;
 * "/x" so na pasta do .gitignore; com / no meio vale o caminho a partir
 * dela; sem / vale o nome em qualquer pasta abaixo dela; "!x" volta a
 * incluir. Sem diferenciar maiusculas (FAT). */
#define IGN_MAX 1024
static char* ign_pat[IGN_MAX];
static char* ign_base[IGN_MAX];   /* pasta do .gitignore, com / no fim ("" = raiz) */
static int ign_n, ign_loaded;
static char ign_root[PATHMAX];
#define IGN_DIRS 256
static char* ign_dirs[IGN_DIRS];  /* pastas cujo .gitignore ja foi lido */
static int ign_ndirs;

static int globi(const char* p, const char* s) {
  for (;;) {
    if (!*p) return !*s;
    if (*p == '*') {
      /* "**\/" tambem casa com nenhuma pasta: develop/**\/x.obj pega develop/x.obj */
      if (p[1] == '*' && p[2] == '/' && globi(p + 3, s)) return 1;
      while (*p == '*') p++;
      if (!*p) return 1;
      for (; *s; s++)
        if (globi(p, s)) return 1;
      return 0;
    }
    if (!*s) return 0;
    if (*p == '?') {
      p++;
      s++;
      continue;
    }
    if (*p == '[') {
      const char* q = p + 1;
      int neg = 0, ok = 0, c = lowc((unsigned char)*s);
      if (*q == '!' || *q == '^') {
        neg = 1;
        q++;
      }
      while (*q && *q != ']') {
        if (q[1] == '-' && q[2] && q[2] != ']') {
          if (c >= lowc((unsigned char)q[0]) && c <= lowc((unsigned char)q[2])) ok = 1;
          q += 3;
        } else {
          if (c == lowc((unsigned char)*q)) ok = 1;
          q++;
        }
      }
      if (ok == neg) return 0;
      p = *q ? q + 1 : q;
      s++;
      continue;
    }
    if (lowc((unsigned char)*p) != lowc((unsigned char)*s)) return 0;
    p++;
    s++;
  }
}

/* base: a pasta do .gitignore relativa a raiz, com / no fim ("" = raiz) */
static void ign_add_file(const char* path, const char* base) {
  int len;
  char* t = load_file(path, &len);
  char* p;
  if (!t) return;
  for (p = t; *p && ign_n < IGN_MAX;) {
    char* nl = strchr(p, '\n');
    int l;
    if (nl) *nl = 0;
    l = (int)strlen(p);
    while (l && (p[l - 1] == '\r' || p[l - 1] == ' ' || p[l - 1] == '\t')) p[--l] = 0;
    if (l && p[0] != '#') {
      ign_pat[ign_n] = xstrdup(p);
      ign_base[ign_n++] = xstrdup(base);
    }
    if (!nl) break;
    p = nl + 1;
  }
  free(t);
}

static void ign_load(const char* root) {
  char path[PATHMAX + 40];
  if (ign_loaded && strcmp(ign_root, root) == 0) return;
  while (ign_n) {
    ign_n--;
    free(ign_pat[ign_n]);
    free(ign_base[ign_n]);
  }
  while (ign_ndirs) free(ign_dirs[--ign_ndirs]);
  strcpy(ign_root, root);
  ign_loaded = 1;
  path_join(path, root, ".gitignore");
  ign_add_file(path, "");
  path_join(path, root, ".git\\info\\exclude");
  ign_add_file(path, "");
}

/* o .gitignore da pasta rel (com \, relativa a raiz), uma vez so */
static void ign_load_dir(const char* root, const char* rel) {
  char path[PATHMAX * 2 + 40], base[PATHMAX * 2];
  int i;
  for (i = 0; i < ign_ndirs; i++)
    if (ci_cmp(ign_dirs[i], rel) == 0) return;
  if (ign_ndirs < IGN_DIRS) ign_dirs[ign_ndirs++] = xstrdup(rel);
  sprintf(path, "%s\\%s\\.gitignore", root, rel);
  if (k_fsize(path) < 0) return;
  for (i = 0; rel[i] && i < (int)sizeof(base) - 2; i++) base[i] = rel[i] == '\\' ? '/' : rel[i];
  base[i++] = '/';
  base[i] = 0;
  ign_add_file(path, base);
}

/* rel com \ (relativo a raiz); 1 = ignorado */
static int ignored(const char* rel, int is_dir) {
  char r[PATHMAX * 2];
  const char* base;
  int i, ign = 0;
  for (i = 0; rel[i] && i < (int)sizeof(r) - 1; i++) r[i] = rel[i] == '\\' ? '/' : rel[i];
  r[i] = 0;
  base = strrchr(r, '/');
  base = base ? base + 1 : r;
  for (i = 0; i < ign_n; i++) {
    char pt[300];
    const char* p = ign_pat[i];
    const char* sub = r;   /* o caminho a partir da pasta do .gitignore */
    int neg = 0, dir_only = 0, anchored = 0, l, m, bl = (int)strlen(ign_base[i]);
    if (bl) {
      int k;
      for (k = 0; k < bl && r[k] && lowc((unsigned char)r[k]) == lowc((unsigned char)ign_base[i][k]); k++) {
      }
      if (k < bl) continue;   /* fora da pasta desse .gitignore */
      sub = r + bl;
    }
    if (*p == '!') {
      neg = 1;
      p++;
    }
    strncpy(pt, p, sizeof(pt) - 1);
    pt[sizeof(pt) - 1] = 0;
    l = (int)strlen(pt);
    if (l && pt[l - 1] == '/') {
      dir_only = 1;
      pt[--l] = 0;
    }
    if (pt[0] == '/') {
      anchored = 1;
      memmove(pt, pt + 1, (size_t)l);
    }
    if (!pt[0] || (dir_only && !is_dir)) continue;
    if (anchored || strchr(pt, '/')) m = globi(pt, sub);
    else m = globi(pt, base);
    if (m) ign = !neg;
  }
  return ign;
}

static int scan_untracked(struct addst* a, const char* rel, untracked_cb cb) {
  char path[PATHMAX * 2];
  char* buf;
  char* p;
  int n, total = 0;
  if (rel[0]) path_join(path, a->root, rel);
  else if (!a->root[2]) sprintf(path, "%s\\", a->root);   /* "D:" -> "D:\" */
  else strcpy(path, a->root);
  /* o .gitignore desta pasta vale para o que esta dentro dela */
  ign_load(a->root);
  if (rel[0]) ign_load_dir(a->root, rel);
  /* k_dir_long: o nome longo inteiro (o k_dirlist dava o apelido 8.3 dos
   * nomes com mais de 31 letras e eles pareciam nao rastreados) */
  {
    struct k_direntl* ents = (struct k_direntl*)malloc(sizeof(struct k_direntl) * 1024);
    int ne = ents ? k_dir_long(path, ents, 1024) : -1, ei, o = 0;
    if (ne < 0) {
      free(ents);
      return 0;
    }
    buf = (char*)malloc((size_t)ne * 260 + 2);
    if (!buf) {
      free(ents);
      return 0;
    }
    for (ei = 0; ei < ne; ei++)
      o += sprintf(buf + o, "%s%s\n", ents[ei].name, ents[ei].dir ? "\\" : "");
    buf[o] = 0;
    free(ents);
  }
  (void)n;
  for (p = buf; *p;) {
    char* nl = strchr(p, '\n');
    char sub[PATHMAX * 2];
    int l, dir = 0;
    if (nl) *nl = 0;
    l = (int)strlen(p);
    if (l && p[l - 1] == '\r') p[--l] = 0;
    if (l && p[l - 1] == '\\') {
      p[--l] = 0;
      dir = 1;
    }
    if (!rel[0]) ign_load(a->root);
    if (l && strcmp(p, ".") != 0 && strcmp(p, "..") != 0 && ci_cmp(p, ".git") != 0 && !(!rel[0] && ci_cmp(p, OLDDIR) == 0)) {
      if (rel[0]) sprintf(sub, "%s\\%s", rel, p);
      else strcpy(sub, p);
      if (ignored(sub, dir)) {
        if (!nl) break;
        p = nl + 1;
        continue;
      }
      if (dir) total += scan_untracked(a, sub, cb);
      else if (!bydos_has(a, sub)) {
        total++;
        if (cb && cb(a, sub) != 0) {
          free(buf);
          return -100000;
        }
      }
    }
    if (!nl) break;
    p = nl + 1;
  }
  free(buf);
  return total;
}

static int cb_add_new(struct addst* a, const char* rel) { return add_new(a, rel, 1); }

/* git status --porcelain / -s: formato de maquina (o Claude Desktop le) */
static int g_porc;   /* 1 = porcelain v1 / -s, 2 = porcelain v2 */
static char g_sterm = '\n';

/* Cores do git status (como o git: verde = vai no commit, vermelho = fora
 * dele e nao rastreado). Vazias sem cor. */
static const char* c_add = "";
static const char* c_out = "";
static const char* c_hi = "";
static const char* c_end = "";

/* cor? --color[=always|never|auto], color.status / color.ui (always, never,
 * false, auto, true); auto = o kernel diz que a saida vai para uma tela que
 * mostra cor (k_con_color: tela do DOS ou janela do Command; nao com |, >,
 * captura da API/PONTE/WINE). Kernel antigo sem k_con_color: sem cor. */
static int want_color(const char* root, int argc, char** argv) {
  char v[32];
  int i, mode = -1; /* -1 auto, 0 nunca, 1 sempre */
  int (*cc)(void);
  if ((cfg_get(root, "color.status", v, sizeof(v)) && v[0]) || (cfg_get(root, "color.ui", v, sizeof(v)) && v[0])) {
    if (ci_cmp(v, "never") == 0 || ci_cmp(v, "false") == 0 || ci_cmp(v, "off") == 0 || strcmp(v, "0") == 0)
      mode = 0;
    else if (ci_cmp(v, "always") == 0)
      mode = 1;
  }
  for (i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--no-color") == 0 || strcmp(argv[i], "--color=never") == 0) mode = 0;
    else if (strcmp(argv[i], "--color") == 0 || strcmp(argv[i], "--color=always") == 0) mode = 1;
    else if (strcmp(argv[i], "--color=auto") == 0) mode = -1;
  }
  if (mode >= 0) return mode;
  cc = (int (*)(void))k_dl_sym(0, "k_con_color", 0);
  return cc ? cc() : 0;
}

/* caminho do disco (\) como o git mostra (/) */
static void print_rel(const char* rel) {
  for (; *rel; rel++) putchar(*rel == '\\' ? '/' : *rel);
}

static int cb_print(struct addst* a, const char* rel) {
  (void)a;
  if (g_porc == 2) printf("? ");
  else if (g_porc) printf("?? ");
  else printf("  %s", c_out);
  print_rel(rel);
  if (g_porc) putchar(g_sterm);
  else printf("%s\n", c_end);
  return 0;
}
/* uma linha do status de maquina: v1 "XY nome", v2 "1 XY N... modos shas nome" */
static void porc_line(char x, char y, const struct nent* e, const struct nent* h) {
  char hh[41], hi[41];
  if (g_porc != 2) {
    printf("%c%c %s%c", x, y, e ? e->lng : h->lng, g_sterm);
    return;
  }
  sha_hex(h ? h->sha : e->sha, hh);
  sha_hex(e ? e->sha : h->sha, hi);
  if (!h) strcpy(hh, "0000000000000000000000000000000000000000");
  printf("1 %c%c N... %s %s %s %s %s %s%c", x, y, h ? (h->mode[0] ? h->mode : "100644") : "000000",
         e ? (e->mode[0] ? e->mode : "100644") : "000000", y == 'D' ? "000000" : (e ? (e->mode[0] ? e->mode : "100644") : "000000"),
         hh, hi, e ? e->lng : h->lng, g_sterm);
}

/* -u: todos os rastreados; prefix limita a uma pasta */
static int add_update(struct addst* a, const char* prefix) {
  int i, n = a->idx->n;
  for (i = 0; i < n; i++) {
    struct nent* e = &a->idx->e[i];
    if (!on_disk(e)) continue;
    if (prefix[0] && !(ci_start(e->dos, prefix) && e->dos[strlen(prefix)] == '\\')) continue;
    if (add_tracked(a, e) != 0) return -1;
  }
  return 0;
}

static int cmd_add(int argc, char** argv) {
  char root[PATHMAX], rel[PATHMAX * 2], path[PATHMAX * 2];
  struct list idx;
  struct addst a;
  int i, all = 0, upd = 0, nfiles = 0;
  if (find_root(0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  index_load(root, &idx);
  memset(&a, 0, sizeof(a));
  a.root = root;
  a.idx = &idx;
  for (i = 2; i < argc; i++) {
    if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--update") == 0) upd = 1;
    else if (strcmp(argv[i], "-A") == 0 || strcmp(argv[i], "--all") == 0) all = 1;
    else nfiles++;
  }
  if (!upd && !all && !nfiles) {
    printf("uso: git add -u | -A | arquivo-ou-pasta...\n");
    return 1;
  }
  if (upd || all) {
    if (add_update(&a, "") != 0) goto fail;
    if (all) {
      bydos_build(&a);
      if (scan_untracked(&a, "", cb_add_new) < 0) goto fail;
    }
  }
  for (i = 2; i < argc; i++) {
    struct nent* e;
    if (argv[i][0] == '-') continue;
    if (rel_path(root, argv[i], rel) != 0) goto fail;
    path_join(path, root, rel);
    if (!rel[0] || is_dir(path)) {
      /* pasta: rastreados dela e os novos */
      if (add_update(&a, rel) != 0) goto fail;
      bydos_build(&a);
      if (scan_untracked(&a, rel, cb_add_new) < 0) goto fail;
      continue;
    }
    e = list_find_dos(&idx, rel);
    if (e) {
      if (add_tracked(&a, e) != 0) goto fail;
    } else if (k_fsize(path) >= 0) {
      if (add_new(&a, rel, 0) != 0) goto fail;
    } else {
      printf("nao existe: %s\n", argv[i]);
      return 1;
    }
  }
  if (list_save(root, "INDEX.TXT", &idx) != 0) {
    printf("nao gravou o INDEX.TXT\n");
    return 1;
  }
  if (!a.changed && !a.added && !a.removed) printf("nada mudou\n");
  return 0;
fail:
  printf("erro: %s\n", err);
  return 1;
}

/* ---------- restore e reset ---------- */

/* dos (caminho do disco relativo a root) esta em rel? rel "" = tudo; pasta =
 * o que esta dentro dela */
static int rel_has(const char* rel, int dir, const char* dos) {
  int n = (int)strlen(rel);
  if (!n) return 1;
  if (!dir) return ci_cmp(dos, rel) == 0;
  return ci_start(dos, rel) && (dos[n] == '\\' || dos[n] == 0);
}

/* git restore [--staged] [--worktree] caminho...
 *   sem opcao:  a pasta volta a versao do stage (INDEX.TXT)
 *   --staged:   o stage volta a versao do commit atual (NAMES.TXT); arquivo
 *               novo (que nao esta no commit) sai do stage e fica na pasta
 *   os dois:    stage e pasta voltam ao commit atual */
static int cmd_restore(int argc, char** argv) {
  char root[PATHMAX], rel[PATHMAX * 2], path[PATHMAX * 2], full[PATHMAX * 2];
  struct list idx, head;
  int i, k, staged = 0, work = 0, nfiles = 0, restored = 0, unstaged = 0, missing = 0;
  if (find_root(0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  for (i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--staged") == 0 || strcmp(argv[i], "-S") == 0) staged = 1;
    else if (strcmp(argv[i], "--worktree") == 0 || strcmp(argv[i], "-W") == 0) work = 1;
    else if (argv[i][0] == '-') {
      printf("opcao desconhecida: %s\n", argv[i]);
      return 1;
    } else nfiles++;
  }
  if (!nfiles) {
    printf("uso: git restore [--staged] [--worktree] arquivo-ou-pasta...  (. = tudo)\n");
    return 1;
  }
  if (!staged) work = 1;
  index_load(root, &idx);
  list_load(root, "NAMES.TXT", &head);
  for (i = 2; i < argc; i++) {
    int dir, hit = 0;
    if (argv[i][0] == '-') continue;
    if (rel_path(root, argv[i], rel) != 0) {
      printf("erro: %s\n", err);
      return 1;
    }
    path_join(path, root, rel);
    dir = !rel[0] || is_dir(path);
    if (staged) {
      /* stage = commit atual: o que esta no stage e o que esta no commit */
      for (k = 0; k < idx.n; k++) {
        struct nent* e = &idx.e[k];
        struct nent* h;
        if (e->dos[0] == '*' || !rel_has(rel, dir, e->dos)) continue;
        hit = 1;
        h = list_find(&head, e->lng);
        if (!h) {
          if (!e->gone) unstaged++;
          e->gone = 1;
          continue;
        }
        if (e->gone || memcmp(e->sha, h->sha, 20) != 0) unstaged++;
        memcpy(e->sha, h->sha, 20);
        memcpy(e->mode, h->mode, sizeof(e->mode));
        e->hasmode = h->hasmode;
        e->gone = 0;
      }
      for (k = 0; k < head.n; k++) {
        struct nent* h = &head.e[k];
        struct nent* e;
        if (h->dos[0] == '*' || !rel_has(rel, dir, h->dos)) continue;
        hit = 1;
        if (list_find(&idx, h->lng)) continue;
        e = list_push(&idx);
        if (!e) {
          printf("sem memoria\n");
          return 1;
        }
        *e = *h;
        e->seen = 0;
        e->gone = 0;
        unstaged++;
      }
    }
    if (work) {
      for (k = 0; k < idx.n; k++) {
        struct nent* e = &idx.e[k];
        unsigned char* data;
        int len;
        if (!on_disk(e) || !rel_has(rel, dir, e->dos)) continue;
        hit = 1;
        data = blob_get(root, e->sha, &len);
        if (!data) {
          printf("sem o conteudo de %s\n", e->lng);
          missing++;
          continue;
        }
        path_join(full, root, e->dos);
        make_parents(root, full);
        if (k_fsave(full, (const char*)data, len) != 0) {
          printf("nao gravou %s\n", full);
          missing++;
        } else {
          restored++;
        }
        free(data);
      }
    }
    if (!hit) printf("nao esta no git: %s\n", argv[i]);
  }
  if (staged && list_save(root, "INDEX.TXT", &idx) != 0) {
    printf("nao gravou o INDEX.TXT\n");
    return 1;
  }
  if (staged) printf("%d tirado(s) do stage\n", unstaged);
  if (work) printf("%d arquivo(s) restaurado(s)\n", restored);
  return missing ? 1 : 0;
}

/* git reset origin: o commit atual volta a ser o do servidor (o do ultimo
 * pull ou push). Os commits locais saem do ramo; os arquivos da pasta nao
 * mudam (o que difere aparece no git status) e o stage fica vazio. Destrava
 * o pull quando ha commits locais e o servidor mudou. */
static int cmd_reset(int argc, char** argv) {
  struct remote r;
  char root[PATHMAX], hex[41], bhex[41], path[PATHMAX];
  unsigned char cur[20], base[20];
  int gr;
  if (argc != 3 || (strcmp(argv[2], "origin") != 0 && strncmp(argv[2], "origin/", 7) != 0)) {
    printf("uso: git reset origin   (volta ao commit do servidor; a pasta nao muda)\n");
    return 1;
  }
  if (find_root(0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  if (state_read(root, &r, cur, base) != 0) {
    printf("%s\\%s\\STATE.TXT invalido\n", root, GITDIR);
    return 1;
  }
  sha_hex(cur, hex);
  sha_hex(base, bhex);
  if (memcmp(cur, base, 20) == 0) {
    printf("Ja esta no commit do servidor (%.12s): nada a desfazer.\n", bhex);
    return 0;
  }
  if (!k_net_ready()) {
    printf("rede nao pronta (o reset baixa o commit do servidor para refazer a lista)\n");
    return 1;
  }
  printf("commit local %.12s -> commit do servidor %.12s\n", hex, bhex);
  memcpy(r.head, base, 20);
  list_load(root, "NAMES.TXT", &olds);
  gr = fetch_checkout(root, &r, 1); /* keep: so refaz a lista, nao grava por cima */
  if (gr != 0) {
    printf(gr < 0 ? "\nerro: %s\n" : "erro: %s\n", err);
    return 1;
  }
  sprintf(path, "%s\\%s\\INDEX.TXT", root, GITDIR);
  k_fdelete(path);
  printf("Pronto. O commit %.12s continua guardado no .git.\n", hex);
  printf("Os arquivos da pasta nao mudaram: git status mostra o que difere do servidor.\n");
  return 0;
}

/* ---------- status ---------- */

static int count_ahead(const char* root, const unsigned char* cur, const unsigned char* base) {
  unsigned char c[20], tree[20], parent[20];
  int n = 0;
  memcpy(c, cur, 20);
  while (memcmp(c, base, 20) != 0 && n < 10000) {
    int len, type;
    char* t = (char*)obj_read(root, c, &type, &len);
    if (!t) break;
    n++;
    if (commit_parse(t, tree, parent) != 0 || sha_zero(parent)) {
      free(t);
      break;
    }
    free(t);
    memcpy(c, parent, 20);
  }
  return n;
}

static const char* branch_of(const struct remote* r) {
  return strncmp(r->ref, "refs/heads/", 11) == 0 ? r->ref + 11 : (r->ref[0] ? r->ref : "main");
}

/* git status [--porcelain[=v1]|-s|--short] [-b|--branch] [-u..] [--] [pasta] */
static int cmd_status(int argc, char** argv) {
  char root[PATHMAX], full[PATHMAX + 40], hex[41];
  struct remote r;
  struct list head, idx;
  struct addst a;
  unsigned char cur[20], base[20], now[20];
  int i, staged = 0, unstaged = 0, ahead, nun, br = 0;
  const char* where = 0;
  g_porc = 0;
  g_sterm = '\n';
  for (i = 2; i < argc; i++) {
    const char* o = argv[i];
    if (strcmp(o, "--porcelain=v2") == 0 || strcmp(o, "--porcelain=2") == 0) g_porc = 2;
    else if (strncmp(o, "--porcelain", 11) == 0 || strcmp(o, "-s") == 0 || strcmp(o, "--short") == 0) g_porc = 1;
    else if (strcmp(o, "-z") == 0) g_sterm = 0;
    else if (strcmp(o, "-b") == 0 || strcmp(o, "--branch") == 0) br = 1;
    else if (strcmp(o, "-sb") == 0 || strcmp(o, "-bs") == 0) g_porc = br = 1;
    else if (o[0] == '-') continue; /* -uall, -z, --ignore-submodules... */
    else if (!where) where = o;
  }
  if (find_root(where, root) != 0) {
    printf("fatal: %s\n", err);
    return 128;
  }
  if (!g_porc && want_color(root, argc, argv)) {
    /* ESC [ ... m com o 27 explicito (o cc do SODOS e proprio) */
    static char g[] = {27, '[', '3', '2', 'm', 0};
    static char rd[] = {27, '[', '3', '1', 'm', 0};
    static char hi[] = {27, '[', '1', 'm', 0};
    static char e0[] = {27, '[', '0', 'm', 0};
    c_add = g;
    c_out = rd;
    c_hi = hi;
    c_end = e0;
  }
  state_read(root, &r, cur, base);
  sha_hex(cur, hex);
  ahead = count_ahead(root, cur, base);
  if (g_porc == 2) {
    if (br) {
      printf("# branch.oid %s%c", hex, g_sterm);
      printf("# branch.head %s%c", branch_of(&r), g_sterm);
      printf("# branch.upstream origin/%s%c", branch_of(&r), g_sterm);
      printf("# branch.ab +%d -0%c", ahead, g_sterm);
    }
  } else if (g_porc) {
    if (br) {
      if (ahead) printf("## %s...origin/%s [ahead %d]%c", branch_of(&r), branch_of(&r), ahead, g_sterm);
      else printf("## %s...origin/%s%c", branch_of(&r), branch_of(&r), g_sterm);
    }
  } else if (sha_zero(cur)) {
    printf("No ramo %s, ainda sem commits\n", branch_of(&r));
  } else {
    printf("No ramo %s%s%s, commit %.12s\n", c_hi, r.ref, c_end, hex);
    if (ahead) printf("%d commit(s) local(is) para mandar (git push)\n", ahead);
  }
  list_load(root, "NAMES.TXT", &head);
  index_load(root, &idx);
  /* index x HEAD */
  for (i = 0; i < idx.n; i++) {
    struct nent* e = &idx.e[i];
    struct nent* h;
    if (e->gone) continue;
    h = list_find(&head, e->lng);
    if (!h || memcmp(h->sha, e->sha, 20) != 0 || strcmp(h->mode, e->mode) != 0) {
      if (g_porc) {
        staged++;
        porc_line(h ? 'M' : 'A', g_porc == 2 ? '.' : ' ', e, h);
        continue;
      }
      if (!staged++) printf("Para o commit (staged):\n");
      printf("  %s%s %s%s\n", c_add, h ? "modificado:" : "novo:      ", e->lng, c_end);
    }
  }
  for (i = 0; i < head.n; i++)
    if (!list_find(&idx, head.e[i].lng)) {
      if (g_porc) {
        staged++;
        porc_line('D', g_porc == 2 ? '.' : ' ', 0, &head.e[i]);
        continue;
      }
      if (!staged++) printf("Para o commit (staged):\n");
      printf("  %sapagado:    %s%s\n", c_add, head.e[i].lng, c_end);
    }
  /* disco x index */
  for (i = 0; i < idx.n; i++) {
    struct nent* e = &idx.e[i];
    const char* what = 0;
    if (!on_disk(e)) continue;
    path_join(full, root, e->dos);
    if (file_sha(full, now) != 0) what = "apagado:   ";
    else if (memcmp(now, e->sha, 20) != 0) what = "modificado:";
    if (what) {
      if (g_porc) {
        unstaged++;
        porc_line(g_porc == 2 ? '.' : ' ', what[0] == 'a' ? 'D' : 'M', e, list_find(&head, e->lng));
        continue;
      }
      if (!unstaged++) printf("Fora do commit (git add -u para incluir):\n");
      printf("  %s%s %s%s\n", c_out, what, e->lng, c_end);
    }
  }
  memset(&a, 0, sizeof(a));
  a.root = root;
  a.idx = &idx;
  bydos_build(&a);
  nun = scan_untracked(&a, "", 0);
  if (nun > 0) {
    if (!g_porc) printf("Nao rastreados (git add arquivo, ou git add -A):\n");
    scan_untracked(&a, "", cb_print);
  }
  if (!g_porc && !staged && !unstaged && nun <= 0) printf("nada mudou (%d arquivos)\n", idx.n);
  return 0;
}

/* ---------- diff ---------- */

struct line {
  const char* p;
  int n;
};

struct op {
  int t;    /* 0 igual, -1 saiu, 1 entrou */
  int a;
  int b;
};

static int split_lines(const char* s, int len, struct line** out) {
  int n = 0, i, k = 0;
  struct line* v;
  for (i = 0; i < len; i++)
    if (s[i] == '\n') n++;
  if (len && s[len - 1] != '\n') n++;
  v = (struct line*)malloc(sizeof(struct line) * (size_t)(n + 1));
  for (i = 0; i < len;) {
    int j = i;
    while (j < len && s[j] != '\n') j++;
    v[k].p = s + i;
    v[k].n = j - i;
    k++;
    i = j + 1;
  }
  *out = v;
  return k;
}

static int line_exact; /* 1 = \r no fim conta (o merge do pull --rebase) */

static int line_eq(const struct line* a, const struct line* b) {
  int x = a->n, y = b->n;
  if (!line_exact && x && a->p[x - 1] == '\r') x--;
  if (!line_exact && y && b->p[y - 1] == '\r') y--;
  return x == y && memcmp(a->p, b->p, (size_t)x) == 0;
}

#define DIFF_DMAX 2000

/* Myers O(ND). Devolve o numero de ops; ops em *out (malloc). */
static int diff_ops(struct line* A, int N, struct line* B, int M, struct op** out) {
  int pre = 0, suf = 0, n, m, max, d, k, x, y, D = -1, no = 0, i;
  int* V;
  int** trace;
  struct op* ops = (struct op*)malloc(sizeof(struct op) * (size_t)(N + M + 1));
  struct line* a;
  struct line* b;
  while (pre < N && pre < M && line_eq(&A[pre], &B[pre])) pre++;
  while (suf < N - pre && suf < M - pre && line_eq(&A[N - 1 - suf], &B[M - 1 - suf])) suf++;
  a = A + pre;
  b = B + pre;
  n = N - pre - suf;
  m = M - pre - suf;
  max = n + m;
  if (max > DIFF_DMAX) max = DIFF_DMAX;
  V = (int*)calloc((size_t)(2 * max + 3), sizeof(int));
  trace = (int**)calloc((size_t)(max + 2), sizeof(int*));
  for (d = 0; d <= max && D < 0; d++) {
    trace[d] = (int*)malloc(sizeof(int) * (size_t)(2 * max + 3));
    memcpy(trace[d], V, sizeof(int) * (size_t)(2 * max + 3));
    for (k = -d; k <= d; k += 2) {
      if (k == -d || (k != d && V[max + 1 + k - 1] < V[max + 1 + k + 1])) x = V[max + 1 + k + 1];
      else x = V[max + 1 + k - 1] + 1;
      y = x - k;
      while (x < n && y < m && line_eq(&a[x], &b[y])) {
        x++;
        y++;
      }
      V[max + 1 + k] = x;
      if (x >= n && y >= m) {
        D = d;
        break;
      }
    }
  }
  /* sufixo, de tras para frente */
  for (i = 0; i < suf; i++) {
    ops[no].t = 0;
    ops[no].a = N - 1 - i;
    ops[no].b = M - 1 - i;
    no++;
  }
  if (D < 0) {
    /* diferentes demais: tudo sai e tudo entra */
    for (i = m - 1; i >= 0; i--) {
      ops[no].t = 1;
      ops[no].a = 0;
      ops[no].b = pre + i;
      no++;
    }
    for (i = n - 1; i >= 0; i--) {
      ops[no].t = -1;
      ops[no].a = pre + i;
      ops[no].b = 0;
      no++;
    }
  } else {
    x = n;
    y = m;
    for (d = D; d > 0; d--) {
      int* v = trace[d];
      int pk, px, py;
      k = x - y;
      if (k == -d || (k != d && v[max + 1 + k - 1] < v[max + 1 + k + 1])) pk = k + 1;
      else pk = k - 1;
      px = v[max + 1 + pk];
      py = px - pk;
      while (x > px && y > py) {
        x--;
        y--;
        ops[no].t = 0;
        ops[no].a = pre + x;
        ops[no].b = pre + y;
        no++;
      }
      if (x == px) {
        y--;
        ops[no].t = 1;
        ops[no].a = pre + x;
        ops[no].b = pre + y;
      } else {
        x--;
        ops[no].t = -1;
        ops[no].a = pre + x;
        ops[no].b = pre + y;
      }
      no++;
      x = px;
      y = py;
    }
    while (x > 0 && y > 0) {
      x--;
      y--;
      ops[no].t = 0;
      ops[no].a = pre + x;
      ops[no].b = pre + y;
      no++;
    }
  }
  for (i = pre - 1; i >= 0; i--) {
    ops[no].t = 0;
    ops[no].a = i;
    ops[no].b = i;
    no++;
  }
  /* inverte */
  for (i = 0; i < no / 2; i++) {
    struct op t = ops[i];
    ops[i] = ops[no - 1 - i];
    ops[no - 1 - i] = t;
  }
  for (d = 0; d <= max + 1; d++) free(trace[d]);
  free(trace);
  free(V);
  *out = ops;
  return no;
}

static void print_line(char c, const struct line* l) {
  int n = l->n;
  if (n && l->p[n - 1] == '\r') n--;
  printf("%c%.*s\n", c, n, l->p);
}

static int is_binary(const unsigned char* s, int n) {
  int i;
  for (i = 0; i < n && i < 8000; i++)
    if (!s[i]) return 1;
  return 0;
}

static int not_repo(void);
/* git diff --numstat / --name-only / --name-status / --stat: so o resumo
 * (o Claude Desktop pede --numstat HEAD para a lista de arquivos mudados) */
static int g_dmode; /* 0 diff, 1 numstat, 2 name-only, 3 name-status, 4 stat */
static char g_dterm = '\n';

/* diff unificado de dois textos; 3 linhas de contexto */
static void diff_text(const char* name, const char* oldt, int on, const char* newt, int nn, int isnew, int isdel) {
  struct line* A;
  struct line* B;
  struct op* ops;
  int N, M, no, i, oa = 0, ob = 0;
  if (g_dmode) {
    int add = 0, del = 0, bin = is_binary((const unsigned char*)oldt, on) || is_binary((const unsigned char*)newt, nn);
    if (g_dmode == 2) {
      printf("%s%c", name, g_dterm);
      return;
    }
    if (g_dmode == 3) {
      printf("%c\t%s%c", isnew ? 'A' : isdel ? 'D' : 'M', name, g_dterm);
      return;
    }
    if (!bin) {
      N = split_lines(oldt, on, &A);
      M = split_lines(newt, nn, &B);
      no = diff_ops(A, N, B, M, &ops);
      for (i = 0; i < no; i++) {
        if (ops[i].t < 0) del++;
        else if (ops[i].t > 0) add++;
      }
      free(ops);
      free(A);
      free(B);
    }
    if (g_dmode == 4) printf(" %s | %d %s\n", name, add + del, bin ? "Bin" : "");
    else if (bin) printf("-\t-\t%s%c", name, g_dterm);
    else printf("%d\t%d\t%s%c", add, del, name, g_dterm);
    return;
  }
  printf("diff --git a/%s b/%s\n", name, name);
  if (isnew) printf("arquivo novo\n");
  if (isdel) printf("arquivo apagado\n");
  if (is_binary((const unsigned char*)oldt, on) || is_binary((const unsigned char*)newt, nn)) {
    printf("Arquivos binarios diferem\n");
    return;
  }
  printf("--- %s%s\n", isnew ? "" : "a/", isnew ? "/dev/null" : name);
  printf("+++ %s%s\n", isdel ? "" : "b/", isdel ? "/dev/null" : name);
  N = split_lines(oldt, on, &A);
  M = split_lines(newt, nn, &B);
  no = diff_ops(A, N, B, M, &ops);
  i = 0;
  while (i < no) {
    int s, e, j, ca = 0, cb = 0, sa, sb, r;
    if (ops[i].t == 0) {
      i++;
      continue;
    }
    s = i - 3 < 0 ? 0 : i - 3;
    e = i;
    for (;;) {
      while (e < no && ops[e].t != 0) e++;
      r = e;
      while (r < no && ops[r].t == 0) r++;
      if (r < no && r - e <= 6) {
        e = r;
        continue;
      }
      e = e + 3 > no ? no : e + 3;
      break;
    }
    /* linhas antes do pedaco */
    oa = 0;
    ob = 0;
    for (j = 0; j < s; j++) {
      if (ops[j].t <= 0) oa++;
      if (ops[j].t >= 0) ob++;
    }
    for (j = s; j < e; j++) {
      if (ops[j].t <= 0) ca++;
      if (ops[j].t >= 0) cb++;
    }
    sa = ca ? oa + 1 : oa;
    sb = cb ? ob + 1 : ob;
    printf("@@ -%d,%d +%d,%d @@\n", sa, ca, sb, cb);
    for (j = s; j < e; j++) {
      if (ops[j].t == 0) print_line(' ', &A[ops[j].a]);
      else if (ops[j].t < 0) print_line('-', &A[ops[j].a]);
      else print_line('+', &B[ops[j].b]);
    }
    i = e;
  }
  free(ops);
  free(A);
  free(B);
}

static int diff_wanted(int nf, char** files, const struct nent* e) {
  int i;
  if (!nf) return 1;
  for (i = 0; i < nf; i++) {
    int l = (int)strlen(files[i]);
    if (!files[i][0] || ci_cmp(e->dos, files[i]) == 0 ||
        (ci_start(e->dos, files[i]) && e->dos[l] == '\\'))
      return 1;
  }
  return 0;
}

static int cmd_diff(int argc, char** argv) {
  char root[PATHMAX], full[PATHMAX + 40];
  struct list head, idx;
  char* files[64];
  int nf = 0, cached = 0, i, shown = 0, vs_head = 0, after_dd = 0;
  if (find_root(0, root) != 0) return not_repo();
  g_dmode = 0;
  g_dterm = '\n';
  quiet_pack = pack_quiet = 0;
  for (i = 2; i < argc; i++) {
    const char* o = argv[i];
    if (!after_dd && (strcmp(o, "--cached") == 0 || strcmp(o, "--staged") == 0)) cached = 1;
    else if (!after_dd && strcmp(o, "--numstat") == 0) g_dmode = 1;
    else if (!after_dd && strcmp(o, "--name-only") == 0) g_dmode = 2;
    else if (!after_dd && strcmp(o, "--name-status") == 0) g_dmode = 3;
    else if (!after_dd && (strcmp(o, "--stat") == 0 || strcmp(o, "--shortstat") == 0)) g_dmode = 4;
    else if (!after_dd && strcmp(o, "-z") == 0) g_dterm = 0;
    else if (!after_dd && strcmp(o, "--") == 0) after_dd = 1;
    else if (!after_dd && o[0] == '-') continue; /* --no-textconv, --submodule=short... */
    else if (!after_dd && (strcmp(o, "HEAD") == 0 || strcmp(o, "@") == 0)) vs_head = 1;
    else if (nf < 64) {
      char rel[PATHMAX * 2];
      if (rel_path(root, argv[i], rel) != 0) {
        printf("%s\n", err);
        return 1;
      }
      files[nf++] = xstrdup(rel);
    }
  }
  if (g_dmode) quiet_pack = pack_quiet = 1; /* saida de maquina: sem "lendo ..." */
  list_load(root, "NAMES.TXT", &head);
  index_load(root, &idx);
  if (cached || vs_head) {
    /* index x HEAD */
    for (i = 0; i < idx.n; i++) {
      struct nent* e = &idx.e[i];
      struct nent* h;
      unsigned char* o = 0;
      unsigned char* nw;
      int ol = 0, nl;
      if (e->gone || !diff_wanted(nf, files, e)) continue;
      h = list_find(&head, e->lng);
      if (h && memcmp(h->sha, e->sha, 20) == 0) continue;
      if (h && !(o = blob_get(root, h->sha, &ol))) {
        printf("sem o conteudo antigo de %s\n", e->lng);
        continue;
      }
      nw = blob_get(root, e->sha, &nl);
      if (!nw) {
        printf("sem o conteudo de %s\n", e->lng);
        free(o);
        continue;
      }
      diff_text(e->lng, o ? (char*)o : "", ol, (char*)nw, nl, !h, 0);
      shown++;
      free(o);
      free(nw);
    }
    for (i = 0; i < head.n; i++) {
      struct nent* h = &head.e[i];
      unsigned char* o;
      int ol;
      if (list_find(&idx, h->lng) || !diff_wanted(nf, files, h)) continue;
      o = blob_get(root, h->sha, &ol);
      if (!o) continue;
      diff_text(h->lng, (char*)o, ol, "", 0, 0, 1);
      shown++;
      free(o);
    }
    if (!vs_head) return 0;
  }
  /* disco x index (com HEAD: depois do index x HEAD acima) */
  for (i = 0; i < idx.n; i++) {
    struct nent* e = &idx.e[i];
    unsigned char now[20];
    unsigned char* o;
    char* nw;
    int ol, nl = 0;
    if (!on_disk(e) || !diff_wanted(nf, files, e)) continue;
    path_join(full, root, e->dos);
    nw = load_file(full, &nl);
    if (nw) {
      git_hash("blob", (unsigned char*)nw, nl, now);
      if (memcmp(now, e->sha, 20) == 0) {
        free(nw);
        continue;
      }
    }
    o = blob_get(root, e->sha, &ol);
    if (!o) {
      printf("%s mudou, mas o conteudo antigo nao esta aqui (sem o pack em .git\\objects\\pack; faca o clone de novo)\n", e->lng);
      free(nw);
      continue;
    }
    diff_text(e->lng, (char*)o, ol, nw ? nw : "", nl, 0, !nw);
    shown++;
    free(o);
    free(nw);
  }
  (void)shown;
  return 0;
}

/* ---------- commit ---------- */

static int days_from_civil(int y, int m, int d) {
  int era, yoe, doy, doe;
  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = y - era * 400;
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static int now_epoch(void) {
  int dt = k_date(), tm = k_time();
  if (dt <= 0 || tm < 0) return -1;
  return days_from_civil(dt / 10000, dt / 100 % 100, dt % 100) * 86400 + tm / 10000 * 3600 + tm / 100 % 100 * 60 +
         tm % 100;
}

/* index -> arvores, commit, NAMES.TXT. 0 ok, 1 nada para commitar. */
static int do_commit(const char* root, const char* msg) {
  struct remote r;
  struct list head, idx;
  struct tctx th, ti;
  unsigned char cur[20], base[20], htree[20], tree[20], csha[20];
  char name[128], email[128], tz[16], hex[41], path[PATHMAX];
  char* hc;
  int hl, t, i, has_index;
  struct sbuf s;
  if (state_read(root, &r, cur, base) != 0) {
    sprintf(err, "STATE.TXT invalido");
    return -1;
  }
  cfg_get(root, "user.name", name, sizeof(name));
  cfg_get(root, "user.email", email, sizeof(email));
  if (!name[0] || !email[0]) {
    sprintf(err, "falta o autor. Rode:\n  git config --global user.name Seu Nome\n  git config --global user.email voce@exemplo.com");
    return -1;
  }
  if (!cfg_get(root, "tz", tz, sizeof(tz))) strcpy(tz, "+0000");
  t = now_epoch();
  if (t < 0) {
    sprintf(err, "o relogio do sistema nao tem data");
    return -1;
  }
  if (sha_zero(cur)) {
    /* primeiro commit (repositorio vazio): sem pai; a arvore "anterior" e a
     * vazia */
    list_load(root, "NAMES.TXT", &head);
    memset(&th, 0, sizeof(th));
    th.root = root;
    list_tree(&head, &th, htree);
    memcpy(tree, htree, 20);
  } else {
    hc = commit_get(root, cur, &hl);
    if (!hc || commit_parse(hc, htree, 0) != 0) {
      sprintf(err, "nao achei o commit atual (COMMIT.TXT)");
      free(hc);
      return -1;
    }
    free(hc);
    /* a lista do HEAD tem que dar a arvore do HEAD */
    list_load(root, "NAMES.TXT", &head);
    memset(&th, 0, sizeof(th));
    th.root = root;
    list_tree(&head, &th, tree);
  }
  if (memcmp(tree, htree, 20) != 0) {
    if (memcmp(cur, base, 20) != 0 || reindex(root, &r, cur) != 0) {
      if (!err[0]) sprintf(err, "a lista NAMES.TXT nao confere com o commit");
      return -1;
    }
    list_load(root, "NAMES.TXT", &head);
    th.nknown = 0;
    list_tree(&head, &th, tree);
    if (memcmp(tree, htree, 20) != 0) {
      sprintf(err, "a lista NAMES.TXT nao confere com o commit, nem refeita");
      return -1;
    }
  }
  has_index = index_load(root, &idx);
  /* entradas antigas do INDEX sem modo pegam o modo do HEAD */
  for (i = 0; i < idx.n; i++) {
    struct nent* h;
    if (idx.e[i].hasmode) continue;
    h = list_find(&head, idx.e[i].lng);
    if (h) strcpy(idx.e[i].mode, h->mode);
    idx.e[i].hasmode = 1;
  }
  memset(&ti, 0, sizeof(ti));
  ti.root = root;
  ti.write = 1;
  ti.known = th.known;
  ti.nknown = th.nknown;
  ti.capknown = th.capknown;
  if (list_tree(&idx, &ti, tree) != 0) return -1;
  if (memcmp(tree, htree, 20) == 0) {
    if (has_index) {
      git_file(root, "INDEX.TXT", path);
      k_fdelete(path);
    }
    return 1;
  }
  memset(&s, 0, sizeof(s));
  sha_hex(tree, hex);
  sb_add(&s, "tree ", 5);
  sb_add(&s, hex, 40);
  if (!sha_zero(cur)) {
    sb_add(&s, "\nparent ", 8);
    sha_hex(cur, hex);
    sb_add(&s, hex, 40);
  }
  {
    char who[400];
    sprintf(who, "\nauthor %s <%s> %d %s\ncommitter %s <%s> %d %s\n\n", name, email, t, tz, name, email, t, tz);
    sb_add(&s, who, -1);
  }
  sb_add(&s, msg, -1);
  if (s.p[s.len - 1] != '\n') sb_add(&s, "\n", 1);
  if (obj_write(root, OBJ_COMMIT, s.p, s.len, csha) != 0) {
    free(s.p);
    return -1;
  }
  free(s.p);
  if (list_save(root, "NAMES.TXT", &idx) != 0) {
    sprintf(err, "nao gravou NAMES.TXT");
    return -1;
  }
  git_file(root, "INDEX.TXT", path);
  k_fdelete(path);
  state_write(root, &r, csha, base);
  sha_hex(csha, hex);
  {
    const char* br = strrchr(r.ref, '/');
    int nl = 0;
    while (msg[nl] && msg[nl] != '\n') nl++;
    printf("[%s %.7s] %.*s\n", br ? br + 1 : r.ref, hex, nl, msg);
  }
  return 0;
}

static int cmd_commit(int argc, char** argv) {
  char root[PATHMAX];
  struct sbuf msg;
  int i, all = 0, r;
  memset(&msg, 0, sizeof(msg));
  for (i = 2; i < argc; i++) {
    const char* a = argv[i];
    if (strcmp(a, "-a") == 0 || strcmp(a, "--all") == 0) {
      all = 1;
      continue;
    }
    /* -F arquivo / --file=arquivo: a mensagem inteira, com as linhas (o
     * shell do SODOS nao passa quebra de linha num argumento; antes um
     * commit com "Co-Authored-By:" so saia certo pelo git da VM) */
    if ((strcmp(a, "-F") == 0 || strcmp(a, "--file") == 0) && i + 1 < argc) a = argv[++i];
    else if (strncmp(a, "--file=", 7) == 0) a += 7;
    else a = 0;
    if (a) {
      int fl, e;
      char* ft = load_file(a, &fl);
      if (!ft) {
        printf("nao li a mensagem de %s\n", a);
        return 1;
      }
      for (e = 0; e < fl; e++)
        if (ft[e] != '\r') sb_byte(&msg, ft[e]);
      free(ft);
      while (msg.len && (msg.p[msg.len - 1] == '\n' || msg.p[msg.len - 1] == ' ')) msg.p[--msg.len] = 0;
      continue;
    }
    a = argv[i];
    if (strcmp(a, "-m") == 0 || strcmp(a, "-am") == 0 || strcmp(a, "-ma") == 0) {
      if (strcmp(a, "-m") != 0) all = 1;
      /* o resto da linha e a mensagem (sem aspas tambem serve); cada -m a
       * mais comeca um paragrafo, como no git: -m "titulo" -m "corpo" */
      for (i++; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0) {
          if (msg.len) sb_add(&msg, "\n\n", 2);
          continue;
        }
        if (msg.len && msg.p[msg.len - 1] != '\n') sb_add(&msg, " ", 1);
        sb_add(&msg, argv[i], -1);
      }
      break;
    }
  }
  if (!msg.len) {
    printf("uso: git commit [-a] -m mensagem [-m paragrafo]  ou  git commit [-a] -F arquivo\n");
    return 1;
  }
  if (find_root(0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  if (all) {
    struct list idx;
    struct addst a;
    index_load(root, &idx);
    memset(&a, 0, sizeof(a));
    a.root = root;
    a.idx = &idx;
    a.quiet = 1;
    if (add_update(&a, "") != 0 || list_save(root, "INDEX.TXT", &idx) != 0) {
      printf("erro: %s\n", err);
      return 1;
    }
  }
  err[0] = 0;
  r = do_commit(root, msg.p);
  free(msg.p);
  if (r == 1) {
    printf("nada para o commit (use git add -u, git add arquivo ou git commit -a)\n");
    return 1;
  }
  if (r != 0) {
    printf("erro: %s\n", err);
    return 1;
  }
  return 0;
}

/* ---------- pull --rebase ----------
 * Os commits daqui (de base ate cur, objetos soltos) sao refeitos em cima do
 * commit novo do servidor (r->head), um por um, como no git rebase: cada um
 * leva so o que ele mudou em relacao ao pai. Arquivo mudado aqui e no
 * servidor e juntado por linha (diff3); mudancas que se encostam sao
 * conflito, e entao nada muda (nem a pasta nem o .git). O pack do pull
 * anterior (onde esta a base) e o novo ficam os dois carregados (pack_swap).
 * Arquivo mudado sem commit que o rebase precisa mudar: para, ou com
 * --autostash e juntado do mesmo jeito (conflito fica com <<<<<<< no arquivo). */

struct val {
  int has;
  unsigned char sha[20];
  char mode[8];
};

static void val_of(const struct nent* e, struct val* v) {
  memset(v, 0, sizeof(*v));
  if (!e || e->gone) return;
  v->has = 1;
  memcpy(v->sha, e->sha, 20);
  strcpy(v->mode, e->mode);
}

static int val_eq(const struct val* a, const struct val* b) {
  if (a->has != b->has) return 0;
  return !a->has || (memcmp(a->sha, b->sha, 20) == 0 && strcmp(a->mode, b->mode) == 0);
}

static int rb_old; /* 1 = o pack anterior esta guardado (pack_swap) */

static unsigned char* dup_n(const unsigned char* d, int n) {
  unsigned char* o = (unsigned char*)malloc((size_t)n + 1);
  if (!o) return 0;
  memcpy(o, d, (size_t)n);
  o[n] = 0;
  return o;
}

/* objeto de qualquer lugar: pack novo, pack anterior ou .git\objects. malloc. */
static unsigned char* rb_get(const char* root, const unsigned char* sha, int* type, int* len) {
  unsigned char* d = pack_get(sha, type, len);
  if (d) return dup_n(d, *len);
  if (rb_old) {
    unsigned char* o = 0;
    pack_swap();
    d = pack_get(sha, type, len);
    if (d) o = dup_n(d, *len);
    pack_swap();
    if (o) return o;
  }
  return obj_read(root, sha, type, len);
}

/* o pack do clone/pull anterior, guardado com pack_swap. Devolve o buffer. */
static char* rb_load_old(const char* root) {
  char path[PATHMAX];
  int n;
  char* t;
  char* buf;
  git_file(root, "PACK.TXT", path);
  t = load_file(path, &n);
  if (t) {
    char* nl = strchr(t, '\n');
    if (nl) *nl = 0;
    sprintf(path, "%s\\%s", root, t);
    free(t);
  } else {
    git_file(root, "HEAD.PCK", path);
  }
  buf = load_file(path, &n);
  if (!buf) return 0;
  printf("lendo o pack anterior\n");
  if (pack_load((unsigned char*)buf, n, err) < 0) {
    printf("aviso: %s\n", err);
    pack_free();
    free(buf);
    return 0;
  }
  pack_swap();
  rb_old = 1;
  return buf;
}

/* arvore -> lista de arquivos (caminho inteiro, sha, modo; sem nome no disco) */
static int flat_tree(const char* root, const unsigned char* tsha, const char* pre, struct list* l) {
  int type, size, p = 0;
  unsigned char* t = rb_get(root, tsha, &type, &size);
  if (!t || type != OBJ_TREE) {
    char hex[41];
    free(t);
    sha_hex(tsha, hex);
    sprintf(err, "nao achei a arvore %.12s (nem nos packs nem em .git\\objects)", hex);
    return -1;
  }
  while (p < size) {
    char mode[8], name[256], lng[PATHMAX + 256];
    const unsigned char* sha;
    int k = 0;
    while (p < size && t[p] != ' ' && k < 7) mode[k++] = (char)t[p++];
    mode[k] = 0;
    p++;
    k = 0;
    while (p < size && t[p] && k < 255) name[k++] = (char)t[p++];
    name[k] = 0;
    p++;
    if (p + 20 > size) break;
    sha = t + p;
    p += 20;
    if (pre[0]) snprintf(lng, sizeof(lng), "%s/%s", pre, name);
    else snprintf(lng, sizeof(lng), "%s", name);
    if (strcmp(mode, "40000") == 0) {
      if (flat_tree(root, sha, lng, l) != 0) {
        free(t);
        return -1;
      }
    } else {
      struct nent* e = list_push(l);
      e->dos = (char*)"";
      e->lng = xstrdup(lng);
      memcpy(e->sha, sha, 20);
      strcpy(e->mode, mode);
      e->hasmode = 1;
    }
  }
  free(t);
  return 0;
}

static int flat_commit_tree(const char* root, const unsigned char* tsha, struct list* l) {
  memset(l, 0, sizeof(*l));
  if (flat_tree(root, tsha, "", l) != 0) return -1;
  list_sort(l);
  return 0;
}

static void flat_free(struct list* l) {
  int i;
  for (i = 0; i < l->n; i++) free(l->e[i].lng);
  free(l->e);
  memset(l, 0, sizeof(*l));
}

/* pelo caminho, mesmo apagada (gone) */
static struct nent* rb_find(struct list* l, const char* lng) {
  int lo = 0, hi = l->n - 1;
  while (lo <= hi) {
    int m = (lo + hi) / 2, c = strcmp(l->e[m].lng, lng);
    if (!c) return &l->e[m];
    if (c < 0) lo = m + 1;
    else hi = m - 1;
  }
  return 0;
}

/* --- diff3 --- */

struct hunk {
  int s, e;   /* linhas da base [s,e) */
  int bs, be; /* viram estas do lado [bs,be) */
};

/* linhas com o \n de cada uma: juntar de volta da o mesmo texto */
static int split_keep(const char* s, int len, struct line** out) {
  int n = 0, i, k = 0;
  struct line* v;
  for (i = 0; i < len; i++)
    if (s[i] == '\n') n++;
  v = (struct line*)malloc(sizeof(struct line) * (size_t)(n + 2));
  for (i = 0; i < len;) {
    int j = i;
    while (j < len && s[j] != '\n') j++;
    if (j < len) j++;
    v[k].p = s + i;
    v[k].n = j - i;
    k++;
    i = j;
  }
  *out = v;
  return k;
}

static int to_hunks(const struct op* ops, int no, struct hunk** out) {
  struct hunk* h = (struct hunk*)malloc(sizeof(struct hunk) * (size_t)(no + 1));
  int i = 0, n = 0, a = 0, b = 0;
  while (i < no) {
    if (ops[i].t == 0) {
      a++;
      b++;
      i++;
      continue;
    }
    h[n].s = a;
    h[n].bs = b;
    while (i < no && ops[i].t != 0) {
      if (ops[i].t < 0) a++;
      else b++;
      i++;
    }
    h[n].e = a;
    h[n].be = b;
    n++;
  }
  *out = h;
  return n;
}

static void put_lines(struct sbuf* s, const struct line* v, int from, int to) {
  int i;
  for (i = from; i < to; i++) sb_add(s, v[i].p, v[i].n);
}

/* a base [cs,ce) com os hunks h[i0,i1) de um lado aplicados */
static void side_text(struct sbuf* s, const struct line* B, const struct line* S, const struct hunk* h, int i0,
                      int i1, int cs, int ce) {
  int k = cs, i;
  for (i = i0; i < i1; i++) {
    put_lines(s, B, k, h[i].s);
    put_lines(s, S, h[i].bs, h[i].be);
    k = h[i].e;
  }
  put_lines(s, B, k, ce);
}

static void conflict_part(struct sbuf* out, const char* mark, const struct sbuf* t) {
  sb_add(out, mark, -1);
  sb_add(out, t->p, t->len);
  if (t->len && t->p[t->len - 1] != '\n') sb_add(out, "\n", 1);
}

/* Junta as mudancas base->lt e base->xt. 0 ok (out = texto), -1 conflito,
 * -2 binario. markers = 1: conflito vira <<<<<<< aqui / ======= / >>>>>>>
 * servidor no texto e devolve o numero de conflitos (> 0). */
static int merge3(const char* bt, int bn, const char* lt, int ln, const char* xt, int xn, int markers,
                  struct sbuf* out) {
  struct line* B;
  struct line* L;
  struct line* X;
  struct op* o1;
  struct op* o2;
  struct hunk* h1;
  struct hunk* h2;
  int nb, nl, nx, n1, n2, i = 0, j = 0, pos = 0, nconf = 0;
  memset(out, 0, sizeof(*out));
  if (is_binary((const unsigned char*)bt, bn) || is_binary((const unsigned char*)lt, ln) ||
      is_binary((const unsigned char*)xt, xn))
    return -2;
  nb = split_keep(bt, bn, &B);
  nl = split_keep(lt, ln, &L);
  nx = split_keep(xt, xn, &X);
  line_exact = 1;
  n1 = diff_ops(B, nb, L, nl, &o1);
  n2 = diff_ops(B, nb, X, nx, &o2);
  line_exact = 0;
  n1 = to_hunks(o1, n1, &h1);
  n2 = to_hunks(o2, n2, &h2);
  free(o1);
  free(o2);
  sb_add(out, "", 0);
  while (i < n1 || j < n2) {
    int i0 = i, j0 = j, cs, ce;
    if (j >= n2 || (i < n1 && h1[i].s <= h2[j].s)) {
      cs = h1[i].s;
      ce = h1[i].e;
      i++;
    } else {
      cs = h2[j].s;
      ce = h2[j].e;
      j++;
    }
    /* o que encosta (mesma linha ou vizinha) entra no mesmo bloco */
    for (;;) {
      if (i < n1 && h1[i].s <= ce) {
        if (h1[i].e > ce) ce = h1[i].e;
        i++;
      } else if (j < n2 && h2[j].s <= ce) {
        if (h2[j].e > ce) ce = h2[j].e;
        j++;
      } else {
        break;
      }
    }
    put_lines(out, B, pos, cs);
    if (j == j0) {
      side_text(out, B, L, h1, i0, i, cs, ce);
    } else if (i == i0) {
      side_text(out, B, X, h2, j0, j, cs, ce);
    } else {
      struct sbuf a, b;
      memset(&a, 0, sizeof(a));
      memset(&b, 0, sizeof(b));
      sb_add(&a, "", 0);
      sb_add(&b, "", 0);
      side_text(&a, B, L, h1, i0, i, cs, ce);
      side_text(&b, B, X, h2, j0, j, cs, ce);
      if (a.len == b.len && memcmp(a.p, b.p, (size_t)a.len) == 0) {
        sb_add(out, a.p, a.len); /* a mesma mudanca dos dois lados */
      } else {
        nconf++;
        if (markers) {
          conflict_part(out, "<<<<<<< aqui (sem commit)\n", &a);
          conflict_part(out, "=======\n", &b);
          sb_add(out, ">>>>>>> servidor\n", -1);
        }
      }
      free(a.p);
      free(b.p);
      if (nconf && !markers) break;
    }
    pos = ce;
  }
  put_lines(out, B, pos, nb);
  free(B);
  free(L);
  free(X);
  free(h1);
  free(h2);
  if (nconf && !markers) {
    free(out->p);
    memset(out, 0, sizeof(*out));
    return -1;
  }
  return nconf;
}

/* Mudanca de um commit (b -> l) num caminho que o servidor deixou em x.
 * 0 = res e o resultado (blob juntado ja gravado), -1 = conflito (impresso). */
static int rb_merge(const char* root, const char* lng, const struct val* b, const struct val* l,
                    const struct val* x, struct val* res) {
  const char* why = 0;
  memcpy(res, x, sizeof(*res));
  if (!b->has) why = "criado aqui e no servidor, diferentes";
  else if (!l->has) why = "apagado aqui e mudado no servidor";
  else if (!x->has) why = "mudado aqui e apagado no servidor";
  else if (strcmp(l->mode, b->mode) == 0) strcpy(res->mode, x->mode);
  else if (strcmp(x->mode, b->mode) == 0 || strcmp(x->mode, l->mode) == 0) strcpy(res->mode, l->mode);
  else why = "o modo mudou aqui e no servidor";
  if (!why) {
    if (memcmp(l->sha, b->sha, 20) == 0) {
      memcpy(res->sha, x->sha, 20);
    } else if (memcmp(x->sha, b->sha, 20) == 0 || memcmp(x->sha, l->sha, 20) == 0) {
      memcpy(res->sha, l->sha, 20);
    } else if (strcmp(res->mode, "100644") != 0 && strcmp(res->mode, "100755") != 0) {
      why = "link ou submodulo mudado aqui e no servidor";
    } else {
      int t1, t2, t3, n1 = 0, n2 = 0, n3 = 0, m;
      unsigned char* d1 = rb_get(root, b->sha, &t1, &n1);
      unsigned char* d2 = rb_get(root, l->sha, &t2, &n2);
      unsigned char* d3 = rb_get(root, x->sha, &t3, &n3);
      struct sbuf o;
      memset(&o, 0, sizeof(o));
      if (!d1 || !d2 || !d3) {
        why = "objeto que nao esta aqui";
      } else {
        m = merge3((char*)d1, n1, (char*)d2, n2, (char*)d3, n3, 0, &o);
        if (m == -2) why = "arquivo binario mudado aqui e no servidor";
        else if (m != 0) why = "as mudancas daqui e do servidor mexem nas mesmas linhas";
        else if (obj_write(root, OBJ_BLOB, o.p, o.len, res->sha) != 0) why = err;
        else printf("  juntado: %s\n", lng);
      }
      free(d1);
      free(d2);
      free(d3);
      free(o.p);
    }
  }
  if (why) {
    printf("  conflito: %s (%s)\n", lng, why);
    return -1;
  }
  return 0;
}

/* o commit orig refeito com outra arvore e outro pai; o author fica, o
 * committer e quem esta aqui (who), ou o mesmo se who = 0 */
static int rb_commit(const char* root, const char* orig, const unsigned char* tree, const unsigned char* parent,
                     const char* who, unsigned char* out) {
  struct sbuf s;
  char hex[41];
  const char* body = strstr(orig, "\n\n");
  const char* p;
  int r;
  memset(&s, 0, sizeof(s));
  sha_hex(tree, hex);
  sb_add(&s, "tree ", 5);
  sb_add(&s, hex, 40);
  sha_hex(parent, hex);
  sb_add(&s, "\nparent ", 8);
  sb_add(&s, hex, 40);
  sb_add(&s, "\n", 1);
  for (p = orig; *p && (!body || p <= body);) {
    const char* nl = strchr(p, '\n');
    int n = nl ? (int)(nl - p) + 1 : (int)strlen(p);
    if (strncmp(p, "author ", 7) == 0 || (!who && strncmp(p, "committer ", 10) == 0)) sb_add(&s, p, n);
    if (!nl) break;
    p = nl + 1;
  }
  if (who) sb_add(&s, who, -1);
  sb_add(&s, body ? body + 1 : "\n", -1);
  r = obj_write(root, OBJ_COMMIT, s.p, s.len, out);
  free(s.p);
  return r;
}

/* primeira linha da mensagem */
static void subject_of(const char* t, char* out, int max) {
  const char* b = strstr(t, "\n\n");
  int n = 0;
  if (b) {
    b += 2;
    while (b[n] && b[n] != '\n' && n < max - 1) n++;
    memcpy(out, b, (size_t)n);
  }
  out[n] = 0;
}

struct stash {
  char* dos;
  char* lng;
  char* data;
  int len;
  int conflicts;
};

/* A pasta pode ir de olds (o HEAD daqui) para N? Arquivo mudado sem commit
 * que o rebase muda: conta como problema, ou (autostash) e juntado em st.
 * Devolve o numero de problemas (ja impressos). */
static int rb_worktree(const char* root, struct list* old, struct list* N, int autostash, struct stash** st,
                       int* nst) {
  int i = 0, j = 0, bad = 0, cap = 0;
  *st = 0;
  *nst = 0;
  while (i < old->n || j < N->n) {
    struct nent* o;
    struct nent* e;
    char full[PATHMAX + 40];
    unsigned char now[20];
    int c;
    if (j < N->n && N->e[j].gone) {
      j++;
      continue;
    }
    c = i >= old->n ? 1 : j >= N->n ? -1 : strcmp(old->e[i].lng, N->e[j].lng);
    o = c <= 0 ? &old->e[i] : 0;
    e = c >= 0 ? &N->e[j] : 0;
    if (o) i++;
    if (e) j++;
    if (o && e && memcmp(o->sha, e->sha, 20) == 0) continue; /* igual (ou so o modo) */
    if (o) {
      int t1, t2, n0, n1, n2, m;
      char* d0;
      unsigned char* d1;
      unsigned char* d2;
      struct sbuf mo;
      if (!on_disk(o)) continue;
      path_join(full, root, o->dos);
      d0 = load_file(full, &n0);
      if (!d0) {
        if (e) {
          printf("  apagado aqui (sem commit) e mudado pelo pull: %s\n", o->lng);
          bad++;
        }
        continue;
      }
      git_hash("blob", (unsigned char*)d0, n0, now);
      if (memcmp(now, o->sha, 20) == 0 || !e || memcmp(now, e->sha, 20) == 0) {
        /* sem mudanca, ou o pull apaga (o checkout mantem o mudado) */
        free(d0);
        continue;
      }
      if (!autostash) {
        printf("  mudado aqui (sem commit) e mudado pelo pull: %s\n", o->lng);
        free(d0);
        bad++;
        continue;
      }
      d1 = rb_get(root, o->sha, &t1, &n1);
      d2 = rb_get(root, e->sha, &t2, &n2);
      m = d1 && d2 ? merge3((char*)d1, n1, d0, n0, (char*)d2, n2, 1, &mo) : -3;
      free(d1);
      free(d2);
      free(d0);
      if (m < 0) {
        printf("  mudado aqui (sem commit) e pelo pull, %s: %s\n", m == -2 ? "binario" : "nao junta", o->lng);
        bad++;
        continue;
      }
      if (*nst == cap) {
        cap = cap ? cap * 2 : 16;
        *st = (struct stash*)realloc(*st, sizeof(struct stash) * (size_t)cap);
      }
      (*st)[*nst].dos = xstrdup(o->dos);
      (*st)[*nst].lng = xstrdup(o->lng);
      (*st)[*nst].data = mo.p;
      (*st)[*nst].len = mo.len;
      (*st)[*nst].conflicts = m;
      (*nst)++;
    } else {
      /* arquivo novo do pull: um nao rastreado com o mesmo nome seria perdido */
      char rel[PATHMAX + 256];
      int k;
      snprintf(rel, sizeof(rel), "%s", e->lng);
      for (k = 0; rel[k]; k++)
        if (rel[k] == '/') rel[k] = '\\';
      path_join(full, root, rel);
      if ((int)strlen(full) > 250 || k_fsize(full) < 0) continue;
      if (file_sha(full, now) == 0 && memcmp(now, e->sha, 20) != 0) {
        printf("  arquivo nao rastreado no lugar de um novo do servidor: %s\n", e->lng);
        bad++;
      }
    }
  }
  return bad;
}

struct lcommit {
  unsigned char sha[20];
  unsigned char tree[20];
  char* text;
};

static int pull_rebase(const char* root, struct remote* r, const unsigned char* cur, const unsigned char* base,
                       int autostash) {
  struct lcommit* lc = 0;
  struct list P, C, N, A, idx, nm;
  struct tctx tt;
  struct stash* st = 0;
  unsigned char c[20], tree[20], parent[20], head[20], utree[20], ntree[20], chk[20];
  char path[PATHMAX], hex[41], hex2[41], who[400], subj[200];
  char* resp = 0;
  char* oldbuf = 0;
  char* utext = 0;
  unsigned char* pack;
  unsigned char* cm;
  int nlc = 0, caplc = 0, i, k, rc = 1, nconf = 0, nskip = 0, nst = 0, nmade = 0, plen, type, usize, gr, sconf = 0;
  unsigned t0 = k_ms();
  memset(&P, 0, sizeof(P));
  memset(&C, 0, sizeof(C));
  memset(&N, 0, sizeof(N));
  memset(&tt, 0, sizeof(tt));
  /* 0) o stage (git add) tem que estar igual ao HEAD */
  if (list_load(root, "INDEX.TXT", &idx) >= 0) {
    int same;
    list_load(root, "NAMES.TXT", &nm);
    same = idx.n == nm.n;
    for (i = 0; same && i < idx.n; i++)
      same = strcmp(idx.e[i].lng, nm.e[i].lng) == 0 && memcmp(idx.e[i].sha, nm.e[i].sha, 20) == 0;
    free(idx.e);
    free(idx.text);
    free(nm.e);
    free(nm.text);
    if (!same) {
      printf("Ha mudancas no stage (git add) sem commit: faca o git commit (ou git restore --staged .)\n");
      printf("e rode o git pull --rebase de novo. Nada foi mudado.\n");
      return 1;
    }
    git_file(root, "INDEX.TXT", path);
    k_fdelete(path);
  }
  /* 1) os commits daqui, do mais novo ao mais velho */
  memcpy(c, cur, 20);
  while (memcmp(c, base, 20) != 0) {
    int len;
    char* t = (char*)obj_read(root, c, &type, &len);
    if (!t || type != OBJ_COMMIT || commit_parse(t, tree, parent) != 0) {
      sha_hex(c, hex);
      printf("erro: o commit %s nao esta em .git\\objects\n", hex);
      free(t);
      goto out;
    }
    if (nlc == caplc) {
      caplc = caplc ? caplc * 2 : 16;
      lc = (struct lcommit*)realloc(lc, sizeof(*lc) * (size_t)caplc);
    }
    memcpy(lc[nlc].sha, c, 20);
    memcpy(lc[nlc].tree, tree, 20);
    lc[nlc].text = t;
    nlc++;
    if (sha_zero(parent)) {
      if (!sha_zero(base)) {
        printf("erro: os commits daqui nao chegam no ultimo commit do servidor\n");
        goto out;
      }
      break;
    }
    if (nlc > 10000) {
      printf("erro: mais de 10000 commits daqui\n");
      goto out;
    }
    memcpy(c, parent, 20);
  }
  /* 2) o pack anterior (a base dos commits daqui; o --autostash tambem le
   * dele) e o novo (o servidor) */
  if (nlc || autostash) oldbuf = rb_load_old(root);
  dotgit_dirs(root);
  if (get_pack(r, &resp, &pack, &plen) != 0) {
    resp = 0;
    printf("\nerro: %s\n", err);
    goto out;
  }
  printf("  pack de %d KB\n", plen / 1024);
  if (pack_load(pack, plen, err) < 0) {
    printf("erro: %s\n", err);
    goto out;
  }
  cm = pack_get(r->head, &type, &usize);
  if (!cm || type != OBJ_COMMIT || commit_parse((char*)cm, utree, 0) != 0) {
    printf("erro: o commit nao veio no pack\n");
    goto out;
  }
  utext = (char*)dup_n(cm, usize);
  sha_hex(r->head, hex);
  if (nlc) printf("rebase: %d commit(s) daqui por cima de %.12s\n", nlc, hex);
  /* 3) as arvores: base (P), servidor (N) */
  if (nlc && !sha_zero(base)) {
    int bl;
    char* bt = commit_get(root, base, &bl);
    if (!bt || commit_parse(bt, tree, 0) != 0) {
      free(bt);
      printf("erro: nao achei o commit anterior do servidor (COMMIT.TXT)\n");
      goto out;
    }
    free(bt);
    if (flat_commit_tree(root, tree, &P) != 0) {
      printf("erro: %s\n", err);
      goto out;
    }
  }
  if (flat_commit_tree(root, utree, &N) != 0) {
    printf("erro: %s\n", err);
    goto out;
  }
  tt.root = root;
  list_tree(&N, &tt, chk); /* as arvores do servidor ficam conhecidas: nao viram objetos soltos */
  if (memcmp(chk, utree, 20) != 0) {
    printf("erro: a arvore do servidor refeita aqui nao confere; nada foi mudado\n");
    goto out;
  }
  tt.write = 1;
  wr_on = 1;
  wr_n = 0;
  {
    char name[128], email[128], tz[16];
    int t = now_epoch();
    cfg_get(root, "user.name", name, sizeof(name));
    cfg_get(root, "user.email", email, sizeof(email));
    if (!cfg_get(root, "tz", tz, sizeof(tz))) strcpy(tz, "+0000");
    if (name[0] && email[0] && t >= 0) sprintf(who, "committer %s <%s> %d %s\n", name, email, t, tz);
    else who[0] = 0;
  }
  memcpy(head, r->head, 20);
  memcpy(ntree, utree, 20);
  /* 4) cada commit daqui, do mais velho ao mais novo */
  for (k = nlc - 1; k >= 0 && !nconf; k--) {
    int a = 0, b = 0;
    subject_of(lc[k].text, subj, sizeof(subj));
    if (flat_commit_tree(root, lc[k].tree, &C) != 0) {
      printf("erro: %s\n", err);
      goto undo;
    }
    memset(&A, 0, sizeof(A));
    while (a < P.n || b < C.n) {
      int cmpv = a >= P.n ? 1 : b >= C.n ? -1 : strcmp(P.e[a].lng, C.e[b].lng);
      struct nent* pe = cmpv <= 0 ? &P.e[a] : 0;
      struct nent* ce = cmpv >= 0 ? &C.e[b] : 0;
      struct nent* xe;
      struct val vb, vl, vx, res;
      const char* lng = pe ? pe->lng : ce->lng;
      if (pe) a++;
      if (ce) b++;
      val_of(pe, &vb);
      val_of(ce, &vl);
      if (val_eq(&vb, &vl)) continue;
      xe = rb_find(&N, lng);
      val_of(xe, &vx);
      if (val_eq(&vx, &vb)) memcpy(&res, &vl, sizeof(res));
      else if (val_eq(&vx, &vl)) continue;
      else if (rb_merge(root, lng, &vb, &vl, &vx, &res) != 0) {
        nconf++;
        continue;
      }
      if (!res.has) {
        if (xe) xe->gone = 1;
      } else {
        struct nent* e = xe;
        if (!e) {
          e = list_push(&A);
          e->lng = xstrdup(lng);
          e->dos = (char*)"";
        }
        e->gone = 0;
        memcpy(e->sha, res.sha, 20);
        strcpy(e->mode, res.mode);
        e->hasmode = 1;
      }
    }
    for (i = 0; i < A.n; i++) {
      struct nent* e = list_push(&N);
      memcpy(e, &A.e[i], sizeof(*e));
    }
    free(A.e);
    if (A.n) list_sort(&N);
    flat_free(&P);
    memcpy(&P, &C, sizeof(P));
    memset(&C, 0, sizeof(C));
    if (nconf) {
      printf("O commit \"%s\" daqui e o servidor mudaram as mesmas partes.\n", subj);
      break;
    }
    if (list_tree(&N, &tt, tree) != 0) {
      printf("erro: %s\n", err);
      goto undo;
    }
    if (memcmp(tree, ntree, 20) == 0) {
      printf("  pulado (o servidor ja tem essa mudanca): %s\n", subj);
      nskip++;
      continue;
    }
    if (rb_commit(root, lc[k].text, tree, head, who[0] ? who : 0, head) != 0) {
      printf("erro: %s\n", err);
      goto undo;
    }
    memcpy(ntree, tree, 20);
    nmade++;
    sha_hex(head, hex2);
    printf("  %.7s %s\n", hex2, subj);
  }
  if (nconf) {
    printf("Rebase cancelado: nada mudou (pasta e .git como antes do pull).\n");
    printf("Para juntar a mao: git reset origin (a pasta fica com o que foi feito aqui),\n");
    printf("git pull --rebase --autostash (o conflito fica marcado com <<<<<<< nos arquivos),\n");
    printf("arrume os arquivos, git commit -a -m ... e git push.\n");
    goto undo;
  }
  /* 5) a pasta: o que mudou aqui sem commit */
  list_load(root, "NAMES.TXT", &olds);
  if (rb_worktree(root, &olds, &N, autostash, &st, &nst) != 0) {
    printf("Nada foi mudado. Faca o git commit -a desses arquivos (ou guarde-os) e rode o pull de novo,\n");
    printf("ou  git pull --rebase --autostash  para juntar as mudancas sem commit.\n");
    goto undo;
  }
  /* 6) arquivo do resultado que so existia no pack anterior vira objeto solto */
  for (i = 0; i < N.n; i++) {
    struct nent* e = &N.e[i];
    unsigned char* d;
    int len;
    if (e->gone || strcmp(e->mode, "160000") == 0 || pack_has(e->sha) || obj_exists(root, e->sha)) continue;
    d = rb_get(root, e->sha, &type, &len);
    if (!d || obj_write(root, OBJ_BLOB, d, len, chk) != 0) {
      free(d);
      sha_hex(e->sha, hex2);
      printf("erro: o objeto %s (%s) nao esta em lugar nenhum\n", hex2, e->lng);
      goto undo;
    }
    free(d);
  }
  wr_on = 0;
  /* 7) grava: COMMIT.TXT (o do servidor), pasta, pack, NAMES, STATE */
  git_file(root, "COMMIT.TXT", path);
  k_fsave(path, utext, usize);
  if (rb_old) {
    pack_swap();
    pack_free();
    pack_swap();
    rb_old = 0;
  }
  free(oldbuf);
  oldbuf = 0;
  gr = checkout_save(root, r, resp, pack, plen, ntree, head, 0, 1, t0);
  resp = 0;
  for (i = 0; i < nst; i++) {
    char full[PATHMAX + 40];
    path_join(full, root, st[i].dos);
    if (k_fsave(full, st[i].data, st[i].len) != 0) {
      printf("  NAO GRAVOU a mudanca sem commit de %s\n", st[i].lng);
      gr = 1;
    } else if (st[i].conflicts) {
      printf("  conflito marcado (<<<<<<< aqui / >>>>>>> servidor): %s\n", st[i].lng);
      sconf++;
    } else {
      printf("  mudanca sem commit reaplicada: %s\n", st[i].lng);
    }
  }
  if (gr < 0) {
    printf("\nerro: %s\n", err);
    goto out;
  }
  sha_hex(head, hex2);
  if (nlc) {
    printf("Rebase feito: %s agora em %.7s, %d commit(s) daqui por cima de %.7s", branch_of(r), hex2, nmade, hex);
    if (nskip) printf(" (%d pulado(s))", nskip);
    printf(".\n");
    if (nmade) printf("Falta o git push.\n");
  }
  if (sconf) printf("%d arquivo(s) com conflito: arrume e faca git commit -a.\n", sconf);
  rc = gr || sconf ? 1 : 0;
  goto out;
undo:
  /* cancelado: os objetos novos saem */
  for (i = 0; i < wr_n; i++) {
    obj_path(root, wr_sha[i], path);
    k_fdelete(path);
  }
  rc = 1;
out:
  wr_on = 0;
  wr_n = 0;
  if (resp) {
    pack_free();
    free(resp);
  }
  if (rb_old) {
    pack_swap();
    pack_free();
    pack_swap();
    rb_old = 0;
  }
  free(oldbuf);
  free(utext);
  flat_free(&P);
  flat_free(&C);
  flat_free(&N);
  free(tt.known);
  for (i = 0; i < nlc; i++) free(lc[i].text);
  free(lc);
  for (i = 0; i < nst; i++) {
    free(st[i].dos);
    free(st[i].lng);
    free(st[i].data);
  }
  free(st);
  return rc;
}

/* ---------- push ---------- */

struct oset {
  unsigned char (*sha)[20];
  int* type;
  int n;
  int cap;
};

static int oset_has(struct oset* o, const unsigned char* sha) {
  int i;
  for (i = 0; i < o->n; i++)
    if (memcmp(o->sha[i], sha, 20) == 0) return 1;
  return 0;
}

static void oset_add(struct oset* o, const unsigned char* sha, int type) {
  if (o->n == o->cap) {
    o->cap = o->cap ? o->cap * 2 : 64;
    o->sha = (unsigned char (*)[20])realloc(o->sha, 20 * (size_t)o->cap);
    o->type = (int*)realloc(o->type, sizeof(int) * (size_t)o->cap);
  }
  memcpy(o->sha[o->n], sha, 20);
  o->type[o->n++] = type;
}

/* arvores e blobs criados aqui; o que nao esta em OBJ o servidor ja tem */
static void walk_tree(const char* root, const unsigned char* sha, struct oset* o) {
  unsigned char* t;
  int type, len, p = 0;
  if (oset_has(o, sha)) return;
  t = obj_read(root, sha, &type, &len);
  if (!t || type != OBJ_TREE) {
    free(t);
    return;
  }
  oset_add(o, sha, OBJ_TREE);
  while (p < len) {
    char mode[8];
    int k = 0;
    const unsigned char* s;
    while (p < len && t[p] != ' ' && k < 7) mode[k++] = (char)t[p++];
    mode[k] = 0;
    while (p < len && t[p]) p++;
    p++;
    s = t + p;
    p += 20;
    if (strcmp(mode, "40000") == 0) walk_tree(root, s, o);
    else if (strcmp(mode, "160000") != 0 && !oset_has(o, s) && obj_exists(root, s)) oset_add(o, s, OBJ_BLOB);
  }
  free(t);
}

static unsigned adler32(const unsigned char* d, int n) {
  unsigned a = 1, b = 0;
  int i;
  for (i = 0; i < n; i++) {
    a = (a + d[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  return (b << 16) | a;
}

/* zlib sem compressao (blocos "stored"): o servidor aceita igual */
static void z_store(struct sbuf* s, const unsigned char* d, int n) {
  unsigned ad = adler32(d, n);
  int p = 0;
  sb_byte(s, 0x78);
  sb_byte(s, 0x01);
  do {
    int k = n - p > 65535 ? 65535 : n - p;
    sb_byte(s, p + k >= n ? 1 : 0);
    sb_byte(s, k & 255);
    sb_byte(s, k >> 8);
    sb_byte(s, ~k & 255);
    sb_byte(s, (~k >> 8) & 255);
    sb_add(s, (const char*)d + p, k);
    p += k;
  } while (p < n);
  sb_byte(s, (int)(ad >> 24) & 255);
  sb_byte(s, (int)(ad >> 16) & 255);
  sb_byte(s, (int)(ad >> 8) & 255);
  sb_byte(s, (int)ad & 255);
}

static int build_pack(const char* root, struct oset* o, struct sbuf* s) {
  unsigned char sum[20];
  sha1_ctx c;
  int i, start = s->len;   /* o pedido ja tem as linhas de comando antes do PACK */
  sb_add(s, "PACK", 4);
  sb_byte(s, 0);
  sb_byte(s, 0);
  sb_byte(s, 0);
  sb_byte(s, 2);
  sb_byte(s, (o->n >> 24) & 255);
  sb_byte(s, (o->n >> 16) & 255);
  sb_byte(s, (o->n >> 8) & 255);
  sb_byte(s, o->n & 255);
  for (i = 0; i < o->n; i++) {
    int type, len;
    unsigned size;
    unsigned char* d = obj_read(root, o->sha[i], &type, &len);
    int c0;
    if (!d) {
      char hex[41];
      sha_hex(o->sha[i], hex);
      sprintf(err, "objeto %s sumiu de .git\\objects", hex);
      return -1;
    }
    size = (unsigned)len;
    c0 = (type << 4) | (int)(size & 15);
    size >>= 4;
    if (size) c0 |= 0x80;
    sb_byte(s, c0);
    while (size) {
      int b = (int)(size & 0x7F);
      size >>= 7;
      sb_byte(s, b | (size ? 0x80 : 0));
    }
    z_store(s, d, len);
    free(d);
  }
  sha1_init(&c);
  sha1_update(&c, s->p + start, s->len - start);
  sha1_final(&c, sum);
  sb_add(s, (const char*)sum, 20);
  return 0;
}

static void base64(const unsigned char* in, int n, char* out) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int i, o = 0;
  for (i = 0; i < n; i += 3) {
    unsigned v = (unsigned)in[i] << 16;
    if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
    if (i + 2 < n) v |= in[i + 2];
    out[o++] = t[(v >> 18) & 63];
    out[o++] = t[(v >> 12) & 63];
    out[o++] = i + 1 < n ? t[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? t[v & 63] : '=';
  }
  out[o] = 0;
}

/* ---------- login no GitHub (como o Git Credential Manager do Windows) ----------
 * Fluxo "device code" do OAuth: o GitHub da um codigo curto, a pessoa abre
 * github.com/login/device em qualquer navegador (celular, PC), digita o
 * codigo e autoriza. O git pergunta de tempos em tempos ate o GitHub
 * entregar o token, que fica no X:\GITCONF.TXT (token= e user=). Ninguem
 * cria chave nem token a mao.
 * Precisa do Client ID de um "OAuth App" do GitHub com "Enable Device Flow"
 * ligado. O Client ID e publico (nao e segredo) e o fluxo nao usa o client
 * secret. Vem de GIT_CLIENT_ID ou de: git config --global clientid ID */
#define GIT_CLIENT_ID "Ov23liW7Qf9Rc1K7lI4h" /* OAuth App "SODOS git" */
#define GH_SCOPE "repo%20workflow" /* repositorios (inclusive privados) e .github\workflows */
#define GH_FORM "application/x-www-form-urlencoded"

static int hexd(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = lowc(c);
  return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0;
}

/* Um campo da resposta do GitHub: formulario (a=1&b=x%2Fy) ou JSON ("a":1).
 * 1 = achou (out sem o %xx). */
static int gh_field(const char* t, const char* key, char* out, int max) {
  int kl = (int)strlen(key), n = 0;
  const char* p;
  out[0] = 0;
  if (!t) return 0;
  for (p = t; (p = strstr(p, key)) != 0; p += kl) {
    const char* v = p + kl + 1;
    if (p > t && p[-1] == '"' && p[kl] == '"') {
      while (*v == ' ' || *v == ':') v++;
      if (*v == '"')
        for (v++; *v && *v != '"' && n < max - 1;) out[n++] = *v++;
      else
        while (*v && *v != ',' && *v != '}' && *v != ' ' && *v != '\n' && n < max - 1) out[n++] = *v++;
    } else if ((p == t || p[-1] == '&') && p[kl] == '=') {
      while (*v && *v != '&' && *v != '\r' && *v != '\n' && n < max - 1) {
        if (*v == '%' && v[1] && v[2]) {
          out[n++] = (char)(hexd(v[1]) * 16 + hexd(v[2]));
          v += 3;
        } else {
          out[n++] = *v == '+' ? ' ' : *v;
          v++;
        }
      }
    } else {
      continue;
    }
    out[n] = 0;
    return 1;
  }
  return 0;
}

/* Espera s segundos cedendo o turno. 1 = a pessoa apertou Esc. */
static int login_wait(int s) {
  unsigned t0 = k_ms();
  while (k_ms() - t0 < (unsigned)s * 1000u) {
    if (k_trygetc() == 27) return 1;
    k_yield();
  }
  return 0;
}

static int gh_post(const char* url, const char* body, char** out) {
  int len;
  return http_req(url, GH_FORM, body, (int)strlen(body), out, &len, err);
}

static int do_login(void) {
  char cid[100], dev[128], ucode[32], vuri[200], tmp[160], tok[256], login[128], body[600], path[PATHMAX];
  char* t = 0;
  int st, len, interval = 5, expires = 900;
  unsigned t0;
  strcpy(cid, GIT_CLIENT_ID);
  if (cfg_get(0, "clientid", tmp, sizeof(tmp)) && tmp[0]) {
    strncpy(cid, tmp, sizeof(cid) - 1);
    cid[sizeof(cid) - 1] = 0;
  }
  if (cid[0] == 'I' && cid[1] == 'v') {
    printf("Aviso: o clientid %s parece ser de um GitHub App (comeca com Iv).\n", cid);
    printf("O certo e um OAuth App (o Client ID comeca com Ov): veja no fim do login.\n");
  }
  if (!cid[0]) {
    printf("Falta o Client ID do aplicativo de login do GitHub (uma vez so, vale para\n");
    printf("todo mundo que usar este GIT.EXE):\n");
    printf("  1. github.com > Settings > Developer settings > OAuth Apps (nao GitHub Apps)\n");
    printf("     > New OAuth App\n");
    printf("     nome: SODOS git; Homepage URL e Callback URL: https://github.com\n");
    printf("  2. marque \"Enable Device Flow\" e salve (nao precisa de client secret)\n");
    printf("  3. git config --global clientid O_CLIENT_ID   (ou GIT_CLIENT_ID no GIT.C)\n");
    return 1;
  }
  if (!k_net_ready()) {
    printf("rede nao pronta\n");
    return 1;
  }
  http_auth[0] = 0;
  sprintf(body, "client_id=%s&scope=%s", cid, GH_SCOPE);
  st = gh_post("https://github.com/login/device/code", body, &t);
  if (st < 0) {
    printf("erro: %s\n", err);
    return 1;
  }
  if (st != 200 || !gh_field(t, "device_code", dev, sizeof(dev)) || !gh_field(t, "user_code", ucode, sizeof(ucode))) {
    if (!gh_field(t, "error_description", tmp, sizeof(tmp)) && !gh_field(t, "error", tmp, sizeof(tmp)))
      strcpy(tmp, "sem detalhes");
    printf("o GitHub recusou o pedido de login (HTTP %d): %s\n", st, tmp);
    printf("confira o clientid (%s) e se o app tem \"Enable Device Flow\" ligado\n", cid);
    free(t);
    return 1;
  }
  if (!gh_field(t, "verification_uri", vuri, sizeof(vuri)) || !vuri[0]) strcpy(vuri, "https://github.com/login/device");
  if (gh_field(t, "interval", tmp, sizeof(tmp)) && atoi(tmp) > 0) interval = atoi(tmp);
  if (gh_field(t, "expires_in", tmp, sizeof(tmp)) && atoi(tmp) > 0) expires = atoi(tmp);
  free(t);
  printf("\nPara entrar no GitHub, abra em qualquer navegador (no celular ou no PC):\n\n");
  printf("    %s\n\n", vuri);
  printf("e digite o codigo:   %s\n\n", ucode);
  printf("Esperando a autorizacao (Esc cancela)");
  fflush(stdout);
  t0 = k_ms();
  for (;;) {
    if (login_wait(interval)) {
      printf("\ncancelado\n");
      return 1;
    }
    if ((k_ms() - t0) / 1000u > (unsigned)expires) {
      printf("\no codigo expirou: rode git login de novo\n");
      return 1;
    }
    sprintf(body, "client_id=%s&device_code=%s&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code",
            cid, dev);
    t = 0;
    st = gh_post("https://github.com/login/oauth/access_token", body, &t);
    if (st < 0) { /* a rede falhou desta vez: tenta no proximo intervalo */
      printf("!");
      fflush(stdout);
      continue;
    }
    if (gh_field(t, "access_token", tok, sizeof(tok)) && tok[0]) {
      free(t);
      break;
    }
    gh_field(t, "error", tmp, sizeof(tmp));
    if (strcmp(tmp, "authorization_pending") == 0) {
      printf(".");
      fflush(stdout);
    } else if (strcmp(tmp, "slow_down") == 0) {
      char iv[16];
      interval = gh_field(t, "interval", iv, sizeof(iv)) && atoi(iv) > interval ? atoi(iv) : interval + 5;
    } else {
      if (strcmp(tmp, "access_denied") == 0) printf("\no acesso foi recusado na pagina do GitHub\n");
      else if (strcmp(tmp, "expired_token") == 0) printf("\no codigo expirou: rode git login de novo\n");
      else printf("\no GitHub respondeu HTTP %d: %s\n", st, tmp[0] ? tmp : "sem token");
      free(t);
      return 1;
    }
    free(t);
  }
  /* quem entrou: o login vai no "usuario" do Basic do push */
  login[0] = 0;
  t = 0;
  sprintf(http_auth, "Authorization: Bearer %s", tok);
  sprintf(http_ua, "SODOS-git/%s", GIT_SODOS_VER); /* a API recusa "git/..." */
  st = http_req("https://api.github.com/user", 0, 0, 0, &t, &len, err);
  http_auth[0] = 0;
  http_ua[0] = 0;
  if (st == 200) gh_field(t, "login", login, sizeof(login));
  free(t);
  cfg_path(0, 1, path);
  if (cfg_set(path, "token", tok) != 0 || cfg_set(path, "user", login) != 0) {
    printf("\nnao gravou %s\n", path);
    return 1;
  }
  printf("\n\nPronto: entrou no GitHub%s%s. O acesso ficou em %s.\n", login[0] ? " como " : "", login, path);
  if (strncmp(tok, "ghu_", 4) == 0) {
    /* token de GitHub App: so vale onde o app estiver instalado e com as
     * permissoes dele, e por padrao expira em 8 horas */
    printf("\nAtencao: o clientid %s e de um GitHub App, nao de um OAuth App.\n", cid);
    printf("O push so funciona nos repositorios onde o app estiver instalado (com\n");
    printf("permissao Contents: Read and write), e o acesso expira em 8 horas.\n");
    printf("Melhor: github.com > Settings > Developer settings > OAuth Apps > New OAuth App,\n");
    printf("marque \"Enable Device Flow\" e depois:\n");
    printf("  git config --global clientid NOVO_ID     e     git login\n");
  }
  return 0;
}

static int cmd_login(void) { return do_login(); }

static int cmd_logout(void) {
  char path[PATHMAX];
  cfg_path(0, 1, path);
  if (cfg_set(path, "token", "") != 0 || cfg_set(path, "user", "") != 0) {
    printf("nao gravou %s\n", path);
    return 1;
  }
  printf("Saiu: o token foi apagado de %s.\n", path);
  printf("No GitHub o app continua autorizado ate voce tirar em Settings > Applications.\n");
  return 0;
}

/* Clone/pull de repositorio privado do GitHub: Basic com o token do git
 * login (ou de SET GIT_TOKEN). 1 = ha token e http_auth foi preenchido. */
static int gh_auth(const char* url) {
  char token[256], login[128], line[400], cred[560];
  if (!ci_start(url, "https://github.com/")) return 0;
  if (!cfg_get(0, "token", token, sizeof(token)) || !token[0]) return 0;
  if (!cfg_get(0, "user", login, sizeof(login)) || !login[0]) strcpy(login, "x-access-token");
  sprintf(line, "%s:%s", login, token);
  base64((unsigned char*)line, (int)strlen(line), cred);
  if (strlen(cred) + 30 > sizeof(http_auth)) return 0;
  sprintf(http_auth, "Authorization: Basic %s", cred);
  return 1;
}

/* git push|pull [origin [ramo]] [-u] [pasta]: so o que parece pasta vale
 * (antes "git push origin main" procurava a pasta "origin") */
static const char* path_arg(int argc, char** argv) {
  int i;
  for (i = 2; i < argc; i++) {
    const char* a = argv[i];
    if (a[0] == '-' || strcmp(a, "origin") == 0) continue;
    if (strchr(a, ':') || strchr(a, '\\') || strcmp(a, ".") == 0 || strcmp(a, "..") == 0) return a;
  }
  return 0;
}

static int cmd_push(int argc, char** argv) {
  struct remote r;
  char root[PATHMAX], token[256], login[128], cred[400], hex[41], bhex[41], url[400], line[300];
  unsigned char cur[20], base[20], c[20], tree[20], parent[20];
  struct oset o;
  struct sbuf req;
  char* body;
  int len, st, p, ncommits = 0, ok = 0, unpack_ok = 0;
  if (find_root(path_arg(argc, argv), root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  if (state_read(root, &r, cur, base) != 0) {
    printf("STATE.TXT invalido\n");
    return 1;
  }
  if (memcmp(cur, base, 20) == 0) {
    printf("Nada para mandar (nenhum commit local).\n");
    return 0;
  }
  if (strncmp(r.ref, "refs/heads/", 11) != 0) {
    printf("o ramo %s nao e um refs/heads/...: nao sei para onde mandar\n", r.ref);
    return 1;
  }
  if (!cfg_get(root, "token", token, sizeof(token)) || !token[0]) {
    if (!ci_start(r.url, "https://github.com/")) {
      printf("falta o token de acesso deste servidor. Guarde com:\n");
      printf("  git config --global token SEU_TOKEN     (ou SET GIT_TOKEN=...)\n");
      return 1;
    }
    /* como no Windows: o primeiro push pede o login */
    printf("Ainda nao entrou no GitHub neste SODOS.\n");
    if (do_login() != 0) return 1;
    if (!cfg_get(root, "token", token, sizeof(token)) || !token[0]) return 1;
    printf("\n");
  }
  if (!cfg_get(root, "user", login, sizeof(login)) || !login[0]) strcpy(login, "x-access-token");
  sprintf(line, "%s:%s", login, token);
  base64((unsigned char*)line, (int)strlen(line), cred);
  sprintf(http_auth, "Authorization: Basic %s", cred);
  if (!k_net_ready()) {
    printf("rede nao pronta\n");
    return 1;
  }
  if (get_refs(&r, 1) != 0) {
    printf("erro: %s\n", err);
    return 1;
  }
  if (memcmp(r.head, cur, 20) == 0) {
    printf("O servidor ja tem este commit (%.12s).\n", hex_of(cur, hex));
    state_write(root, &r, cur, cur);
    return 0;
  }
  /* objetos dos commits locais: de cur para tras por TODOS os pais (um
   * merge feito por outro git tem dois), parando no que o servidor ja tem
   * (o commit dele agora, o remote= daqui). Commit que nao esta solto aqui
   * (num pack: veio do servidor) tambem para. server_in = o commit do
   * servidor esta no historico daqui: o push e um avanco simples. */
  {
    unsigned char (*q)[20] = 0;
    int nq = 0, cq = 0, qi = 0, server_in = sha_zero(r.head) || memcmp(r.head, base, 20) == 0;
    memset(&o, 0, sizeof(o));
    q = (unsigned char (*)[20])malloc(20 * 64);
    cq = 64;
    memcpy(q[nq++], cur, 20);
    while (qi < nq) {
      int type, l, np, k;
      unsigned char par[8][20];
      char* t;
      memcpy(c, q[qi++], 20);
      if (memcmp(c, r.head, 20) == 0) {
        server_in = 1;
        continue;
      }
      if (memcmp(c, base, 20) == 0 || oset_has(&o, c)) continue;
      t = (char*)obj_read(root, c, &type, &l);
      if (!t || type != OBJ_COMMIT || commit_parse(t, tree, parent) != 0) {
        free(t);
        if (memcmp(c, cur, 20) == 0) {
          printf("commit %s nao esta em .git\\objects\n", hex_of(c, hex));
          free(q);
          return 1;
        }
        continue; /* num pack: o servidor ja tem */
      }
      np = commit_parents(t, par, 8);
      free(t);
      oset_add(&o, c, OBJ_COMMIT);
      walk_tree(root, tree, &o);
      ncommits++;
      for (k = 0; k < np; k++) {
        if (nq == cq) {
          cq *= 2;
          q = (unsigned char (*)[20])realloc(q, 20 * (size_t)cq);
        }
        memcpy(q[nq++], par[k], 20);
      }
    }
    free(q);
    if (!server_in) {
      printf("O servidor mudou: %s esta em %.12s, e esse commit nao esta no historico daqui.\n", r.ref,
             hex_of(r.head, hex));
      printf("Rode  git pull  (refaz os commits daqui por cima dos do servidor) e depois git push.\n");
      return 1;
    }
  }
  memset(&req, 0, sizeof(req));
  /* o valor velho do ref e o que o servidor tem agora (pode ser mais novo
   * que o remote=, quando outro git fez merge ou rebase com ele) */
  sha_hex(r.head, bhex);
  sha_hex(cur, hex);
  {
    int n = sprintf(line, "%s %s %s", bhex, hex, r.ref);
    line[n++] = 0;
    n += sprintf(line + n, "report-status agent=git/sodos\n");
    pkt_n(&req, line, n);
  }
  sb_add(&req, "0000", 4);
  if (build_pack(root, &o, &req) != 0) {
    printf("erro: %s\n", err);
    free(req.p);
    return 1;
  }
  printf("mandando %d commit(s), %d objetos (%d KB)\n", ncommits, o.n, req.len / 1024);
  sprintf(url, "%s/git-receive-pack", r.url);
  st = http_req(url, "application/x-git-receive-pack-request", req.p, req.len, &body, &len, err);
  free(req.p);
  if (st < 0) {
    printf("erro: %s\n", err);
    return 1;
  }
  if (st != 200) {
    printf("erro: HTTP %d em git-receive-pack\n", st);
    if (body && len) printf("%.*s\n", len > 400 ? 400 : len, body);
    free(body);
    return 1;
  }
  for (p = 0; p + 4 <= len;) {
    int n = pkt_len(body + p), l;
    char* s;
    if (n < 0) break;
    if (n == 0) {
      p += 4;
      continue;
    }
    if (n < 4 || p + n > len) break;
    l = n - 4 < (int)sizeof(line) - 1 ? n - 4 : (int)sizeof(line) - 1;
    memcpy(line, body + p + 4, (size_t)l);
    line[l] = 0;
    p += n;
    if (l && line[l - 1] == '\n') line[--l] = 0;
    s = line;
    if (strcmp(s, "unpack ok") == 0) unpack_ok = 1;
    else if (strncmp(s, "ok ", 3) == 0 && strcmp(s + 3, r.ref) == 0) ok = 1;
    else printf("servidor: %s\n", s);
  }
  free(body);
  if (!unpack_ok || !ok) {
    printf("o servidor nao aceitou o push\n");
    return 1;
  }
  state_write(root, &r, cur, cur);
  printf("%.7s..%.7s  %s\n", bhex, hex, r.ref);
  return 0;
}

/* ---------- log ---------- */

/* dias desde 1970-01-01 -> data */
static void civil(int days, int* y, int* m, int* d) {
  int z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
  int doe = z - era * 146097;
  int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = yoe + era * 400 + (*m <= 2);
}

static void print_person(const char* label, char* line) {
  /* "Nome <email> 1730000000 -0300" */
  char* gt = strrchr(line, '>');
  int t, tz, y, mo, d, sign, mins;
  char* tzs;
  if (!gt) {
    printf("%s %s\n", label, line);
    return;
  }
  gt[1] = 0;
  t = atoi(gt + 2);
  tzs = strchr(gt + 2, ' ');
  tz = tzs ? atoi(tzs + 1) : 0;
  sign = tz < 0 ? -1 : 1;
  mins = sign * ((sign * tz) / 100 * 60 + (sign * tz) % 100);
  t += mins * 60;
  civil(t / 86400, &y, &mo, &d);
  printf("%s %s\nData:  %04d-%02d-%02d %02d:%02d %s\n", label, line, y, mo, d, (t % 86400) / 3600,
         (t % 3600) / 60, tzs ? tzs + 1 : "");
}

static int cmd_log(int argc, char** argv) {
  char root[PATHMAX], hex[41];
  struct remote r;
  unsigned char cur[20], base[20], tree[20], parent[20];
  int n = 0;
  if (find_root(argc > 2 ? argv[2] : 0, root) != 0) {
    printf("%s\n", err);
    return 1;
  }
  state_read(root, &r, cur, base);
  if (sha_zero(cur)) {
    printf("ainda nao ha commits em %s\n", branch_of(&r));
    return 0;
  }
  for (;;) {
    char* t;
    char* p;
    int len, body = 0;
    sha_hex(cur, hex);
    t = commit_get(root, cur, &len);
    if (!t) {
      if (!n) printf("sem o commit %s\n", hex);
      break;
    }
    if (n) printf("\n");
    printf("commit %s", hex);
    if (!n) printf(" (%s)", r.ref);
    if (memcmp(cur, base, 20) == 0) printf(" (servidor)");
    printf("\n");
    commit_parse(t, tree, parent);
    for (p = t; *p;) {
      char* nl = strchr(p, '\n');
      if (nl) *nl = 0;
      if (body) printf("    %s\n", p);
      else if (!*p) {
        body = 1;
        printf("\n");
      } else if (strncmp(p, "author ", 7) == 0) {
        print_person("Autor:", p + 7);
      }
      if (!nl) break;
      p = nl + 1;
    }
    free(t);
    n++;
    if (sha_zero(parent)) break;
    memcpy(cur, parent, 20);
  }
  printf("(clone raso: o historico para no commit do clone/pull)\n");
  return 0;
}

/* ---------- config ---------- */

static int cmd_config(int argc, char** argv) {
  char root[PATHMAX], path[PATHMAX], val[300];
  struct sbuf v;
  const char* key;
  int i = 2, global = 0, have_root, get = 0;
  for (; i < argc && argv[i][0] == '-'; i++) {
    if (strcmp(argv[i], "--global") == 0) global = 1;
    else if (strcmp(argv[i], "--get") == 0) get = 1;
    else if (strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-l") == 0) break;
    /* --local, --includes, --null, --type=...: ignorados */
  }
  have_root = find_root(0, root) == 0;
  if (i < argc && have_root && (get || i + 1 >= argc)) {
    /* chaves que o git de verdade tem e aqui saem do STATE.TXT */
    struct remote r;
    unsigned char c[20], b[20];
    state_read(root, &r, c, b);
    if (strcmp(argv[i], "remote.origin.url") == 0) {
      printf("%s\n", r.url);
      return 0;
    }
    if (strncmp(argv[i], "branch.", 7) == 0 && strstr(argv[i], ".remote")) {
      printf("origin\n");
      return 0;
    }
    if (strncmp(argv[i], "branch.", 7) == 0 && strstr(argv[i], ".merge")) {
      printf("refs/heads/%s\n", branch_of(&r));
      return 0;
    }
  }
  if (!global && !have_root) global = 1;
  if (i >= argc || strcmp(argv[i], "--list") == 0 || strcmp(argv[i], "-l") == 0) {
    int len;
    char* t;
    cfg_path(have_root ? root : 0, 1, path);
    t = load_file(path, &len);
    printf("# %s\n%s", path, t ? t : "");
    free(t);
    if (have_root) {
      cfg_path(root, 0, path);
      t = load_file(path, &len);
      printf("# %s\n%s", path, t ? t : "");
      free(t);
    }
    return 0;
  }
  if (i + 1 >= argc) {
    if (!cfg_get(have_root ? root : 0, argv[i], val, sizeof(val))) return 1;
    if (strcmp(argv[i], "token") == 0 && strlen(val) > 8) strcpy(val + 4, "...");
    printf("%s\n", val);
    return 0;
  }
  key = argv[i];
  memset(&v, 0, sizeof(v));
  for (i++; i < argc; i++) {
    if (v.len) sb_add(&v, " ", 1);
    sb_add(&v, argv[i], -1);
  }
  cfg_path(have_root ? root : 0, global, path);
  if (strcmp(v.p, "\"\"") == 0) v.p[0] = 0;
  if (cfg_set(path, key, v.p) != 0) {
    printf("nao gravou %s\n", path);
    return 1;
  }
  free(v.p);
  return 0;
}

/* ---------- .git de fachada ----------
 * Programas que leem o repositorio direto do disco (o Claude Code le
 * .git\HEAD, .git\logs\HEAD e .git\config para saber o ramo e o remoto)
 * nao conhecem o _GIT. Depois de cada comando o GIT.EXE deixa um .git
 * minimo coerente com o _GIT\STATE.TXT: HEAD, refs, config e logs. Os
 * objetos continuam so no _GIT (o .git\SODOS.TXT explica). So grava o que
 * mudou. */
static void put_if_diff(const char* path, const char* text) {
  int len = 0, n = (int)strlen(text);
  char* old = load_file(path, &len);
  if (!old || len != n || memcmp(old, text, (size_t)n) != 0) k_fsave(path, text, n);
  free(old);
}

/* .git\index (DIRC versao 2) a partir do que vai no proximo commit: os
 * caminhos longos com /, o sha e o modo; tempos zerados (outro git
 * confere o conteudo). So grava se mudou. */
static int ix_cmp(const void* a, const void* b) {
  return strcmp((*(struct nent* const*)a)->lng, (*(struct nent* const*)b)->lng);
}
static void write_index(const char* root) {
  struct list idx;
  struct nent** v;
  struct sbuf o;
  char path[PATHMAX + 40];
  unsigned char hdr[12], sum[20];
  sha1_ctx c;
  int i, n = 0;
  index_load(root, &idx);
  v = (struct nent**)malloc(sizeof(*v) * (size_t)(idx.n ? idx.n : 1));
  if (!v) return;
  for (i = 0; i < idx.n; i++)
    if (!idx.e[i].gone) v[n++] = &idx.e[i];
  qsort(v, (size_t)n, sizeof(*v), ix_cmp);
  memset(&o, 0, sizeof(o));
  memcpy(hdr, "DIRC", 4);
  hdr[4] = hdr[5] = hdr[6] = 0;
  hdr[7] = 2;
  hdr[8] = (unsigned char)(n >> 24);
  hdr[9] = (unsigned char)(n >> 16);
  hdr[10] = (unsigned char)(n >> 8);
  hdr[11] = (unsigned char)n;
  sb_add(&o, (const char*)hdr, 12);
  for (i = 0; i < n; i++) {
    unsigned char e[62];
    struct nent* x = v[i];
    unsigned mode = strcmp(x->mode, "100755") == 0 ? 0x81EDu : strcmp(x->mode, "120000") == 0 ? 0xA000u : 0x81A4u;
    int sz, nl = (int)strlen(x->lng), pad, k;
    char full[PATHMAX + 40];
    path_join(full, root, x->dos);
    sz = k_fsize(full);
    if (sz < 0) sz = 0;
    memset(e, 0, sizeof(e));
    e[24] = (unsigned char)(mode >> 24);
    e[25] = (unsigned char)(mode >> 16);
    e[26] = (unsigned char)(mode >> 8);
    e[27] = (unsigned char)mode;
    e[36] = (unsigned char)(sz >> 24);
    e[37] = (unsigned char)(sz >> 16);
    e[38] = (unsigned char)(sz >> 8);
    e[39] = (unsigned char)sz;
    memcpy(e + 40, x->sha, 20);
    e[60] = (unsigned char)((nl > 0xFFF ? 0xFFF : nl) >> 8);
    e[61] = (unsigned char)(nl > 0xFFF ? 0xFF : nl);
    sb_add(&o, (const char*)e, 62);
    for (k = 0; k < nl; k++) sb_byte(&o, x->lng[k] == '\\' ? '/' : x->lng[k]);
    pad = 8 - ((62 + nl) % 8);
    for (k = 0; k < pad; k++) sb_byte(&o, 0);
  }
  sha1_init(&c);
  sha1_update(&c, o.p, o.len);
  sha1_final(&c, sum);
  sb_add(&o, (const char*)sum, 20);
  sprintf(path, "%s\\.git\\index", root);
  {
    int len = 0;
    char* old = load_file(path, &len);
    if (!old || len != o.len || memcmp(old, o.p, (size_t)len) != 0) k_fsave(path, o.p, o.len);
    free(old);
  }
  free(o.p);
  free(v);
}

/* .git\config: o que outro git gravou fica (user.name, pull.rebase...);
 * so entra o que falta: [core], o origin e o ramo. Antes era regravado
 * inteiro a cada comando. */
static void config_ensure(const char* path, const char* url, const char* b) {
  int len = 0;
  char* old = load_file(path, &len);
  struct sbuf s;
  char sec[200];
  memset(&s, 0, sizeof(s));
  sb_add(&s, "", 0);
  if (old) {
    sb_add(&s, old, len);
    if (len && old[len - 1] != '\n') sb_add(&s, "\n", 1);
  }
  if (!old || !strstr(old, "[core]"))
    sb_add(&s, "[core]\n\trepositoryformatversion = 0\n\tfilemode = false\n\tbare = false\n", -1);
  if (!old || !strstr(old, "[remote \"origin\"]")) {
    sb_add(&s, "[remote \"origin\"]\n\turl = ", -1);
    sb_add(&s, url, -1);
    sb_add(&s, "\n\tfetch = +refs/heads/*:refs/remotes/origin/*\n", -1);
  }
  if (b) {
    sprintf(sec, "[branch \"%s\"]", b);
    if (!old || !strstr(old, sec)) {
      sb_add(&s, sec, -1);
      sb_add(&s, "\n\tremote = origin\n\tmerge = refs/heads/", -1);
      sb_add(&s, b, -1);
      sb_add(&s, "\n", 1);
    }
  }
  if (!old || s.len != len) k_fsave(path, s.p, s.len);
  free(old);
  free(s.p);
}

/* .git\logs\HEAD: uma linha a mais quando o ramo andou; as do outro git
 * ficam (antes o arquivo virava uma linha so). */
static void reflog_add(const char* path, const char* hex) {
  int len = 0, e, s;
  char* old = load_file(path, &len);
  char prev[41], line[200];
  struct sbuf b;
  strcpy(prev, "0000000000000000000000000000000000000000");
  if (old) {
    e = len;
    while (e > 0 && (old[e - 1] == '\n' || old[e - 1] == '\r')) e--;
    s = e;
    while (s > 0 && old[s - 1] != '\n') s--;
    if (e - s >= 81 && old[s + 40] == ' ') {
      memcpy(prev, old + s + 41, 40);
      prev[40] = 0;
    }
    if (strcmp(prev, hex) == 0) {
      free(old);
      return;
    }
  }
  sprintf(line, "%s %s SODOS <sodos@localhost> 0 +0000\tgit do SODOS\n", prev, hex);
  memset(&b, 0, sizeof(b));
  sb_add(&b, "", 0);
  if (old && len) {
    sb_add(&b, old, len);
    if (old[len - 1] != '\n') sb_add(&b, "\n", 1);
  }
  sb_add(&b, line, -1);
  k_fsave(path, b.p, b.len);
  free(b.p);
  free(old);
}

static void sync_dotgit(const char* root) {
  char d[PATHMAX + 40], f[PATHMAX + 200], t[1200], hex[41], bhex[41];
  struct remote r;
  unsigned char cur[20], base[20];
  const char* b;
  if (state_read(root, &r, cur, base) != 0) return;
  b = branch_of(&r);
  sha_hex(cur, hex);
  sha_hex(base, bhex);
  sprintf(d, "%s\\.git", root);
  dotgit_dirs(root);
  if (sha_zero(cur)) {
    /* repositorio vazio: so HEAD (ramo ainda nao nascido) e config */
    sprintf(f, "%s\\HEAD", d);
    sprintf(t, "ref: refs/heads/%s\n", b);
    put_if_diff(f, t);
    sprintf(f, "%s\\config", d);
    config_ensure(f, r.url, 0);
    write_index(root);
    return;
  }
  sprintf(f, "%s\\HEAD", d);
  sprintf(t, "ref: refs/heads/%s\n", b);
  put_if_diff(f, t);
  sprintf(f, "%s\\refs\\heads\\%s", d, b);
  sprintf(t, "%s\n", hex);
  put_if_diff(f, t);
  sprintf(f, "%s\\refs\\remotes\\origin\\%s", d, b);
  sprintf(t, "%s\n", bhex);
  if (sha_zero(base)) k_fdelete(f); /* o servidor ainda nao tem o ramo */
  else put_if_diff(f, t);
  sprintf(f, "%s\\config", d);
  config_ensure(f, r.url, b);
  sprintf(f, "%s\\logs\\HEAD", d);
  reflog_add(f, hex);
  /* o .git\shallow e do clone/pull (shallow_note): aqui nao mexe mais */
  sprintf(f, "%s\\description", d);
  put_if_diff(f, "Repositorio do git do SODOS (GIT.EXE); dados proprios em .git\\sodos.\n");
  write_index(root);
}

/* ---------- comandos de consulta (o Claude Desktop / Claude Code usam) ----------
 * A saida imita a do git de verdade (Git for Windows: caminhos com /), para
 * quem le a saida por programa. Fora de um repositorio: "fatal: ..." e 128. */

/* "D:\\REPO\\X" -> "D:/REPO/X" */
static void print_slash(const char* p) {
  for (; *p; p++) putchar(*p == '\\' ? '/' : *p);
  putchar('\n');
}

static int not_repo(void) {
  printf("fatal: not a git repository (or any of the parent directories): .git\n");
  return 128;
}

static int cmd_rev_parse(int argc, char** argv) {
  char root[PATHMAX], hex[41];
  struct remote r;
  unsigned char cur[20], base[20];
  int i, have = find_root(0, root) == 0, abbrev = 0, shortn = 0, verify = 0, rc = 0;
  if (have) state_read(root, &r, cur, base);
  for (i = 2; i < argc; i++) {
    const char* o = argv[i];
    if (strcmp(o, "--is-inside-work-tree") == 0) {
      if (!have) return not_repo();
      printf("true\n");
    } else if (strcmp(o, "--is-inside-git-dir") == 0 || strcmp(o, "--is-bare-repository") == 0 ||
               strcmp(o, "--is-shallow-repository") == 0) {
      if (!have) return not_repo();
      printf(strcmp(o, "--is-shallow-repository") == 0 ? "true\n" : "false\n");
    } else if (strcmp(o, "--show-toplevel") == 0) {
      if (!have) return not_repo();
      if (strlen(root) == 2) strcat(root, "\\");
      print_slash(root);
    } else if (strcmp(o, "--git-dir") == 0 || strcmp(o, "--git-common-dir") == 0 ||
               strcmp(o, "--absolute-git-dir") == 0) {
      char g[PATHMAX + 8];
      if (!have) return not_repo();
      sprintf(g, "%s\\.git", root);
      sync_dotgit(root);
      print_slash(g);
    } else if (strcmp(o, "--show-prefix") == 0 || strcmp(o, "--show-cdup") == 0) {
      if (!have) return not_repo();
      printf("\n");
    } else if (strcmp(o, "--abbrev-ref") == 0) {
      abbrev = 1;
    } else if (strncmp(o, "--short", 7) == 0) {
      shortn = o[7] == '=' ? atoi(o + 8) : 7;
    } else if (strcmp(o, "--verify") == 0 || strcmp(o, "-q") == 0 || strcmp(o, "--quiet") == 0) {
      verify = 1;
    } else if (strcmp(o, "--symbolic-full-name") == 0) {
      abbrev = 2;
    } else if (o[0] != '-') {
      /* HEAD, HEAD^{commit}, @, o nome do ramo, origin/ramo */
      int is_head = strncmp(o, "HEAD", 4) == 0 || strcmp(o, "@") == 0 ||
                    (have && (strcmp(o, branch_of(&r)) == 0 || strcmp(o, r.ref) == 0));
      int is_up = have && strncmp(o, "origin/", 7) == 0 && strcmp(o + 7, branch_of(&r)) == 0;
      if (!have) return not_repo();
      if (strstr(o, "@{u}") || strstr(o, "@{upstream}")) {
        if (abbrev) printf("origin/%s\n", branch_of(&r));
        else {
          sha_hex(base, hex);
          printf("%s\n", hex);
        }
        continue;
      }
      if (!is_head && !is_up) {
        if (!verify) printf("%s\n", o);
        printf("fatal: ambiguous argument '%s': unknown revision\n", o);
        rc = 128;
        continue;
      }
      if (abbrev == 1) printf("%s\n", is_up ? o : branch_of(&r));
      else if (abbrev == 2) printf("refs/heads/%s\n", branch_of(&r));
      else {
        sha_hex(is_up ? base : cur, hex);
        if (shortn > 0 && shortn < 40) hex[shortn] = 0;
        printf("%s\n", hex);
      }
    }
  }
  return rc;
}

static int cmd_branch(int argc, char** argv) {
  char root[PATHMAX];
  struct remote r;
  unsigned char cur[20], base[20];
  int i, all = 0;
  if (find_root(0, root) != 0) return not_repo();
  state_read(root, &r, cur, base);
  for (i = 2; i < argc; i++) {
    if (strcmp(argv[i], "--show-current") == 0) {
      printf("%s\n", branch_of(&r));
      return 0;
    }
    if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0 || strcmp(argv[i], "-r") == 0) all = 1;
    else if (argv[i][0] != '-' && strcmp(argv[i], "--list") != 0) {
      printf("fatal: o git do SODOS nao cria ramos (so o ramo do clone: %s)\n", branch_of(&r));
      return 128;
    }
  }
  printf("* %s\n", branch_of(&r));
  if (all) printf("  remotes/origin/%s\n", branch_of(&r));
  return 0;
}

static int cmd_symbolic_ref(int argc, char** argv) {
  char root[PATHMAX];
  struct remote r;
  unsigned char cur[20], base[20];
  int i, sh = 0;
  if (find_root(0, root) != 0) return not_repo();
  state_read(root, &r, cur, base);
  for (i = 2; i < argc; i++)
    if (strcmp(argv[i], "--short") == 0) sh = 1;
  if (sh) printf("%s\n", branch_of(&r));
  else printf("refs/heads/%s\n", branch_of(&r));
  return 0;
}

static int cmd_remote(int argc, char** argv) {
  char root[PATHMAX];
  struct remote r;
  unsigned char cur[20], base[20];
  if (find_root(0, root) != 0) return not_repo();
  state_read(root, &r, cur, base);
  if (argc > 2 && strcmp(argv[2], "-v") == 0) {
    printf("origin\t%s (fetch)\norigin\t%s (push)\n", r.url, r.url);
    return 0;
  }
  if (argc > 3 && strcmp(argv[2], "get-url") == 0) {
    if (strcmp(argv[argc - 1], "origin") != 0) {
      printf("error: No such remote '%s'\n", argv[argc - 1]);
      return 2;
    }
    printf("%s\n", r.url);
    return 0;
  }
  if (argc > 2 && strcmp(argv[2], "show") != 0) {
    printf("fatal: o git do SODOS so tem o remoto origin (%s)\n", r.url);
    return 128;
  }
  printf("origin\n");
  return 0;
}

static int cmd_worktree(int argc, char** argv) {
  char root[PATHMAX], hex[41];
  struct remote r;
  unsigned char cur[20], base[20];
  int porc = argc > 3 && strcmp(argv[3], "--porcelain") == 0;
  if (argc < 3 || strcmp(argv[2], "list") != 0) {
    printf("fatal: o git do SODOS nao tem worktrees (so 'git worktree list')\n");
    return 128;
  }
  if (find_root(0, root) != 0) return not_repo();
  state_read(root, &r, cur, base);
  sha_hex(cur, hex);
  if (strlen(root) == 2) strcat(root, "\\");
  if (porc) {
    printf("worktree ");
    print_slash(root);
    printf("HEAD %s\nbranch refs/heads/%s\n\n", hex, branch_of(&r));
  } else {
    char* p;
    for (p = root; *p; p++)
      if (*p == '\\') *p = '/';
    printf("%s  %.7s [%s]\n", root, hex, branch_of(&r));
  }
  return 0;
}

/* git ls-files [-v] [-z] [-s|--stage] [pathspec ignorada] */
static int cmd_ls_files(int argc, char** argv) {
  char root[PATHMAX], hx[41];
  struct list idx;
  int i, tag = 0, stage = 0;
  char term = '\n';
  for (i = 2; i < argc; i++) {
    if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "-t") == 0) tag = 1;
    else if (strcmp(argv[i], "-z") == 0) term = 0;
    else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--stage") == 0) stage = 1;
  }
  if (find_root(0, root) != 0) return not_repo();
  index_load(root, &idx);
  for (i = 0; i < idx.n; i++) {
    if (idx.e[i].gone) continue;
    if (tag) printf("H ");
    if (stage) {
      sha_hex(idx.e[i].sha, hx);
      printf("%s %s 0\t", idx.e[i].mode[0] ? idx.e[i].mode : "100644", hx);
    }
    printf("%s%c", idx.e[i].lng, term);
  }
  return 0;
}

/* %H %h %s %an %ae %at %ct %aI %cI %n, mais texto */
static void log_format(const char* fmt, const char* hex, char* t) {
  char subj[300], an[120], ae[120];
  long at = 0;
  char* p;
  int body = 0;
  subj[0] = an[0] = ae[0] = 0;
  for (p = t; *p;) {
    char* nl = strchr(p, '\n');
    if (nl) *nl = 0;
    if (body) {
      if (!subj[0]) {
        strncpy(subj, p, sizeof(subj) - 1);
        subj[sizeof(subj) - 1] = 0;
      }
    } else if (!*p) {
      body = 1;
    } else if (strncmp(p, "author ", 7) == 0) {
      char* lt = strchr(p, '<');
      char* gt = strchr(p, '>');
      if (lt && gt && gt > lt) {
        int n = (int)(lt - (p + 7));
        while (n > 0 && p[7 + n - 1] == ' ') n--;
        if (n > (int)sizeof(an) - 1) n = (int)sizeof(an) - 1;
        memcpy(an, p + 7, (size_t)n);
        an[n] = 0;
        n = (int)(gt - lt - 1);
        if (n > (int)sizeof(ae) - 1) n = (int)sizeof(ae) - 1;
        memcpy(ae, lt + 1, (size_t)n);
        ae[n] = 0;
        at = atol(gt + 2);
      }
    }
    if (!nl) break;
    p = nl + 1;
  }
  for (; *fmt; fmt++) {
    if (*fmt != '%') {
      putchar(*fmt);
      continue;
    }
    fmt++;
    if (*fmt == 'H') printf("%s", hex);
    else if (*fmt == 'h') printf("%.7s", hex);
    else if (*fmt == 's') printf("%s", subj);
    else if (*fmt == 'n') putchar('\n');
    else if (*fmt == 'x' && fmt[1] && fmt[2]) { /* %x00: o byte em hexa */
      char h[3];
      h[0] = fmt[1];
      h[1] = fmt[2];
      h[2] = 0;
      putchar((int)strtol(h, 0, 16));
      fmt += 2;
    } else if (*fmt == 'a' || *fmt == 'c') {
      char k = fmt[1];
      if (!k) break;
      fmt++;
      if (k == 'n') printf("%s", an);
      else if (k == 'e') printf("%s", ae);
      else if (k == 't') printf("%ld", at);
      else if (k == 'I' || k == 'd' || k == 'D') {
        int y, mo, d;
        civil((int)(at / 86400), &y, &mo, &d);
        printf("%04d-%02d-%02dT%02d:%02d:%02dZ", y, mo, d, (int)(at % 86400) / 3600, (int)(at % 3600) / 60,
               (int)(at % 60));
      } else if (k == 'r') printf("recentemente");
    } else if (*fmt == '%') putchar('%');
    else if (!*fmt) break;
  }
}

/* git log --oneline | --format=... | --pretty=format:... [-n N | -N | --max-count=N] */
static int cmd_log_fmt(int argc, char** argv) {
  char root[PATHMAX], hex[41];
  struct remote r;
  unsigned char cur[20], base[20], tree[20], parent[20];
  const char* fmt = 0;
  int i, max = 1 << 30, n = 0;
  for (i = 2; i < argc; i++) {
    const char* o = argv[i];
    if (strcmp(o, "--oneline") == 0) fmt = "%h %s";
    else if (strncmp(o, "--format=", 9) == 0) fmt = o + 9;
    else if (strncmp(o, "--pretty=format:", 16) == 0) fmt = o + 16;
    else if (strncmp(o, "--pretty=tformat:", 17) == 0) fmt = o + 17;
    else if (strcmp(o, "--pretty=oneline") == 0) fmt = "%H %s";
    else if (strncmp(o, "--max-count=", 12) == 0) max = atoi(o + 12);
    else if (strcmp(o, "-n") == 0 && i + 1 < argc) max = atoi(argv[++i]);
    else if (o[0] == '-' && o[1] >= '0' && o[1] <= '9') max = atoi(o + 1);
  }
  if (find_root(0, root) != 0) return not_repo();
  state_read(root, &r, cur, base);
  while (n < max) {
    int len;
    char* t;
    sha_hex(cur, hex);
    t = commit_get(root, cur, &len);
    if (!t) break;
    commit_parse(t, tree, parent);
    log_format(fmt, hex, t);
    putchar('\n');
    free(t);
    n++;
    if (sha_zero(parent)) break;
    memcpy(cur, parent, 20);
  }
  return 0;
}

static int log_wants_fmt(int argc, char** argv) {
  int i;
  for (i = 2; i < argc; i++)
    if (argv[i][0] == '-') return 1;
  return 0;
}

static void usage(void) {
  printf("git do SODOS %s (clone raso)\n", GIT_SODOS_VER);
  printf("  git clone https://github.com/dono/projeto [pasta]\n");
  printf("  git pull [pasta]        atualiza para o ultimo commit\n");
  printf("  git pull                com commits daqui, refaz por cima dos do servidor\n");
  printf("  git pull --rebase       (o mesmo; --no-rebase nao junta)\n");
  printf("        [--autostash]     junta tambem as mudancas sem commit\n");
  printf("  git status [pasta]      o que mudou\n");
  printf("  git add -u              poe no commit os rastreados mudados/apagados\n");
  printf("  git add -A              idem, mais os arquivos novos\n");
  printf("  git add arquivo|pasta   poe esse no commit\n");
  printf("  git diff [--cached] [arquivo]\n");
  printf("  git commit [-a] -m mensagem [-m paragrafo]   ou   -F arquivo (varias linhas)\n");
  printf("  git push                manda os commits (o primeiro pede o login)\n");
  printf("  git login               entra no GitHub: abre github.com/login/device e digita o codigo\n");
  printf("  git logout              apaga o token guardado\n");
  printf("  git log [pasta]\n");
  printf("  git config [--global] chave [valor]   user.name, user.email, token, tz (-0300)\n");
}

static int git_main(int argc, char** argv);
int main(int argc, char** argv) {
  char root[PATHMAX];
  int rc = git_main(argc, argv);
  /* depois do comando: os arquivos padrao do .git em dia, mas so se o
   * comando mudou o estado (commit, add, pull, push, reset, restore).
   * Antes era depois de qualquer um: o rev-parse/config que o Claude
   * Desktop chama sem parar regravava os refs e desfazia o que outro git
   * (o da VM) tinha acabado de fazer. No clone a pasta atual nao e o
   * repositorio (ja foi feito no proprio clone). */
  if (st_dirty && !(argc > 1 && strcmp(argv[1], "clone") == 0) && find_root(0, root) == 0) sync_dotgit(root);
  return rc;
}

/* git for-each-ref [--count=N] [--format=...] [padrao...]: o ramo local e o
 * origin. O Claude Desktop pede --count=1 --format=%(objectname) para saber
 * se o repositorio ja tem commit. Formato: %(objectname), %(objectname:short),
 * %(refname), %(refname:short), %(objecttype); sem formato, como o git. */
static int cmd_for_each_ref(int argc, char** argv) {
  char root[PATHMAX], refs[2][200], hex[41];
  unsigned char cur[20], base[20], sha[2][20];
  struct remote r;
  const char* fmt = 0;
  int i, k, n = 0, max = 1 << 30, shown = 0, npat = 0;
  const char* pat[8];
  for (i = 2; i < argc; i++) {
    if (strncmp(argv[i], "--count=", 8) == 0) max = atoi(argv[i] + 8);
    else if (strncmp(argv[i], "--format=", 9) == 0) fmt = argv[i] + 9;
    else if (argv[i][0] != '-' && npat < 8) pat[npat++] = argv[i];
  }
  if (find_root(0, root) != 0) return not_repo();
  if (state_read(root, &r, cur, base) != 0) return 0;
  if (!sha_zero(cur)) {
    strcpy(refs[n], r.ref);
    memcpy(sha[n++], cur, 20);
  }
  if (!sha_zero(base) && strncmp(r.ref, "refs/heads/", 11) == 0) {
    sprintf(refs[n], "refs/remotes/origin/%s", r.ref + 11);
    memcpy(sha[n++], base, 20);
  }
  for (k = 0; k < n && shown < max; k++) {
    const char* p;
    int ok = npat == 0;
    for (i = 0; i < npat && !ok; i++) ok = strncmp(refs[k], pat[i], strlen(pat[i])) == 0;
    if (!ok) continue;
    sha_hex(sha[k], hex);
    if (!fmt) {
      printf("%s commit\t%s\n", hex, refs[k]);
      shown++;
      continue;
    }
    for (p = fmt; *p;) {
      if (strncmp(p, "%(objectname:short)", 19) == 0) {
        printf("%.7s", hex);
        p += 19;
      } else if (strncmp(p, "%(objectname)", 13) == 0) {
        printf("%s", hex);
        p += 13;
      } else if (strncmp(p, "%(refname:short)", 16) == 0) {
        const char* s = refs[k];
        if (strncmp(s, "refs/heads/", 11) == 0) s += 11;
        else if (strncmp(s, "refs/remotes/", 13) == 0) s += 13;
        printf("%s", s);
        p += 16;
      } else if (strncmp(p, "%(refname)", 10) == 0) {
        printf("%s", refs[k]);
        p += 10;
      } else if (strncmp(p, "%(objecttype)", 13) == 0) {
        printf("commit");
        p += 13;
      } else if (p[0] == '%' && p[1] == '%') {
        putchar('%');
        p += 2;
      } else {
        putchar(*p++);
      }
    }
    putchar('\n');
    shown++;
  }
  return 0;
}

static int git_main(int argc, char** argv) {
  /* opcoes antes do comando: -C pasta, -c chave=valor, --no-pager... */
  while (argc > 1 && argv[1][0] == '-') {
    const char* o = argv[1];
    int take = 1;
    if (strcmp(o, "--version") == 0 || strcmp(o, "version") == 0) {
      printf("git version 2.45.0.sodos (GIT.EXE %s)\n", GIT_SODOS_VER);
      return 0;
    }
    if (strcmp(o, "-C") == 0 && argc > 2) {
      if (k_chdir(argv[2]) != 0) {
        printf("fatal: cannot change to '%s': No such file or directory\n", argv[2]);
        return 128;
      }
      take = 2;
    } else if (strcmp(o, "-c") == 0 && argc > 2) {
      take = 2;
    } else if (strcmp(o, "--help") == 0 || strcmp(o, "-h") == 0) {
      usage();
      return 0;
    }
    /* --no-pager, --no-optional-locks, --literal-pathspecs, -P, --git-dir=...: ignorados */
    argc -= take;
    argv += take;
  }
  if (argc < 2) {
    usage();
    return 1;
  }
  if (strcmp(argv[1], "version") == 0) {
    printf("git version 2.45.0.sodos (GIT.EXE %s)\n", GIT_SODOS_VER);
    return 0;
  }
  /* comandos de maquina (o Claude Desktop le a saida): a adocao do que
   * outro git fez acontece igual, mas sem mensagem */
  {
    static const char* quiet[] = {"rev-parse", "branch",   "symbolic-ref", "remote",       "worktree",
                                  "ls-files",  "config",   "for-each-ref", "log"};
    int q;
    for (q = 0; q < (int)(sizeof(quiet) / sizeof(quiet[0])); q++)
      if (strcmp(argv[1], quiet[q]) == 0 && (q != 8 || log_wants_fmt(argc, argv))) adopt_quiet = 1;
    for (q = 2; q < argc; q++)
      if (strncmp(argv[q], "--porcelain", 11) == 0 || strcmp(argv[q], "-z") == 0 ||
          strcmp(argv[q], "--numstat") == 0 || strcmp(argv[q], "--name-only") == 0 ||
          strcmp(argv[q], "--name-status") == 0 || strcmp(argv[q], "--short") == 0)
        adopt_quiet = 1;
  }
  if (strcmp(argv[1], "for-each-ref") == 0) return cmd_for_each_ref(argc, argv);
  if (strcmp(argv[1], "rev-parse") == 0) return cmd_rev_parse(argc, argv);
  if (strcmp(argv[1], "branch") == 0) return cmd_branch(argc, argv);
  if (strcmp(argv[1], "symbolic-ref") == 0) return cmd_symbolic_ref(argc, argv);
  if (strcmp(argv[1], "remote") == 0) return cmd_remote(argc, argv);
  if (strcmp(argv[1], "worktree") == 0) return cmd_worktree(argc, argv);
  if (strcmp(argv[1], "ls-files") == 0) return cmd_ls_files(argc, argv);
  if (strcmp(argv[1], "log") == 0 && log_wants_fmt(argc, argv)) return cmd_log_fmt(argc, argv);
  if (strcmp(argv[1], "clone") == 0) return cmd_clone(argc, argv);
  if (strcmp(argv[1], "pull") == 0) return cmd_pull(argc, argv);
  if (strcmp(argv[1], "status") == 0) return cmd_status(argc, argv);
  if (strcmp(argv[1], "log") == 0) return cmd_log(argc, argv);
  if (strcmp(argv[1], "add") == 0) return cmd_add(argc, argv);
  if (strcmp(argv[1], "restore") == 0) return cmd_restore(argc, argv);
  if (strcmp(argv[1], "reset") == 0) return cmd_reset(argc, argv);
  if (strcmp(argv[1], "commit") == 0) return cmd_commit(argc, argv);
  if (strcmp(argv[1], "push") == 0) return cmd_push(argc, argv);
  if (strcmp(argv[1], "diff") == 0) return cmd_diff(argc, argv);
  if (strcmp(argv[1], "config") == 0) return cmd_config(argc, argv);
  if (strcmp(argv[1], "login") == 0) return cmd_login();
  if (strcmp(argv[1], "logout") == 0) return cmd_logout();
  printf("git: '%s' is not a git command (git do SODOS). Veja 'git --help'.\n", argv[1]);
  return 1;
}
