/* play.c - PLAY.EXE: toca videos do YouTube no WIN pelo link publico, com som.
 *
 * O SODOS nao decodifica H.264/VP9/AAC nem fala com o YouTube; o Linux do WSL
 * (SET WSL=1) faz isso. O PLAY.EXE pede pela caixa de comandos do WSL
 * (wsl/wslbox.h, a mesma do WSL exec):
 *   python3 /d/develop/play/YTPLAY.PY 'LINK' LARGURA QPS QUALIDADE HZ CANAIS
 * e recebe pelo anel de stdout pacotes com JPEG (MJPEG) e som (PCM de 16
 * bits): decodifica os JPEG com o IMG.DLL e mostra na janela; o som vai para
 * o kernel (k_audio_*, SET AUDIO=1) e serve de relogio do video. O stderr
 * traz as linhas de estado (PLAY titulo/duracao/som/erro/fim).
 *
 * A caixa e do WSL.EXE, um programa LP64 (SOX2): os ponteiros dela tem 8
 * bytes. Este programa e x32 (cc do SODOS), entao acessa a caixa por
 * deslocamentos fixos (conferidos com o gcc no wslbox.h, versao 1). O cc nao
 * tem operacao atomica: a vaga e pega de cima para baixo, o lado oposto do
 * WSL exec e do BASH.EXE, que pegam de baixo para cima com cmpxchg.
 *
 *   play                       janela: cole o link (Ctrl+V) e Enter
 *   play LINK [LARGURA] [QPS]  ja comeca a tocar (padrao 480 e 15)
 *   play -t LINK [SEGUNDOS]    teste sem janela: decodifica o que chega (vazao)
 *   play -r LINK [SEGUNDOS]    teste sem janela no ritmo do video (fila cheia,
 *                              como a janela): mostra recebidos/perdidos/paradas
 * Espaco pausa, + e - mudam o volume, Enter toca de novo no fim, Esc fecha.
 * SET PLAYSOM=0 desliga o som; SET PLAYSOM=HZ muda a taxa (padrao 24000,
 * estereo; HZ seguido de M = mono).
 *
 *   cc -I D:\ -L D:\ -o PLAY.EXE PLAY.C gui.dll img.dll
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>
#include <gui.h>
#include <img.h>

/* Exports do kernel que nao estao no so.h (wsl/sodos_api.h). Para um programa
 * x32 o kernel so garante os 32 bits de baixo do retorno (eax): o resto do
 * rax pode vir com lixo. Por isso sao ponteiros de 32 bits aqui, e a caixa e
 * conferida pelo magic e pela versao. */
void* k_svc_get(const char* name);
void* k_malloc_shared(unsigned n);
void k_free_shared(void* p);

/* Som do kernel (kernel/audio.c, docs/AUDIO.md; so.h a partir do build 61).
 * Declarados aqui tambem porque o D:\SO.H instalado pode ser mais velho. */
int k_audio_open(int rate, int channels, int bits);
int k_audio_write(const void* buf, int bytes);
int k_audio_close(int wait);
int k_audio_pending(void);
int k_audio_volume(int pct);

typedef unsigned long long u64;

/* ---------- caixa de comandos do WSL (layout LP64 do wslbox.h v1) ---------- */
#define BOX_MAGIC 0x584F4257u
#define BOX_VERSION 1
#define B_VERSION 4
#define B_ALIVE 8
#define B_READY 12
#define B_WAKE 24
#define B_SLOT 120
#define N_SLOTS 8
#define SLOT_SZ 131400
#define S_STATE 0
#define S_SEQ 4
#define S_GEN 8
#define S_FLAGS 12
#define S_CWD 16
#define S_CMD 280
#define S_CMDLEN 288
#define S_FED 292
#define S_KILL 296
#define S_KILLED 300
#define S_EXIT 304
#define S_OUT 308
#define S_ERR 65852
#define R_HEAD 0
#define R_TAIL 4
#define R_BUF 8
#define RING 65536u
#define SL_FREE 0
#define SL_CLAIM 1
#define SL_POSTED 2
#define SL_SENT 3
#define SL_DONE 4

#define I32(p, off) (*(volatile int*)((p) + (off)))
#define U32(p, off) (*(volatile unsigned*)((p) + (off)))

typedef struct {
  unsigned char* box;
  unsigned char* slot;
  char* cmd;
  int ativo;   /* pedido postado e ainda nao devolvido */
  int fim;     /* o comando terminou (SL_DONE) */
  int codigo;
  int matando;
  unsigned t_kill;
} job_t;

static unsigned char* caixa(void) {
  unsigned char* b = (unsigned char*)k_svc_get("WSL");
  if (!b) return 0;
  if (U32(b, 0) != BOX_MAGIC || I32(b, B_VERSION) != BOX_VERSION) return 0;
  return b;
}

static void acorda(unsigned char* b) {
  unsigned lo = U32(b, B_WAKE), hi = U32(b, B_WAKE + 4);
  if (lo && !hi) k_futex_wake((volatile int*)lo, 1);
}

/* 1 = o servico do WSL esta pronto; 0 = ainda nao (liga com SET WSL=1). */
static int wsl_pronto(void) {
  char v[8];
  unsigned char* b = caixa();
  if (b && I32(b, B_ALIVE) && I32(b, B_READY)) return 1;
  if (k_getenv("WSL", v, (int)sizeof v) <= 0 || v[0] != '1') k_setenv("WSL", "1");
  return 0;
}

/* Posta o comando numa vaga livre. 0 ok; -1 sem caixa; -2 sem vaga; -3 sem memoria. */
static int job_inicia(job_t* j, const char* cmd) {
  unsigned char* b = caixa();
  unsigned char* s = 0;
  char* m;
  int i, n = (int)strlen(cmd);
  memset(j, 0, sizeof *j);
  if (!b || !I32(b, B_ALIVE) || !I32(b, B_READY)) return -1;
  for (i = N_SLOTS - 1; i >= 0 && !s; i--) {
    unsigned char* t = b + B_SLOT + i * SLOT_SZ;
    if (I32(t, S_STATE) == SL_FREE) {
      I32(t, S_STATE) = SL_CLAIM;
      s = t;
    }
  }
  if (!s) return -2;
  m = (char*)k_malloc_shared((unsigned)n + 1);
  if (!m) {
    I32(s, S_STATE) = SL_FREE;
    return -3;
  }
  j->cmd = m;
  memcpy(j->cmd, cmd, (size_t)n + 1);
  U32(s, S_CMD) = (unsigned)m;   /* ponteiro de 64 bits do lado LP64 */
  U32(s, S_CMD + 4) = 0;
  U32(s, S_CMDLEN) = (unsigned)n;
  I32(s, S_FLAGS) = 0;
  s[S_CWD] = 0;   /* o agente usa /root; o comando tem caminhos completos */
  U32(s, S_OUT + R_HEAD) = 0;
  U32(s, S_OUT + R_TAIL) = 0;
  U32(s, S_ERR + R_HEAD) = 0;
  U32(s, S_ERR + R_TAIL) = 0;
  I32(s, S_KILL) = 0;
  I32(s, S_KILLED) = 0;
  I32(s, S_EXIT) = 0;
  U32(s, S_GEN) = U32(s, S_GEN) + 1;
  I32(s, S_STATE) = SL_POSTED;
  acorda(b);
  j->box = b;
  j->slot = s;
  j->ativo = 1;
  return 0;
}

static int anel_usado(unsigned char* r) { return (int)(U32(r, R_HEAD) - U32(r, R_TAIL)); }

/* Le ate max bytes do anel (stdout ou stderr do comando). */
static int anel_le(unsigned char* r, unsigned char* dst, int max) {
  unsigned h = U32(r, R_HEAD), t = U32(r, R_TAIL);
  int n = 0;
  while (t != h && n < max) {
    unsigned off = t % RING, k = h - t;
    if (k > RING - off) k = RING - off;
    if ((int)k > max - n) k = (unsigned)(max - n);
    memcpy(dst + n, r + R_BUF + off, k);
    n += (int)k;
    t += k;
  }
  U32(r, R_TAIL) = t;
  return n;
}

/* Atualiza fim/codigo. Chame depois de drenar os aneis. */
static void job_olha(job_t* j) {
  if (!j->ativo || j->fim) return;
  if (I32(j->slot, S_STATE) == SL_DONE) {
    j->fim = 1;
    j->codigo = I32(j->slot, S_EXIT);
  } else if (!I32(j->box, B_ALIVE)) {
    j->fim = 1;
    j->codigo = 255;
  }
}

static void job_mata(job_t* j) {
  if (!j->ativo || j->fim || j->matando) return;
  I32(j->slot, S_KILL) = 1;
  j->matando = 1;
  j->t_kill = k_ms();
  acorda(j->box);
}

/* Devolve a vaga. So quando o comando ja terminou (o servico escreve nela). */
static void job_solta(job_t* j) {
  if (!j->ativo) return;
  if (!j->fim) return;
  if (j->cmd) k_free_shared(j->cmd);
  U32(j->slot, S_CMD) = 0;
  U32(j->slot, S_CMD + 4) = 0;
  I32(j->slot, S_STATE) = SL_FREE;
  j->cmd = 0;
  j->ativo = 0;
}

/* ---------- fluxo MJPEG -> fila de quadros ---------- */
#define FQ_MAX 600
#define FQ_BYTES (12 * 1024 * 1024)
#define PB_CAP (2 * 1024 * 1024)

static unsigned char* fq_buf[FQ_MAX];
static int fq_len[FQ_MAX];
static int fq_ini, fq_n, fq_bytes;
static unsigned char* pb;     /* bytes do stdout ainda sem quadro inteiro */
static int pb_n, pb_scan;
static int q_total;           /* quadros recebidos desde o inicio */
static int q_perdidos;        /* quadros jogados fora com a fila cheia (deve ficar 0) */

static void fila_limpa(void) {
  while (fq_n > 0) {
    free(fq_buf[fq_ini]);
    fq_ini = (fq_ini + 1) % FQ_MAX;
    fq_n--;
  }
  fq_ini = fq_n = fq_bytes = 0;
  pb_n = pb_scan = 0;
  q_total = 0;
  q_perdidos = 0;
}

static int fila_cheia(void) { return fq_n >= FQ_MAX - 1 || fq_bytes >= FQ_BYTES; }

static void fila_poe(const unsigned char* p, int n) {
  unsigned char* c;
  if (fq_n >= FQ_MAX) {
    q_perdidos++;
    return;
  }
  c = (unsigned char*)malloc((size_t)n);
  if (!c) {
    q_perdidos++;
    return;
  }
  memcpy(c, p, (size_t)n);
  fq_buf[(fq_ini + fq_n) % FQ_MAX] = c;
  fq_len[(fq_ini + fq_n) % FQ_MAX] = n;
  fq_n++;
  fq_bytes += n;
  q_total++;
}

/* Tira da fila o proximo JPEG (o chamador libera com free). */
static unsigned char* fila_tira(int* n) {
  unsigned char* c;
  if (fq_n <= 0) return 0;
  c = fq_buf[fq_ini];
  *n = fq_len[fq_ini];
  fq_bytes -= *n;
  fq_ini = (fq_ini + 1) % FQ_MAX;
  fq_n--;
  return c;
}

/* Separa os JPEG do que chegou: de FF D8 ate FF D9. Com a fila cheia para:
 * os quadros que sobram ficam no pb ate abrir lugar (antes eram jogados fora,
 * e o video pulava pedacos quando a fila enchia). */
static void separa_quadros(void) {
  /* ini anda pelo pb; o resto e puxado para o comeco uma vez so, no fim
   * (puxar a cada quadro custava ate 512 KB de memmove por quadro). pb_scan:
   * onde continuar procurando o FF D9 do quadro que comeca em pb[0]. */
  int ini = 0;
  for (;;) {
    int s = -1, e = -1, i;
    if (fila_cheia()) break;
    for (i = ini; i + 1 < pb_n; i++)
      if (pb[i] == 0xFF && pb[i + 1] == 0xD8) {
        s = i;
        break;
      }
    if (s < 0) {   /* sem comeco de quadro: guarda so um FF do fim */
      ini = (pb_n > ini && pb[pb_n - 1] == 0xFF) ? pb_n - 1 : pb_n;
      pb_scan = 0;
      break;
    }
    if (s != 0) pb_scan = 0;   /* pb_scan so vale para o quadro em pb[0] */
    ini = s;
    i = pb_scan > s + 2 ? pb_scan : s + 2;
    for (; i + 1 < pb_n; i++)
      if (pb[i] == 0xFF && pb[i + 1] == 0xD9) {
        e = i + 2;
        break;
      }
    if (e < 0) {
      pb_scan = pb_n - 1 > s + 2 ? pb_n - 1 : s + 2;
      if (pb_n - s >= PB_CAP) {   /* JPEG grande demais: descarta */
        ini = pb_n;
        pb_scan = 0;
      }
      break;
    }
    fila_poe(pb + s, e - s);
    ini = e;
    pb_scan = 0;
  }
  if (ini > 0) {
    memmove(pb, pb + ini, (size_t)(pb_n - ini));
    pb_n -= ini;
    pb_scan = pb_scan > ini ? pb_scan - ini : 0;
  }
}

/* ---------- estado vindo do stderr (linhas PLAY ...) ---------- */
static char g_titulo[200];
static char g_erro[240];
static char g_info[240];
static int g_duracao;
static int g_fim_rc = -1;
static char g_fim[64];   /* "yt-dlp ffmpeg" da linha PLAY fim (por que parou) */
static char ln_buf[512];
static int ln_n;

static void linha_estado(const char* l) {
  if (!strncmp(l, "PLAY titulo ", 12)) {
    snprintf(g_titulo, sizeof g_titulo, "%s", l + 12);
  } else if (!strncmp(l, "PLAY duracao ", 13)) {
    g_duracao = atoi(l + 13);
  } else if (!strncmp(l, "PLAY formato ", 13)) {
    snprintf(g_info, sizeof g_info, "formato %s", l + 13);
  } else if (!strncmp(l, "PLAY erro ", 10)) {
    if (!g_erro[0]) snprintf(g_erro, sizeof g_erro, "%s", l + 10);
  } else if (!strncmp(l, "PLAY fim ", 9)) {
    g_fim_rc = atoi(l + 9);
    snprintf(g_fim, sizeof g_fim, "%s", l + 9);
  } else if (l[0] && !g_erro[0]) {
    snprintf(g_info, sizeof g_info, "%s", l);
  }
}

static void estado_le(job_t* j) {
  unsigned char tmp[1024];
  int n, i;
  while ((n = anel_le(j->slot + S_ERR, tmp, (int)sizeof tmp)) > 0) {
    for (i = 0; i < n; i++) {
      if (tmp[i] == '\n') {
        ln_buf[ln_n] = 0;
        linha_estado(ln_buf);
        ln_n = 0;
      } else if (tmp[i] != '\r' && ln_n < (int)sizeof ln_buf - 1) {
        ln_buf[ln_n++] = (char)tmp[i];
      }
    }
  }
}

/* ---------- fluxo com som: pacotes H/V/A do YTPLAY.PY ---------- */
/* Com HZ no comando, o YTPLAY.PY manda pacotes: tipo (H, V ou A) + tamanho
 * (3 bytes, little-endian) + dados. H vem primeiro ("SOM hz canais"; hz 0 =
 * o video nao tem som), V e um pedaco do MJPEG (os JPEG atravessam pacotes)
 * e A um pedaco do PCM s16le. Sem HZ, ou com um YTPLAY.PY velho, vem o MJPEG
 * cru, como antes: o primeiro byte e FF, nao H. */
static int g_som_hz = 24000, g_som_ch = 2;   /* SET PLAYSOM */
static char g_som_msg[80];    /* por que esta sem som (barra) */
static int g_vol = -1;        /* volume da placa (barra) */
static int mx_modo;           /* 0 = nao sabe ainda; 1 = MJPEG cru; 2 = pacotes */
static unsigned char mx_cab[4];
static int mx_cab_n, mx_tipo, mx_falta;
static char mx_h[64];
static int mx_h_n;

/* PCM que chegou e ainda nao foi para o kernel. som_wr e som_rd contam os
 * bytes desde o comeco do video. O anel guarda mais som_guarda bytes ja
 * entregues: a pausa corta a fila da placa e volta o som_rd, sem perder nada.
 * 60 s de som: a fila de quadros tem ~40 s e o pb mais uns 10. */
#define SOM_SEG 60
#define SOM_FILA_MS 170       /* na fila do kernel (o driver aceita ate 200 ms) */
static unsigned char* som_buf;
static unsigned som_cap, som_guarda;
static u64 som_wr, som_rd;
static int som_hz, som_ch, som_bps, som_quadro;   /* do pacote H */
static int som_video;         /* o H disse que o video tem som */
static int som_fora;          /* som largado neste video: o que chega vai fora */
static u64 som_total;         /* bytes de som recebidos */
static int som_aberto;        /* k_audio_open feito */
static u64 som_dado;          /* bytes entregues ao kernel neste video */
static int cl_som;            /* o relogio do video e o som (senao o TSC) */

static unsigned som_livre(void) {
  u64 usado = som_wr - som_rd + som_guarda;
  return usado >= som_cap ? 0 : (unsigned)(som_cap - usado);
}

static int som_ms_buf(void) {
  return som_bps ? (int)((som_wr - som_rd) * 1000u / (u64)som_bps) : 0;
}

static void som_cabecalho(const char* h) {
  const char* p;
  int hz, ch;
  if (strncmp(h, "SOM ", 4)) return;
  hz = atoi(h + 4);
  p = strchr(h + 4, ' ');
  ch = p ? atoi(p + 1) : 2;
  if (hz == 0 && som_buf) snprintf(g_som_msg, sizeof g_som_msg, "sem som: o video nao tem som");
  if (hz < 8000 || hz > 48000 || (ch != 1 && ch != 2) || !som_buf) return;   /* sem som */
  som_hz = hz;
  som_ch = ch;
  som_quadro = ch * 2;
  som_bps = hz * som_quadro;
  som_video = 1;
}

/* Le do anel de stdout para o destino de cada pacote, ate limite bytes. Para
 * quando o destino esta cheio (fila de quadros, pb ou anel do som: o Linux
 * espera) ou o anel do stdout esta vazio. */
static void le_fluxo(job_t* j, int limite) {
  static unsigned char lixo[4096];
  unsigned char* r = j->slot + S_OUT;
  int lidos = 0;
  while (lidos < limite) {
    int n;
    if (mx_modo == 0) {   /* o primeiro byte diz o formato */
      if (anel_usado(r) < 1) break;
      mx_modo = r[R_BUF + U32(r, R_TAIL) % RING] == 'H' ? 2 : 1;
    }
    if (mx_modo == 1 || (mx_falta > 0 && mx_tipo == 'V')) {
      int cabe = PB_CAP - pb_n;
      if (fila_cheia() || pb_n > PB_CAP - 65536) break;
      if (mx_modo == 2 && cabe > mx_falta) cabe = mx_falta;
      n = anel_le(r, pb + pb_n, cabe);
      if (n <= 0) break;
      pb_n += n;
      if (mx_modo == 2) mx_falta -= n;
      lidos += n;
      separa_quadros();
    } else if (mx_falta == 0) {   /* cabecalho do pacote */
      n = anel_le(r, mx_cab + mx_cab_n, 4 - mx_cab_n);
      if (n <= 0) break;
      mx_cab_n += n;
      lidos += n;
      if (mx_cab_n < 4) continue;
      mx_cab_n = 0;
      mx_tipo = mx_cab[0];
      mx_falta = mx_cab[1] | mx_cab[2] << 8 | mx_cab[3] << 16;
      if (mx_tipo == 'H') mx_h_n = 0;
    } else if (mx_tipo == 'A' && som_buf && !som_fora) {
      unsigned livre = som_livre(), off = (unsigned)(som_wr % som_cap), k = som_cap - off;
      if (livre < 4) break;
      if (k > livre) k = livre;
      if (k > (unsigned)mx_falta) k = (unsigned)mx_falta;
      n = anel_le(r, som_buf + off, (int)k);
      if (n <= 0) break;
      som_wr += (unsigned)n;
      som_total += (unsigned)n;
      mx_falta -= n;
      lidos += n;
    } else {   /* H, som que nao vai tocar ou tipo desconhecido */
      n = anel_le(r, lixo, mx_falta < (int)sizeof lixo ? mx_falta : (int)sizeof lixo);
      if (n <= 0) break;
      if (mx_tipo == 'A') som_total += (unsigned)n;
      if (mx_tipo == 'H' && mx_h_n + n < (int)sizeof mx_h) {
        memcpy(mx_h + mx_h_n, lixo, (size_t)n);
        mx_h_n += n;
      }
      mx_falta -= n;
      lidos += n;
      if (mx_tipo == 'H' && mx_falta == 0) {
        mx_h[mx_h_n] = 0;
        som_cabecalho(mx_h);
      }
    }
  }
}

/* Puxa o que chegou: estado sempre; quadros e som enquanto tem lugar
 * (cheio = o Linux espera, a caixa segura). */
static int g_vezes = 8;   /* blocos de 64 KB por chamada (a janela usa 2 tocando) */
static void bomba(job_t* j) {
  if (!j->ativo) return;
  estado_le(j);
  separa_quadros();   /* primeiro o que ja estava no pb, na ordem */
  le_fluxo(j, g_vezes * 65536);
  job_olha(j);
  if (j->fim) {
    estado_le(j);
    le_fluxo(j, 0x7FFFFFFF);
  }
}

/* ---------- link e comando ---------- */
static char g_dir_linux[200] = "/d/develop/play";

/* SET PLAYDIR=D:\OUTRA\PASTA muda onde ficam o YTPLAY.PY e linux\. */
static void le_playdir(void) {
  char v[180];
  int i, o;
  if (k_getenv("PLAYDIR", v, (int)sizeof v) <= 0 || !v[0]) return;
  if (v[1] == ':') {
    o = snprintf(g_dir_linux, sizeof g_dir_linux, "/%c", v[0] | 0x20);
    for (i = 2; v[i] && o < (int)sizeof g_dir_linux - 1; i++)
      g_dir_linux[o++] = v[i] == '\\' ? '/' : v[i];
    g_dir_linux[o] = 0;
  } else {
    snprintf(g_dir_linux, sizeof g_dir_linux, "%s", v);
  }
  o = (int)strlen(g_dir_linux);
  while (o > 1 && g_dir_linux[o - 1] == '/') g_dir_linux[--o] = 0;
}

static int id_char(int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '_';
}

/* Normaliza o link em out. 0 ok; -1 invalido (vazio, espaco, aspa...). */
static int link_ok(const char* in, char* out, int cap) {
  int i, n;
  while (*in == ' ') in++;
  n = (int)strlen(in);
  while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == '\r' || in[n - 1] == '\n')) n--;
  if (n < 5 || n > 400) return -1;
  for (i = 0; i < n; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c <= ' ' || c >= 127 || c == '\'' || c == '"' || c == '\\' || c == '`') return -1;
  }
  if (n == 11) {
    for (i = 0; i < 11 && id_char((unsigned char)in[i]); i++) {
    }
    if (i == 11) {   /* so o codigo do video */
      const char* base = "https://www.youtube.com/watch?v=";
      int bl = (int)strlen(base);
      if (bl + 12 > cap) return -1;
      memcpy(out, base, (size_t)bl);
      memcpy(out + bl, in, 11);
      out[bl + 11] = 0;
      return 0;
    }
  }
  if (strncmp(in, "http://", 7) && strncmp(in, "https://", 8)) {
    if (n + 9 > cap) return -1;
    memcpy(out, "https://", 8);
    memcpy(out + 8, in, (size_t)n);
    out[8 + n] = 0;
    return 0;
  }
  if (n + 1 > cap) return -1;
  memcpy(out, in, (size_t)n);
  out[n] = 0;
  return 0;
}

static int g_largura = 480;
static int g_qps = 15;
static int g_qualidade = 7;

/* SET PLAYSOM=0 desliga o som; PLAYSOM=HZ (8000 a 48000) muda a taxa; com M
 * no fim (PLAYSOM=16000M), mono. A placa so tem 48 kHz e acima: o kernel
 * converte (24000 dobra certinho). 24 kHz estereo = 96 KB/s pela caixa. */
static void le_playsom(void) {
  char v[16];
  if (k_getenv("PLAYSOM", v, (int)sizeof v) <= 0 || !v[0]) return;
  g_som_hz = atoi(v);
  if (g_som_hz && (g_som_hz < 8000 || g_som_hz > 48000)) g_som_hz = 24000;
  if (strchr(v, 'm') || strchr(v, 'M')) g_som_ch = 1;
}


/* 1 = vale pedir o som ao Linux: tem placa. Sem AUDIO no ambiente, liga
 * (SET AUDIO=1), como o PLAY faz com o WSL; AUDIO=0 e respeitado. */
static int som_placa(void) {
  char v[16];
  if (!g_som_hz) {
    snprintf(g_som_msg, sizeof g_som_msg, "sem som (PLAYSOM=0)");
    return 0;
  }
  if (k_getenv("AUDIO", v, (int)sizeof v) <= 0 || !v[0]) {
    k_setenv("AUDIO", "1");
  } else if (v[0] == '0' && !v[1]) {
    snprintf(g_som_msg, sizeof g_som_msg, "sem som (AUDIO=0)");
    return 0;
  }
  if (k_audio_volume(-1) < 0) {
    snprintf(g_som_msg, sizeof g_som_msg, "sem som: sem placa (veja o comando AUDIO)");
    return 0;
  }
  return 1;
}

/* com_som: pede o som ao Linux (a janela so pede se tem placa). */
static int inicia_video(job_t* j, const char* link, int com_som) {
  char cmd[700];
  int hz = 0;
  fila_limpa();
  g_titulo[0] = g_erro[0] = g_info[0] = 0;
  g_duracao = 0;
  g_fim_rc = -1;
  g_fim[0] = 0;
  ln_n = 0;
  mx_modo = mx_cab_n = mx_tipo = mx_falta = mx_h_n = 0;
  if (som_aberto) k_audio_close(0);
  som_aberto = 0;
  som_wr = som_rd = som_total = som_dado = 0;
  som_video = som_fora = cl_som = 0;
  som_bps = 0;
  if (com_som && g_som_hz) {
    unsigned cap = (unsigned)(g_som_hz * g_som_ch * 2) * SOM_SEG;
    if (som_buf && som_cap != cap) {
      free(som_buf);
      som_buf = 0;
    }
    if (!som_buf) {
      som_buf = (unsigned char*)malloc(cap);
      som_cap = som_buf ? cap : 0;
      som_guarda = (unsigned)(g_som_hz * g_som_ch * 2) & ~3u;   /* 1 s */
    }
    if (som_buf) hz = g_som_hz;
    else snprintf(g_som_msg, sizeof g_som_msg, "sem som: sem memoria");
  }
  snprintf(cmd, sizeof cmd, "python3 %s/YTPLAY.PY '%s' %d %d %d %d %d", g_dir_linux, link,
           g_largura, g_qps, g_qualidade, hz, g_som_ch);
  return job_inicia(j, cmd);
}

static void som_fecha(int espera);

/* Corta o som, espera o comando acabar (ate ms) e devolve a vaga. */
static void encerra(job_t* j, int ms) {
  unsigned t0 = k_ms();
  unsigned char lixo[4096];
  som_fecha(0);
  cl_som = 0;
  if (!j->ativo) return;
  job_mata(j);
  while (!j->fim && (int)(k_ms() - t0) < ms) {
    while (anel_le(j->slot + S_OUT, lixo, (int)sizeof lixo) > 0) {
    }
    estado_le(j);
    job_olha(j);
    if (!j->fim) k_futex_wait((volatile int*)(j->slot + S_SEQ), I32(j->slot, S_SEQ), 20);
  }
  job_solta(j);
}

/* ---------- relogio do video ---------- */
/* O k_ms conta as interrupcoes do timer e atrasa quando o kernel perde ticks
 * (medido em 24/11/26: 4,5% devagar com a VM do WSL ligada; o video saia 4,5%
 * lento). O ritmo dos quadros usa o TSC, calibrado pelas viradas de segundo
 * do relogio do CMOS (k_time) enquanto o PLAY roda: o erro cai com o tempo
 * (~0,7% aos 4 s, ~0,05% aos 60 s). Antes de 4 s de calibracao vale o k_ms. */
static int rl_seg_ant = -1;   /* ultimo segundo do CMOS visto */
static int rl_seg0 = -1;      /* segundo da 1a virada vista */
static u64 rl_tsc0;           /* TSC nessa virada */
static unsigned rl_kms0;      /* k_ms nessa virada (para medir o erro dele) */
static u64 rl_hz;             /* ciclos do TSC por segundo; 0 = sem calibrar */
static u64 rl_base_tsc;       /* relogio() = rl_base_ms + (tsc - rl_base_tsc) */
static unsigned rl_base_ms;
static int rl_kms_erro;       /* erro do k_ms em decimos de % (-45 = 4,5% lento) */

static u64 tsc_le(void) {
  unsigned hi = 0, lo = k_rdtsc(&hi);
  return ((u64)hi << 32) | lo;
}

static unsigned relogio_em(u64 tsc) {
  if (!rl_hz) return k_ms();
  if (tsc < rl_base_tsc) return rl_base_ms;   /* TSC de outra CPU atrasado */
  return rl_base_ms + (unsigned)((tsc - rl_base_tsc) * (u64)1000 / rl_hz);
}

static unsigned relogio(void) { return relogio_em(tsc_le()); }

/* Chame a cada volta do laco: acha as viradas de segundo do CMOS. */
static void relogio_calibra(void) {
  int t = k_time(), s, ds;
  u64 tsc;
  unsigned kms;
  if (t < 0) return;
  s = t / 10000 * 3600 + t / 100 % 100 * 60 + t % 100;
  if (s == rl_seg_ant) return;
  tsc = tsc_le();
  kms = k_ms();
  if (rl_seg_ant < 0) {   /* o primeiro valor nao e virada: nao se sabe quando comecou */
    rl_seg_ant = s;
    return;
  }
  rl_seg_ant = s;
  if (rl_seg0 < 0 || tsc <= rl_tsc0) {
    rl_seg0 = s;
    rl_tsc0 = tsc;
    rl_kms0 = kms;
    return;
  }
  ds = s - rl_seg0;
  if (ds < 0) ds += 86400;
  if (ds < 4) return;
  {
    u64 hz = (tsc - rl_tsc0) / (u64)ds;
    unsigned agora;
    if (hz < (u64)1000000) return;
    agora = relogio_em(tsc);   /* sem salto: o relogio continua de onde esta */
    rl_base_ms = agora;
    rl_base_tsc = tsc;
    rl_hz = hz;
    rl_kms_erro = (int)((kms - rl_kms0) / (unsigned)ds) - 1000;
  }
}

/* ---------- relogio do conteudo e som ---------- */
/* cl_agora: em que ms do video estamos. Com som, quem manda e a placa: o que
 * ja tocou = entregue ao kernel - o que ainda esta na fila dele (o
 * k_audio_pending vem da posicao do DMA). Sem som (sem placa, video mudo,
 * som acabou antes), o TSC calibrado, a partir de cl_pos0 em cl_t0. */
static unsigned cl_pos0, cl_t0;

static unsigned quadro_ms(int n) { return (unsigned)((u64)(unsigned)n * 1000u / (unsigned)g_qps); }

static unsigned som_pos_ms(void) {
  int p = som_aberto ? k_audio_pending() : 0;
  u64 tocou = som_dado > (u64)(unsigned)p ? som_dado - (u64)(unsigned)p : 0;
  return som_bps ? (unsigned)(tocou * 1000u / (u64)som_bps) : 0;
}

static unsigned cl_agora(unsigned agora) {
  return cl_som ? som_pos_ms() : cl_pos0 + (agora - cl_t0);
}

static void cl_tsc_de(unsigned pos, unsigned agora) {
  cl_som = 0;
  cl_pos0 = pos;
  cl_t0 = agora;
}

static void som_fecha(int espera) {
  if (som_aberto) k_audio_close(espera);
  som_aberto = 0;
}

/* 0 ok. Sem placa (driver nulo: o som iria para lugar nenhum e a fila nunca
 * encheria), com outro programa tocando ou com a placa recusando: <0. */
static int som_abre(void) {
  int rc;
  if (som_aberto) return 0;
  rc = k_audio_open(som_hz, som_ch, 16);
  if (rc == 0 && k_audio_volume(-1) < 0) {
    k_audio_close(0);
    rc = -4;
  }
  if (rc != 0) {
    snprintf(g_som_msg, sizeof g_som_msg, "%s",
             rc == -2   ? "sem som: outro programa esta tocando"
             : rc == -3 ? "sem som: a placa recusou o formato"
             : rc == -4 ? "sem som: sem placa (veja o comando AUDIO)"
                        : "sem som: erro ao abrir");
    return rc;
  }
  som_aberto = 1;
  g_vol = k_audio_volume(-1);
  return 0;
}

/* Larga o som deste video: o relogio passa ao TSC de onde esta. msg 0 = a
 * mensagem que ja esta em g_som_msg. */
static void som_larga(const char* msg) {
  unsigned agora = relogio(), pos = cl_agora(agora);
  som_fecha(0);
  som_fora = 1;
  som_rd = som_wr;
  cl_tsc_de(pos, agora);
  if (msg) snprintf(g_som_msg, sizeof g_som_msg, "%s", msg);
}

/* Mantem a fila do kernel com SOM_FILA_MS: escreve so o que cabe (o
 * k_audio_write dormiria com a fila cheia e seguraria a janela). */
static void som_alimenta(void) {
  int pend, alvo, vez;
  if (!som_aberto) return;
  pend = k_audio_pending();
  alvo = som_bps / 1000 * SOM_FILA_MS;
  for (vez = 0; vez < 8 && pend < alvo && som_wr > som_rd; vez++) {
    unsigned off = (unsigned)(som_rd % som_cap), k = som_cap - off;
    u64 tem = som_wr - som_rd;
    int w;
    if ((u64)k > tem) k = (unsigned)tem;
    if (k > (unsigned)(alvo - pend)) k = (unsigned)(alvo - pend);
    k -= k % (unsigned)som_quadro;
    if (!k) break;
    w = k_audio_write(som_buf + off, (int)k);
    if (w < 0) {
      som_larga("sem som: a placa parou de tocar");
      return;
    }
    if (w == 0) break;
    som_rd += (unsigned)w;
    som_dado += (unsigned)w;
    pend += w;
  }
}

/* Pausa: corta a fila da placa e volta o anel ao que ainda nao foi ouvido. */
static void som_pausa(void) {
  int p;
  if (!som_aberto) return;
  p = k_audio_pending();
  if (p < 0) p = 0;
  if ((u64)(unsigned)p > som_dado) p = (int)som_dado;
  if ((unsigned)p > som_guarda) p = (int)som_guarda;
  p -= p % som_quadro;
  som_fecha(0);
  som_rd -= (unsigned)p;
  som_dado -= (unsigned)p;
}

/* ---------- quadro escalado para a janela ---------- */
/* O quadro e escalado uma vez quando chega (ou quando a janela muda) e vai
 * para a tela numa chamada so de k_gui_pixels. O gui_image faria uma divisao
 * por pixel e uma chamada ao kernel por linha, a cada redesenho. */
static unsigned* g_esc;   /* RGBA no tamanho do retangulo de destino */
static int g_esc_cap, g_esc_w, g_esc_h, g_esc_ger = -1;
static int* g_xi;
static int g_xi_cap;

static int escala(const img_t* im, int w, int h, int ger) {
  int x, y, sy, sy_ant = -1;
  unsigned* d;
  if (!im->rgba || im->w < 1 || im->h < 1 || w < 1 || h < 1) return -1;
  if (ger == g_esc_ger && w == g_esc_w && h == g_esc_h) return 0;
  if (w * h > g_esc_cap) {
    free(g_esc);
    g_esc = (unsigned*)malloc((size_t)w * (size_t)h * 4);
    g_esc_cap = g_esc ? w * h : 0;
    if (!g_esc) return -1;
  }
  if (w > g_xi_cap) {
    free(g_xi);
    g_xi = (int*)malloc((size_t)w * sizeof(int));
    g_xi_cap = g_xi ? w : 0;
    if (!g_xi) return -1;
  }
  for (x = 0; x < w; x++) {
    int sx = x * im->w / w;
    g_xi[x] = sx < im->w ? sx : im->w - 1;
  }
  d = g_esc;
  for (y = 0; y < h; y++, d += w) {
    const unsigned* src;
    sy = y * im->h / h;
    if (sy >= im->h) sy = im->h - 1;
    if (sy == sy_ant) {   /* linha repetida (ampliacao): copia a de cima */
      memcpy(d, d - w, (size_t)w * 4);
      continue;
    }
    src = (const unsigned*)im->rgba + (size_t)sy * (size_t)im->w;
    for (x = 0; x < w; x++) d[x] = src[g_xi[x]];
    sy_ant = sy;
  }
  g_esc_w = w;
  g_esc_h = h;
  g_esc_ger = ger;
  return 0;
}

static void mostra_quadro(const img_t* im, int ger, int x, int y, int w, int h) {
  if (escala(im, w, h, ger) == 0) k_gui_pixels(x, y, w, h, g_esc, w * 4);
}

/* ---------- modo de teste (-t / -r): sem janela, pela PONTE ---------- */
static unsigned g_dec_ms, g_esc_ms;
static int g_dec_n;

/* -t: decodifica tudo o que chega, o mais rapido possivel (vazao).
 * -r: mostra no ritmo do video (g_qps), como a janela: a fila enche e o
 *     Linux espera; se a fila esvazia, junta g_qps quadros de novo (parada). */
static int teste(const char* link, int segundos, int ritmo) {
  job_t j;
  char url[440];
  unsigned t0, t_toca = 0, t_log;
  int rc, primeiro = 0, liga = 0, mostrados = 0, base = 0, paradas = 0;
  img_t im;
  if (link_ok(link, url, (int)sizeof url) != 0) {
    printf("link invalido\n");
    return 2;
  }
  pb = (unsigned char*)malloc(PB_CAP);
  if (!pb) return 3;
  t0 = k_ms();
  while (!wsl_pronto()) {
    if (!liga) printf("ligando o WSL...\n");
    liga = 1;
    if ((int)(k_ms() - t0) > 120000) {
      printf("o WSL nao ficou pronto\n");
      return 4;
    }
    k_yield();
  }
  rc = inicia_video(&j, url, g_som_hz != 0);   /* o som chega, conta, mas nao toca */
  printf("job %d vaga %d\n", rc, rc == 0 ? (int)((j.slot - j.box - B_SLOT) / SLOT_SZ) : -1);
  if (rc != 0) return 5;
  t0 = k_ms();
  t_log = relogio();
  while (!(j.fim && fq_n == 0) && (int)(k_ms() - t0) < segundos * 1000) {
    unsigned agora;
    int devidos;
    bomba(&j);
    relogio_calibra();
    agora = relogio();
    if (!ritmo) {
      devidos = mostrados + fq_n;
    } else {
      if (!t_toca && (fq_n >= g_qps || (j.fim && fq_n > 0))) {
        t_toca = agora ? agora : 1;
        base = mostrados;
      }
      devidos = t_toca ? base + (int)((agora - t_toca) * (unsigned)g_qps / 1000u) : 0;
      if (t_toca && fq_n == 0 && !j.fim && mostrados < devidos) {
        paradas++;   /* a fila esvaziou: a janela voltaria a "Carregando" */
        t_toca = 0;
      }
    }
    if ((int)(agora - t_log) >= 30000) {
      t_log = agora;
      printf("%3u s: recebidos %d, mostrados %d (video %d:%02d), fila %d, paradas %d, "
             "som %d ms (anel %d ms)\n",
             (agora - t0) / 1000, q_total, mostrados, mostrados / g_qps / 60,
             mostrados / g_qps % 60, fq_n, paradas,
             som_bps ? (int)(som_total * 1000u / (u64)som_bps) : 0, som_ms_buf());
    }
    while (fq_n > 0 && mostrados < devidos) {
      int n;
      unsigned td;
      unsigned char* q = fila_tira(&n);
      mostrados++;
      im.rgba = 0;
      td = k_ms();
      if (img_decode(q, n, &im) == 0) {
        g_dec_ms += k_ms() - td;
        g_dec_n++;
        if (!primeiro)
          printf("primeiro quadro %d bytes -> %dx%d (%u ms)\n", n, im.w, im.h, k_ms() - t0);
        td = k_ms();   /* a escala de uma janela tipica (1152x864) */
        escala(&im, 1152, 864, g_dec_n);
        g_esc_ms += k_ms() - td;
        img_free(&im);
      } else if (!primeiro) {
        printf("primeiro quadro %d bytes: img_decode falhou\n", n);
      }
      primeiro = 1;
      free(q);
    }
    if (som_bps) {   /* sem tocar: -r gasta o som no ritmo dos quadros, -t na hora */
      u64 alvo = ritmo ? (u64)(unsigned)mostrados * (u64)som_bps / (u64)g_qps : som_wr;
      if (alvo > som_wr) alvo = som_wr;
      if (alvo > som_rd) som_rd = alvo;
    }
    if (ritmo || !j.fim) k_futex_wait((volatile int*)(j.slot + S_SEQ), I32(j.slot, S_SEQ), 20);
  }
  bomba(&j);
  while (fq_n > 0) {
    int n;
    free(fila_tira(&n));
  }
  printf("titulo: %s\nduracao: %d s\n%s\n", g_titulo, g_duracao, g_info);
  if (g_erro[0]) printf("erro: %s\n", g_erro);
  printf("quadros: %d recebidos, %d mostrados (video %d:%02d) em %u ms; paradas %d\n", q_total,
         mostrados, mostrados / g_qps / 60, mostrados / g_qps % 60, k_ms() - t0, paradas);
  printf("fim %d codigo %d; PLAY fim (download ffmpeg): %s; perdidos %d\n", j.fim, j.codigo,
         g_fim[0] ? g_fim : "(nao veio)", q_perdidos);
  if (mx_modo != 2)
    printf("som: nao pedido (fluxo MJPEG cru)\n");
  else if (!som_video)
    printf("som: o video nao tem som (ou PLAYSOM=0)\n");
  else
    printf("som: %d Hz, %d canal(is); %u KB recebidos = %d ms (imagem: %d ms)\n", som_hz,
           som_ch, (unsigned)(som_total / 1024u), (int)(som_total * 1000u / (u64)som_bps),
           q_total * 1000 / g_qps);
  if (rl_hz)
    printf("relogio: TSC %u MHz pelo CMOS; k_ms anda %d por mil do certo\n",
           (unsigned)(rl_hz / (u64)1000000), rl_kms_erro + 1000);
  else
    printf("relogio: sem calibrar (menos de 4 s de CMOS), ritmo pelo k_ms\n");
  if (g_dec_n)
    printf("img_decode: %d quadros, media %u ms; escala 1152x864: media %u ms\n", g_dec_n,
           g_dec_ms / (unsigned)g_dec_n, g_esc_ms / (unsigned)g_dec_n);
  encerra(&j, 5000);
  printf("vaga devolvida: %s\n", j.ativo ? "nao" : "sim");
  return j.codigo == 0 ? 0 : 1;
}

/* ---------- janela ---------- */
enum { E_LINK, E_LIGANDO, E_CARREGA, E_TOCA, E_FIM };

static void fit_rect(int sw, int ah, int iw, int ih, int* x, int* y, int* w, int* h) {
  int aw = sw - 16;
  if (aw < 1) aw = 1;
  if (ah < 1) ah = 1;
  if (iw < 1) iw = 1;
  if (ih < 1) ih = 1;
  if (iw * ah > ih * aw) {
    *w = aw;
    *h = ih * aw / iw;
  } else {
    *h = ah;
    *w = iw * ah / ih;
  }
  if (*w < 1) *w = 1;
  if (*h < 1) *h = 1;
  *x = (sw - *w) / 2;
  *y = (ah - *h) / 2;
  if (*y < 0) *y = 0;
}

static void hms(char* out, int cap, int s) {
  if (s < 0) s = 0;
  if (s >= 3600)
    snprintf(out, (size_t)cap, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
  else
    snprintf(out, (size_t)cap, "%d:%02d", s / 60, s % 60);
}

/* SET PLAYLOG: linhas na memoria; o arquivo e gravado a cada 60 s e no fim. */
static char log_buf[24000];
static int log_n, log_liga;

static void log_poe(const char* l, int n) {
  if (!log_liga || n <= 0) return;
  if (log_n + n >= (int)sizeof log_buf) log_n = 0;
  memcpy(log_buf + log_n, l, (size_t)n);
  log_n += n;
}

int main(int argc, char** argv) {
  job_t job;
  gui_edit_t ed;
  img_t cur;
  char url[440];
  int estado = E_LINK, pausa = 0, vivo = 1, mostrados = 0;
  unsigned t_liga = 0;
  /* o quadro n vai para a tela quando o relogio do conteudo (cl_agora: o som
   * ou o TSC) passa de quadro_ms(n) = n * 1000 / g_qps, sem acumular o
   * arredondamento de 1000 / g_qps (que a 15 q/s adiantava 1%); pulados =
   * quadros jogados fora para alcancar o som */
  int pulados = 0;
  unsigned t_vol = 0;   /* ultima mudanca de volume (a barra mostra por 2 s) */
  /* redesenho so quando muda algo (quadro novo, estado, tecla) ou a cada
   * meio segundo; ger conta os quadros decodificados (cache da escala) */
  int ger = 0, sujo = 1, estado_ant = -1, fq_ant = -1, qps_real = 0, qps_cont = 0;
  unsigned t_desenho = 0, t_qps = 0;
  /* medidas do ultimo segundo, para a barra: ms de decodificacao e de desenho
   * por quadro e atraso medio do quadro em relacao ao horario dele */
  unsigned m_dec = 0, m_tela = 0, m_atr = 0;
  int m_n = 0, m_nt = 0, a_dec = 0, a_tela = 0, a_atr = 0, saltos = 0, paradas = 0;
  /* SET PLAYLOG=ARQ: a cada 5 s grava uma linha de estado (diagnostico) */
  char log_arq[160];
  int log_linhas = 0, log_gravou_fim = 0;
  unsigned t_log = 0;

  memset(&job, 0, sizeof job);
  le_playdir();
  /* PLAYLOG=1 -> D:\TEMP\PLAYLOG.TXT; PLAYLOG=NOME (sem : nem \) -> D:\TEMP\NOME.TXT;
   * com caminho, o caminho. 0 ou vazio desliga. */
  if (k_getenv("PLAYLOG", log_arq, (int)sizeof log_arq) <= 0 || log_arq[0] == '0') log_arq[0] = 0;
  if (!strcmp(log_arq, "1")) {
    snprintf(log_arq, sizeof log_arq, "D:\\TEMP\\PLAYLOG.TXT");
  } else if (log_arq[0] && !strchr(log_arq, ':') && !strchr(log_arq, '\\') &&
             strlen(log_arq) <= 8) {
    char nome[16];
    snprintf(nome, sizeof nome, "%s", log_arq);
    snprintf(log_arq, sizeof log_arq, "D:\\TEMP\\%s.TXT", nome);
  }
  log_liga = log_arq[0] != 0;
  le_playsom();
  if (argc > 2 && (!strcmp(argv[1], "-t") || !strcmp(argv[1], "-r")))
    return teste(argv[2], argc > 3 ? atoi(argv[3]) : 60, argv[1][1] == 'r');
  if (argc > 2) g_largura = atoi(argv[2]);
  if (argc > 3) g_qps = atoi(argv[3]);
  if (g_largura < 160 || g_largura > 800) g_largura = 480;
  if (g_qps < 1 || g_qps > 30) g_qps = 15;

  pb = (unsigned char*)malloc(PB_CAP);
  if (!pb) {
    printf("PLAY: sem memoria\n");
    return 1;
  }
  cur.rgba = 0;
  cur.w = cur.h = 0;
  gui_edit_set(&ed, "");
  if (argc > 1) gui_edit_set(&ed, argv[1]);
  if (gui_open_pct("PLAY - YouTube", 60, 70) != 0) {
    printf("PLAY roda no WIN: abra pelo Explorer ou pelo Command do WIN.\n");
    printf("Sem janela, para testar:  play -t LINK\n");
    return 1;
  }
  if (argc > 1 && link_ok(ed.text, url, (int)sizeof url) == 0) {
    estado = E_LIGANDO;
    t_liga = relogio();
  }

  while (vivo) {
    gui_event_t ev;
    int sw = gui_width(), sh = gui_height(), ch = gui_char_h();
    int barra = ch + 10;
    unsigned agora, td0;
    int espera = 30;
    char msg[400], t1[16], t2[16], t3[96], t4[96];

    relogio_calibra();
    agora = relogio();

    /* ---- trabalho ---- */
    if (estado == E_LIGANDO) {
      if (wsl_pronto()) {
        int rc;
        g_som_msg[0] = 0;
        rc = inicia_video(&job, url, som_placa());
        if (rc == 0) {
          estado = E_CARREGA;
          mostrados = 0;
          pulados = 0;
          pausa = 0;
        } else {
          snprintf(g_erro, sizeof g_erro,
                   rc == -2 ? "todas as vagas do WSL ocupadas" : "nao consegui falar com o WSL");
          estado = E_FIM;
        }
      } else if ((int)(agora - t_liga) > 120000) {
        snprintf(g_erro, sizeof g_erro, "o Linux do WSL nao ficou pronto em 2 min (Ctrl+F10)");
        estado = E_FIM;
      }
    }
    g_vezes = estado == E_TOCA ? 2 : 8;   /* tocando: volta curta */
    if (job.ativo) bomba(&job);
    if (estado == E_CARREGA) {
      /* com som, junta tambem um pouco de som (se ele vier: video cheio ou
       * fim sem som = o som acabou ou nao vem) */
      int som_ok = !som_video || som_fora || som_ms_buf() >= 300 || job.fim || fila_cheia();
      if ((fq_n >= g_qps && som_ok) || (job.fim && fq_n > 0)) {
        estado = E_TOCA;
        if (som_video && !som_fora && som_abre() != 0) som_fora = 1;   /* segue mudo */
        cl_som = som_video && !som_fora;
        if (!cl_som) cl_tsc_de(quadro_ms(mostrados), agora);
      } else if (job.fim) {
        if (!g_erro[0]) snprintf(g_erro, sizeof g_erro, "nenhum quadro recebido (codigo %d)",
                                 job.codigo);
        job_solta(&job);
        estado = E_FIM;
      }
    }
    if (estado == E_TOCA && !pausa && cl_som) {
      som_alimenta();
      if (cl_som && som_wr == som_rd && fq_n > 0 && k_audio_pending() == 0) {
        /* som seco com quadros na fila: se a imagem ja encheu ou acabou, o som
         * acabou antes (segue pelo TSC); senao a rede atrasou o som */
        if (job.fim || fila_cheia() || pb_n > PB_CAP - 65536) {
          som_larga("som: acabou antes da imagem");
        } else {
          estado = E_CARREGA;
          paradas++;
        }
      }
    }
    if (estado == E_TOCA && !pausa) {
      unsigned pos = cl_agora(agora);
      if ((int)(pos - quadro_ms(mostrados)) >= 0) {
        if (fq_n > 0) {
          int n, pulou = 0;
          unsigned char* q;
          unsigned td;
          img_t im;
          /* o som manda: atrasado mais de um quadro, pula sem decodificar */
          while (cl_som && fq_n > 1 && (int)(pos - quadro_ms(mostrados + 1)) >= 0) {
            free(fila_tira(&n));
            mostrados++;
            pulados++;
            pulou++;
          }
          if (pulou && log_liga) {
            char l[160];
            log_poe(l, snprintf(l, sizeof l, "rel %u pulou %d pos %u q %u pend %d anel %d\n",
                                agora, pulou, pos, quadro_ms(mostrados), k_audio_pending(),
                                som_ms_buf()));
          }
          q = fila_tira(&n);
          td = k_ms();
          im.rgba = 0;
          m_atr += pos - quadro_ms(mostrados);
          if (q && img_decode(q, n, &im) == 0) {
            img_free(&cur);
            cur = im;
            ger++;
            qps_cont++;
            sujo = 1;
          }
          m_dec += k_ms() - td;
          m_n++;
          free(q);
          mostrados++;
          if (!cl_som && (int)(pos - quadro_ms(mostrados)) > 500) {
            /* sem som, parou mais de meio segundo: recomeca daqui */
            cl_tsc_de(quadro_ms(mostrados - 1), agora);
            saltos++;
          }
        } else if (job.fim) {
          /* acabou antes da duracao: diz o que o Linux devolveu */
          if (!g_erro[0] && (g_duracao <= 0 || mostrados / g_qps < g_duracao - 3))
            snprintf(g_erro, sizeof g_erro,
                     "o video parou em %d:%02d (download ffmpeg: %s; codigo %d)",
                     mostrados / g_qps / 60, mostrados / g_qps % 60,
                     g_fim[0] ? g_fim : "sem PLAY fim", job.codigo);
          som_fecha(1);   /* o fim do som (ate ~170 ms) */
          cl_som = 0;
          job_solta(&job);
          estado = E_FIM;
        } else {
          estado = E_CARREGA;   /* a rede atrasou: junta mais quadros */
          paradas++;
        }
      }
    }
    if (job.ativo && job.fim && estado != E_TOCA && estado != E_CARREGA) job_solta(&job);

    if (log_arq[0] && (int)(agora - t_log) >= 5000) {
      /* uma linha a cada 5 s na memoria; o arquivo so a cada 60 s e no fim
       * (gravar no disco segura o kernel e atrasaria os quadros) */
      char l[300];
      int n, pend = som_aberto && som_bps ? k_audio_pending() / (som_bps / 1000) : -1;
      t_log = agora;
      /* som: 1 = relogio pelo som; pend = ms na fila da placa; anel = ms que
       * chegaram e nao foram para a placa; pos-q = relogio menos o horario do
       * proximo quadro (ms); pulados = quadros jogados fora para alcancar o som */
      n = snprintf(l, sizeof l,
                   "rel %u kms %u cmos %d est %d mostr %d fila %d receb %d perd %d saltos %d "
                   "paradas %d hz %u kmserro %d dec %d tela %d atr %d som %d pend %d anel %d "
                   "pos-q %d pulados %d\n",
                   agora, k_ms(), k_time(), estado, mostrados, fq_n, q_total, q_perdidos, saltos,
                   paradas, (unsigned)(rl_hz / (u64)1000), rl_kms_erro, a_dec, a_tela, a_atr,
                   cl_som, pend, som_ms_buf(), (int)(cl_agora(agora) - quadro_ms(mostrados)),
                   pulados);
      log_poe(l, n);
      if (++log_linhas % 12 == 0 || (estado == E_FIM && !log_gravou_fim)) {
        k_fsave(log_arq, log_buf, log_n);
        if (estado == E_FIM) log_gravou_fim = 1;
      }
      if (estado != E_FIM) log_gravou_fim = 0;
    }
    if ((int)(agora - t_qps) >= 1000) {   /* quadros mostrados no ultimo segundo */
      qps_real = qps_cont;
      qps_cont = 0;
      t_qps = agora;
      a_dec = m_n ? (int)(m_dec / (unsigned)m_n) : 0;
      a_atr = m_n ? (int)(m_atr / (unsigned)m_n) : 0;
      a_tela = m_nt ? (int)(m_tela / (unsigned)m_nt) : 0;
      m_dec = m_tela = m_atr = 0;
      m_n = m_nt = 0;
      sujo = 1;
    }
    if (estado != estado_ant || (estado == E_CARREGA && fq_n != fq_ant)) sujo = 1;
    estado_ant = estado;
    fq_ant = fq_n;

    /* ---- desenho ---- */
    if (!sujo && (int)(agora - t_desenho) < 500) goto teclas;
    sujo = 0;
    t_desenho = agora;
    td0 = k_ms();
    if (estado == E_LINK || estado == E_FIM) {
      int y = 24, ex = 16, ew = sw - 32;
      gui_fill(0, 0, sw, sh, 0x101010);
      if (estado == E_FIM && cur.rgba) {
        int x, yy, w, h;
        fit_rect(sw, sh - barra - 4 * (ch + 10), cur.w, cur.h, &x, &yy, &w, &h);
        mostra_quadro(&cur, ger, x, yy, w, h);
        y = yy + h + 12;
      }
      gui_text(ex, y, estado == E_FIM ? "Fim. Enter toca de novo; troque o link se quiser."
                                      : "Cole o link do YouTube (Ctrl+V) e tecle Enter:",
               -1, 0xFFFFFF, 0x101010, GUI_TEXT_NOBG);
      gui_edit_draw(&ed, ex, y + ch + 8, ew, ch + 10, 1);
      if (g_erro[0])
        gui_text_fit(ex, y + 2 * ch + 26, g_erro, ew, 0xFF8080, 0x101010, GUI_TEXT_NOBG);
      gui_text_fit(ex, y + 3 * ch + 34,
                   "Precisa do Linux do WSL (SET WSL=1) e de internet. Som: SET AUDIO=1 (o PLAY "
                   "liga sozinho); + e - mudam o volume.",
                   ew, 0xA0A0A0, 0x101010, GUI_TEXT_NOBG);
    } else {
      if (cur.rgba) {
        /* so as faixas em volta do quadro levam fundo; o quadro cobre o resto */
        int x, y, w, h, ah = sh - barra;
        fit_rect(sw, ah, cur.w, cur.h, &x, &y, &w, &h);
        if (y > 0) gui_fill(0, 0, sw, y, 0x101010);
        if (y + h < ah) gui_fill(0, y + h, sw, ah - y - h, 0x101010);
        if (x > 0) gui_fill(0, y, x, h, 0x101010);
        if (x + w < sw) gui_fill(x + w, y, sw - x - w, h, 0x101010);
        mostra_quadro(&cur, ger, x, y, w, h);
      } else {
        gui_fill(0, 0, sw, sh, 0x101010);
      }
      if (estado == E_LIGANDO) {
        gui_text(16, 24, "Ligando o Linux do WSL (ate 2 min)...", -1, 0xFFFFFF, 0x101010,
                 GUI_TEXT_NOBG);
      } else if (estado == E_CARREGA) {
        snprintf(msg, sizeof msg, "Carregando... %d quadros", fq_n);
        gui_text(16, 24, msg, -1, 0xFFFFFF, 0x101010, GUI_TEXT_NOBG);
        if (g_titulo[0]) gui_text_fit(16, 24 + ch + 6, g_titulo, sw - 32, 0xC0C0C0, 0x101010,
                                      GUI_TEXT_NOBG);
      }
    }
    hms(t1, sizeof t1, mostrados / g_qps);
    hms(t2, sizeof t2, g_duracao);
    if (estado == E_TOCA && !pausa && !log_arq[0])
      snprintf(t3, sizeof t3, "play %d/%d q/s", qps_real, g_qps);
    else if (estado == E_TOCA && !pausa)   /* SET PLAYLOG: tempos na barra tambem */
      snprintf(t3, sizeof t3, "play %d/%d q/s  dec %d tela %d atraso %d ms  saltos %d  k_ms %s%d.%d%%",
               qps_real, g_qps, a_dec, a_tela, a_atr, saltos, rl_kms_erro < 0 ? "-" : "+",
               (rl_kms_erro < 0 ? -rl_kms_erro : rl_kms_erro) / 10,
               (rl_kms_erro < 0 ? -rl_kms_erro : rl_kms_erro) % 10);
    else
      snprintf(t3, sizeof t3, "%s", estado == E_TOCA ? "pausa"
                                    : estado == E_CARREGA ? "carregando"
                                    : estado == E_FIM ? "fim" : "");
    if (cl_som && (estado == E_TOCA || estado == E_CARREGA))
      snprintf(t4, sizeof t4, "%s %d%%", (int)(agora - t_vol) < 2000 ? "volume" : "som", g_vol);
    else
      snprintf(t4, sizeof t4, "%s", g_som_msg[0] ? g_som_msg : "sem som");
    snprintf(msg, sizeof msg, "%s  %s / %s  %s  %s   espaco pausa  +/- volume  Esc fecha",
             g_titulo[0] ? g_titulo : "YouTube", t1, t2, t3, t4);
    gui_fill(0, sh - barra, sw, barra, 0x000000);
    gui_text_fit(8, sh - barra + 5, msg, sw - 16, 0xE0E0E0, 0x000000, GUI_TEXT_NOBG);
    gui_present();
    m_tela += k_ms() - td0;
    m_nt++;

    /* ---- teclas ---- */
  teclas:
    if (estado == E_TOCA && !pausa) {
      int falta = (int)(quadro_ms(mostrados) - cl_agora(relogio()));
      espera = falta < 1 ? 0 : falta > 30 ? 30 : falta;
      if (cl_som && espera > 20) espera = 20;   /* a fila do som tem 170 ms */
    } else if (estado == E_LINK || (estado == E_FIM && !job.ativo)) {
      espera = 200;
    }
    if (gui_wait(&ev, espera) == GUI_EV_NONE) continue;
    sujo = 1;   /* tecla, rato ou janela mudou de tamanho */
    if (ev.type == GUI_EV_CLOSE) break;
    if (ev.type != GUI_EV_KEY) continue;
    if (ev.key == 27) {
      if (estado == E_LINK || estado == E_FIM) break;
      encerra(&job, 3000);   /* Esc no video: para e volta ao link */
      estado = E_FIM;
      if (!g_erro[0]) snprintf(g_erro, sizeof g_erro, "parado");
      continue;
    }
    if (estado == E_LINK || estado == E_FIM) {
      if (gui_edit_key(&ed, ev.key) == 2) {
        if (link_ok(ed.text, url, (int)sizeof url) == 0) {
          encerra(&job, 3000);
          g_erro[0] = 0;
          estado = E_LIGANDO;
          t_liga = relogio();
        } else {
          snprintf(g_erro, sizeof g_erro, "link invalido");
        }
      }
      continue;
    }
    if (ev.key == ' ' && estado == E_TOCA) {
      char l[200];
      int ln = 0;
      if (log_liga)
        ln = snprintf(l, sizeof l, "rel %u %s: antes pos %u q %u pend %d som_dado %u", relogio(),
                      pausa ? "volta" : "pausa", cl_agora(relogio()), quadro_ms(mostrados),
                      som_aberto ? k_audio_pending() : -1, (unsigned)som_dado);
      pausa = !pausa;
      if (pausa)
        som_pausa();   /* corta a fila da placa; o anel volta ao que nao foi ouvido */
      else if (cl_som && som_abre() != 0)
        som_larga(0);  /* outro programa pegou o som na pausa: segue mudo */
      if (!pausa && !cl_som) cl_tsc_de(quadro_ms(mostrados), relogio());
      if (log_liga)
        log_poe(l, ln + snprintf(l + ln, sizeof l - (size_t)ln, "; depois pos %u som_dado %u\n",
                                 cl_agora(relogio()), (unsigned)som_dado));
    }
    if (ev.key == '+' || ev.key == '=' || ev.key == '-') {   /* volume da placa */
      int v = k_audio_volume(-1);
      if (v >= 0) {
        v += ev.key == '-' ? -10 : 10;
        k_audio_volume(v < 0 ? 0 : v > 100 ? 100 : v);
        g_vol = k_audio_volume(-1);
        t_vol = relogio();
      }
    }
  }
  if (log_arq[0] && log_n) k_fsave(log_arq, log_buf, log_n);
  encerra(&job, 3000);
  img_free(&cur);
  free(g_esc);
  free(g_xi);
  free(som_buf);
  fila_limpa();
  gui_close();
  return 0;
}
