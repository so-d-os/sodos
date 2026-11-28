/* install.c - o que o SETUP instala e onde (lista ITENS) e a funcao install().
 *
 * Cada linha da lista e { origem, destino, opcoes }:
 *  - origem: nome do arquivo dentro de <pasta do SETUP.EXE>\ARQUIVOS (o
 *    empacota.sh copia para la, a partir das mesmas linhas);
 *  - destino: pasta terminada em \ (o arquivo fica com o mesmo nome) ou o
 *    caminho completo do arquivo;
 *  - INST_PRESERVA: so grava se o destino ainda nao existe (configuracao que o
 *    usuario edita).
 *
 * Para mudar onde algo vai, mude so o destino aqui. Os que estao marcados
 * "na raiz" ficam em D:\ porque o codigo do SODOS procura neles pelo caminho
 * fixo (o kernel, o WIN, o WINE, o cc e o make); quando esse codigo mudar, o
 * destino muda aqui tambem.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>
#include "install.h"

#define INST_PRESERVA 1

typedef struct {
  const char* origem;
  const char* destino;
  int opcoes;
} inst_item_t;

#define BIN  "D:\\SODOS\\BIN\\"
#define SYS  "D:\\SODOS\\"
#define SRC  "D:\\SODOS\\DEV\\SRC\\"
#define EXEM "D:\\SODOS\\DEV\\EXEMPLOS\\"
#define RAIZ "D:\\"

/* As linhas {"ORIGEM", DESTINO, ...} sao lidas tambem pelo empacota.sh:
 * mantenha uma por linha. */
static const inst_item_t ITENS[] = {
  /* ---- configuracao inicial (nao sobrescreve a do usuario) ---- */
  {"AUTOEXEC.BAT", RAIZ, INST_PRESERVA},

  /* ---- na raiz: o boot procura antes do AUTOEXEC.BAT, quando o PATH esta
   * vazio e o PATHSODOS ainda e o padrao U:\SODOS (kernel.c, env.h) ---- */
  {"BAT.EXE",      RAIZ, 0},  /* roda o AUTOEXEC.BAT */
  {"PENDRIVE.EXE", RAIZ, 0},
  {"HTTPD.EXE",    RAIZ, 0},  /* a API (8797) */

  /* ---- sistema: o kernel procura no PATHSODOS (D:\SODOS) ---- */
  {"WSL.EXE",      SYS, 0},  /* vt.c: D:\WSL.EXE ou D:\SODOS\WSL.EXE */
  {"AIPANEL.EXE",  SYS, 0},
  {"DISKSYNC.EXE", SYS, 0},
  {"FONTADV.EXE",  SYS, 0},
  {"IMGBIG.DLL",   SYS, 0},
  {"LINKEXT.EXE",  SYS, 0},
  {"MENUCFG.EXE",  SYS, 0},
  {"TEMAS.EXE",    SYS, 0},
  {"VIDEOCFG.EXE", SYS, 0},
  {"WINTAB.EXE",   SYS, 0},

  /* ---- comandos: D:\SODOS\BIN (o install() poe no PATH) ---- */
  {"AGENT.EXE",    BIN, 0},
  {"CC.EXE",       BIN, 0},
  {"COBRA.EXE",    BIN, 0},
  {"COMMAND.EXE",  BIN, 0},
  {"CURL.EXE",     BIN, 0},
  {"FFLAUNCH.EXE", BIN, 0},
  {"GIT.EXE",      BIN, 0},
  {"JS.EXE",       BIN, 0},
  {"LINK.EXE",     BIN, 0},  /* o cc chama "LINK" pelo PATH */
  {"LLAMA.EXE",    BIN, 0},
  {"MAKE.EXE",     BIN, 0},
  {"MC.EXE",       BIN, 0},
  {"NETSURF.EXE",  BIN, 0},
  {"PLAY.EXE",     BIN, 0},
  {"PONTE.EXE",    BIN, 0},
  {"TELAS.EXE",    BIN, 0},
  {"TOP.EXE",      BIN, 0},
  {"TROCA.EXE",    BIN, 0},
  {"WGET.EXE",     BIN, 0},
  {"WIDGETS.EXE",  BIN, 0},
  {"WIFI.EXE",     BIN, 0},
  {"XCOPY.EXE",    BIN, 0},

  /* ---- na raiz: caminho fixo no WIN, no WINE e no shell ---- */
  {"EXPLORER.EXE", RAIZ, 0},  /* visual.c, wine/dll/picker.c, proc.c */
  {"BROWSER.EXE",  RAIZ, 0},  /* visual.c */
  {"VIDPLAY.EXE",  RAIZ, 0},  /* visual.c */
  {"IMAGEVW.EXE",  RAIZ, 0},  /* visual.c, LINKEXT.CFG */
  {"WEBCAMV.EXE",  RAIZ, 0},  /* shell/cmd_usb.c */

  /* ---- na raiz: o cc procura em D: (cc/main.c) e o make tambem ---- */
  {"CRT0.OBJ",     RAIZ, 0},
  {"KERNEL.IMP",   RAIZ, 0},
  {"LIBC.DLL",     RAIZ, 0},
  {"LIBCX.DLL",    RAIZ, 0},
  {"GUI.DLL",      RAIZ, 0},
  {"GLIB.DLL",     RAIZ, 0},
  {"IMG.DLL",      RAIZ, 0},
  {"TLS.DLL",      RAIZ, 0},
  {"POSIX.DLL",    RAIZ, 0},
  {"POSIXX.DLL",   RAIZ, 0},
  {"TERM.DLL",     RAIZ, 0},
  {"AI_CLIENT.H",  RAIZ, 0},
  {"ASSERT.H",     RAIZ, 0},
  {"CTYPE.H",      RAIZ, 0},
  {"CURSES.H",     RAIZ, 0},
  {"DIRENT.H",     RAIZ, 0},
  {"DLFCN.H",      RAIZ, 0},
  {"ERRNO.H",      RAIZ, 0},
  {"FCNTL.H",      RAIZ, 0},
  {"FLOAT.H",      RAIZ, 0},
  {"GLIB.H",       RAIZ, 0},
  {"GUI.H",        RAIZ, 0},
  {"IMG.H",        RAIZ, 0},
  {"INTTYPES.H",   RAIZ, 0},
  {"LANGINFO.H",   RAIZ, 0},
  {"LIBGEN.H",     RAIZ, 0},
  {"LIMITS.H",     RAIZ, 0},
  {"LOCALE.H",     RAIZ, 0},
  {"MALLOC.H",     RAIZ, 0},
  {"MATH.H",       RAIZ, 0},
  {"NL_TYPES.H",   RAIZ, 0},
  {"SETJMP.H",     RAIZ, 0},
  {"SIGNAL.H",     RAIZ, 0},
  {"SO.H",         RAIZ, 0},
  {"SOX.H",        RAIZ, 0},
  {"STDARG.H",     RAIZ, 0},
  {"STDBOOL.H",    RAIZ, 0},
  {"STDDEF.H",     RAIZ, 0},
  {"STDINT.H",     RAIZ, 0},
  {"STDIO.H",      RAIZ, 0},
  {"STDLIB.H",     RAIZ, 0},
  {"STRING.H",     RAIZ, 0},
  {"TERMIOS.H",    RAIZ, 0},
  {"TIME.H",       RAIZ, 0},
  {"TLS.H",        RAIZ, 0},
  {"UCHAR.H",      RAIZ, 0},
  {"UCONTEXT.H",   RAIZ, 0},
  {"UNISTD.H",     RAIZ, 0},
  {"WCHAR.H",      RAIZ, 0},
  {"WCTYPE.H",     RAIZ, 0},
  {"WIDGETS.H",    RAIZ, 0},
  {"SYS\\AUXV.H",     "D:\\SYS\\", 0},
  {"SYS\\PRCTL.H",    "D:\\SYS\\", 0},
  {"SYS\\RESOURCE.H", "D:\\SYS\\", 0},
  {"SYS\\STAT.H",     "D:\\SYS\\", 0},
  {"SYS\\SYSCALL.H",  "D:\\SYS\\", 0},
  {"SYS\\TIME.H",     "D:\\SYS\\", 0},
  {"SYS\\TYPES.H",    "D:\\SYS\\", 0},
  {"SYS\\UCONTEXT.H", "D:\\SYS\\", 0},
  {"SYS\\WAIT.H",     "D:\\SYS\\", 0},

  /* ---- fontes da libc, do cc, do link, da TLS, da GUI, do browser ---- */
  {"CRT0.C",       SRC "LIBC\\", 0},
  {"CTORS64.C",    SRC "LIBC\\", 0},
  {"CTYPE.C",      SRC "LIBC\\", 0},
  {"DIVDI.C",      SRC "LIBC\\", 0},
  {"FPCONV.C",     SRC "LIBC\\", 0},
  {"MATH.C",       SRC "LIBC\\", 0},
  {"MISC.C",       SRC "LIBC\\", 0},
  {"PRINTF.C",     SRC "LIBC\\", 0},
  {"SCANF.C",      SRC "LIBC\\", 0},
  {"SETJMP.C",     SRC "LIBC\\", 0},
  {"SETJMP64.C",   SRC "LIBC\\", 0},
  {"SIGFAULT64.C", SRC "LIBC\\", 0},
  {"STDIO.C",      SRC "LIBC\\", 0},
  {"STDLIB.C",     SRC "LIBC\\", 0},
  {"STRING.C",     SRC "LIBC\\", 0},
  {"TIME.C",       SRC "LIBC\\", 0},
  {"WCHAR.C",      SRC "LIBC\\", 0},
  {"POSIX.C",      SRC "LIBC\\", 0},
  {"CC.H",         SRC "CC\\", 0},
  {"GEN.C",        SRC "CC\\", 0},
  {"LEX.C",        SRC "CC\\", 0},
  {"MAIN.C",       SRC "CC\\", 0},
  {"PARSE.C",      SRC "CC\\", 0},
  {"PP.C",         SRC "CC\\", 0},
  {"TYPE.C",       SRC "CC\\", 0},
  {"UTIL.C",       SRC "CC\\", 0},
  {"LINK.C",       SRC "LINK\\", 0},
  {"CRYPTO.C",     SRC "TLS\\", 0},
  {"CRYPTO.H",     SRC "TLS\\", 0},
  {"P256-M.C",     SRC "TLS\\", 0},
  {"P256-M.H",     SRC "TLS\\", 0},
  {"P384.C",       SRC "TLS\\", 0},
  {"P384.H",       SRC "TLS\\", 0},
  {"ROOTS.C",      SRC "TLS\\", 0},
  {"ROOTS.H",      SRC "TLS\\", 0},
  {"TLS.C",        SRC "TLS\\", 0},
  {"X509.C",       SRC "TLS\\", 0},
  {"X509.H",       SRC "TLS\\", 0},
  {"GUI.C",        SRC "GUI\\", 0},
  {"WIDGETS.C",    SRC "GUI\\", 0},
  {"IMG.C",        SRC "IMG\\", 0},
  {"IMG_BIG.C",    SRC "IMG\\", 0},
  {"IMAGEVW.C",    SRC "IMG\\", 0},
  {"BROWSER.C",    SRC "BROWSER\\", 0},
  {"BROWSER.H",    SRC "BROWSER\\", 0},
  {"BRDOM.JS",     SRC "BROWSER\\", 0},
  {"CACHE.CPP",    SRC "BROWSER\\", 0},
  {"CACHE.H",      SRC "BROWSER\\", 0},
  {"HTML.C",       SRC "BROWSER\\", 0},
  {"HTTP.C",       SRC "BROWSER\\", 0},
  {"INFLATE.C",    SRC "BROWSER\\", 0},
  {"JS.C",         SRC "BROWSER\\", 0},
  {"SEARCH.C",     SRC "BROWSER\\", 0},
  {"URL.C",        SRC "BROWSER\\", 0},
  {"YOUTUBE.C",    SRC "BROWSER\\", 0},

  /* ---- exemplos do cc ---- */
  {"BAT.C",        EXEM, 0},
  {"CALC.C",       EXEM, 0},
  {"CALCOPS.C",    EXEM, 0},
  {"CALCOPS.H",    EXEM, 0},
  {"COMMAND.C",    EXEM, 0},
  {"CRASHT.C",     EXEM, 0},
  {"DEMO.BAT",     EXEM, 0},
  {"DYNLOAD.C",    EXEM, 0},
  {"FAT32T.C",     EXEM, 0},
  {"HELLO.C",      EXEM, 0},
  {"HELLO64.C",    EXEM, 0},
  {"HELLO64C.C",   EXEM, 0},
  {"HTTPD.C",      EXEM, 0},
  {"HTTPS.C",      EXEM, 0},
  {"MATHX.C",      EXEM, 0},
  {"MATHX.H",      EXEM, 0},
  {"SIGT64.C",     EXEM, 0},
  {"SMPT.C",       EXEM, 0},
  {"SOCKT.C",      EXEM, 0},
  {"SPECIAL.CPP",  EXEM, 0},
  {"TGLIB.C",      EXEM, 0},
  {"TTERM.C",      EXEM, 0},
  {"USEMATH.C",    EXEM, 0},
  {"VEC.CPP",      EXEM, 0},
  {"VEC.H",        EXEM, 0},
  {"VMAIN.CPP",    EXEM, 0},
  {"WGET.C",       EXEM, 0},
  {"XHELLO.C",     EXEM, 0},
};

#define N_ITENS ((int)(sizeof(ITENS) / sizeof(ITENS[0])))
#define BLOCO 65536

int install_total(void) { return N_ITENS; }

static void junta(char* out, int max, const char* a, const char* b) {
  int i = 0;
  while (a && *a && i < max - 1) out[i++] = *a++;
  if (i > 0 && out[i - 1] != '\\' && out[i - 1] != ':' && i < max - 1) out[i++] = '\\';
  while (b && *b && i < max - 1) out[i++] = *b++;
  out[i] = 0;
}

/* so o nome do arquivo: "SYS\TYPES.H" -> "TYPES.H" */
static const char* so_nome(const char* p) {
  const char* r = p;
  for (; *p; p++)
    if (*p == '\\' || *p == '/') r = p + 1;
  return r;
}

/* destino final do item, com a raiz de teste no lugar do "D:" */
static void destino_de(const inst_item_t* it, const char* raiz, char* out, int max) {
  char d[300];
  int n = (int)strlen(it->destino);
  if (n > 0 && it->destino[n - 1] == '\\') junta(d, (int)sizeof(d), it->destino, so_nome(it->origem));
  else junta(d, (int)sizeof(d), it->destino, 0);
  if (raiz && raiz[0] && (d[0] == 'D' || d[0] == 'd') && d[1] == ':')
    junta(out, max, raiz, d + 3);
  else {
    strncpy(out, d, (size_t)max - 1);
    out[max - 1] = 0;
  }
}

/* cria as pastas de "D:\A\B\ARQ.EXT" (D:\A e D:\A\B) */
static int garante_pastas(const char* arq) {
  char p[300];
  int i, n = (int)strlen(arq);
  if (n >= (int)sizeof(p)) return -1;
  strcpy(p, arq);
  for (i = 3; i < n; i++) {
    if (p[i] != '\\') continue;
    p[i] = 0;
    /* k_mkdir: 0 criou, 1 ja existia, < 0 erro */
    if (k_mkdir(p) < 0) {
      p[i] = '\\';
      return -1;
    }
    p[i] = '\\';
  }
  return 0;
}

static void anota(inst_resultado_t* res, const char* a, const char* b) {
  snprintf(res->ultimo_erro, sizeof(res->ultimo_erro), "%s %s", a, b);
}

long install_bytes(const char* origem) {
  long t = 0;
  int i;
  char o[300];
  for (i = 0; i < N_ITENS; i++) {
    int s;
    junta(o, (int)sizeof(o), origem, ITENS[i].origem);
    s = k_fsize(o);
    if (s > 0) t += s;
  }
  return t;
}

/* ---- PATH do AUTOEXEC.BAT ---------------------------------------------- */

static int tem_bin(const char* s) {
  const char* k = "D:\\SODOS\\BIN";
  int n = (int)strlen(k);
  for (; *s; s++) {
    int j = 0;
    while (j < n && s[j]) {
      char a = s[j], b = k[j];
      if (a >= 'a' && a <= 'z') a = (char)(a - 32);
      if (a != b) break;
      j++;
    }
    if (j == n && (s[n] == 0 || s[n] == ';' || s[n] == '\\' || s[n] == '\r' || s[n] == '\n' || s[n] == ' '))
      return 1;
  }
  return 0;
}

static int linha_set_path(const char* b, int i) {
  const char* k = "SET PATH=";
  int j;
  while (b[i] == ' ' || b[i] == '\t') i++;
  for (j = 0; k[j]; j++) {
    char c = b[i + j];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c != k[j]) return -1;
  }
  return i + j; /* comeco do valor */
}

/* Poe D:\SODOS\BIN no comeco do "set PATH=" do AUTOEXEC.BAT (ou cria a
 * linha logo no comeco). 0 ok. */
static int path_no_autoexec(const char* arq) {
  static char b[16384], o[16384 + 64];
  FILE* f = fopen(arq, "rb");
  int n, i = 0, k = 0, feito = 0;
  if (!f) return -1;
  n = (int)fread(b, 1, sizeof(b) - 1, f);
  fclose(f);
  if (n < 0) n = 0;
  b[n] = 0;
  while (i < n) {
    int ini = i, v = linha_set_path(b, i), fim = i;
    while (fim < n && b[fim] != '\n') fim++;
    if (fim < n) fim++;
    if (v >= 0 && !feito) {
      char lin[1024];
      int m = fim - v;
      if (m > (int)sizeof(lin) - 1) m = (int)sizeof(lin) - 1;
      memcpy(lin, b + v, (size_t)m);
      lin[m] = 0;
      if (tem_bin(lin)) return 0; /* ja tem */
      memcpy(o + k, b + ini, (size_t)(v - ini));
      k += v - ini;
      memcpy(o + k, "D:\\SODOS\\BIN;", 13);
      k += 13;
      memcpy(o + k, b + v, (size_t)(fim - v));
      k += fim - v;
      feito = 1;
    } else {
      memcpy(o + k, b + ini, (size_t)(fim - ini));
      k += fim - ini;
    }
    i = fim;
  }
  if (!feito) {
    /* sem set PATH: poe depois do @echo off (primeira linha) */
    const char* nova = "set PATH=D:\\SODOS\\BIN;D:\\;D:\\SODOS\r\n";
    int p = 0, ln = (int)strlen(nova);
    while (p < n && b[p] != '\n') p++;
    if (p < n) p++;
    k = 0;
    memcpy(o, b, (size_t)p);
    k = p;
    memcpy(o + k, nova, (size_t)ln);
    k += ln;
    memcpy(o + k, b + p, (size_t)(n - p));
    k += n - p;
  }
  f = fopen(arq, "wb");
  if (!f) return -1;
  if ((int)fwrite(o, 1, (size_t)k, f) != k) {
    fclose(f);
    return -1;
  }
  fclose(f);
  return 0;
}

/* o PATH do sistema ja, sem esperar o proximo boot */
static void path_agora(void) {
  char p[1024], nv[1100];
  if (k_getenv("PATH", p, (int)sizeof(p)) < 0) p[0] = 0;
  if (tem_bin(p)) return;
  snprintf(nv, sizeof(nv), "D:\\SODOS\\BIN%s%s", p[0] ? ";" : "", p);
  k_setenv("PATH", nv);
}

/* ---- copia ------------------------------------------------------------- */

int install(const char* origem, const char* raiz, inst_progresso_fn cb, void* ud,
            inst_resultado_t* res) {
  static char buf[BLOCO];
  long total = install_bytes(origem), feitos = 0;
  int i;
  memset(res, 0, sizeof(*res));
  if (total <= 0) total = 1;
  for (i = 0; i < N_ITENS && !res->parado; i++) {
    const inst_item_t* it = &ITENS[i];
    char o[300], d[300];
    FILE *fo, *fd;
    int tam;
    junta(o, (int)sizeof(o), origem, it->origem);
    destino_de(it, raiz, d, (int)sizeof(d));
    tam = k_fsize(o);
    if (tam < 0) {
      res->faltando++;
      continue;
    }
    if ((it->opcoes & INST_PRESERVA) && k_fsize(d) >= 0) {
      res->mantidos++;
      feitos += tam;
      continue;
    }
    if (cb && cb(feitos, total, d, i, N_ITENS, ud)) {
      res->parado = 1;
      break;
    }
    if (garante_pastas(d) != 0) {
      res->erros++;
      anota(res, "nao criei a pasta de", d);
      feitos += tam;
      continue;
    }
    fo = fopen(o, "rb");
    if (!fo) {
      res->erros++;
      anota(res, "nao abri", o);
      feitos += tam;
      continue;
    }
    fd = fopen(d, "wb");
    if (!fd) {
      fclose(fo);
      res->erros++;
      anota(res, "nao gravei", d);
      feitos += tam;
      continue;
    }
    for (;;) {
      int n = (int)fread(buf, 1, sizeof(buf), fo);
      if (n <= 0) break;
      if ((int)fwrite(buf, 1, (size_t)n, fd) != n) {
        res->erros++;
        anota(res, "disco cheio? ao gravar", d);
        break;
      }
      feitos += n;
      if (cb && cb(feitos, total, d, i, N_ITENS, ud)) {
        res->parado = 1;
        break;
      }
    }
    fclose(fd);
    fclose(fo);
    if (!res->parado) res->copiados++;
  }
  if (!res->parado) {
    char ae[300];
    inst_item_t a = {"AUTOEXEC.BAT", RAIZ, 0};
    destino_de(&a, raiz, ae, (int)sizeof(ae));
    if (k_fsize(ae) >= 0 && path_no_autoexec(ae) == 0) res->path_ok = 1;
    else {
      res->erros++;
      anota(res, "nao pus D:\\SODOS\\BIN no PATH de", ae);
    }
    if (!raiz || !raiz[0]) path_agora();
    if (cb) cb(total, total, "", N_ITENS, N_ITENS, ud);
  }
  return res->erros ? -1 : 0;
}
