/*
 * bashbox.c - O bash do Claude Code sem prender a tarefa principal.
 *
 * O D:\WINE\BIN\bash.exe e o WSL.EXE do SODOS com outro nome: ele so poe o
 * comando na caixa do servico do WSL (wsl/wslbox.h, SET WSL=1) e devolve a
 * saida e o codigo. Rodado como programa do SODOS (run_sodos_child, proc.c),
 * ele ocupava a tarefa principal do processo enquanto o comando durava; todo
 * outro CreateProcess (git, o rg do Grep, outro bash) esperava a vez e, apos
 * 60 s, falhava com ERROR_NOT_SUPPORTED: o "ENOTSUP ... uv_spawn" do Claude
 * Code. Com a VM sem agente, cada bash esperava 120 s e levava junto todo o
 * resto.
 *
 * Aqui o CreateProcess do bash.exe nao vai a tarefa principal: uma thread
 * deste processo faz o que o BASH.EXE faria (wsl/main.c, bash_main e
 * client_run) e escreve a saida no stdout/stderr do STARTUPINFO enquanto o
 * comando roda. Varios bash correm juntos (WSLB_SLOTS vagas na caixa) e o
 * TerminateProcess do pai mata o comando no Linux (K <id> ao agente).
 *
 * Vem para ca so quando o servico esta ligado ou pedido (SET WSL=1); sem
 * isso o BASH.EXE roda como antes, porque e ele quem faz o SET WSL=1 (o
 * kernel abre a tela Ctrl+F10 nessa hora, coisa que uma thread nao pode).
 * SET WINE_BASHBOX=0 desliga este caminho.
 */
#include <so.h>
#include <string.h>
#include <stdio.h>
#include "wine_int.h"
#include "../../wsl/wslbox.h"

void* k_svc_get(const char* name);
struct wobj* wthread_internal(void* start, void* param); /* obj.c */
DWORD wnew_pid(void);                                     /* proc.c */

struct bjob {
  struct wobj* proc;   /* o processo que o pai recebeu (o pai espera nele) */
  struct wobj* out;    /* stdout/stderr do STARTUPINFO (uma referencia nossa) */
  struct wobj* err;
  char* cmd;           /* comando ja com /d/... no lugar de D:\... */
  uint32_t len;
  int flags;           /* WSLB_LOGIN */
  int what;            /* 0 = comando, 1 = --version, 2 = uso errado */
  volatile int kill;   /* TerminateProcess do pai */
  DWORD kill_code;
  char cwd[WSLB_CWD];
};

static char bash_tag; /* proc->p2 = &bash_tag: o processo e um destes */

/* ---------- copias do wsl/main.c (o BASH.EXE faz igual) ---------- */

static struct wslbox* box_get(void) {
  struct wslbox* b = (struct wslbox*)k_svc_get("WSL");
  return b && b->magic == WSLB_MAGIC && b->version == WSLB_VERSION ? b : 0;
}

/* a imagem SODOS-Debian (a unica com o sodos-agent) esta na pasta? */
static int img_present(void) {
  char dir[96], p[128];
  if (k_getenv("WSLDIR", dir, (int)sizeof dir) <= 0) strcpy(dir, "D:\\WSL");
  snprintf(p, sizeof p, "%s\\DEBIAN.000", dir);
  if (k_fsize(p) > 0) return 1;
  snprintf(p, sizeof p, "%s\\DEBIAN.IMG", dir);
  return k_fsize(p) > 0;
}

/* X:\a\b -> /x/a/b (a pasta atual do pedido) */
static void cwd_linux(const char* w, char* out, int max) {
  int i, o;
  out[0] = 0;
  if (!(w && w[0] && w[1] == ':')) return;
  o = snprintf(out, (size_t)max, "/%c", (char)(w[0] | 0x20));
  for (i = 2; w[i] && o < max - 1; i++) out[o++] = w[i] == '\\' ? '/' : w[i];
  out[o] = 0;
  if (o > 1 && out[o - 1] == '/') out[o - 1] = 0;
}

/* Caminhos do Windows no comando -> Linux, como no Git Bash (wsl/main.c win2lin) */
static char* win2lin(const char* in, uint32_t* len) {
  uint32_t n = *len, i, o = 0;
  char* out = (char*)k_malloc(n * 2 + 16);
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
    out[o++] = '/';
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

/* bash [-l|--login] [-i] -c "comando" (wsl/main.c bash_main) */
static void parse_args(struct bjob* j, int argc, char** argv) {
  const char* cmd = 0;
  int i;
  if (argc >= 2 && !strcmp(argv[1], "--version")) {
    j->what = 1;
    return;
  }
  for (i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (!strcmp(a, "--login")) {
      j->flags |= WSLB_LOGIN;
    } else if (a[0] == '-' && a[1] && a[1] != '-') {
      const char* q;
      int want_c = 0;
      for (q = a + 1; *q; q++) {
        if (*q == 'l') j->flags |= WSLB_LOGIN;
        else if (*q == 'c') want_c = 1;
      }
      if (want_c && !cmd) {
        int k;
        for (k = i + 1; k < argc; k++) {
          if (!strcmp(argv[k], "-l") || !strcmp(argv[k], "--login") || !strcmp(argv[k], "-i")) {
            if (strcmp(argv[k], "-i")) j->flags |= WSLB_LOGIN;
            continue;
          }
          cmd = argv[k];
          i = k;
          break;
        }
      }
    } else if (!cmd) {
      break; /* bash script.sh: nao suportado */
    }
  }
  if (!cmd) {
    j->what = 2;
    return;
  }
  j->len = (uint32_t)strlen(cmd);
  j->cmd = win2lin(cmd, &j->len);
}

/* ---------- a thread ---------- */

static void put(struct bjob* j, int is_err, const void* d, uint32_t n) {
  struct wobj* o = is_err ? j->err : j->out;
  DWORD done = 0;
  if (!n) return;
  if (!o) {
    wcon_write((const char*)d, n); /* sem handle no STARTUPINFO: o console, como antes */
    return;
  }
  if (o->type == OBJ_PIPE) wpipe_write(o, d, n, &done, 0); /* pai fechou a leitura: some */
  else if (o->type == OBJ_FILE) wfile_write(o, d, n, &done, 0); /* tasks\*.output do Claude Code */
}

static void say(struct bjob* j, const char* s) { put(j, 1, s, (uint32_t)strlen(s)); }

static void drain(struct bjob* j, struct wslb_ring* r, int is_err) {
  uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE), t = r->tail;
  while (t != h) {
    uint32_t off = t % WSLB_RING, n = h - t;
    if (n > WSLB_RING - off) n = WSLB_RING - off;
    put(j, is_err, r->buf + off, n);
    t += n;
  }
  __atomic_store_n(&r->tail, t, __ATOMIC_RELEASE);
}

static int run(struct bjob* j) {
  static volatile int nap;
  struct wslbox* b;
  struct wslb_slot* s = 0;
  uint64_t t0 = wnow_ms();
  int i, code, asked = 0;
  if (j->what == 3) return 255; /* sem thread (wbash_start ja disse) */
  if (j->what == 1) {
    static const char v[] = "GNU bash, version 5.2.21(1)-release (x86_64-pc-linux-gnu)\n"
                            "SODOS: o bash roda no Linux do WSL.EXE (SET WSL=1), comandos por -c\n";
    put(j, 0, v, (uint32_t)sizeof v - 1);
    return 0;
  }
  if (j->what == 2) {
    say(j, "bash (SODOS): use bash -c \"comando\"; o console do Linux fica no Ctrl+F10 (SET WSL=1)\n");
    return 2;
  }
  if (!j->cmd) {
    say(j, "bash (SODOS): sem memoria para o comando\n");
    return 255;
  }
  if (j->len > WSLB_CMD_MAX) {
    say(j, "WSL: comando grande demais\n");
    return 255;
  }
  b = box_get();
  if (!(b && b->alive && b->ready) && !img_present()) {
    char opt[160];
    if (k_getenv("WSLOPT", opt, (int)sizeof opt) <= 0 || !strstr(opt, "-D")) {
      say(j, "WSL: falta a imagem SODOS-Debian (D:\\WSL\\DEBIAN.000... ou DEBIAN.IMG, VMLINUZ, INITRD.IMG;\n"
             "     SET WSLDIR muda a pasta). Os comandos do bash rodam pelo agente dessa imagem; a distro\n"
             "     do Windows nao tem o agente.\n");
      return 255;
    }
  }
  while (!(b = box_get()) || !b->alive || !b->ready) {
    if (j->kill) return (int)j->kill_code;
    if (wnow_ms() - t0 > 120000u) {
      say(j, "WSL: a VM nao ficou pronta em 120 s (veja a tela Ctrl+F10 e WSL status)\n");
      return 255;
    }
    k_futex_wait(&nap, 0, 50);
  }
  while (!s) {
    for (i = 0; i < WSLB_SLOTS && !s; i++)
      if (__sync_bool_compare_and_swap(&b->slot[i].state, SL_FREE, SL_CLAIM)) s = &b->slot[i];
    if (!s) {
      if (j->kill) return (int)j->kill_code;
      if (wnow_ms() - t0 > 120000u) {
        say(j, "WSL: todas as vagas da VM ocupadas\n");
        return 255;
      }
      k_futex_wait(&nap, 0, 20);
    }
  }
  s->cmd = (char*)k_malloc_shared(j->len + 1);
  if (!s->cmd) {
    s->state = SL_FREE;
    say(j, "WSL: sem memoria para o comando\n");
    return 255;
  }
  memcpy(s->cmd, j->cmd, j->len);
  s->cmd[j->len] = 0;
  s->cmd_len = j->len;
  s->flags = j->flags;
  memcpy(s->cwd, j->cwd, WSLB_CWD);
  s->out.head = s->out.tail = s->err.head = s->err.tail = 0;
  s->kill = s->killed = 0;
  s->exit_code = 0;
  s->gen++;
  __atomic_store_n(&s->state, SL_POSTED, __ATOMIC_RELEASE);
  if (b->wake) k_futex_wake(b->wake, 1);
  for (;;) {
    int seq = s->seq;
    drain(j, &s->out, 0);
    drain(j, &s->err, 1);
    if (s->state == SL_DONE) break;
    if (!b->alive) {
      s->exit_code = 255;
      break;
    }
    if (j->kill && !asked) {
      /* o agente mata o grupo do processo e responde X; a vaga fecha igual */
      asked = 1;
      s->kill = 1;
      if (b->wake) k_futex_wake(b->wake, 1);
    }
    k_futex_wait(&s->seq, seq, 50);
  }
  drain(j, &s->out, 0);
  drain(j, &s->err, 1);
  code = s->exit_code;
  k_free_shared(s->cmd);
  s->cmd = 0;
  __atomic_store_n(&s->state, SL_FREE, __ATOMIC_RELEASE);
  return j->kill ? (int)j->kill_code : code;
}

static WINAPI DWORD bash_thread(void* arg) {
  struct bjob* j = (struct bjob*)arg;
  struct wobj* o = j->proc;
  int rc = run(j);
  if (wine_debug || rc) {
    char b[48];
    snprintf(b, sizeof b, "codigo %d, pid %u", rc, (unsigned)o->u2);
    wlog("bash (em paralelo) terminou: ", b);
  }
  /* primeiro o EOF (o pai le ate o fim), depois o fim do processo, como no
   * wspawn_world_service */
  if (j->out) wobj_unref(j->out);
  if (j->err) wobj_unref(j->err);
  wlock();
  o->p1 = 0; /* TerminateProcess nao acha mais o pedido */
  o->exit_code = (DWORD)rc;
  o->exited = 1;
  wunlock();
  wsignal_obj(o);
  wobj_unref(o);
  if (j->cmd) k_free(j->cmd);
  k_free(j);
  return 0;
}

/* ---------- entradas (proc.c, k32.c) ---------- */

int wbash_wanted(void) {
  char v[4];
  struct wslbox* b;
  if (k_getenv("WINE_BASHBOX", v, (int)sizeof v) > 0 && v[0] == '0') return 0;
  b = box_get();
  if (b && b->alive) return 1;
  /* servico pedido (AUTOEXEC com SET WSL=1) ainda subindo; sem o pedido o
   * BASH.EXE de sempre faz o SET WSL=1 (so uma tarefa pode abrir a tela) */
  return k_getenv("WSL", v, (int)sizeof v) > 0 && v[0] == '1';
}

/* argv ja separado (proc.c); dir = lpCurrentDirectory ou 0; std = handles do
 * STARTUPINFO no processo atual. Devolve o handle do processo ou 0 (*err). */
HANDLE wbash_start(int argc, char** argv, const char* dir, const HANDLE* std, DWORD* err) {
  struct wproc* p = wp();
  struct bjob* j = (struct bjob*)k_malloc((unsigned)sizeof *j);
  struct wobj* o = wobj_new(OBJ_PROCESS);
  HANDLE h;
  int i;
  if (!j || !o) {
    if (j) k_free(j);
    *err = ERROR_NOT_ENOUGH_MEMORY;
    return 0;
  }
  memset(j, 0, sizeof *j);
  parse_args(j, argc, argv);
  cwd_linux(dir && dir[0] ? dir : p->cwd, j->cwd, WSLB_CWD);
  o->u2 = wnew_pid();
  o->p1 = j;
  o->p2 = &bash_tag;
  j->proc = o;
  /* stdout e stderr: referencias nossas (o pai fecha as dele logo depois do
   * CreateProcess; o EOF so vem quando a thread solta estas) */
  for (i = 1; i <= 2; i++) {
    struct wobj* s = std && std[i] ? wh_get(std[i], 0) : 0;
    if (s && s->type != OBJ_PIPE && s->type != OBJ_FILE) s = 0;
    if (s) {
      wlock();
      s->refs++;
      wunlock();
    }
    if (i == 1) j->out = s;
    else j->err = s;
  }
  o->refs = 1; /* a thread; o handle abaixo soma o do pai */
  h = wh_new(o);
  if (!h) {
    if (j->out) wobj_unref(j->out);
    if (j->err) wobj_unref(j->err);
    if (j->cmd) k_free(j->cmd);
    k_free(j);
    k_free_shared(o);
    *err = ERROR_NOT_ENOUGH_MEMORY;
    return 0;
  }
  if (!wthread_internal((void*)bash_thread, j)) {
    /* sem thread: o processo "sai" na hora com erro (o pai ja tem o handle) */
    say(j, "bash (SODOS): sem memoria para a thread do pedido\n");
    j->what = 3;
    bash_thread(j);
    return h;
  }
  wlog("processo filho (bash em paralelo, caixa do WSL): ", j->what ? argv[argc > 1 ? 1 : 0] : j->cmd);
  return h;
}

/* TerminateProcess de um destes: pede ao agente para matar o comando. 1 se era. */
int wbash_terminate(HANDLE h, DWORD code) {
  struct wobj* o = wh_get(h, OBJ_PROCESS);
  int r = 0;
  if (!o || o->p2 != &bash_tag) return 0;
  wlock();
  if (o->p1) {
    struct bjob* j = (struct bjob*)o->p1;
    j->kill_code = code;
    j->kill = 1;
  }
  r = 1;
  wunlock();
  return r;
}
