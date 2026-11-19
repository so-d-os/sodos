/*
 * proc.c - Processos filhos: CreateProcess e ShellExecute(Ex).
 *
 * Um programa novo no SODOS so nasce da tarefa principal (k_exec), aninhado:
 * ela roda o filho ate o fim e so entao volta. As threads do Windows pedem
 * aqui e a tarefa principal atende no laco de espera do main.c
 * (wspawn_world_service). Para o pai nada muda: ele recebe um handle de
 * processo, espera com WaitForSingleObject e le o codigo com
 * GetExitCodeProcess. O filho e outro processo Windows completo (pid do
 * SODOS, wproc e TEBs proprios) e herda a janela do pai no WIN.
 *
 * "runas" do ShellExecute e so um lancamento comum: no SODOS todo programa
 * ja e administrador.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"
struct inh_spec;
static struct wobj** inh_snapshot(struct wproc* pp, const struct inh_spec* spec);
static void inh_drop(struct wobj** t);
const HANDLE* wattr_handles(const void* list, DWORD* count); /* procmisc.c */
/* o que o filho herda, decidido no CreateProcess (spawn passa adiante) */
struct inh_spec {
  int mode; /* 0 = nada, 1 = lista, 2 = tudo */
  const HANDLE* list;
  DWORD count;
};

static HANDLE spawn(const char* cmdline, DWORD* err, const struct inh_spec* spec) {
  struct wproc* p = wp();
  struct wobj* o;
  HANDLE h;
  char* c;
  size_t n = strlen(cmdline) + 1;
  uint64_t t0 = wnow_ms();
  o = wobj_new(OBJ_PROCESS);
  c = (char*)k_malloc((unsigned)n);
  if (!o || !c) {
    *err = ERROR_NOT_ENOUGH_MEMORY;
    return 0;
  }
  memcpy(c, cmdline, n);
  o->refs = 1; /* a tarefa principal tambem usa o objeto */
  o->u2 = wnew_pid(); /* pid do filho, como o Windows devolve no CreateProcess */
  h = wh_new(o);
  /* um pedido por vez */
  while (__sync_lock_test_and_set(&p->spawn_req, 1)) {
    if (wnow_ms() - t0 > 60000 || p->exit_req) {
      *err = ERROR_NOT_SUPPORTED;
      return 0;
    }
    k_thread_yield();
  }
  p->spawn_obj = o;
  p->spawn_cmd = c;
  /* ja aqui: o Chromium fecha handles do filho logo que o CreateProcess volta */
  p->spawn_inh = spec && spec->mode ? inh_snapshot(p, spec) : 0;
  __sync_lock_test_and_set(&p->spawn_req, 2);
  k_futex_wake(&p->exit_req, -1);
  wlog("processo filho: ", cmdline);
  return h;
}

/* "prog.exe" a "b c" d -> argv (regras do CRT da Microsoft, simplificadas). */
static int split_args(char* s, char** argv, int max) {
  int n = 0;
  while (*s && n < max) {
    char* d;
    int inq = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) break;
    argv[n++] = d = s;
    while (*s && (inq || (*s != ' ' && *s != '\t'))) {
      if (*s == '\\' && s[1] == '"') {
        *d++ = '"';
        s += 2;
      } else if (*s == '"') {
        inq = !inq;
        s++;
      } else {
        *d++ = *s++;
      }
    }
    if (*s) s++;
    *d = 0;
  }
  return n;
}

/* ---------- explorer.exe interno ----------
 * Instaladores abrem o app instalado com "explorer.exe shell:AppsFolder\
 * <familia>!<app>" ou com o protocolo dele ("claude://"). O SODOS tem um
 * D:\EXPLORER.EXE proprio (outro programa), entao o WINE poe um marcador em
 * D:\WINE\SYSTEM32\EXPLORER.EXE (o PATH dos EXEs do Windows) e, ao rodar
 * esse caminho, resolve o pedido pelo AppxManifest.xml do pacote em
 * D:\WINE\APPS. */
const char* wine_dir(const char* rel);
const uint8_t* wfile_map(const char* path, uint64_t* size, void** tofree);
void wensure_dir(const char* path);

void wexplorer_stub(void) {
  static const char note[] = "explorer.exe interno do WINE.DLL do SODOS (marcador; nao e um programa)\r\n";
  char p[300];
  strncpy(p, wine_dir("SYSTEM32\\EXPLORER.EXE"), sizeof(p) - 1);
  p[sizeof(p) - 1] = 0;
  if (k_fsize(p) > 0) return;
  wensure_dir(wine_dir("SYSTEM32"));
  k_fsave(p, note, (int)sizeof(note) - 1);
}

static int ends_ci(const char* s, const char* suf) {
  size_t a = strlen(s), b = strlen(suf);
  return a >= b && wstreq(s + a - b, suf);
}
static int is_explorer(const char* path) {
  const char* b = path;
  const char* q;
  for (q = path; *q; q++)
    if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
  return wstreq(b, "explorer.exe") || wstreq(b, "explorer");
}

/* valor do atributo attr dentro da tag que comeca em t */
static int tag_attr(const char* t, const char* attr, char* out, int max) {
  const char* end = strchr(t, '>');
  size_t n = strlen(attr);
  const char* q;
  for (q = t + 1; end && q < end; q++)
    if ((q[-1] == ' ' || q[-1] == '\n' || q[-1] == '\t') && strncmp(q, attr, n) == 0 && q[n] == '=' &&
        q[n + 1] == '"') {
      int i = 0;
      q += n + 2;
      while (*q && *q != '"' && i < max - 1) out[i++] = *q++;
      out[i] = 0;
      return 1;
    }
  return 0;
}

/* Le o manifesto de dir e acha o executavel do app (id) ou, com proto, do app
 * que declara o protocolo. Grava o caminho completo em out. */
static int manifest_exe(const char* dir, const char* id, const char* proto, char* out, int max) {
  char man[300], exe[200], cur_id[100];
  const char* x;
  const char* t;
  uint64_t size = 0;
  void* mem = 0;
  char* buf;
  int ok = 0;
  strncpy(man, dir, sizeof(man) - 20);
  man[sizeof(man) - 20] = 0;
  strcat(man, "\\AppxManifest.xml");
  x = (const char*)wfile_map(man, &size, &mem);
  if (!x || !size) return 0;
  buf = (char*)k_malloc((unsigned)size + 1);
  if (!buf) {
    if (mem) k_free(mem);
    return 0;
  }
  memcpy(buf, x, (size_t)size);
  buf[size] = 0;
  if (mem) k_free(mem);
  exe[0] = cur_id[0] = 0;
  for (t = buf; (t = strchr(t, '<')) != 0; t++) {
    if (strncmp(t, "<Application ", 13) == 0) {
      cur_id[0] = exe[0] = 0;
      tag_attr(t, "Id", cur_id, sizeof(cur_id));
      tag_attr(t, "Executable", exe, sizeof(exe));
      if (id && wstreq(cur_id, id) && exe[0]) {
        ok = 1;
        break;
      }
    } else if (proto && strncmp(t, "<uap:Protocol ", 14) == 0) {
      char nm[64];
      if (tag_attr(t, "Name", nm, sizeof(nm)) && wstreq(nm, proto) && exe[0]) {
        ok = 1;
        break;
      }
    }
  }
  if (ok) {
    strncpy(out, dir, (size_t)max - 1);
    out[max - 1] = 0;
    strncat(out, "\\", (size_t)max - strlen(out) - 1);
    strncat(out, exe, (size_t)max - strlen(out) - 1);
  }
  k_free(buf);
  return ok;
}

struct find_w {
  DWORD attr;
  uint8_t times[24];
  DWORD hi, lo, r0, r1;
  wchar16 name[260];
  wchar16 alt[14];
} __attribute__((packed));

/* Percorre D:\WINE\APPS; family = "Nome_publicador" (ou 0 = qualquer). */
static int find_app(const char* family, const char* id, const char* proto, char* out, int max) {
  typedef HANDLE(WINAPI * ff_fn)(const wchar16*, struct find_w*);
  typedef BOOL(WINAPI * fn_fn)(HANDLE, struct find_w*);
  typedef BOOL(WINAPI * fc_fn)(HANDLE);
  ff_fn ff = (ff_fn)builtin_lookup("FindFirstFileW");
  fn_fn fnx = (fn_fn)builtin_lookup("FindNextFileW");
  fc_fn fc = (fc_fn)builtin_lookup("FindClose");
  char pat[300], apps[260], name[64], suf[40];
  const char* us = family ? strrchr(family, '_') : 0;
  wchar16* wp;
  struct find_w fd;
  HANDLE h;
  int found = 0;
  if (!ff || !fnx || !fc) return 0;
  name[0] = suf[0] = 0;
  if (us) {
    size_t n = (size_t)(us - family);
    if (n >= sizeof(name)) n = sizeof(name) - 1;
    memcpy(name, family, n);
    name[n] = 0;
    strcpy(suf, "__");
    strncat(suf, us + 1, sizeof(suf) - 3);
  }
  strncpy(apps, wine_dir("APPS"), sizeof(apps) - 1);
  apps[sizeof(apps) - 1] = 0;
  strcpy(pat, apps);
  strcat(pat, "\\*");
  wp = wdup8to16(pat);
  h = wp ? ff(wp, &fd) : (HANDLE)(intptr_t)-1;
  k_free(wp);
  if (h == (HANDLE)(intptr_t)-1 || !h) return 0;
  do {
    char* dn = wdup16to8(fd.name);
    if (dn && dn[0] != '.' && (fd.attr & 0x10)) {
      int match = 1;
      if (us) {
        size_t k = strlen(name);
        match = strlen(dn) > k && dn[k] == '_' && ends_ci(dn, suf);
        if (match) {
          char c = dn[k];
          dn[k] = 0;
          match = wstreq(dn, name);
          dn[k] = c;
        }
      }
      if (match) {
        char dir[300];
        strcpy(dir, apps);
        strcat(dir, "\\");
        strncat(dir, dn, sizeof(dir) - strlen(dir) - 1);
        found = manifest_exe(dir, id, proto, out, max);
      }
    }
    k_free(dn);
  } while (!found && fnx(h, &fd));
  fc(h);
  return found;
}

/* argv do explorer.exe -> programa a rodar (0 = nada a fazer) */
static int explorer_target(int argc, char** argv, char* out, int max) {
  const char* a = argc > 1 ? argv[1] : "";
  if (strncmp(a, "shell:", 6) == 0 && (a[16] == '\\' || a[16] == '/')) {
    char fam[128];
    const char* bang = strchr(a + 17, '!');
    size_t n;
    if (!bang) return 0;
    n = (size_t)(bang - (a + 17));
    if (n >= sizeof(fam)) n = sizeof(fam) - 1;
    memcpy(fam, a + 17, n);
    fam[n] = 0;
    return find_app(fam, bang + 1, 0, out, max);
  }
  {
    const char* colon = strstr(a, "://");
    if (colon && colon > a && colon - a < 40) {
      char proto[48];
      size_t n = (size_t)(colon - a);
      memcpy(proto, a, n);
      proto[n] = 0;
      return find_app(0, 0, proto, out, max);
    }
  }
  return 0;
}

/* ---------- processos filhos em paralelo ----------
 * O WIN roda cada programa como tarefa propria (k_win_launch_arg). A linha de
 * comando completa fica nesta tabela (o WIN so carrega um argumento curto):
 * o filho comeca com "--wine-spawn=N", acha o registro N, troca pelo que o
 * pai pediu e herda os handles do pai nos mesmos numeros. Na saida, grava o
 * codigo no objeto de processo do pai e o acorda. Sem WIN no ar (ou com a
 * fila cheia por muito tempo), o filho roda aninhado como antes. */
#define SPAWN_MAX 16
struct spawnrec {
  volatile int used;
  int id;
  char* cmdline;            /* do pai */
  struct wobj* obj;         /* objeto de processo no pai */
  struct wproc* parent;     /* 0 quando o pai ja saiu */
  struct wproc* child;
  DWORD pid;                /* pid do filho, ja dado no CreateProcess */
  volatile int claimed;
  struct wobj** inh;        /* handles herdados, copiados no CreateProcess */
};

/* O Chromium fecha no pai, logo depois do CreateProcess, handles que so o
 * filho usa (o pipe do crashpad). Como o filho so os pega depois de carregar,
 * a lista e tirada ja no CreateProcess, com uma referencia em cada objeto. */
static struct wobj** inh_snapshot(struct wproc* pp, const struct inh_spec* spec) {
  struct wobj** t = (struct wobj**)k_malloc_shared(WHANDLE_MAX * sizeof(struct wobj*));
  int i;
  if (!t) return 0;
  memset(t, 0, WHANDLE_MAX * sizeof(struct wobj*));
  wlock();
  if (spec->mode == 1) {
    /* PROC_THREAD_ATTRIBUTE_HANDLE_LIST: so os handles da lista (o Windows
     * exige isso do Chromium; herdar tudo dividia ~1000 objetos e o filho
     * soltava memoria do pai: o heap do kernel corrompia) */
    DWORD k;
    for (k = 0; k < spec->count; k++) {
      uintptr_t v = (uintptr_t)spec->list[k];
      if (v >= 0x200 && !(v & 3) && (v - 0x200) / 4 < WHANDLE_MAX) {
        i = (int)((v - 0x200) / 4);
        t[i] = pp->handles[i];
      }
    }
  } else {
    for (i = 1; i < WHANDLE_MAX; i++) t[i] = pp->handles[i];
  }
  for (i = 1; i < WHANDLE_MAX; i++)
    if (t[i]) t[i]->refs++;
  wunlock();
  return t;
}
static void inh_drop(struct wobj** t) {
  int i;
  if (!t) return;
  for (i = 1; i < WHANDLE_MAX; i++)
    if (t[i]) wobj_unref(t[i]);
  k_free_shared(t);
}
static struct spawnrec spawns[SPAWN_MAX];
static volatile int spawn_lock, spawn_seq, pid_seq;

static void sp_lock(void) {
  while (__sync_lock_test_and_set(&spawn_lock, 1)) k_thread_yield();
}
static void sp_unlock(void) { __sync_lock_release(&spawn_lock); }

DWORD wnew_pid(void) { return 0x100u + 4u * (DWORD)__sync_fetch_and_add(&pid_seq, 1); }

static int split_args(char* s, char** argv, int max);
static int wine_wants_async(void);

int wspawn_claim(int* argc, char*** argv) {
  /* do processo (o WINE.DLL e um so para todos: estatico aqui fazia o
   * segundo filho trocar o argv do primeiro) */
  char* line;
  char** av;
  struct wproc* p = wp();
  struct spawnrec* r = 0;
  const char* a;
  int id = 0, i, n;
  if (*argc < 2 || strncmp((*argv)[1], "--wine-spawn=", 13) != 0) return 0;
  for (a = (*argv)[1] + 13; *a >= '0' && *a <= '9'; a++) id = id * 10 + (*a - '0');
  sp_lock();
  for (i = 0; i < SPAWN_MAX; i++)
    if (spawns[i].used && spawns[i].id == id && !spawns[i].claimed) {
      r = &spawns[i];
      r->claimed = 1;
      r->child = p;
      break;
    }
  line = r && r->parent ? (char*)k_malloc((unsigned)strlen(r->cmdline) + 1) : 0;
  if (line) strcpy(line, r->cmdline);
  sp_unlock();
  if (!r || !r->parent) {
    wlog("processo filho sem registro do pai (o pai desistiu): ", (*argv)[1]);
    return -1;
  }
  p->spawn_rec = r;
  p->pid = r->pid; /* o pid que o pai ja recebeu */
  *(uint64_t*)(p->teb + TEB_PID) = p->pid;
  /* heranca: os handles do pai, nos mesmos numeros (o Chromium passa numeros
   * de handle na linha de comando) */
  {
    struct wobj** t = r->inh;
    r->inh = 0;
    if (t) {
      wlock();
      for (i = 1; i < WHANDLE_MAX; i++)
        if (t[i] && !p->handles[i]) { /* a referencia ja foi tomada */
          p->handles[i] = t[i];
          t[i] = 0;
        }
      wunlock();
      {
        int got = 0, miss = 0;
        for (i = 1; i < WHANDLE_MAX; i++) {
          if (t[i]) miss++;
          else if (p->handles[i]) got++;
        }
        wlogx("heranca: handles no filho ", (uint64_t)got);
        wlogx("heranca: slots ja ocupados ", (uint64_t)miss);
      }
      inh_drop(t); /* o que nao coube */
    }
  }
  av = (char**)k_malloc(256 * sizeof(char*));
  if (!line || !av) {
    wlog("processo filho: sem memoria para a linha de comando", 0);
    return -1;
  }
  n = split_args(line, av, 255);
  av[n] = 0;
  /* argv[0] do pai pode ser so o nome; o caminho real fica o que o WIN abriu */
  if (n > 0) av[0] = (*argv)[0];
  *argc = n;
  *argv = av;
  wlog("processo filho em paralelo: ", r->cmdline);
  return 1;
}

/* DuplicateHandle para um filho nosso (obj.c): o objeto entra na tabela do
 * filho (ou na lista que ele copia ao comecar), num numero alto para nao
 * colidir com os que ele mesmo abre. Devolve o numero no filho; 0 = nao e
 * filho deste processo. */
HANDLE wdup_to_child(struct wobj* proc, struct wobj* o) {
  struct spawnrec* r = 0;
  int i, slot = -1;
  sp_lock();
  for (i = 0; i < SPAWN_MAX; i++)
    if (spawns[i].used && spawns[i].obj == proc) {
      r = &spawns[i];
      break;
    }
  if (!r) {
    sp_unlock();
    return 0;
  }
  wlock();
  if (r->child) { /* ja rodando: direto na tabela dele */
    for (i = WHANDLE_MAX - 1; i > WHANDLE_MAX / 2; i--)
      if (!r->child->handles[i]) {
        r->child->handles[i] = o;
        slot = i;
        break;
      }
  } else {
    if (!r->inh) {
      r->inh = (struct wobj**)k_malloc_shared(WHANDLE_MAX * sizeof(struct wobj*));
      if (r->inh) memset(r->inh, 0, WHANDLE_MAX * sizeof(struct wobj*));
    }
    if (r->inh)
      for (i = WHANDLE_MAX - 1; i > WHANDLE_MAX / 2; i--)
        if (!r->inh[i]) {
          r->inh[i] = o;
          slot = i;
          break;
        }
  }
  if (slot >= 0) o->refs++;
  wunlock();
  sp_unlock();
  return slot >= 0 ? (HANDLE)(uintptr_t)(0x200u + (unsigned)slot * 4u) : 0;
}

/* DuplicateHandle(processo filho, h, este processo): o broker do Mojo puxa os
 * handles que um filho manda (o filho nao consegue abrir o pai). Devolve o
 * objeto com uma referencia a mais (para a tabela de quem pede); close tira
 * o handle da tabela do filho (DUPLICATE_CLOSE_SOURCE). 0 = nao achou. */
struct wobj* wdup_from_child(struct wobj* proc, HANDLE h, int close) {
  struct spawnrec* r = 0;
  struct wobj* o = 0;
  uintptr_t v = (uintptr_t)h;
  int i, slot;
  if (v < 0x200u || (v & 3) || (v - 0x200u) / 4 >= WHANDLE_MAX) return 0;
  slot = (int)((v - 0x200u) / 4);
  sp_lock();
  for (i = 0; i < SPAWN_MAX; i++)
    if (spawns[i].used && spawns[i].obj == proc) {
      r = &spawns[i];
      break;
    }
  if (!r) {
    sp_unlock();
    return 0;
  }
  wlock();
  if (r->child) {
    o = r->child->handles[slot];
    if (o && close) r->child->handles[slot] = 0; /* a referencia passa para quem pede */
    else if (o) o->refs++;
  } else if (r->inh) {
    o = r->inh[slot];
    if (o && close) r->inh[slot] = 0;
    else if (o) o->refs++;
  }
  wunlock();
  sp_unlock();
  return o;
}

void wspawn_exit(int code) {
  struct wproc* p = wp();
  struct spawnrec* r = p->spawn_rec;
  int i;
  sp_lock();
  if (r) {
    struct wproc* pp = r->parent;
    if (pp) {
      r->obj->exit_code = (DWORD)code;
      r->obj->exited = 1;
      k_futex_wake(&wine_gen, -1); /* quem espera o processo no pai */
    }
    r->used = 0;
    p->spawn_rec = 0;
  }
  /* filhos deste processo ficam orfaos (o objeto do pai vai embora) */
  for (i = 0; i < SPAWN_MAX; i++)
    if (spawns[i].used && spawns[i].parent == p) {
      spawns[i].parent = 0;
      if (!spawns[i].claimed) {
        inh_drop(spawns[i].inh);
        spawns[i].inh = 0;
        spawns[i].used = 0;
      }
    }
  sp_unlock();
}

/* Pede ao WIN para abrir o filho. 1 = iniciado em paralelo, 0 = nao deu. */
int k_win_launch_arg(const char* exe, const char* arg);
static int spawn_async(struct wproc* p, struct wobj* o, const char* path, const char* cmd) {
  struct spawnrec* r = 0;
  char arg[32];
  int i, id;
  uint64_t t0 = wnow_ms();
  sp_lock();
  for (i = 0; i < SPAWN_MAX; i++)
    if (!spawns[i].used) {
      r = &spawns[i];
      break;
    }
  if (!r) {
    sp_unlock();
    return 0;
  }
  id = __sync_add_and_fetch(&spawn_seq, 1);
  memset(r, 0, sizeof(*r));
  r->used = 1;
  r->id = id;
  r->cmdline = (char*)cmd;
  r->obj = o;
  r->parent = p;
  r->pid = (DWORD)o->u2;
  sp_unlock();
  r->inh = p->spawn_inh;
  p->spawn_inh = 0;
  if (r->inh) {
    int c = 0;
    for (i = 1; i < WHANDLE_MAX; i++)
      if (r->inh[i]) c++;
    wlogx("heranca: handles do pai copiados ", (uint64_t)c);
  }
  strcpy(arg, "--wine-spawn=");
  {
    char t[12];
    int k = 0, j = (int)strlen(arg), v = id;
    do {
      t[k++] = (char)('0' + v % 10);
      v /= 10;
    } while (v);
    while (k) arg[j++] = t[--k];
    arg[j] = 0;
  }
  while (k_win_launch_arg(path, arg) != 0) {
    if (wnow_ms() - t0 > 3000) { /* WIN fora do ar ou fila presa */
      sp_lock();
      inh_drop(r->inh);
      r->inh = 0;
      r->used = 0;
      sp_unlock();
      return 0;
    }
    k_futex_wait(&p->exit_req, 0, 20);
  }
  return 1;
}

/* Na tarefa principal do processo (main.c). */
void wspawn_world_service(void) {
  struct wproc* p = wp();
  struct wobj* o;
  char* cmd;
  char* argv[32];
  unsigned argv32[32];
  int argc, i, rc;
  if (p->spawn_req != 2) return;
  o = p->spawn_obj;
  cmd = p->spawn_cmd;
  char* whole = (char*)k_malloc((unsigned)strlen(cmd) + 1); /* split_args corta cmd */
  if (whole) strcpy(whole, cmd);
  argc = split_args(cmd, argv, 31);
  if (argc < 1) {
    rc = -1;
  } else {
    char path[300];
    wpath(argv[0], path, (int)sizeof(path));
    if (is_explorer(argv[0]) && !wstreq(path, "D:\\EXPLORER.EXE")) {
      /* explorer.exe do Windows: abre o app pedido, se ele estiver instalado */
      char target[300];
      if (explorer_target(argc, argv, target, (int)sizeof(target))) {
        wlog("explorer.exe: abrindo ", target);
        wpath(target, path, (int)sizeof(path));
        argc = 1;
      } else {
        wlog("explorer.exe: nada a abrir para ", argc > 1 ? argv[1] : "(sem argumento)");
        path[0] = 0;
      }
    }
    if (!path[0]) {
      rc = 0;
    } else if (whole && wine_wants_async() && spawn_async(p, o, path, whole)) {
      /* roda em paralelo: o filho grava o codigo e acorda o pai ao sair */
      wlog("processo filho iniciado em paralelo: ", path);
      __sync_lock_release(&p->spawn_req);
      return; /* cmd fica com o registro ate o filho copiar */
    } else {
      /* o k_exec recebe o argv com ponteiros de 4 bytes (ABI antiga) */
      for (i = 0; i < argc; i++) argv32[i] = (unsigned)(uintptr_t)argv[i];
      argv32[0] = (unsigned)(uintptr_t)path;
      rc = k_exec(path, argc, (char**)argv32);
      if (rc <= -1000 && rc >= -1004) { /* MOD_ERR_*: nao carregou */
        wlog("processo filho nao rodou: ", k_last_error());
        rc = 1;
      }
    }
  }
  wlogx("processo filho terminou, codigo ", (uint32_t)rc);
  inh_drop(p->spawn_inh);
  p->spawn_inh = 0;
  k_free(cmd);
  k_free(whole);
  wlock();
  o->exit_code = (DWORD)rc;
  o->exited = 1;
  wunlock();
  __sync_lock_release(&p->spawn_req);
  wsignal();
}

/* SET WINE_SPAWN=0 volta ao filho aninhado (o pai espera ele terminar) */
static int wine_wants_async(void) {
  char v[4];
  return !(k_getenv("WINE_SPAWN", v, (int)sizeof(v)) > 0 && v[0] == '0');
}

static void append_quoted(char* out, size_t max, const char* s) {
  size_t o = strlen(out);
  int q = !*s || strchr(s, ' ') != 0;
  if (o && o < max - 1) out[o++] = ' ';
  if (q && o < max - 1) out[o++] = '"';
  for (; *s && o < max - 2; s++) out[o++] = *s;
  if (q && o < max - 1) out[o++] = '"';
  out[o] = 0;
}

struct process_info {
  HANDLE process, thread;
  DWORD pid, tid;
};
static WINAPI BOOL CreateProcessW(const wchar16* app, wchar16* cmd, void* pa, void* ta, BOOL inh, DWORD fl,
                                  void* env, const wchar16* dir, void* si, struct process_info* pi) {
  /* ate 32 K, o limite do Windows (o renderer do Chromium passa de 1 K e os
   * handles do Mojo vem no fim da linha) */
  enum { LINE_MAX_ = 32768 };
  char* line = (char*)k_malloc(LINE_MAX_);
  char* a = app ? wdup16to8(app) : 0;
  char* c = cmd ? wdup16to8(cmd) : 0;
  DWORD err = 0;
  HANDLE h;
  struct inh_spec spec;
  (void)pa; (void)ta; (void)env; (void)dir;
  /* heranca: bInheritHandles=FALSE nada; STARTUPINFOEX com HANDLE_LIST so a
   * lista; sem lista, tudo (nao guardamos a marca HANDLE_FLAG_INHERIT) */
  spec.mode = inh ? 2 : 0;
  spec.list = 0;
  spec.count = 0;
  if (inh && (fl & 0x80000u) && si) { /* EXTENDED_STARTUPINFO_PRESENT */
    const void* al = *(void* const*)((const uint8_t*)si + 104); /* lpAttributeList */
    spec.list = wattr_handles(al, &spec.count);
    if (spec.list) spec.mode = 1;
  }
  if (!line) {
    k_free(a);
    k_free(c);
    wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
    return 0;
  }
  line[0] = 0;
  if (c) {
    strncpy(line, c, LINE_MAX_ - 1);
    line[LINE_MAX_ - 1] = 0;
  } else if (a) {
    append_quoted(line, LINE_MAX_, a);
  }
  k_free(a);
  k_free(c);
  h = line[0] ? spawn(line, &err, &spec) : 0;
  k_free(line);
  if (!h) {
    wset_lasterr(err ? err : ERROR_FILE_NOT_FOUND);
    return 0;
  }
  if (pi) {
    struct wobj* t = wobj_new(OBJ_PROCESS);
    pi->process = h;
    pi->thread = t ? wh_new(t) : 0;
    struct wobj* po = wh_get(h, OBJ_PROCESS);
    pi->pid = po && po->u2 ? (DWORD)po->u2 : 0x200;
    pi->tid = pi->pid + 4;
  }
  return 1;
}

static WINAPI BOOL CreateProcessA(const char* app, char* cmd, void* pa, void* ta, BOOL inh, DWORD fl, void* env,
                                  const char* dir, void* si, struct process_info* pi) {
  wchar16* a = app ? wdup8to16(app) : 0;
  wchar16* c = cmd ? wdup8to16(cmd) : 0;
  wchar16* d = dir ? wdup8to16(dir) : 0;
  BOOL r = CreateProcessW(a, c, pa, ta, inh, fl, env, d, si, pi);
  k_free(a);
  k_free(c);
  k_free(d);
  return r;
}

struct shexinfo {
  DWORD size, mask;
  HANDLE hwnd;
  const wchar16* verb;
  const wchar16* file;
  const wchar16* params;
  const wchar16* dir;
  int show;
  HANDLE inst;
  void* idlist;
  const wchar16* cls;
  HANDLE keyclass;
  DWORD hotkey;
  HANDLE icon;
  HANDLE process;
};
static WINAPI BOOL ShellExecuteExW(struct shexinfo* i) {
  char line[1024];
  char* f = i->file ? wdup16to8(i->file) : 0;
  char* par = i->params ? wdup16to8(i->params) : 0;
  DWORD err = 0;
  HANDLE h = 0;
  line[0] = 0;
  if (f) {
    char p[300];
    /* so programas; documentos/URLs ainda nao */
    size_t n = strlen(f);
    wpath(f, p, (int)sizeof(p));
    if (n > 4 && wstreq(f + n - 4, ".exe") && k_fsize(p) > 0) {
      append_quoted(line, sizeof(line), f);
      if (par && *par) {
        strncat(line, " ", sizeof(line) - strlen(line) - 1);
        strncat(line, par, sizeof(line) - strlen(line) - 1);
      }
      h = spawn(line, &err, 0); /* ShellExecute: nada herdado */
    } else {
      wlog("ShellExecute: nao e um programa (ignorado): ", f);
      err = ERROR_FILE_NOT_FOUND;
    }
  }
  k_free(f);
  k_free(par);
  if (!h) {
    i->inst = (HANDLE)(uintptr_t)2;
    if (i->mask & 0x40) i->process = 0;
    wset_lasterr(err ? err : ERROR_FILE_NOT_FOUND);
    return 0;
  }
  i->inst = (HANDLE)(uintptr_t)42;
  if (i->mask & 0x40) i->process = h; /* SEE_MASK_NOCLOSEPROCESS */
  else wh_close(h);
  return 1;
}
static WINAPI HANDLE ShellExecuteW(HANDLE w, const wchar16* verb, const wchar16* file, const wchar16* par,
                                   const wchar16* dir, int show) {
  struct shexinfo i;
  memset(&i, 0, sizeof(i));
  i.size = sizeof(i);
  i.hwnd = w;
  i.verb = verb;
  i.file = file;
  i.params = par;
  i.dir = dir;
  i.show = show;
  ShellExecuteExW(&i);
  return i.inst;
}
static WINAPI BOOL TerminateProcess_child(HANDLE h, DWORD code) {
  (void)h;
  (void)code;
  return 0;
}

#define S(n) {#n, (void*)n}
const struct wine_sym proc_syms[] = {S(CreateProcessW), S(CreateProcessA), S(ShellExecuteExW), S(ShellExecuteW), {0, 0}};
