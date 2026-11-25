/*
 * profsave.c - perfil do programa (WINEAPPDATA) salvo entre boots.
 *
 * O AppData do Claude fica no ramdisk Z: (SET WINEAPPDATA=Z:\WINEAPP), que e
 * rapido mas zera a cada boot: o login sumia e cada teste pedia um codigo
 * novo por e-mail. Com SET WINEAPPSAVE=D:\WINEAPP.SAV o processo de cima:
 *   - na partida, se WINEAPPDATA ainda nao existe neste boot, desempacota o
 *     arquivo nele;
 *   - a cada minuto e no ExitProcess, empacota a arvore (sem caches e logs)
 *     e grava no arquivo, so quando mudou.
 * Um arquivo so (e nao a arvore) porque o D: do notebook e FAT32 pelo
 * FAT32.DLL: nomes longos, pastas e renomear ali nao sao garantidos.
 *
 * O Chrome (RODACHRM.BAT) usa --user-data-dir=Z:\CHROMEUD, fora do
 * WINEAPPDATA: o login do Google sumia a cada boot. Com SET
 * WINEUDSAVE=D:\CHROME.SAV o processo de cima que recebeu --user-data-dir
 * restaura e salva essa pasta do mesmo jeito, num arquivo seu. Esse processo
 * salva so o perfil dele (o do Claude fica com o Claude: o D: nao recebe a
 * mesma gravacao duas vezes), mas ainda restaura o WINEAPPDATA, que ele cria
 * ao resolver o APPDATA e o Claude aberto depois nao restauraria mais.
 *
 * Formato: "SODOSPRF1\n", depois por entrada: u32 tamanho do caminho, caminho
 * relativo (com \), u32 tamanho (0xFFFFFFFF = pasta), dados.
 */
#include <so.h>
#include <stdio.h>
#include <string.h>
#include "wine_int.h"

#define PF_MAGIC "SODOSPRF1\n"
#define PF_MAX_FILE (16u << 20)
#define PF_MAX_TOTAL (96u << 20)
#define PF_DIR 0xFFFFFFFFu
#define PF_BIG 0xFFFFFFFEu /* arquivo grande: fica a parte em <save>.BIG\XXXXXXXX.BIN */
#define PF_BIG_MAX (1024u << 20)

static uint32_t pf_fnv32(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

/* D:\WINEAPP.SAV -> D:\WINEAPP.BIG (pasta) e D:\WINEAPP.BIG\XXXXXXXX.BIN */
static void pf_bigdir(const char* save, char* out, int n) {
  int i, dot = -1;
  for (i = 0; save[i] && i < n - 5; i++) {
    out[i] = save[i];
    if (save[i] == '.') dot = i;
    if (save[i] == '\\') dot = -1;
  }
  if (dot < 0) dot = i;
  out[dot] = 0;
  strcat(out, ".BIG");
}
static void pf_bigname(const char* save, const char* rel, char* out, int n) {
  static const char hx[] = "0123456789ABCDEF";
  char d[280];
  uint32_t h = pf_fnv32(rel);
  int i;
  char t[9];
  pf_bigdir(save, d, (int)sizeof(d));
  for (i = 7; i >= 0; i--) {
    t[i] = hx[h & 15];
    h >>= 4;
  }
  t[8] = 0;
  snprintf(out, (size_t)n, "%s\\%s.BIN", d, t);
}

/* copia de arquivo inteiro (ate 1 GB) pela memoria alta */
static int pf_copy(const char* from, const char* to, int sz) {
  uint8_t* d = (uint8_t*)wheap_alloc((size_t)sz + 1);
  int ok;
  if (!d) return 0;
  ok = k_fload(from, (char*)d, sz + 1) == sz;
  if (ok) ok = k_fsave(to, (const char*)d, sz) == sz || k_fsize(to) == sz;
  wheap_free(d);
  return ok;
}

static const char* pf_save_path; /* WINEAPPSAVE da rodada (pf_walk) */

static int pf_isdir(const char* path) {
  struct k_direntl one;
  return k_dir_long(path, &one, 1) >= 0;
}

struct pbuf {
  uint8_t* p;
  size_t n, cap;
  int fail;
};

static void pb_put(struct pbuf* b, const void* d, size_t n) {
  if (b->fail) return;
  if (b->n + n > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : (1u << 20);
    uint8_t* q;
    while (nc < b->n + n) nc *= 2;
    if (nc > PF_MAX_TOTAL + (1u << 20)) {
      b->fail = 1;
      return;
    }
    q = (uint8_t*)wheap_realloc(b->p, nc);
    if (!q) {
      b->fail = 1;
      return;
    }
    b->p = q;
    b->cap = nc;
  }
  memcpy(b->p + b->n, d, n);
  b->n += n;
}
static void pb_u32(struct pbuf* b, uint32_t v) { pb_put(b, &v, 4); }

/* caches e logs do Chromium: refeitos sozinhos, e sao o grosso do disco */
static int pf_skip(const char* name) {
  static const char* const skip[] = {"Cache", "Code Cache", "GPUCache", "DawnGraphiteCache", "DawnWebGPUCache",
                                     "GrShaderCache", "ShaderCache", "Crashpad", "blob_storage",
                                     "CacheStorage", "ScriptCache", "component_crx_cache", "Shared Dictionary",
                                     "Crash Reports", "lockfile", "SingletonLock", "SingletonCookie",
                                     "SingletonSocket", "LOCK",
                                     /* Chrome: componentes que o updater baixa de novo, metricas e
                                      * logs (o --enable-logging cresce sem parar: cada minuto
                                      * seria uma gravacao nova no D:) */
                                     "GPUPersistentCache", "BrowserMetrics", "chrome_debug.log", "debug.log",
                                     "OptGuideOnDeviceModel", "OptGuidePredictionModels",
                                     "OnDeviceHeadSuggestModel", "optimization_guide_model_store", "SODA",
                                     "SODALanguagePacks", "WidevineCdm", "screen_ai", "extensions_crx_cache", 0};
  int i;
  for (i = 0; skip[i]; i++)
    if (!strcmp(name, skip[i])) return 1;
  return 0;
}

static int pf_walk(struct pbuf* b, const char* root, const char* rel, int depth) {
  struct k_direntl* e;
  char path[600];
  int n, i;
  if (depth > 12) return 0;
  snprintf(path, sizeof(path), "%s%s%s", root, *rel ? "\\" : "", rel);
  e = (struct k_direntl*)wheap_alloc(sizeof(*e) * 512);
  if (!e) return -1;
  n = k_dir_long(path, e, 512);
  for (i = 0; i < n && !b->fail; i++) {
    char sub[600], full[600];
    uint32_t len;
    if (pf_skip(e[i].name)) continue;
    snprintf(sub, sizeof(sub), "%s%s%s", rel, *rel ? "\\" : "", e[i].name);
    len = (uint32_t)strlen(sub);
    if (e[i].dir) {
      pb_u32(b, len);
      pb_put(b, sub, len);
      pb_u32(b, PF_DIR);
      pf_walk(b, root, sub, depth + 1);
      continue;
    }
    snprintf(full, sizeof(full), "%s\\%s", root, sub);
    if (e[i].size > PF_MAX_FILE) {
      /* o binario do Claude Code (claude-code\<versao>\claude.exe, ~370 MB)
       * vai a parte: sem ele cada boot baixava tudo de novo pelo Wi-Fi */
      char big[300];
      int sz = k_fsize(full);
      if (!pf_save_path || sz <= 0 || (uint32_t)sz > PF_BIG_MAX) continue;
      pf_bigname(pf_save_path, sub, big, (int)sizeof(big));
      if (k_fsize(big) != sz) {
        char d[280];
        pf_bigdir(pf_save_path, d, (int)sizeof(d));
        k_mkdir(d);
        if (!pf_copy(full, big, sz)) {
          wlog("perfil: falhou copiar arquivo grande ", full);
          continue;
        }
        wlog("perfil: arquivo grande salvo em ", big);
      }
      pb_u32(b, len);
      pb_put(b, sub, len);
      pb_u32(b, PF_BIG);
      pb_u32(b, (uint32_t)sz);
      continue;
    }
    {
      int sz = k_fsize(full);
      uint8_t* d;
      if (sz < 0 || (uint32_t)sz > PF_MAX_FILE) continue;
      d = (uint8_t*)wheap_alloc((size_t)sz + 1);
      if (!d) continue;
      /* k_fload le ate max-1 (poe o 0 final): max = tamanho + 1 */
      if (sz && k_fload(full, (char*)d, sz + 1) != sz) {
        wheap_free(d);
        continue;
      }
      pb_u32(b, len);
      pb_put(b, sub, len);
      pb_u32(b, (uint32_t)sz);
      pb_put(b, d, (size_t)sz);
      wheap_free(d);
    }
  }
  wheap_free(e);
  return 0;
}

/* um perfil: a pasta (no ramdisk) e o arquivo que a guarda no D: */
struct pf_prof {
  char data[260], save[260];
  uint64_t last_ms, hash;
};
static struct pf_prof pf_app; /* WINEAPPDATA -> WINEAPPSAVE (o Claude) */
static struct pf_prof pf_ud;  /* --user-data-dir -> WINEUDSAVE (o Chrome) */
static int pf_ud_on;          /* decidido na partida (wprof_restore) */

/* o SET pode mudar com o programa aberto: le de novo a cada vez */
static int pf_app_env(struct pf_prof* f) {
  int a = k_getenv("WINEAPPDATA", f->data, (int)sizeof(f->data));
  int s = k_getenv("WINEAPPSAVE", f->save, (int)sizeof(f->save));
  if (a <= 0 || a >= (int)sizeof(f->data) || s <= 0 || s >= (int)sizeof(f->save)) return 0;
  f->data[a] = 0;
  f->save[s] = 0;
  return 1;
}

static uint64_t pf_fnv(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  size_t i;
  for (i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
  return h ^ n;
}

/* empacota e grava se mudou; force = no ExitProcess (sem esperar o minuto) */
static void pf_save_one(struct pf_prof* f, int force) {
  const char* data = f->data;
  const char* save = f->save;
  struct pbuf b = {0, 0, 0, 0};
  uint64_t now = wnow_ms(), h;
  if (!force && now - f->last_ms < 60000) return;
  f->last_ms = now;
  if (!pf_isdir(data)) return;
  pb_put(&b, PF_MAGIC, sizeof(PF_MAGIC) - 1);
  pf_save_path = save;
  pf_walk(&b, data, "", 0);
  pf_save_path = 0;
  if (b.fail) {
    wlog("perfil: grande demais ou sem memoria, nao salvo: ", save);
  } else if ((h = pf_fnv(b.p, b.n)) != f->hash) {
    /* grava ao lado e so depois troca: se a maquina desligar no meio (o
     * notebook desligou sozinho; a VM de teste e morta a toda hora) o arquivo
     * antigo continua inteiro. wprof_restore tenta save, .NEW e .OLD. */
    char nw[280], od[280];
    snprintf(nw, sizeof(nw), "%s.NEW", save);
    snprintf(od, sizeof(od), "%s.OLD", save);
    if (k_fsave(nw, (const char*)b.p, (int)b.n) == (int)b.n || k_fsize(nw) == (int)b.n) {
      if (k_fsize(save) >= 0) {
        if (k_fsize(od) >= 0) k_fdelete(od);
        if (k_frename(save, od) != 0) k_fdelete(save);
      }
      if (k_frename(nw, save) != 0) wlog("perfil: nao trocou o nome, ficou em ", nw);
      f->hash = h;
      wlog("perfil salvo em ", save);
      wlogx("perfil salvo, bytes ", b.n);
    } else {
      wlog("perfil: falhou gravar ", save);
    }
  }
  wheap_free(b.p);
}

void wprof_save(int force) {
  /* com --user-data-dir (o Chrome) so o perfil dele: o do Claude e o
   * processo do Claude que salva */
  if (pf_ud_on) {
    pf_save_one(&pf_ud, force);
    return;
  }
  if (pf_app_env(&pf_app)) pf_save_one(&pf_app, force);
}

static void pf_mkdirs(const char* path) {
  char t[600];
  size_t i, n = strlen(path);
  if (n >= sizeof(t)) return;
  memcpy(t, path, n + 1);
  for (i = 3; i < n; i++)
    if (t[i] == '\\') {
      t[i] = 0;
      k_mkdir(t);
      t[i] = '\\';
    }
  k_mkdir(t);
}

/* 1 se o pacote esta inteiro (as entradas terminam exatamente no fim). Um
 * arquivo cortado por queda de energia restaurava so o comeco do perfil: o
 * Claude abria deslogado e o proximo salvamento gravava isso por cima. */
static int pf_whole(const uint8_t* p, size_t sz) {
  size_t o = sizeof(PF_MAGIC) - 1;
  if (sz < o || memcmp(p, PF_MAGIC, o)) return 0;
  while (o < sz) {
    uint32_t len, n;
    if (o + 4 > sz) return 0;
    memcpy(&len, p + o, 4);
    o += 4;
    if (len >= 500 || o + len + 4 > sz) return 0;
    o += len;
    memcpy(&n, p + o, 4);
    o += 4;
    if (n == PF_DIR) continue;
    if (n == PF_BIG) {
      if (o + 4 > sz) return 0;
      o += 4;
      continue;
    }
    if (o + n > sz) return 0;
    o += n;
  }
  return 1;
}

/* Carrega o pacote de path se estiver inteiro; 0 se nao. */
static uint8_t* pf_load(const char* path, int* psz) {
  int sz = k_fsize(path);
  uint8_t* p;
  if (sz <= (int)(sizeof(PF_MAGIC) - 1)) return 0;
  p = (uint8_t*)wheap_alloc((size_t)sz + 1);
  if (!p) return 0;
  if (k_fload(path, (char*)p, sz + 1) != sz || !pf_whole(p, (size_t)sz)) {
    wlog("perfil: arquivo invalido ou cortado ", path);
    wheap_free(p);
    return 0;
  }
  *psz = sz;
  return p;
}

/* desempacota se o perfil ainda nao existe neste boot */
static void pf_restore_one(struct pf_prof* f) {
  const char* data = f->data;
  const char* save = f->save;
  char full[600], alt[280];
  int sz = 0, files = 0;
  uint8_t* p;
  size_t o;
  if (pf_isdir(data)) return; /* ja existe neste boot: o de agora vale */
  p = pf_load(save, &sz);
  if (!p) {
    snprintf(alt, sizeof(alt), "%s.NEW", save);
    p = pf_load(alt, &sz);
  }
  if (!p) {
    snprintf(alt, sizeof(alt), "%s.OLD", save);
    p = pf_load(alt, &sz);
  }
  if (!p) return;
  pf_mkdirs(data);
  o = sizeof(PF_MAGIC) - 1;
  while (o + 8 <= (size_t)sz) {
    uint32_t len, n;
    memcpy(&len, p + o, 4);
    o += 4;
    if (len >= 500 || o + len + 4 > (size_t)sz) break;
    snprintf(full, sizeof(full), "%s\\%.*s", data, (int)len, (const char*)(p + o));
    o += len;
    memcpy(&n, p + o, 4);
    o += 4;
    if (n == PF_DIR) {
      pf_mkdirs(full);
      continue;
    }
    if (n == PF_BIG) {
      uint32_t bsz;
      char big[300], rel[520];
      if (o + 4 > (size_t)sz) break;
      memcpy(&bsz, p + o, 4);
      o += 4;
      snprintf(rel, sizeof(rel), "%.*s", (int)len, (const char*)(p + o - 8 - len));
      pf_bigname(save, rel, big, (int)sizeof(big));
      if (k_fsize(big) == (int)bsz && pf_copy(big, full, (int)bsz)) {
        files++;
        wlog("perfil: arquivo grande restaurado ", full);
      } else {
        wlog("perfil: arquivo grande faltando ", big);
      }
      continue;
    }
    if (o + n > (size_t)sz) break;
    k_fsave(full, (const char*)(p + o), (int)n);
    o += n;
    files++;
  }
  f->hash = pf_fnv(p, (size_t)sz);
  f->last_ms = wnow_ms();
  wlog("perfil restaurado em ", data);
  wlogx("perfil restaurado, arquivos ", (uint64_t)files);
  wheap_free(p);
}

/* --user-data-dir=X (com ou sem aspas) da linha de comando, em d; 1 se achou
 * um caminho completo (X:\...) */
static int pf_ud_arg(int argc, char** argv, char* d, int n) {
  static const char opt[] = "--user-data-dir=";
  int i, k;
  for (i = 1; i < argc; i++) {
    const char* s;
    if (!argv[i] || strncmp(argv[i], opt, sizeof(opt) - 1)) continue;
    s = argv[i] + sizeof(opt) - 1;
    if (*s == '"') s++;
    for (k = 0; s[k] && s[k] != '"' && k < n - 1; k++) d[k] = s[k] == '/' ? '\\' : s[k];
    d[k] = 0;
    while (k > 3 && d[k - 1] == '\\') d[--k] = 0;
    return k >= 3 && d[1] == ':' && d[2] == '\\';
  }
  return 0;
}

/* partida do programa de cima: o perfil do WINEAPPDATA e, com SET WINEUDSAVE,
 * o --user-data-dir deste programa */
void wprof_restore(int argc, char** argv) {
  int app = pf_app_env(&pf_app), s;
  if (app) pf_restore_one(&pf_app);
  s = k_getenv("WINEUDSAVE", pf_ud.save, (int)sizeof(pf_ud.save));
  if (s <= 0 || s >= (int)sizeof(pf_ud.save) || !pf_ud_arg(argc, argv, pf_ud.data, (int)sizeof(pf_ud.data))) return;
  pf_ud.save[s] = 0;
  /* o mesmo arquivo ou a mesma pasta do Claude: um apagaria o outro */
  if (app && (wstreq(pf_ud.save, pf_app.save) || wstreq(pf_ud.data, pf_app.data))) {
    wlog("perfil: WINEUDSAVE igual ao do WINEAPPDATA, ignorado: ", pf_ud.save);
    return;
  }
  pf_ud_on = 1;
  pf_restore_one(&pf_ud);
}
