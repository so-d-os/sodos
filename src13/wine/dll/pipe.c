/*
 * pipe.c - Named pipes (\\.\pipe\nome) entre processos do WINE.
 *
 * Todos os processos do Windows estao no mesmo espaco de enderecos e no
 * mesmo WINE.DLL: uma instancia de pipe e uma estrutura numa tabela global,
 * com duas filas (para o servidor e para o cliente). A memoria das filas e
 * das instancias e "compartilhada" (k_malloc_shared): sobrevive ao processo
 * que escreveu, porque o outro lado ainda vai ler.
 *
 *   servidor  CreateNamedPipe -> ConnectNamedPipe (espera ou OVERLAPPED)
 *   cliente   CreateFile("\\.\pipe\nome") / WaitNamedPipe
 *   dados     ReadFile/WriteFile (sincronos ou OVERLAPPED, evento e IOCP),
 *             modo byte ou mensagem, PeekNamedPipe, TransactNamedPipe
 *   fim       CloseHandle de um lado: o outro recebe ERROR_BROKEN_PIPE
 *
 * Escrita nunca bloqueia (sem limite de buffer). Uma leitura pendente por
 * lado (o que o Mojo e o crashpad fazem).
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

void* k_malloc_shared(uint32_t n);
void k_free_shared(void* p);
int wiocp_post(struct wobj* port, DWORD bytes, uintptr_t key, void* ov);

#define PIPE_ACCESS_INBOUND 1
#define PIPE_ACCESS_OUTBOUND 2
#define FILE_FLAG_OVERLAPPED 0x40000000u
#define PIPE_TYPE_MESSAGE 4
#define PIPE_READMODE_MESSAGE 2
#define PIPE_NOWAIT 1

#define ST_PENDING 0x103u
#define ST_BROKEN 0xC000014Bu   /* STATUS_PIPE_BROKEN */
#define ST_MORE 0x80000005u     /* STATUS_BUFFER_OVERFLOW */
#define ST_CANCEL 0xC0000120u   /* STATUS_CANCELLED */
#define ST_LISTEN 0xC000014Du   /* STATUS_PIPE_LISTENING (sem cliente) */

#define ERROR_BROKEN_PIPE 109
#define ERROR_PIPE_BUSY 231
#define ERROR_NO_DATA 232
#define ERROR_PIPE_CONNECTED 535
#define ERROR_PIPE_LISTENING 536
#define ERROR_MORE_DATA 234
#define ERROR_IO_PENDING 997
#define ERROR_SEM_TIMEOUT 121
#define FILE_FLAG_FIRST_PIPE_INSTANCE 0x00080000u

struct overlapped {
  uintptr_t internal, internal_high;
  DWORD off, off_high;
  HANDLE event;
};
struct chunk {
  struct chunk* next;
  DWORD len, off;
  uint8_t data[];
};
struct pend {
  struct overlapped* ov;
  uint8_t* buf;
  DWORD len;
  int used;
  int sync; /* leitura sincrona esperando: sem IOCP nem evento */
  struct wproc* owner; /* o buffer e o OVERLAPPED sao memoria deste processo */
};
#define PIPE_MAX 256
struct pinst {
  int used;
  wchar16 name[128];
  DWORD omode, pmode;
  int connected;           /* ha cliente ligado */
  int open[2];             /* handles abertos em cada lado (0 servidor, 1 cliente) */
  int ever_client;         /* ja houve cliente (para ERROR_PIPE_CONNECTED) */
  DWORD cflags;            /* flags do CreateFile do cliente (FILE_FLAG_OVERLAPPED) */
  int closing;             /* o cliente fechou: a instancia so aceita outro
                            * depois do DisconnectNamedPipe (como no Windows) */
  DWORD pid[2];
  struct chunk *q[2], *qt[2]; /* q[x] = o que o lado x vai ler */
  int msg_read[2];
  struct pend rd[2];
  struct pend conn;        /* ConnectNamedPipe pendente */
  struct wobj* obj[2];     /* objeto do handle de cada lado (para a IOCP) */
};
static struct pinst* pipes[PIPE_MAX];

/* conclusoes a entregar depois de soltar a trava */
struct done_ev {
  struct wobj* o;
  struct overlapped* ov;
  uint32_t status;
  DWORD bytes;
};
#define MAX_DONE 8
struct done_list {
  struct done_ev e[MAX_DONE];
  int n;
};
static void defer(struct done_list* d, struct wobj* o, struct overlapped* ov, uint32_t st, DWORD n) {
  if (d->n < MAX_DONE) {
    d->e[d->n].o = o;
    d->e[d->n].ov = ov;
    d->e[d->n].status = st;
    d->e[d->n].bytes = n;
    d->n++;
  }
  /* o OVERLAPPED ja fica com o resultado (quem olha o Internal ve agora) */
  ov->internal_high = n;
  ov->internal = st;
}
static void deliver(struct done_list* d) {
  int i;
  for (i = 0; i < d->n; i++) {
    struct done_ev* e = &d->e[i];
    /* hEvent com o bit 0 ligado: "nao poste na porta de conclusao" (Windows).
     * O libuv le o pipe assim e cancela quando nao ha dado; a conclusao
     * cancelada caia na IOCP dele, que tomava por "ha dados" e lia de novo,
     * sem parar: a thread principal do Claude Desktop girava para sempre
     * lendo o stdout do Claude Code. */
    uintptr_t evh = (uintptr_t)e->ov->event;
    int nopost = (evh & 1) != 0;
    if (e->o && e->o->p1 && !nopost) wiocp_post((struct wobj*)e->o->p1, e->bytes, (uintptr_t)e->o->u1, e->ov);
    if (wine_debug >= 2 && e->o && !nopost) { /* para onde foi a conclusao */
      wlogx(e->o->p1 ? "pipe: conclusao na porta " : "pipe: conclusao SEM porta, objeto ", e->o->p1 ? (uint64_t)(uintptr_t)e->o->p1 : (uint64_t)(uintptr_t)e->o);
    }
    if (evh & ~(uintptr_t)1) {
      typedef BOOL(WINAPI * se_fn)(HANDLE);
      static se_fn se;
      if (!se) se = (se_fn)builtin_lookup("SetEvent");
      if (se) se((HANDLE)(evh & ~(uintptr_t)1));
    }
  }
  if (d->n) wsignal_global(); /* portas e eventos ja foram sinalizados acima; aqui so as esperas sincronas */
}

static int name_eq(const wchar16* a, const wchar16* b) {
  for (;; a++, b++) {
    wchar16 x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x += 32;
    if (y >= 'A' && y <= 'Z') y += 32;
    if (x != y) return 0;
    if (!x) return 1;
  }
}
/* "\\.\pipe\x", "\\?\pipe\x" (o libuv do Node usa este) ou so "x" -> "x" */
static const wchar16* pipe_tail(const wchar16* n) {
  static const char pre[] = "\\\\.\\pipe\\";
  int i;
  for (i = 0; pre[i]; i++) {
    wchar16 c = n[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    if (i == 2 && c == '?') continue;
    if (c != (wchar16)pre[i]) return n;
  }
  return n + i;
}

static void free_queue(struct pinst* pi, int x) {
  struct chunk* c = pi->q[x];
  while (c) {
    struct chunk* n = c->next;
    k_free_shared(c);
    c = n;
  }
  pi->q[x] = pi->qt[x] = 0;
}
static void inst_release_if_done(struct pinst* pi, int slot) {
  if (pi->open[0] || pi->open[1]) return;
  free_queue(pi, 0);
  free_queue(pi, 1);
  pi->used = 0;
  pipes[slot] = 0;
  k_free_shared(pi);
}
static int slot_of(struct pinst* pi) {
  int i;
  for (i = 0; i < PIPE_MAX; i++)
    if (pipes[i] == pi) return i;
  return -1;
}
static DWORD queued(struct pinst* pi, int x) {
  struct chunk* c;
  DWORD n = 0;
  for (c = pi->q[x]; c; c = c->next) n += c->len - c->off;
  return n;
}

/* Le da fila x para buf. Devolve o status (0 ou ST_MORE) e os bytes. */
static uint32_t take(struct pinst* pi, int x, uint8_t* buf, DWORD len, DWORD* got) {
  DWORD n = 0;
  *got = 0;
  if (pi->msg_read[x] || (pi->pmode & PIPE_TYPE_MESSAGE && pi->msg_read[x])) {
    struct chunk* c = pi->q[x];
    DWORD left = c->len - c->off, k = left < len ? left : len;
    memcpy(buf, c->data + c->off, k);
    c->off += k;
    *got = k;
    if (c->off == c->len) {
      pi->q[x] = c->next;
      if (!pi->q[x]) pi->qt[x] = 0;
      k_free_shared(c);
      return 0;
    }
    return ST_MORE; /* o resto da mensagem fica para a proxima leitura */
  }
  while (n < len && pi->q[x]) {
    struct chunk* c = pi->q[x];
    DWORD left = c->len - c->off, k = left < len - n ? left : len - n;
    memcpy(buf + n, c->data + c->off, k);
    c->off += k;
    n += k;
    if (c->off == c->len) {
      pi->q[x] = c->next;
      if (!pi->q[x]) pi->qt[x] = 0;
      k_free_shared(c);
    }
  }
  *got = n;
  return 0;
}

/* Leitura pendente do lado x pode andar? (com a trava) */
static void plog(const char* what, struct pinst* pi, uint64_t v);
static void serve_read(struct pinst* pi, int x, struct done_list* d) {
  struct pend* r = &pi->rd[x];
  DWORD got;
  uint32_t st;
  if (!r->used) return;
  if (pi->q[x]) {
    st = take(pi, x, r->buf, r->len, &got);
    r->used = 0;
    plog(x ? "entregue (cliente)" : "entregue (servidor)", pi, got);
    defer(d, r->sync ? 0 : pi->obj[x], r->ov, st, got);
  } else if (!pi->open[x ^ 1] && (x == 0 ? pi->ever_client : 1)) {
    r->used = 0;
    defer(d, r->sync ? 0 : pi->obj[x], r->ov, ST_BROKEN, 0);
  }
}

static HANDLE end_handle(struct pinst* pi, int end) {
  struct wobj* o = wobj_new(OBJ_PIPE);
  HANDLE h;
  if (!o) return 0;
  o->p2 = pi;
  o->u2 = (uint64_t)end;
  h = wh_new(o);
  if (!h) {
    k_free(o);
    return 0;
  }
  pi->obj[end] = o;
  return h;
}

static WINAPI HANDLE CreateNamedPipeW(const wchar16* name, DWORD omode, DWORD pmode, DWORD maxi, DWORD outsz,
                                      DWORD insz, DWORD timeout, void* sa) {
  struct pinst* pi;
  HANDLE h;
  int i, slot = -1;
  (void)maxi; (void)outsz; (void)insz; (void)timeout; (void)sa;
  if (!name) {
    wset_lasterr(ERROR_INVALID_PARAMETER);
    return INVALID_HANDLE_VALUE;
  }
  pi = (struct pinst*)k_malloc_shared(sizeof(struct pinst));
  if (!pi) {
    wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
    return INVALID_HANDLE_VALUE;
  }
  memset(pi, 0, sizeof(*pi));
  {
    const wchar16* t = pipe_tail(name);
    for (i = 0; t[i] && i < 127; i++) pi->name[i] = t[i];
  }
  pi->omode = omode;
  pi->pmode = pmode;
  pi->msg_read[0] = (pmode & PIPE_READMODE_MESSAGE) != 0;
  pi->open[0] = 1;
  pi->pid[0] = wp()->pid;
  pi->used = 1;
  wlock();
  /* FILE_FLAG_FIRST_PIPE_INSTANCE: recusa se o nome ja existe. O libuv
   * (stdio dos filhos do Node) cria "uv\<n>-<pid>" assim e troca o <n> ate
   * passar; sem a recusa todos os pipes tinham o mesmo nome e o cliente
   * caia numa instancia velha: ConnectNamedPipe esperava para sempre e o
   * Claude travava ao abrir a aba Code. */
  if (omode & FILE_FLAG_FIRST_PIPE_INSTANCE)
    for (i = 0; i < PIPE_MAX; i++)
      if (pipes[i] && pipes[i]->used && name_eq(pipes[i]->name, pi->name)) {
        slot = -2;
        break;
      }
  if (slot == -1)
    for (i = 0; i < PIPE_MAX; i++)
      if (!pipes[i]) {
        slot = i;
        pipes[i] = pi;
        break;
      }
  wunlock();
  if (slot < 0) {
    k_free_shared(pi);
    wset_lasterr(slot == -2 ? ERROR_ACCESS_DENIED : ERROR_PIPE_BUSY);
    return INVALID_HANDLE_VALUE;
  }
  h = end_handle(pi, 0);
  if (!h) return INVALID_HANDLE_VALUE;
  if (wine_debug) {
    char* n8 = wdup16to8(pi->name);
    wlog("pipe criado: ", n8);
    k_free(n8);
  }
  wset_lasterr(0);
  return h;
}
static WINAPI HANDLE CreateNamedPipeA(const char* name, DWORD omode, DWORD pmode, DWORD maxi, DWORD outsz,
                                      DWORD insz, DWORD timeout, void* sa) {
  wchar16* w = name ? wdup8to16(name) : 0;
  HANDLE h = CreateNamedPipeW(w, omode, pmode, maxi, outsz, insz, timeout, sa);
  k_free(w);
  return h;
}

/* WINEDEBUG=2: uma linha por operacao de pipe ("pipe ler crashpad_256_X 0x10") */
static void plog(const char* what, struct pinst* pi, uint64_t v) {
  char line[200];
  int n = 0, i;
  static const char hx[] = "0123456789abcdef";
  if (wine_debug < 2 || !pi) return;
  while (*what && n < 40) line[n++] = *what++;
  line[n++] = ' ';
  for (i = 0; pi->name[i] && n < 150; i++) line[n++] = (char)pi->name[i];
  line[n++] = ' ';
  line[n++] = '0';
  line[n++] = 'x';
  for (i = 28; i >= 0; i -= 4) line[n++] = hx[(v >> i) & 15];
  line[n] = 0;
  wlog("pipe ", line);
}

/* Cliente: CreateFile("\\.\pipe\nome") (file.c chama). */
HANDLE wpipe_open(const wchar16* name, DWORD flags) {
  const wchar16* t = pipe_tail(name);
  struct done_list d;
  struct pinst* pi = 0;
  int i, any = 0;
  HANDLE h;
  d.n = 0;
  wlock();
  for (i = 0; i < PIPE_MAX; i++) {
    struct pinst* c = pipes[i];
    if (!c || !c->used || !name_eq(c->name, t)) continue;
    any = 1;
    if (!c->connected && !c->closing && c->open[0] && !c->open[1]) {
      pi = c;
      break;
    }
  }
  if (pi) {
    pi->connected = 1;
    pi->ever_client = 1;
    pi->open[1] = 1;
    pi->pid[1] = wp()->pid;
    pi->cflags = flags;
    pi->msg_read[1] = 0;
    if (pi->conn.used) {
      pi->conn.used = 0;
      defer(&d, pi->obj[0], pi->conn.ov, 0, 0);
    }
  }
  wunlock();
  if (!pi) {
    if (wine_debug >= 2) wlog("pipe abrir: ", any ? "ocupado" : "nao existe");
    wset_lasterr(any ? ERROR_PIPE_BUSY : ERROR_FILE_NOT_FOUND);
    return INVALID_HANDLE_VALUE;
  }
  plog("abrir (cliente)", pi, 0);
  h = end_handle(pi, 1);
  deliver(&d);
  if (!h) return INVALID_HANDLE_VALUE;
  wset_lasterr(0);
  return h;
}

static struct pinst* pipe_of(HANDLE h, int* end) {
  struct wobj* o = wh_get(h, OBJ_PIPE);
  if (!o) return 0;
  if (end) *end = (int)o->u2;
  return (struct pinst*)o->p2;
}

/* NtQueryInformationFile num pipe (nt.c): -1 = nao e pipe; senao o Mode
 * (0 = sobreposto, 0x20 = FILE_SYNCHRONOUS_IO_NONALERT). O libuv (Node)
 * pergunta isso no stdio herdado; sem resposta dava "open EINVAL". */
int wpipe_mode(HANDLE h) {
  int end;
  struct pinst* pi = pipe_of(h, &end);
  DWORD fl;
  if (!pi) return -1;
  fl = end == 0 ? pi->omode : pi->cflags;
  return (fl & 0x40000000u) ? 0 : 0x20;
}
/* GetFileInformationByHandleEx num pipe (file.c): -1 = nao e pipe. O
 * crashpad-handler pede FileNameInfo no pipe herdado para saber o nome; o
 * Windows responde "\\nome", relativo a \\.\pipe. */
int wpipe_fileinfo(HANDLE h, int cls, void* buf, DWORD len) {
  struct pinst* pi = pipe_of(h, 0);
  DWORD n, i;
  if (!pi) return -1;
  memset(buf, 0, len);
  switch (cls) {
    case 1: /* FileStandardInfo */
      if (len < 24) break;
      ((DWORD*)buf)[4] = 1;
      return 1;
    case 2: /* FileNameInfo: DWORD tamanho em bytes + WCHAR[] */
      if (len < 6) break;
      for (n = 0; pi->name[n]; n++) {
      }
      ((DWORD*)buf)[0] = (n + 1) * 2;
      {
        wchar16* d = (wchar16*)((uint8_t*)buf + 4);
        DWORD room = (len - 4) / 2;
        if (room > 0) d[0] = '\\';
        for (i = 0; i < n && i + 1 < room; i++) d[i + 1] = pi->name[i];
        if (n + 1 > room) {
          wset_lasterr(ERROR_MORE_DATA);
          return 0;
        }
      }
      return 1;
  }
  wset_lasterr(ERROR_INVALID_PARAMETER);
  return 0;
}

static WINAPI BOOL ConnectNamedPipe(HANDLE h, struct overlapped* ov) {
  int end;
  struct pinst* pi = pipe_of(h, &end);
  if (!pi || end != 0) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  wlock();
  if (pi->connected) {
    wunlock();
    plog("conectar: ja ligado", pi, (uint64_t)(uintptr_t)ov);
    wset_lasterr(ERROR_PIPE_CONNECTED);
    return 0;
  }
  if (pi->closing) { /* o cliente ja fechou: falta o DisconnectNamedPipe */
    wunlock();
    plog("conectar: cliente fechou", pi, 0);
    wset_lasterr(ERROR_NO_DATA);
    return 0;
  }
  plog(ov ? "conectar: pendente" : "conectar: esperando", pi, 0);
  if (ov && wp()->exiting) {
    wunlock();
    wset_lasterr(995);
    return 0;
  }
  if (ov) {
    ov->internal = ST_PENDING;
    ov->internal_high = 0;
    pi->conn.ov = ov;
    pi->conn.used = 1;
    pi->conn.owner = wp();
    wunlock();
    wset_lasterr(ERROR_IO_PENDING);
    return 0;
  }
  wunlock();
  {
    uint64_t t0 = wnow_ms();
    int noted = 0;
    wwait_begin_w("ConnectNamedPipe (espera cliente)", pi, pi->name, __builtin_return_address(0));
    for (;;) { /* sincrono: espera o cliente */
      int g = wine_gen;
      if (pi->connected) {
        wwait_end();
        return 1;
      }
      if (wine_debug >= 2 && !noted && wnow_ms() - t0 > 5000) {
        noted = 1;
        wwait_note("ConnectNamedPipe", __builtin_return_address(0), wnow_ms() - t0);
      }
      k_futex_wait(&wine_gen, g, 50);
    }
  }
}
static WINAPI BOOL DisconnectNamedPipe(HANDLE h) {
  int end;
  struct pinst* pi = pipe_of(h, &end);
  struct done_list d;
  if (!pi || end != 0) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  d.n = 0;
  wlock();
  pi->connected = 0;
  pi->closing = 0;
  pi->open[1] = 0; /* o handle do cliente fica quebrado */
  free_queue(pi, 0);
  free_queue(pi, 1);
  if (pi->rd[1].used) {
    pi->rd[1].used = 0;
    defer(&d, pi->rd[1].sync ? 0 : pi->obj[1], pi->rd[1].ov, ST_BROKEN, 0);
  }
  wunlock();
  deliver(&d);
  return 1;
}
static WINAPI BOOL WaitNamedPipeW(const wchar16* name, DWORD ms) {
  const wchar16* t = pipe_tail(name);
  uint64_t t0 = wnow_ms();
  for (;;) {
    int i, any = 0, free_ = 0, g = wine_gen;
    wlock();
    for (i = 0; i < PIPE_MAX; i++) {
      struct pinst* c = pipes[i];
      if (!c || !c->used || !name_eq(c->name, t)) continue;
      any = 1;
      if (!c->connected && !c->closing && c->open[0]) free_ = 1;
    }
    wunlock();
    if (free_) return 1;
    if (!any) {
      wset_lasterr(ERROR_FILE_NOT_FOUND);
      return 0;
    }
    if (ms != 0xFFFFFFFFu && wnow_ms() - t0 >= (ms ? ms : 50)) {
      wset_lasterr(ERROR_SEM_TIMEOUT);
      return 0;
    }
    k_futex_wait(&wine_gen, g, 50);
  }
}
static WINAPI BOOL WaitNamedPipeA(const char* name, DWORD ms) {
  wchar16* w = wdup8to16(name);
  BOOL r = WaitNamedPipeW(w, ms);
  k_free(w);
  return r;
}

/* ReadFile/WriteFile de file.c chegam aqui para handles de pipe */
int wpipe_read(struct wobj* o, void* buf, DWORD len, DWORD* done, void* ovp) {
  struct overlapped* ov = (struct overlapped*)ovp;
  struct pinst* pi = (struct pinst*)o->p2;
  int x = (int)o->u2;
  struct done_list d;
  DWORD got;
  uint32_t st;
  uint64_t t0 = wnow_ms();
  int noted = 0;
  if (done) *done = 0;
  d.n = 0;
  for (;;) {
    int g = wine_gen;
    wlock();
    if (pi->q[x]) {
      st = take(pi, x, (uint8_t*)buf, len, &got);
      wunlock();
      plog(x ? "ler (cliente)" : "ler (servidor)", pi, got);
      if (done) *done = got;
      if (ov) {
        /* concluido na hora: vai para a IOCP, a nao ser com
         * FILE_SKIP_COMPLETION_PORT_ON_SUCCESS (o resultado fica no OVERLAPPED) */
        defer(&d, o->skip_ok ? 0 : o, ov, st, got);
        deliver(&d);
      }
      if (st == ST_MORE) {
        wset_lasterr(ERROR_MORE_DATA);
        return 0;
      }
      return 1;
    }
    if (!pi->open[x ^ 1] && (x == 1 || pi->ever_client)) {
      wunlock();
      wset_lasterr(ERROR_BROKEN_PIPE);
      if (ov) {
        ov->internal = ST_BROKEN;
        ov->internal_high = 0;
      }
      return 0;
    }
    if (x == 0 && !pi->connected && !pi->ever_client) {
      wunlock();
      wset_lasterr(ERROR_PIPE_LISTENING);
      return 0;
    }
    if (!ov && !pi->rd[x].used && !wp()->exiting) {
      /* sincrono: fica registrado como leitura pendente, para a escrita do
       * outro lado cair direto no buffer. Sem isso o servidor do crashpad
       * escrevia a resposta e logo dava DisconnectNamedPipe, que descartava
       * a resposta antes de o cliente (TransactNamedPipe) a pegar. */
      struct overlapped sov;
      memset(&sov, 0, sizeof(sov));
      sov.internal = ST_PENDING;
      pi->rd[x].ov = &sov;
      pi->rd[x].buf = (uint8_t*)buf;
      pi->rd[x].len = len;
      pi->rd[x].sync = 1;
      pi->rd[x].used = 1;
      pi->rd[x].owner = wp();
      wunlock();
      wwait_begin_w(x ? "ReadFile pipe (cliente)" : "ReadFile pipe (servidor)", pi, pi->name, __builtin_return_address(0));
      for (;;) {
        int g2 = wine_gen;
        if (__atomic_load_n(&sov.internal, __ATOMIC_ACQUIRE) != ST_PENDING) break;
        if (wine_debug >= 2 && !noted && wnow_ms() - t0 > 5000) {
          noted = 1;
          plog(x ? "ler esperando (cliente)" : "ler esperando (servidor)", pi, len);
        }
        k_futex_wait(&wine_gen, g2, 100);
      }
      wwait_end();
      if (done) *done = (DWORD)sov.internal_high;
      plog(x ? "ler (cliente)" : "ler (servidor)", pi, sov.internal_high);
      if (sov.internal == ST_MORE) {
        wset_lasterr(ERROR_MORE_DATA);
        return 0;
      }
      if (sov.internal == ST_BROKEN) {
        wset_lasterr(ERROR_BROKEN_PIPE);
        return 0;
      }
      if (sov.internal == ST_CANCEL) {
        wset_lasterr(995); /* ERROR_OPERATION_ABORTED */
        return 0;
      }
      return 1;
    }
    if (ov && wp()->exiting) { /* processo saindo: nada fica pendente */
      wunlock();
      wset_lasterr(995); /* ERROR_OPERATION_ABORTED */
      return 0;
    }
    if (ov) {
      pi->rd[x].ov = ov;
      pi->rd[x].buf = (uint8_t*)buf;
      pi->rd[x].len = len;
      pi->rd[x].sync = 0;
      pi->rd[x].used = 1;
      pi->rd[x].owner = wp();
      ov->internal = ST_PENDING;
      ov->internal_high = 0;
      wunlock();
      plog(x ? "ler pendente (cliente)" : "ler pendente (servidor)", pi, len);
      wset_lasterr(ERROR_IO_PENDING);
      return 0;
    }
    wunlock();
    if (wine_debug >= 2 && !noted && wnow_ms() - t0 > 5000) {
      noted = 1;
      plog(x ? "ler esperando (cliente)" : "ler esperando (servidor)", pi, len);
    }
    k_futex_wait(&wine_gen, g, 100);
  }
}

int wpipe_write(struct wobj* o, const void* buf, DWORD len, DWORD* done, void* ovp) {
  struct overlapped* ov = (struct overlapped*)ovp;
  struct pinst* pi = (struct pinst*)o->p2;
  int x = (int)o->u2, y = x ^ 1;
  struct done_list d;
  struct chunk* c;
  if (done) *done = 0;
  d.n = 0;
  wlock();
  plog(x ? "escrever (cliente)" : "escrever (servidor)", pi, len);
  if (!pi->open[y] || (x == 0 && !pi->connected)) {
    wunlock();
    plog("escrever: sem o outro lado", pi, (uint64_t)pi->open[y]);
    wset_lasterr(x == 0 && !pi->ever_client ? ERROR_PIPE_LISTENING : ERROR_NO_DATA);
    if (ov) {
      ov->internal = ST_BROKEN;
      ov->internal_high = 0;
    }
    return 0;
  }
  if (len) {
    c = (struct chunk*)k_malloc_shared((uint32_t)(sizeof(struct chunk) + len));
    if (!c) {
      wunlock();
      wset_lasterr(ERROR_NOT_ENOUGH_MEMORY);
      return 0;
    }
    c->next = 0;
    c->len = len;
    c->off = 0;
    memcpy(c->data, buf, len);
    if (pi->qt[y]) pi->qt[y]->next = c;
    else pi->q[y] = c;
    pi->qt[y] = c;
    serve_read(pi, y, &d);
  }
  wunlock();
  if (done) *done = len;
  if (ov) defer(&d, o->skip_ok ? 0 : o, ov, 0, len); /* idem */
  deliver(&d);
  return 1;
}

/* FlushFileBuffers num pipe: espera o outro lado ler tudo (ou fechar). O
 * crashpad responde, chama FlushFileBuffers e depois DisconnectNamedPipe, que
 * descarta o que ainda nao foi lido. */
int wpipe_flush(HANDLE h) {
  struct wobj* o = wh_get(h, OBJ_PIPE);
  struct pinst* pi;
  int y;
  if (!o) return -1;
  pi = (struct pinst*)o->p2;
  y = (int)o->u2 ^ 1;
  for (;;) {
    int g = wine_gen, empty;
    wlock();
    empty = !pi->q[y] || !pi->open[y];
    wunlock();
    if (empty) return 1;
    k_futex_wait(&wine_gen, g, 50);
  }
}

/* main.c (wexit): o processo sai; leituras e esperas de conexao dele ficam
 * sem dono (buffer e OVERLAPPED vao embora com a memoria do processo). Sem
 * isso a escrita do outro lado copiava para a memoria de um processo morto
 * (o renderer saia e o navegador caia no memcpy). */
void wpipe_proc_exit(void) {
  struct wproc* me = wp();
  /* (as outras threads do processo seguem ate o kernel parar: o
   * wpipe_read/ConnectNamedPipe recusam pendencia nova com p->exiting) */
  int i, x;
  wlock();
  for (i = 0; i < PIPE_MAX; i++) {
    struct pinst* pi = pipes[i];
    if (!pi || !pi->used) continue;
    for (x = 0; x < 2; x++)
      if (pi->rd[x].used && pi->rd[x].owner == me) pi->rd[x].used = 0;
    if (pi->conn.used && pi->conn.owner == me) pi->conn.used = 0;
  }
  wunlock();
}

/* CloseHandle do ultimo handle de um lado (wh_close) */
void wpipe_close(struct wobj* o) {
  struct pinst* pi = (struct pinst*)o->p2;
  int x = (int)o->u2, y = x ^ 1, slot;
  struct done_list d;
  if (!pi) return;
  plog(x ? "fechar (cliente)" : "fechar (servidor)", pi, 0);
  d.n = 0;
  wlock();
  if (pi->obj[x] == o) pi->obj[x] = 0;
  pi->open[x] = 0;
  if (x == 1) {
    if (pi->connected) pi->closing = 1;
    pi->connected = 0;
  }
  if (pi->rd[x].used) { /* o proprio lado fechou */
    pi->rd[x].used = 0;
    if (pi->rd[x].sync) defer(&d, 0, pi->rd[x].ov, ST_BROKEN, 0); /* quem espera acorda */
  }
  if (pi->conn.used && x == 0) pi->conn.used = 0;
  serve_read(pi, y, &d);                    /* o outro lado: ERROR_BROKEN_PIPE */
  slot = slot_of(pi);
  if (slot >= 0) inst_release_if_done(pi, slot);
  wunlock();
  deliver(&d);
}

int wpipe_cancel(struct wobj* o, void* ovp) {
  struct overlapped* ov = (struct overlapped*)ovp;
  struct pinst* pi = (struct pinst*)o->p2;
  int x = (int)o->u2, n = 0;
  struct done_list d;
  d.n = 0;
  wlock();
  if (pi->rd[x].used && (!ov || pi->rd[x].ov == ov)) {
    pi->rd[x].used = 0;
    defer(&d, pi->rd[x].sync ? 0 : o, pi->rd[x].ov, ST_CANCEL, 0);
    n++;
  }
  if (x == 0 && pi->conn.used && (!ov || pi->conn.ov == ov)) {
    pi->conn.used = 0;
    defer(&d, o, pi->conn.ov, ST_CANCEL, 0);
    n++;
  }
  wunlock();
  deliver(&d);
  return n;
}

static WINAPI BOOL PeekNamedPipe(HANDLE h, void* buf, DWORD size, DWORD* rd, DWORD* avail, DWORD* left) {
  int x;
  struct pinst* pi = pipe_of(h, &x);
  DWORD n, k = 0;
  if (!pi) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  plog("peek", pi, size);
  wlock();
  n = queued(pi, x);
  if (buf && size && pi->q[x]) {
    struct chunk* c = pi->q[x];
    DWORD left2 = c->len - c->off;
    k = left2 < size ? left2 : size;
    memcpy(buf, c->data + c->off, k);
  }
  if (rd) *rd = k;
  if (avail) *avail = n;
  if (left) *left = pi->q[x] ? pi->q[x]->len - pi->q[x]->off - k : 0;
  if (!pi->q[x] && !pi->open[x ^ 1] && (x == 1 || pi->ever_client)) {
    wunlock();
    wset_lasterr(ERROR_BROKEN_PIPE);
    return 0;
  }
  wunlock();
  return 1;
}
static WINAPI BOOL SetNamedPipeHandleState(HANDLE h, DWORD* mode, DWORD* maxc, DWORD* timeout) {
  int x;
  struct pinst* pi = pipe_of(h, &x);
  (void)maxc;
  (void)timeout;
  if (!pi) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  if (mode) pi->msg_read[x] = (*mode & PIPE_READMODE_MESSAGE) != 0;
  return 1;
}
static WINAPI BOOL GetNamedPipeHandleStateW(HANDLE h, DWORD* state, DWORD* inst, DWORD* maxc, DWORD* timeout,
                                            wchar16* user, DWORD usize) {
  int x;
  struct pinst* pi = pipe_of(h, &x);
  if (!pi) return 0;
  if (state) *state = pi->msg_read[x] ? PIPE_READMODE_MESSAGE : 0;
  if (inst) *inst = 1;
  if (maxc) *maxc = 0;
  if (timeout) *timeout = 0;
  if (user && usize) user[0] = 0;
  return 1;
}
static WINAPI BOOL GetNamedPipeInfo(HANDLE h, DWORD* flags, DWORD* outsz, DWORD* insz, DWORD* maxi) {
  int x;
  struct pinst* pi = pipe_of(h, &x);
  if (!pi) return 0;
  if (flags) *flags = (x ? 0 : 1) | (pi->pmode & PIPE_TYPE_MESSAGE); /* PIPE_SERVER_END = 1 */
  if (outsz) *outsz = 65536;
  if (insz) *insz = 65536;
  if (maxi) *maxi = 255;
  return 1;
}
static WINAPI BOOL GetNamedPipeClientProcessId(HANDLE h, DWORD* pid) {
  struct pinst* pi = pipe_of(h, 0);
  if (!pi || !pid) return 0;
  *pid = pi->pid[1];
  return 1;
}
static WINAPI BOOL GetNamedPipeServerProcessId(HANDLE h, DWORD* pid) {
  struct pinst* pi = pipe_of(h, 0);
  if (!pi || !pid) return 0;
  *pid = pi->pid[0];
  return 1;
}
static WINAPI BOOL TransactNamedPipe(HANDLE h, void* in, DWORD nin, void* out, DWORD nout, DWORD* got,
                                     struct overlapped* ov) {
  struct wobj* o = wh_get(h, OBJ_PIPE);
  DWORD w;
  if (!o) {
    wset_lasterr(ERROR_INVALID_HANDLE);
    return 0;
  }
  if (ov) {
    if (!wpipe_write(o, in, nin, &w, 0)) return 0;
    return wpipe_read(o, out, nout, got, ov);
  }
  /* sincrono: a leitura fica registrada ANTES de escrever (no Windows a
   * operacao e uma so). Senao o servidor (crashpad) responde e da
   * DisconnectNamedPipe antes de o cliente pedir a leitura: ERROR_BROKEN_PIPE. */
  {
    struct pinst* pi = (struct pinst*)o->p2;
    int x = (int)o->u2, armed = 0;
    struct overlapped sov;
    memset(&sov, 0, sizeof(sov));
    sov.internal = ST_PENDING;
    if (got) *got = 0;
    wlock();
    if (!pi->q[x] && !pi->rd[x].used && pi->open[x ^ 1] && !wp()->exiting) {
      pi->rd[x].ov = &sov;
      pi->rd[x].buf = (uint8_t*)out;
      pi->rd[x].len = nout;
      pi->rd[x].sync = 1;
      pi->rd[x].used = 1;
      pi->rd[x].owner = wp();
      armed = 1;
    }
    wunlock();
    if (!wpipe_write(o, in, nin, &w, 0)) {
      if (armed) {
        wlock();
        if (pi->rd[x].used && pi->rd[x].ov == &sov) pi->rd[x].used = 0;
        wunlock();
      }
      return 0;
    }
    if (!armed) return wpipe_read(o, out, nout, got, 0);
    for (;;) {
      int g = wine_gen;
      if (__atomic_load_n(&sov.internal, __ATOMIC_ACQUIRE) != ST_PENDING) break;
      k_futex_wait(&wine_gen, g, 100);
    }
    if (got) *got = (DWORD)sov.internal_high;
    plog("transact: resposta", pi, sov.internal_high);
    if (sov.internal == ST_MORE) {
      wset_lasterr(ERROR_MORE_DATA);
      return 0;
    }
    if (sov.internal == ST_BROKEN || sov.internal == ST_CANCEL) {
      wset_lasterr(sov.internal == ST_BROKEN ? ERROR_BROKEN_PIPE : 995u);
      return 0;
    }
    return 1;
  }
}
static WINAPI BOOL ImpersonateNamedPipeClient(HANDLE h) {
  (void)h;
  return 1; /* um usuario so */
}
static WINAPI BOOL CreatePipe(HANDLE* rd, HANDLE* wr, void* sa, DWORD size) {
  /* pipe anonimo = um pipe com nome unico */
  static volatile int seq;
  wchar16 name[40];
  const char* pre = "anon-";
  int i, n, v = __sync_add_and_fetch(&seq, 1);
  HANDLE s, c;
  char num[16];
  (void)sa;
  (void)size;
  for (i = 0; pre[i]; i++) name[i] = (wchar16)pre[i];
  n = 0;
  do {
    num[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n) name[i++] = (wchar16)num[--n];
  name[i++] = '-';
  v = (int)wp()->pid;
  n = 0;
  do {
    num[n++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  while (n) name[i++] = (wchar16)num[--n];
  name[i] = 0;
  s = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND, 0, 1, 0, 0, 0, 0);
  if (s == INVALID_HANDLE_VALUE) return 0;
  c = wpipe_open(name, 0);
  if (c == INVALID_HANDLE_VALUE) return 0;
  *rd = s;
  *wr = c;
  return 1;
}

/* GetOverlappedResult de qualquer handle; pipes com os codigos de pipe */
static WINAPI BOOL GetOverlappedResultPipe(HANDLE h, struct overlapped* ov, DWORD* bytes, BOOL wait) {
  typedef BOOL(WINAPI * gor)(uintptr_t, struct overlapped*, DWORD*, BOOL, DWORD*);
  struct wobj* o = wh_get(h, 0);
  if (wait)
    while (ov->internal == ST_PENDING) {
      int g = wine_gen;
      if (ov->internal != ST_PENDING) break;
      k_futex_wait(&wine_gen, g, 50);
    }
  if (!o || o->type != OBJ_PIPE) {
    gor f = (gor)builtin_lookup("WSAGetOverlappedResult");
    return f ? f((uintptr_t)h, ov, bytes, 0, 0) : 0;
  }
  if (ov->internal == ST_PENDING) {
    wset_lasterr(996); /* ERROR_IO_INCOMPLETE */
    return 0;
  }
  if (bytes) *bytes = (DWORD)ov->internal_high;
  switch ((uint32_t)ov->internal) {
    case 0: return 1;
    case ST_MORE: wset_lasterr(ERROR_MORE_DATA); return 0;
    case ST_CANCEL: wset_lasterr(995); return 0;
    case ST_BROKEN: wset_lasterr(ERROR_BROKEN_PIPE); return 0;
  }
  wset_lasterr(ERROR_BROKEN_PIPE);
  return 0;
}
static WINAPI BOOL GetOverlappedResultEx(HANDLE h, struct overlapped* ov, DWORD* bytes, DWORD ms, BOOL alert) {
  uint64_t t0 = wnow_ms();
  (void)alert;
  while (ov->internal == ST_PENDING && ms) {
    int g = wine_gen;
    if (ms != 0xFFFFFFFFu && wnow_ms() - t0 >= ms) {
      wset_lasterr(258); /* WAIT_TIMEOUT */
      return 0;
    }
    k_futex_wait(&wine_gen, g, 50);
  }
  return GetOverlappedResultPipe(h, ov, bytes, 0);
}

#define S(n) {#n, (void*)n}
const struct wine_sym pipe_syms[] = {
    S(CreateNamedPipeW), S(CreateNamedPipeA), S(ConnectNamedPipe), S(DisconnectNamedPipe), S(WaitNamedPipeW),
    S(WaitNamedPipeA), S(PeekNamedPipe), S(SetNamedPipeHandleState), S(GetNamedPipeHandleStateW),
    S(GetNamedPipeInfo), S(GetNamedPipeClientProcessId), S(GetNamedPipeServerProcessId), S(TransactNamedPipe),
    S(ImpersonateNamedPipeClient), S(CreatePipe), {"GetOverlappedResult", (void*)GetOverlappedResultPipe},
    S(GetOverlappedResultEx), {0, 0}};
