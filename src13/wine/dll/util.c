/* util.c - Utilidades do WINE.DLL: console, nomes, UTF-16, stubs. */
#include <so.h>
#include <string.h>
#include "wine_int.h"

/* Uma linha numa chamada so (com varios processos as linhas se misturavam);
 * fora do processo de cima o pid entra no prefixo: "[wine 260] ". O TEB so
 * vale depois do k_win_teb: confere o ponteiro para si mesmo (NT_TIB.Self). */
void wlog(const char* a, const char* b) {
  char line[2048];
  int n = 0, k;
  uint8_t* teb = wteb();
  const char* pre = "[wine";
  while (*pre) line[n++] = *pre++;
  if (teb && *(uint8_t**)(teb + 0x30) == teb) {
    unsigned v = (unsigned)*(uint64_t*)(teb + TEB_PID);
    if (v && v != 256) {
      char t[12];
      k = 0;
      do {
        t[k++] = (char)('0' + v % 10);
        v /= 10;
      } while (v);
      line[n++] = ' ';
      while (k) line[n++] = t[--k];
    }
    if (wine_debug >= 2) { /* thread: " t144" (hexa, como nas esperas longas) */
      unsigned tid = (unsigned)*(uint64_t*)(teb + TEB_TID);
      static const char hx[] = "0123456789abcdef";
      int sh, started = 0;
      line[n++] = ' ';
      line[n++] = 't';
      for (sh = 28; sh >= 0; sh -= 4) {
        int d = (int)((tid >> sh) & 15);
        if (d || started || sh == 0) {
          line[n++] = hx[d];
          started = 1;
        }
      }
    }
  }
  line[n++] = ']';
  line[n++] = ' ';
  for (k = 0; a && a[k] && n < (int)sizeof(line) - 2; k++) line[n++] = a[k];
  for (k = 0; b && b[k] && n < (int)sizeof(line) - 2; k++) line[n++] = b[k];
  line[n++] = '\n';
  k_dbg(line, n);
}

void wlogx(const char* a, uint64_t v) {
  char h[19];
  int i;
  h[0] = '0';
  h[1] = 'x';
  for (i = 0; i < 16; i++) h[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
  h[18] = 0;
  wlog(a, h);
}

void wsay(const char* a, const char* b) {
  wlog(a, b);
  k_puts("wine: ");
  if (a) k_puts(a);
  if (b) k_puts(b);
  k_puts("\n");
}

static void con_raw(const char* s, size_t n) {
  size_t i = 0, start = 0;
  /* O console do SODOS quebra linha com \n; o \r de \r\n sairia como lixo. */
  for (i = 0; i < n; i++) {
    if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') {
      if (i > start) k_write(s + start, (int)(i - start));
      start = i + 1;
    }
  }
  if (n > start) k_write(s + start, (int)(n - start));
}

static void clock_(struct wproc* p) {
  while (__sync_lock_test_and_set(&p->clk, 1)) k_thread_yield();
}
static void cunlock(struct wproc* p) { __sync_lock_release(&p->clk); }

/* Da tarefa principal: direto. De uma thread do Windows: fica no buffer e a
 * tarefa principal escreve (wcon_flush), na mesma ordem. */
void wcon_write(const char* s, size_t n) {
  struct wproc* p = wp();
  if (wine_debug >= 2 && n) { /* copia na serial: a saida de um processo que cai cedo se perde */
    char line[240];
    size_t k = n < sizeof(line) - 1 ? n : sizeof(line) - 1, i;
    for (i = 0; i < k; i++) line[i] = (s[i] == 10 || s[i] == 13) ? ' ' : s[i];
    line[k] = 0;
    wlog("saida: ", line);
  }
  if (!wthread()) {
    wcon_flush();
    con_raw(s, n);
    return;
  }
  for (;;) {
    clock_(p);
    if (p->cn + n <= 65536 || !p->cn) {
      if (p->cn + n > p->ccap) {
        uint32_t nc = p->ccap ? p->ccap : 4096;
        char* nb;
        while (nc < p->cn + n) nc *= 2;
        nb = (char*)k_realloc(p->cbuf, nc);
        if (!nb) {
          cunlock(p);
          return;
        }
        p->cbuf = nb;
        p->ccap = nc;
      }
      memcpy(p->cbuf + p->cn, s, n);
      p->cn += (uint32_t)n;
      cunlock(p);
      k_futex_wake(&p->exit_req, -1);
      return;
    }
    cunlock(p);
    k_futex_wake(&p->exit_req, -1);
    if (p->exit_req) return;
    k_thread_yield();
  }
}

void wcon_flush(void) {
  struct wproc* p = wp();
  char* b;
  uint32_t n;
  if (!p->cn) goto input;
  clock_(p);
  b = p->cbuf;
  n = p->cn;
  p->cbuf = 0;
  p->cn = 0;
  p->ccap = 0;
  cunlock(p);
  con_raw(b, n);
  k_free(b);
input:
  /* leitura do teclado pedida por uma thread */
  if (p->in_req == 1) {
    int len = k_readline(p->in_line, (int)sizeof(p->in_line) - 1);
    p->in_len = len;
    p->in_req = 2;
    k_futex_wake(&p->in_req, -1);
  } else if (p->in_req == 3) {
    p->in_len = k_getc();
    p->in_req = 4;
    k_futex_wake(&p->in_req, -1);
  }
}

int wcon_readline(char* buf, int max) {
  struct wproc* p = wp();
  int len;
  if (!wthread()) {
    wcon_flush();
    return k_readline(buf, max);
  }
  p->in_req = 1;
  k_futex_wake(&p->exit_req, -1);
  while (p->in_req != 2 && !p->exit_req) k_futex_wait(&p->in_req, 1, 50);
  len = p->in_req == 2 ? p->in_len : -1;
  p->in_req = 0;
  if (len < 0) return -1;
  if (len >= max) len = max - 1;
  memcpy(buf, p->in_line, (size_t)len);
  buf[len] = 0;
  return len;
}

int wcon_getc(void) {
  struct wproc* p = wp();
  int c;
  if (!wthread()) {
    wcon_flush();
    return k_getc();
  }
  p->in_req = 3;
  k_futex_wake(&p->exit_req, -1);
  while (p->in_req != 4 && !p->exit_req) k_futex_wait(&p->in_req, 3, 50);
  c = p->in_req == 4 ? p->in_len : -1;
  p->in_req = 0;
  return c;
}

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

void wlower(char* d, const char* s, int max) {
  int i = 0;
  while (s[i] && i < max - 1) {
    d[i] = lc(s[i]);
    i++;
  }
  d[i] = 0;
}

void wbase(char* d, const char* path, int max) {
  const char* b = path;
  const char* p;
  for (p = path; *p; p++)
    if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
  wlower(d, b, max);
}

int wstreq(const char* a, const char* b) {
  while (*a && lc(*a) == lc(*b)) {
    a++;
    b++;
  }
  return lc(*a) == lc(*b);
}

size_t wstrlen16(const wchar16* s) {
  size_t n = 0;
  if ((uintptr_t)s < 0x10000 || (intptr_t)s == -1) return 0; /* ATOM, LPSTR_TEXTCALLBACK */
  while (s[n]) n++;
  return n;
}

int wutf16_to_8(const wchar16* s, int n, char* d, int max) {
  int i = 0, o = 0;
  while (n < 0 ? s[i] != 0 : i < n) {
    uint32_t c = s[i++];
    char tmp[4];
    int k, len;
    if (c >= 0xD800 && c < 0xDC00 && (n < 0 ? s[i] != 0 : i < n) && s[i] >= 0xDC00 && s[i] < 0xE000)
      c = 0x10000u + ((c - 0xD800u) << 10) + (uint32_t)(s[i++] - 0xDC00u);
    if (c < 0x80) {
      tmp[0] = (char)c;
      len = 1;
    } else if (c < 0x800) {
      tmp[0] = (char)(0xC0 | (c >> 6));
      tmp[1] = (char)(0x80 | (c & 0x3F));
      len = 2;
    } else if (c < 0x10000) {
      tmp[0] = (char)(0xE0 | (c >> 12));
      tmp[1] = (char)(0x80 | ((c >> 6) & 0x3F));
      tmp[2] = (char)(0x80 | (c & 0x3F));
      len = 3;
    } else {
      tmp[0] = (char)(0xF0 | (c >> 18));
      tmp[1] = (char)(0x80 | ((c >> 12) & 0x3F));
      tmp[2] = (char)(0x80 | ((c >> 6) & 0x3F));
      tmp[3] = (char)(0x80 | (c & 0x3F));
      len = 4;
    }
    if (d) {
      if (o + len > max) break;
      for (k = 0; k < len; k++) d[o + k] = tmp[k];
    }
    o += len;
  }
  return o;
}

int wutf8_to_16(const char* s8, int n, wchar16* d, int max) {
  const uint8_t* s = (const uint8_t*)s8;
  int i = 0, o = 0;
  while (n < 0 ? s[i] != 0 : i < n) {
    uint32_t c = s[i++];
    int more = 0;
    if (c >= 0xF0 && c < 0xF8) {
      c &= 7;
      more = 3;
    } else if (c >= 0xE0) {
      c &= 15;
      more = 2;
    } else if (c >= 0xC0) {
      c &= 31;
      more = 1;
    }
    /* Byte que nao e UTF-8 valido: Latin-1, como o console do SODOS. */
    while (more-- > 0 && (n < 0 ? s[i] != 0 : i < n) && (s[i] & 0xC0) == 0x80) c = (c << 6) | (s[i++] & 0x3Fu);
    if (c >= 0x10000) {
      if (d) {
        if (o + 2 > max) break;
        d[o] = (wchar16)(0xD800 + ((c - 0x10000) >> 10));
        d[o + 1] = (wchar16)(0xDC00 + ((c - 0x10000) & 0x3FF));
      }
      o += 2;
    } else {
      if (d) {
        if (o + 1 > max) break;
        d[o] = (wchar16)c;
      }
      o++;
    }
  }
  return o;
}

/* Memoria compartilhada acima de 4 GB (sobrevive ao processo que pediu): o
 * cache de imagens (pe.c) e as secoes (map.c). A arena baixa (3 GB) e de
 * todos os programas e acabava com o Claude e o Chrome juntos (1,7 GB so de
 * cache e secoes do Mojo, com 21 GB livres acima de 4 GB). O kernel novo
 * exporta k_hmalloc_shared (dono WINE_SHARED_OWNER, solto quando nao ha mais
 * EXE do Windows); no antigo, ou sem cota no SET HIMEM, devolve 0 e quem
 * chama usa a arena baixa como antes. */
typedef void* (*hms_fn)(uint64_t n, uint64_t align);
typedef void (*hfs_fn)(void* p);
static hms_fn hms;
static hfs_fn hfs;
static volatile int hms_tried;
void* wshared_high_alloc(uint64_t n, uint64_t align) {
  if (!hms_tried) {
    hms = (hms_fn)k_dl_sym(0, "k_hmalloc_shared", 0);
    hfs = (hfs_fn)k_dl_sym(0, "k_hfree_shared", 0);
    hms_tried = 1;
  }
  return hms && hfs && n ? hms(n, align ? align : 16) : 0;
}
/* solta o que veio de wshared_high_alloc ou do k_malloc_shared */
void wshared_free(void* p) {
  if (!p) return;
  if ((uintptr_t)p >= 0x100000000ull) {
    if (hfs) hfs(p);
  } else {
    k_free_shared(p);
  }
}

void* wzalloc(size_t n) {
  void* p = k_malloc((unsigned)(n ? n : 1));
  if (p) memset(p, 0, n);
  return p;
}

char* wdup16to8(const wchar16* s) {
  int n;
  char* d;
  if (!s) return 0;
  n = wutf16_to_8(s, -1, 0, 0);
  d = (char*)wzalloc((size_t)n + 1);
  if (d) wutf16_to_8(s, -1, d, n);
  return d;
}

wchar16* wdup8to16(const char* s) {
  int n;
  wchar16* d;
  if (!s) return 0;
  n = wutf8_to_16(s, -1, 0, 0);
  d = (wchar16*)wzalloc(((size_t)n + 1) * 2);
  if (d) wutf8_to_16(s, -1, d, n);
  return d;
}

/* WINEDEBUG=2: espera que ja passou de 5 s (uma linha so: varias threads escrevem) */
static char* put_hex(char* o, uint64_t v) {
  static const char hx[] = "0123456789abcdef";
  int k, started = 0;
  *o++ = '0';
  *o++ = 'x';
  for (k = 60; k >= 0; k -= 4) {
    int d = (int)((v >> k) & 15);
    if (d || started || k == 0) {
      *o++ = hx[d];
      started = 1;
    }
  }
  return o;
}
void wwait_note(const char* what, void* caller, uint64_t waited_ms) {
  struct wmod* m = pe_from_addr((uint64_t)(uintptr_t)caller);
  struct wobj* t = wthread();
  char line[200], *o = line;
  const char* s;
  for (s = what; *s && o < line + 60; s++) *o++ = *s;
  for (s = " thread "; *s; s++) *o++ = *s;
  o = put_hex(o, t ? t->tid : 0);
  for (s = " valor "; *s; s++) *o++ = *s;
  o = put_hex(o, waited_ms);
  for (s = " de "; *s; s++) *o++ = *s;
  if (m) {
    for (s = m->name; *s && o < line + 150; s++) *o++ = *s;
    *o++ = '+';
    o = put_hex(o, (uint64_t)(uintptr_t)caller - (uint64_t)(uintptr_t)m->base);
  } else {
    o = put_hex(o, (uint64_t)(uintptr_t)caller);
  }
  *o = 0;
  wlog("espera longa: ", line);
}

/* WINEDEBUG=2: enderecos de retorno (modulo+rva) achados na pilha, numa linha */
void wstack_note(const char* what, const uint64_t* sp) {
  char line[600], *o = line;
  const char* s;
  int i, shown = 0;
  for (s = what; *s && o < line + 40; s++) *o++ = *s;
  extern const void* wine_driver(void);
  uint64_t wd = (uint64_t)(uintptr_t)wine_driver;
  /* so ate o topo da pilha (TEB.StackBase): acima dele, com a pilha em
   * memoria virtual, nao ha pagina e a leitura derrubava o processo */
  uint64_t top = *(uint64_t*)(wteb() + TEB_STACKBASE);
  int lim = 2048;
  if (top > (uint64_t)(uintptr_t)sp && (top - (uint64_t)(uintptr_t)sp) / 8 < (uint64_t)lim)
    lim = (int)((top - (uint64_t)(uintptr_t)sp) / 8);
  for (i = 0; i < lim && shown < 40 && o < line + 560; i++) {
    struct wmod* m = pe_from_addr(sp[i]);
    if (!m || !m->base) {
      /* dentro do WINE.DLL: "W" + deslocamento a partir de wine_driver */
      if (sp[i] > wd - 0x10000 && sp[i] < wd + 0x100000 && wine_debug >= 3) {
        *o++ = ' ';
        *o++ = 'W';
        o = put_hex(o, sp[i] - wd);
        shown++;
      }
      continue;
    }
    *o++ = ' ';
    o = put_hex(o, sp[i] - (uint64_t)(uintptr_t)m->base);
    shown++;
  }
  *o = 0;
  wlog("pilha: ", line);
}

void WINAPI wine_unimplemented(const char* what) {
  wsay("funcao nao implementada: ", what);
  wexit(0xC0000139);   /* STATUS_ENTRYPOINT_NOT_FOUND */
}

/* Stub para um import que nao existe: o programa so cai se chamar.
 *   48 B9 <imm64>   mov rcx, "dll!nome"
 *   48 B8 <imm64>   mov rax, wine_unimplemented
 *   FF E0           jmp rax                                  */
void* wine_make_stub(const char* dll, const char* name) {
  size_t a = strlen(dll), b = strlen(name);
  uint8_t* p = (uint8_t*)k_malloc((unsigned)(24 + a + b + 2));
  char* txt;
  uint64_t v;
  int i;
  if (!p) return 0;
  txt = (char*)p + 24;
  memcpy(txt, dll, a);
  txt[a] = '!';
  memcpy(txt + a + 1, name, b + 1);
  p[0] = 0x48;
  p[1] = 0xB9;
  v = (uint64_t)(uintptr_t)txt;
  for (i = 0; i < 8; i++) p[2 + i] = (uint8_t)(v >> (8 * i));
  p[10] = 0x48;
  p[11] = 0xB8;
  v = (uint64_t)(uintptr_t)wine_unimplemented;
  for (i = 0; i < 8; i++) p[12 + i] = (uint8_t)(v >> (8 * i));
  p[20] = 0xFF;
  p[21] = 0xE0;
  return p;
}

/* ---------- heap do programa (malloc da CRT e HeapAlloc) ----------
 * O k_malloc vem da arena abaixo de 4 GB, dividida por todos os processos.
 * Com o Claude (6+ processos do Chromium, malloc = HeapAlloc no Electron) ela
 * acabava, e o bitmap de ~600 KB do canvas do hCaptcha virava "sem memoria"
 * (0xe0000008) no renderer: o captcha ficava sem imagem. Blocos grandes (e
 * qualquer bloco quando a arena baixa falha) vao para a RAM acima de 4 GB,
 * com um cabecalho de 16 bytes (tamanho + marca). Ponteiro >= 4 GB = alto. */
#define WH_BIG (64u * 1024u)
#define WH_MAGIC 0x48474948574e4957ull /* "WINWHIGH" */
#define WH_HIGH(p) ((uintptr_t)(p) >= 0x100000000ull)
static void* wh_high_alloc(size_t n) {
  uint64_t* h = (uint64_t*)k_hmalloc((unsigned long long)n + 16, 16);
  if (!h) return 0;
  h[0] = n;
  h[1] = WH_MAGIC;
  return h + 2;
}
static int wh_is_high(void* p) { return p && WH_HIGH(p) && ((uint64_t*)p)[-1] == WH_MAGIC; }
void* wheap_alloc(size_t n) {
  void* p = 0;
  if (!n) n = 1;
  if (n < WH_BIG && n < 0xFFFFFFF0u) p = k_malloc((unsigned)n);
  if (!p) p = wh_high_alloc(n);
  if (!p && n < 0xFFFFFFF0u) p = k_malloc((unsigned)n); /* sem HIMEM: tenta a arena baixa */
  if (!p && wine_debug) wlogx("heap: sem memoria para ", n);
  return p;
}
size_t wheap_size(void* p) {
  if (!p) return 0;
  if (wh_is_high(p)) return (size_t)((uint64_t*)p)[-2];
  return k_msize(p);
}
void wheap_free(void* p) {
  if (!p) return;
  if (wh_is_high(p)) {
    ((uint64_t*)p)[-1] = 0;
    k_hfree((uint64_t*)p - 2);
    return;
  }
  k_free(p);
}
void* wheap_realloc(void* p, size_t n) {
  size_t old;
  void* q;
  if (!p) return wheap_alloc(n);
  if (!n) n = 1;
  if (!wh_is_high(p) && n < WH_BIG) {
    q = k_realloc(p, (unsigned)n);
    if (q) return q;
  }
  old = wheap_size(p);
  if (wh_is_high(p) && n <= old && n >= old / 2) return p; /* encolher pouco: fica */
  q = wheap_alloc(n);
  if (!q) return 0;
  memcpy(q, p, old < n ? old : n);
  wheap_free(p);
  return q;
}

/* ---------- registro de esperas (dump no REBOOT) ----------
 * O WINE.DLL e uma imagem so para todos os processos do Windows: esta tabela
 * ve as threads de todos. Cada espera bloqueante (objetos, secao critica,
 * SRW, variavel de condicao, WaitOnAddress, porta de conclusao, pipe, Sleep)
 * marca a vaga da thread na entrada e limpa na saida. Corrida inofensiva:
 * "what" e gravado por ultimo e e so diagnostico. */
#define WTRK_MAX 2048
struct wtrk {
  struct wproc* p;
  uint8_t* teb;
  uint32_t pid, tid;
  uint32_t kpid;               /* pid do SODOS (wine_pid_gone) */
  const char* volatile what;   /* 0 = rodando */
  const char* detail;
  uint64_t obj, owner, since;
  void* caller;
  int used;
  int wide;                    /* detail e wchar16 (nome de pipe) */
};
static struct wtrk wtrk[WTRK_MAX];
static volatile int wtrk_lk;

void wtrack_thread(struct wobj* t) {
  int i;
  if (!t || t->wslot) return;
  wnokill_up(); /* trava global: ver wine_int.h */
  while (__sync_lock_test_and_set(&wtrk_lk, 1)) __builtin_ia32_pause();
  for (i = 0; i < WTRK_MAX; i++) {
    if (wtrk[i].used) continue;
    wtrk[i].p = wp();
    wtrk[i].teb = t->teb;
    wtrk[i].pid = wp()->pid;
    wtrk[i].kpid = wp()->kpid ? wp()->kpid : k_getpid();
    wtrk[i].tid = t->tid;
    wtrk[i].what = 0;
    wtrk[i].used = 1;
    t->wslot = i + 1;
    break;
  }
  __sync_lock_release(&wtrk_lk);
  wnokill_down();
}

void wtrack_exit(struct wobj* t) {
  if (!t || !t->wslot) return;
  wtrk[t->wslot - 1].used = 0;
  t->wslot = 0;
}

void wtrack_proc_exit(struct wproc* p) {
  int i;
  for (i = 0; i < WTRK_MAX; i++)
    if (wtrk[i].used && wtrk[i].p == p) wtrk[i].used = 0;
}

/* Chamado pelo kernel no fim do processo do SODOS (tambem no KILL e no
 * Ctrl+Backspace, onde o wtrack_proc_exit nao roda): o wproc dele vai ser
 * solto, entao as vagas nao podem continuar apontando para ele. */
void wine_pid_gone(uint32_t kpid) {
  struct wproc* gone[8];
  int i, j, ng = 0;
  if (!kpid) return;
  wlock_pid_gone(kpid); /* obj.c: a trava global nao fica com quem acabou */
  for (i = 0; i < WTRK_MAX; i++) {
    if (!wtrk[i].used || wtrk[i].kpid != kpid) continue;
    for (j = 0; j < ng && gone[j] != wtrk[i].p; j++) {
    }
    if (j == ng && ng < 8) gone[ng++] = wtrk[i].p;
    wtrk[i].used = 0;
  }
  /* vaga gravada com outro kpid (WINE.DLL com kernel antigo: k_getpid errado
   * numa k_thread) mas do mesmo processo: o wproc tambem vai ser solto. So
   * compara ponteiros, nao le o wproc. */
  for (i = 0; ng && i < WTRK_MAX; i++)
    for (j = 0; wtrk[i].used && j < ng; j++)
      if (wtrk[i].p == gone[j]) wtrk[i].used = 0;
}

static struct wtrk* wtrk_cur(void) {
  uint8_t* teb = wteb();
  struct wobj* t;
  if (!teb || *(uint8_t**)(teb + 0x30) != teb) return 0; /* fora de thread do Windows */
  t = *(struct wobj**)(teb + TEB_WTHREAD);
  if (!t || !t->wslot || t->wslot > WTRK_MAX) return 0;
  return &wtrk[t->wslot - 1];
}

void wwait_begin(const char* what, const void* obj, uint64_t owner_teb, const char* detail, void* caller) {
  struct wtrk* w = wtrk_cur();
  if (!w) return;
  w->obj = (uint64_t)(uintptr_t)obj;
  w->owner = owner_teb;
  w->detail = detail;
  w->wide = 0;
  w->caller = caller;
  w->since = wnow_ms();
  __atomic_store_n(&w->what, what, __ATOMIC_RELEASE);
}

void wwait_begin_w(const char* what, const void* obj, const wchar16* detail, void* caller) {
  struct wtrk* w = wtrk_cur();
  if (!w) return;
  w->obj = (uint64_t)(uintptr_t)obj;
  w->owner = 0;
  w->detail = (const char*)detail;
  w->wide = detail != 0;
  w->caller = caller;
  w->since = wnow_ms();
  __atomic_store_n(&w->what, what, __ATOMIC_RELEASE);
}

void wwait_end(void) {
  struct wtrk* w = wtrk_cur();
  if (w) __atomic_store_n(&w->what, (const char*)0, __ATOMIC_RELEASE);
}

static char* dcat(char* o, char* end, const char* s) {
  while (s && *s && o < end) *o++ = *s++;
  return o;
}
static char* ddec(char* o, char* end, uint64_t v) {
  char b[24];
  int n = 0;
  do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 23);
  while (n && o < end) *o++ = b[--n];
  return o;
}
static char* dhex(char* o, char* end, uint64_t v) {
  int i, started = 0;
  if (o + 18 > end) return o;
  *o++ = '0';
  *o++ = 'x';
  for (i = 15; i >= 0; i--) {
    int d = (int)((v >> (i * 4)) & 15);
    if (d || started || i == 0) {
      *o++ = "0123456789abcdef"[d];
      started = 1;
    }
  }
  return o;
}

/* modulo+rva de um endereco, nos modulos do processo p (vivo: a vaga esta em uso) */
static char* dwhere(char* o, char* end, struct wproc* p, uint64_t a) {
  struct wmod* m;
  extern const void* wine_driver(void);
  for (m = p ? p->mods : 0; m; m = m->next) {
    if (m->base && a >= (uint64_t)(uintptr_t)m->base && a < (uint64_t)(uintptr_t)m->base + m->size) {
      o = dcat(o, end, m->name);
      o = dcat(o, end, "+");
      return dhex(o, end, a - (uint64_t)(uintptr_t)m->base);
    }
  }
  o = dcat(o, end, "WINE.DLL+");
  return dhex(o, end, a - (uint64_t)(uintptr_t)wine_driver);
}

/* Chamado pelo kernel (REBOOT, kernel/wine.c) na tarefa do mundo: so le a
 * tabela e escreve na serial. Uma linha por thread do Windows. */
void wine_dump(void) {
  uint64_t now = wnow_ms();
  int i, n = 0, nw = 0;
  k_dbg("[wine-dump] threads do Windows e o que esperam\n", 47);
  for (i = 0; i < WTRK_MAX; i++) {
    struct wtrk* w = &wtrk[i];
    const char* what;
    char line[300], *o = line, *end = line + sizeof(line) - 2;
    if (!w->used) continue;
    what = __atomic_load_n(&w->what, __ATOMIC_ACQUIRE);
    n++;
    o = dcat(o, end, "[wine-dump] pid ");
    o = ddec(o, end, w->pid);
    o = dcat(o, end, " tid ");
    o = dhex(o, end, w->tid);
    if (!what) {
      o = dcat(o, end, ": rodando (ou espera nao marcada)");
    } else {
      int j;
      nw++;
      o = dcat(o, end, ": ");
      o = dcat(o, end, what);
      if (w->detail) {
        o = dcat(o, end, " \"");
        if (w->wide) {
          const wchar16* d16 = (const wchar16*)w->detail;
          int k;
          for (k = 0; k < 120 && d16[k] && o < end; k++) *o++ = d16[k] < 128 ? (char)d16[k] : '?';
        } else {
          o = dcat(o, end, w->detail);
        }
        o = dcat(o, end, "\"");
      }
      o = dcat(o, end, " obj ");
      o = dhex(o, end, w->obj);
      if (w->owner) {
        o = dcat(o, end, " dono ");
        for (j = 0; j < WTRK_MAX; j++)
          if (wtrk[j].used && (uint64_t)(uintptr_t)wtrk[j].teb == w->owner) break;
        if (j < WTRK_MAX) {
          o = dcat(o, end, "pid ");
          o = ddec(o, end, wtrk[j].pid);
          o = dcat(o, end, " tid ");
          o = dhex(o, end, wtrk[j].tid);
        } else {
          o = dcat(o, end, "teb ");
          o = dhex(o, end, w->owner);
        }
      }
      o = dcat(o, end, " ha ");
      o = ddec(o, end, w->since > now ? 0 : now - w->since); /* comecou durante o dump */
      o = dcat(o, end, " ms, de ");
      o = dwhere(o, end, w->p, (uint64_t)(uintptr_t)w->caller);
    }
    *o++ = '\n';
    k_dbg(line, (int)(o - line));
  }
  {
    /* trava global do WINE.DLL: presa com todo mundo esperando = alguem morreu
     * com ela (ver wnokill em wine_int.h) */
    char line[160], *o = line, *end = line + sizeof(line) - 2;
    uint32_t lk_kpid, lk_tid;
    int lk = wlock_state(&lk_kpid, &lk_tid);
    o = dcat(o, end, "[wine-dump] trava global: ");
    if (!lk) {
      o = dcat(o, end, "livre");
    } else {
      o = dcat(o, end, lk == 2 ? "presa, com quem espera" : "presa");
      o = dcat(o, end, ", dono pid do SODOS ");
      o = ddec(o, end, lk_kpid);
      o = dcat(o, end, " tid ");
      o = dhex(o, end, lk_tid);
    }
    *o++ = '\n';
    k_dbg(line, (int)(o - line));
  }
  {
    char line[96], *o = line, *end = line + sizeof(line) - 2;
    o = dcat(o, end, "[wine-dump] fim: ");
    o = ddec(o, end, (uint64_t)n);
    o = dcat(o, end, " threads, ");
    o = ddec(o, end, (uint64_t)nw);
    o = dcat(o, end, " esperando\n");
    k_dbg(line, (int)(o - line));
  }
}

