/* ponte.c - PONTE.EXE: roda no SODOS as linhas de comando pedidas pela VM
 * Linux do WSL (o bash do Claude Code), por arquivos numa pasta do disco.
 *
 * A VM ve as unidades do SODOS por virtio-9p (/d = D:), mas nao tem como
 * chamar programas do SODOS, e a API HTTP 8797 nao e alcancavel de dentro da
 * propria maquina (sem loopback). A ponte: o cliente (SODOSRUN.PY, no Linux)
 * grava FILA\PED.TXT; o PONTE.EXE acha o pedido, apaga o arquivo, roda a
 * linha no prompt (k_shell_capture) na pasta pedida e grava FILA\RESP.TXT
 * com o codigo de saida e a saida capturada.
 *
 *   PED.TXT    PONTE 1 <id>\n<pasta>\n<comando>\n            (CP850)
 *   RESP.TXT   PONTE 1 <id> <codigo> <bytes> <cortou>\n<saida>
 *
 * Rode numa tela livre (Ctrl+F2, Ctrl+F3...) e deixe aberto:
 *   ponte                 fila em D:\DEVELOP\PONTE\FILA
 *   ponte X:\OUTRA\FILA   outra pasta (o cliente usa SODOS_FILA=)
 * ESC sai. So comandos sem interacao: o que pedir tecla espera aqui.
 *
 *   cc -I D:\ -L D:\ -o PONTE.EXE PONTE.C
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <so.h>

#define FILA_PADRAO "D:\\DEVELOP\\PONTE\\FILA"
#define SAIDA_MAX (256 * 1024)
#define CAB_MAX 128      /* espaco para o cabecalho antes da saida */
#define PED_MAX 1024
#define TIQUE_MS 150

static char g_fila[160];
static char g_ped[200];
static char g_resp[200];
static int g_feitos;

static void junta(char* out, int cap, const char* dir, const char* nome) {
  int n = (int)strlen(dir);
  if (n > 0 && dir[n - 1] == '\\')
    snprintf(out, (size_t)cap, "%s%s", dir, nome);
  else
    snprintf(out, (size_t)cap, "%s\\%s", dir, nome);
}

/* Cria a pasta e as de cima que faltarem (k_mkdir so cria um nivel). */
static void cria_pastas(const char* p) {
  char t[160];
  int i;
  for (i = 0; p[i] && i < (int)sizeof(t) - 1; i++) {
    if (p[i] == '\\' && i > 2) {
      t[i] = 0;
      k_mkdir(t);
    }
    t[i] = p[i];
  }
  t[i] = 0;
  k_mkdir(t);
}

/* Copia a linha que comeca em *p (sem \r\n) para out e avanca *p.
 * 0 se nao ha \n ate o fim do texto (pedido ainda sendo gravado). */
static int pega_linha(const char** p, char* out, int cap) {
  const char* s = *p;
  int n = 0;
  while (*s && *s != '\n') {
    if (*s != '\r' && n < cap - 1) out[n++] = *s;
    s++;
  }
  out[n] = 0;
  if (*s != '\n') return 0;
  *p = s + 1;
  return 1;
}

/* Grava a resposta: cabecalho logo antes da saida, no mesmo buffer. */
static void responde(char* buf, const char* id, int codigo, int len, int cortou) {
  char cab[CAB_MAX];
  int hl = snprintf(cab, sizeof(cab), "PONTE 1 %s %d %d %d\n", id, codigo, len, cortou);
  if (hl < 0 || hl >= CAB_MAX) hl = CAB_MAX - 1;
  memcpy(buf + CAB_MAX - hl, cab, (size_t)hl);
  if (k_fsave(g_resp, buf + CAB_MAX - hl, hl + len) != 0)
    printf("  nao consegui gravar %s\n", g_resp);
}

/* Atende o pedido de PED.TXT. 0 se ainda nao esta completo. */
static int atende(char* ped, char* buf) {
  char cab[64], id[40], pasta[160], cmd[256], antes[160];
  const char* p;
  char* saida = buf + CAB_MAX;
  int n, len = 0, rc, cortou = 0;

  n = k_fread(g_ped, 0, ped, PED_MAX);
  if (n <= 0) return 0;
  ped[n] = 0;
  p = ped;
  if (!pega_linha(&p, cab, (int)sizeof(cab))) return 0;
  if (strncmp(cab, "PONTE 1 ", 8) || !cab[8]) {
    printf("  pedido invalido, descartado\n");
    k_fdelete(g_ped);
    return 1;
  }
  if (!pega_linha(&p, pasta, (int)sizeof(pasta))) return 0;
  if (!pega_linha(&p, cmd, (int)sizeof(cmd))) return 0;
  snprintf(id, sizeof(id), "%s", cab + 8);
  k_fdelete(g_ped);   /* o cliente sabe que foi pego */

  g_feitos++;
  printf("[%d] %s> %s\n", g_feitos, pasta[0] ? pasta : "", cmd);
  antes[0] = 0;
  k_pwd(antes, (int)sizeof(antes));
  if (pasta[0] && k_chdir(pasta) != 0) {
    n = snprintf(saida, SAIDA_MAX, "ponte: pasta nao encontrada: %s\n", pasta);
    responde(buf, id, 127, n, 0);
    printf("  pasta nao encontrada\n");
    return 1;
  }
  saida[0] = 0;
  if (!cmd[0]) {
    rc = 0;
  } else {
    rc = k_shell_capture(cmd, saida, SAIDA_MAX, &len);
  }
  if (len < 0) {
    len = -len;
    cortou = 1;
  }
  if (len > SAIDA_MAX - 1) len = SAIDA_MAX - 1;
  if (antes[0]) k_chdir(antes);   /* a pasta atual e do sistema todo */
  responde(buf, id, rc, len, cortou);
  printf("  codigo %d, %d bytes%s\n", rc, len, cortou ? " (cortado)" : "");
  return 1;
}

int main(int argc, char** argv) {
  char* ped;
  char* buf;
  unsigned t0;
  int n;

  snprintf(g_fila, sizeof(g_fila), "%s", argc > 1 ? argv[1] : FILA_PADRAO);
  n = (int)strlen(g_fila);
  while (n > 3 && g_fila[n - 1] == '\\') g_fila[--n] = 0;
  cria_pastas(g_fila);
  junta(g_ped, (int)sizeof(g_ped), g_fila, "PED.TXT");
  junta(g_resp, (int)sizeof(g_resp), g_fila, "RESP.TXT");

  ped = (char*)malloc(PED_MAX + 1);
  buf = (char*)malloc(CAB_MAX + SAIDA_MAX);
  if (!ped || !buf) {
    printf("PONTE: sem memoria\n");
    return 1;
  }

  printf("PONTE: esperando pedidos em %s\n", g_fila);
  printf("Deixe esta tela aberta (Ctrl+F1..F12 troca de tela). ESC sai.\n");
  t0 = k_ms() - TIQUE_MS;
  for (;;) {
    int k = k_trygetc();
    if (k == 27) break;
    if (k_ms() - t0 < TIQUE_MS) {
      k_yield();
      continue;
    }
    t0 = k_ms();
    if (k_fsize(g_ped) < 0) continue;
    atende(ped, buf);
  }
  printf("PONTE: fim (%d comandos)\n", g_feitos);
  free(buf);
  free(ped);
  return 0;
}
