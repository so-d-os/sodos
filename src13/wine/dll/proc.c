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

static HANDLE spawn(const char* cmdline, DWORD* err) {
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
  k_free(cmd);
  wlock();
  o->exit_code = (DWORD)rc;
  o->exited = 1;
  wunlock();
  __sync_lock_release(&p->spawn_req);
  wsignal();
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
  char line[1024];
  char* a = app ? wdup16to8(app) : 0;
  char* c = cmd ? wdup16to8(cmd) : 0;
  DWORD err = 0;
  HANDLE h;
  (void)pa; (void)ta; (void)inh; (void)fl; (void)env; (void)dir; (void)si;
  line[0] = 0;
  if (c) {
    strncpy(line, c, sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;
  } else if (a) {
    append_quoted(line, sizeof(line), a);
  }
  k_free(a);
  k_free(c);
  h = line[0] ? spawn(line, &err) : 0;
  if (!h) {
    wset_lasterr(err ? err : ERROR_FILE_NOT_FOUND);
    return 0;
  }
  if (pi) {
    struct wobj* t = wobj_new(OBJ_PROCESS);
    pi->process = h;
    pi->thread = t ? wh_new(t) : 0;
    pi->pid = 0x200;
    pi->tid = 0x204;
  }
  return 1;
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
      h = spawn(line, &err);
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
const struct wine_sym proc_syms[] = {S(CreateProcessW), S(ShellExecuteExW), S(ShellExecuteW), {0, 0}};
