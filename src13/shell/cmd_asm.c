/* Assembler de um subconjunto de long mode. Saida plana em 0x58000.
 * push/pop/call/ret sao de 64 bits. inc/dec usam FF /0 e FF /1
 * (0x40-0x4F sao prefixos REX). Argumentos de funcao C vao em
 * edi/esi/edx/ecx, nao na pilha. */
#include "shell.h"
#include "drives.h"
#include "mm.h"

#define PROG_BASE 0x00058000u
#define PROG_LIM  0x00008000u
#define SRC_MAX   16384
#define MAXLAB    64

static uint8_t* const prog = (uint8_t*)PROG_BASE;
static int prog_ok;
static uint32_t prog_size;
static uint32_t prog_org;

static uint32_t org;
static uint32_t pc;
static int emitted;
static int pass;
static int line_no;
static int failed;
static char errmsg[80];

struct lab {
  char name[16];
  uint32_t addr;
};
static struct lab labs[MAXLAB];
static int nlab;

static int is_id0(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int is_id(char c) {
  return is_id0(c) || (c >= '0' && c <= '9');
}
static char low(char c) {
  if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
  return c;
}

static int boom(const char* m) {
  int i = 0;
  if (!failed) {
    failed = 1;
    while (m[i] && i < 79) {
      errmsg[i] = m[i];
      i++;
    }
    errmsg[i] = '\0';
  }
  return -1;
}

static char* skip(char* p) {
  while (*p == ' ' || *p == '\t') p++;
  return p;
}

static int take_ident(char** pp, char* dst, int n) {
  char* p = *pp;
  int i = 0;
  if (!is_id0(*p)) return 0;
  while (is_id(*p) && i < n - 1)
    dst[i++] = low(*p++);
  dst[i] = '\0';
  *pp = p;
  return 1;
}

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int take_num(char** pp, int32_t* out) {
  char* p = *pp;
  int sign = 1;
  int any = 0;
  uint32_t v = 0;
  int neg_check;
  if (*p == '+') p++;
  else if (*p == '-') {
    sign = -1;
    p++;
  }
  neg_check = (p != *pp);
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
    p += 2;
    while (hexval(*p) >= 0) {
      v = (v << 4) | (uint32_t)hexval(*p);
      p++;
      any = 1;
    }
  } else {
    while (*p >= '0' && *p <= '9') {
      v = v * 10u + (uint32_t)(*p - '0');
      p++;
      any = 1;
    }
  }
  if (!any) return 0;
  if (neg_check && !any) return 0;
  *out = (sign < 0) ? -(int32_t)v : (int32_t)v;
  *pp = p;
  return 1;
}

static int reg32(const char* s) {
  if (strcmp(s, "eax") == 0) return 0;
  if (strcmp(s, "ecx") == 0) return 1;
  if (strcmp(s, "edx") == 0) return 2;
  if (strcmp(s, "ebx") == 0) return 3;
  if (strcmp(s, "esp") == 0) return 4;
  if (strcmp(s, "ebp") == 0) return 5;
  if (strcmp(s, "esi") == 0) return 6;
  if (strcmp(s, "edi") == 0) return 7;
  return -1;
}

static int find_lab(const char* n) {
  int i;
  for (i = 0; i < nlab; i++) {
    if (strcmp(labs[i].name, n) == 0) return i;
  }
  return -1;
}

static int add_lab(const char* n, uint32_t addr) {
  int i = 0;
  if (find_lab(n) >= 0) return boom("label repetido");
  if (nlab >= MAXLAB) return boom("muitos labels");
  while (n[i] && i < 15) {
    labs[nlab].name[i] = n[i];
    i++;
  }
  labs[nlab].name[i] = '\0';
  labs[nlab].addr = addr;
  nlab++;
  return 0;
}

static int em(uint8_t b) {
  uint32_t off = pc - org;
  if (off >= PROG_LIM) return boom("programa grande");
  if (pass == 2) prog[off] = b;
  pc++;
  emitted++;
  return 0;
}

static int em32(uint32_t v) {
  if (em((uint8_t)v) < 0) return -1;
  if (em((uint8_t)(v >> 8)) < 0) return -1;
  if (em((uint8_t)(v >> 16)) < 0) return -1;
  return em((uint8_t)(v >> 24));
}

static int em16(uint32_t v) {
  if (em((uint8_t)v) < 0) return -1;
  return em((uint8_t)(v >> 8));
}

static int s8(int32_t v) {
  return v >= -128 && v <= 127;
}

/* mov/alu r/m32 com base+disp8/32. reg_field e o campo /r. */
static int em_mem(uint8_t op, int reg_field, int base, int32_t disp, int has_disp) {
  int mod;
  if (em(op) < 0) return -1;
  if (!has_disp && disp == 0 && base != 5)
    mod = 0;
  else if (s8(disp))
    mod = 1;
  else
    mod = 2;
  if (base == 4) {
    if (em((uint8_t)((mod << 6) | (reg_field << 3) | 4)) < 0) return -1;
    if (em(0x24) < 0) return -1;
  } else {
    if (em((uint8_t)((mod << 6) | (reg_field << 3) | base)) < 0) return -1;
  }
  if (mod == 1) return em((uint8_t)disp);
  if (mod == 2) return em32((uint32_t)disp);
  return 0;
}

static int em_rr(uint8_t op, int dst, int src) {
  if (em(op) < 0) return -1;
  return em((uint8_t)(0xC0 | (src << 3) | dst));
}

static int em_ri(int digit, int dst, int32_t imm) {
  if (s8(imm)) {
    if (em(0x83) < 0) return -1;
    if (em((uint8_t)(0xC0 | (digit << 3) | dst)) < 0) return -1;
    return em((uint8_t)imm);
  }
  if (em(0x81) < 0) return -1;
  if (em((uint8_t)(0xC0 | (digit << 3) | dst)) < 0) return -1;
  return em32((uint32_t)imm);
}

enum { K_REG = 1, K_IMM = 2, K_MEM = 3 };

struct operand {
  int kind;
  int reg;
  int32_t imm;
  int has_disp;
  int is_lab;
  char lab[16];
};

static int parse_mem(char** pp, struct operand* o) {
  char* p = *pp;
  char id[16];
  int r;
  if (*p != '[') return 0;
  p = skip(p + 1);
  if (!take_ident(&p, id, (int)sizeof(id))) return boom("memoria");
  r = reg32(id);
  if (r < 0) return boom("registrador");
  o->kind = K_MEM;
  o->reg = r;
  o->imm = 0;
  o->has_disp = 0;
  p = skip(p);
  if (*p == '+' || *p == '-') {
    if (!take_num(&p, &o->imm)) return boom("deslocamento");
    o->has_disp = 1;
    p = skip(p);
  }
  if (*p != ']') return boom("]");
  *pp = p + 1;
  return 1;
}

static int parse_op(char** pp, struct operand* o) {
  char* p = skip(*pp);
  char id[16];
  int32_t n;
  int k;
  o->is_lab = 0;
  o->lab[0] = '\0';
  o->has_disp = 0;
  if (*p == '[') {
    k = parse_mem(&p, o);
    if (k <= 0) return k;
    *pp = p;
    return 1;
  }
  if (take_num(&p, &n)) {
    o->kind = K_IMM;
    o->imm = n;
    *pp = p;
    return 1;
  }
  if (!take_ident(&p, id, (int)sizeof(id))) return 0;
  if (reg32(id) >= 0) {
    o->kind = K_REG;
    o->reg = reg32(id);
    *pp = p;
    return 1;
  }
  o->kind = K_IMM;
  o->is_lab = 1;
  {
    int i = 0;
    while (id[i] && i < 15) {
      o->lab[i] = id[i];
      i++;
    }
    o->lab[i] = '\0';
  }
  *pp = p;
  return 1;
}

static int imm_of(struct operand* o, uint32_t* v) {
  if (o->is_lab) {
    int i;
    if (pass == 1) {
      *v = 0;
      return 0;
    }
    i = find_lab(o->lab);
    if (i < 0) return boom("label indefinido");
    *v = labs[i].addr;
    return 0;
  }
  *v = (uint32_t)o->imm;
  return 0;
}

static char* comma(char* p) {
  p = skip(p);
  if (*p != ',') return 0;
  return skip(p + 1);
}

static int emit_rel(int form, uint32_t target) {
  int len = (form >= 2) ? 6 : 5;
  uint32_t next = pc + (uint32_t)len;
  int32_t rel = (int32_t)(target - next);
  if (form == 0) {
    if (em(0xE9) < 0) return -1;
  } else if (form == 1) {
    if (em(0xE8) < 0) return -1;
  } else if (form == 2) {
    if (em(0x0F) < 0 || em(0x84) < 0) return -1;
  } else {
    if (em(0x0F) < 0 || em(0x85) < 0) return -1;
  }
  return em32((uint32_t)rel);
}

static int do_mnem(char* p, char* id) {
  struct operand a, b;
  uint32_t imm = 0;
  int digit = -1;

  if (strcmp(id, "nop") == 0) return em(0x90);
  if (strcmp(id, "ret") == 0) return em(0xC3);

  if (strcmp(id, "org") == 0) {
    int32_t v;
    p = skip(p);
    if (!take_num(&p, &v)) return boom("org");
    if (emitted) return boom("org depois de codigo");
    org = (uint32_t)v;
    pc = org;
    p = skip(p);
    if (*p) return boom("lixo");
    return 0;
  }

  if (strcmp(id, "db") == 0 || strcmp(id, "dw") == 0 || strcmp(id, "dd") == 0) {
    int wide = (id[1] == 'w') ? 2 : (id[1] == 'd' ? 4 : 1);
    if (!*skip(p)) return boom("sem dados");
    for (;;) {
      p = skip(p);
      if (wide == 1 && *p == '"') {
        p++;
        while (*p && *p != '"') {
          char c = *p++;
          if (c == '\\') {
            if (*p == 'n') c = '\n';
            else if (*p == 'r') c = '\r';
            else if (*p == 't') c = '\t';
            else if (*p == '0') c = 0;
            else if (*p == '\\' || *p == '"') c = *p;
            else return boom("escape");
            p++;
          }
          if (em((uint8_t)c) < 0) return -1;
        }
        if (*p != '"') return boom("string");
        p++;
      } else if (parse_op(&p, &a) != 1 || a.kind != K_IMM) {
        return boom("dado");
      } else {
        if (imm_of(&a, &imm) < 0) return -1;
        if (wide == 1) {
          if (em((uint8_t)imm) < 0) return -1;
        } else if (wide == 2) {
          if (em16(imm) < 0) return -1;
        } else if (em32(imm) < 0) {
          return -1;
        }
      }
      p = skip(p);
      if (*p != ',') break;
      p++;
    }
    if (*skip(p)) return boom("lixo");
    return 0;
  }

  if (strcmp(id, "push") == 0) {
    if (parse_op(&p, &a) != 1) return boom("push");
    if (a.kind == K_REG) {
      if (em((uint8_t)(0x50 + a.reg)) < 0) return -1;
    } else if (a.kind == K_IMM) {
      if (imm_of(&a, &imm) < 0) return -1;
      if (em(0x68) < 0 || em32(imm) < 0) return -1;
    } else {
      return boom("push");
    }
    if (*skip(p)) return boom("lixo");
    return 0;
  }
  if (strcmp(id, "pop") == 0) {
    if (parse_op(&p, &a) != 1 || a.kind != K_REG) return boom("pop");
    if (em((uint8_t)(0x58 + a.reg)) < 0) return -1;
    if (*skip(p)) return boom("lixo");
    return 0;
  }
  if (strcmp(id, "inc") == 0) {
    if (parse_op(&p, &a) != 1 || a.kind != K_REG) return boom("inc");
    if (em(0xFF) < 0) return -1;
    return em((uint8_t)(0xC0 + a.reg));
  }
  if (strcmp(id, "dec") == 0) {
    if (parse_op(&p, &a) != 1 || a.kind != K_REG) return boom("dec");
    if (em(0xFF) < 0) return -1;
    return em((uint8_t)(0xC8 + a.reg));
  }
  if (strcmp(id, "int") == 0) {
    int32_t v;
    p = skip(p);
    if (!take_num(&p, &v)) return boom("int");
    if (em(0xCD) < 0) return -1;
    return em((uint8_t)v);
  }

  if (strcmp(id, "jmp") == 0 || strcmp(id, "call") == 0 ||
      strcmp(id, "je") == 0 || strcmp(id, "jz") == 0 ||
      strcmp(id, "jne") == 0 || strcmp(id, "jnz") == 0) {
    uint32_t target = 0;
    if (parse_op(&p, &a) != 1) return boom("destino");
    if ((strcmp(id, "jmp") == 0 || strcmp(id, "call") == 0) && a.kind == K_REG) {
      int digit = (id[0] == 'c') ? 2 : 4;
      if (em(0xFF) < 0) return -1;
      return em((uint8_t)(0xC0 | (digit << 3) | a.reg));
    }
    if (a.kind != K_IMM || !a.is_lab) return boom("label");
    if (pass == 2) {
      int i = find_lab(a.lab);
      if (i < 0) return boom("label indefinido");
      target = labs[i].addr;
    }
    if (strcmp(id, "jmp") == 0) return emit_rel(0, target);
    if (strcmp(id, "call") == 0) return emit_rel(1, target);
    if (strcmp(id, "je") == 0 || strcmp(id, "jz") == 0) return emit_rel(2, target);
    return emit_rel(3, target);
  }

  if (strcmp(id, "mov") == 0) {
    char* q;
    if (parse_op(&p, &a) != 1) return boom("mov");
    q = comma(p);
    if (!q || parse_op(&q, &b) != 1) return boom("mov");
    p = q;
    if (*skip(p)) return boom("lixo");
    if (a.kind == K_REG && b.kind == K_REG) return em_rr(0x89, a.reg, b.reg);
    if (a.kind == K_REG && b.kind == K_IMM) {
      if (imm_of(&b, &imm) < 0) return -1;
      if (em((uint8_t)(0xB8 + a.reg)) < 0) return -1;
      return em32(imm);
    }
    if (a.kind == K_REG && b.kind == K_MEM)
      return em_mem(0x8B, a.reg, b.reg, b.imm, b.has_disp || b.reg == 5);
    if (a.kind == K_MEM && b.kind == K_REG)
      return em_mem(0x89, b.reg, a.reg, a.imm, a.has_disp || a.reg == 5);
    return boom("mov");
  }

  if (strcmp(id, "add") == 0) digit = 0;
  else if (strcmp(id, "or") == 0) digit = 1;
  else if (strcmp(id, "and") == 0) digit = 4;
  else if (strcmp(id, "sub") == 0) digit = 5;
  else if (strcmp(id, "xor") == 0) digit = 6;
  else if (strcmp(id, "cmp") == 0) digit = 7;
  else return boom("instrucao");

  {
    char* q;
    uint8_t rr = (digit == 0) ? 0x01 : (digit == 1) ? 0x09 : (digit == 4) ? 0x21 :
                 (digit == 5) ? 0x29 : (digit == 6) ? 0x31 : 0x39;
    if (parse_op(&p, &a) != 1 || a.kind != K_REG) return boom("registrador");
    q = comma(p);
    if (!q || parse_op(&q, &b) != 1) return boom("operando");
    if (*skip(q)) return boom("lixo");
    if (b.kind == K_REG) return em_rr(rr, a.reg, b.reg);
    if (b.kind == K_IMM) {
      if (imm_of(&b, &imm) < 0) return -1;
      return em_ri(digit, a.reg, (int32_t)imm);
    }
    return boom("operando");
  }
}

static int do_line(char* p) {
  char id[16];
  p = skip(p);
  if (!*p) return 0;
  if (!take_ident(&p, id, (int)sizeof(id))) return boom("sintaxe");
  if (*p == ':') {
    p++;
    if (pass == 1) {
      if (add_lab(id, pc) < 0) return -1;
    } else {
      int i = find_lab(id);
      if (i < 0 || labs[i].addr != pc) return boom("label");
    }
    p = skip(p);
    if (!*p) return 0;
    if (!take_ident(&p, id, (int)sizeof(id))) return boom("sintaxe");
  }
  return do_mnem(skip(p), id);
}

/* ';' fora de aspas encerra a linha. Dentro de db "..." o ';' e dado. */
static void cut_comment(char* s) {
  int quote = 0;
  int i;
  for (i = 0; s[i]; i++) {
    if (quote) {
      if (s[i] == '\\' && s[i + 1]) {
        i++;
        continue;
      }
      if (s[i] == '"') quote = 0;
      continue;
    }
    if (s[i] == '"') {
      quote = 1;
      continue;
    }
    if (s[i] == ';') {
      s[i] = '\0';
      return;
    }
  }
}

static int assemble_pass(const char* src, int n, int passno) {
  int i = 0;
  pass = passno;
  org = PROG_BASE;
  pc = org;
  emitted = 0;
  line_no = 1;
  while (i < n) {
    char buf[180];
    int k = 0;
    while (i < n && src[i] != '\n' && k < 178) {
      if (src[i] != '\r') buf[k++] = src[i];
      i++;
    }
    if (i < n && src[i] == '\n') i++;
    buf[k] = '\0';
    cut_comment(buf);
    if (do_line(buf) < 0) return -1;
    line_no++;
  }
  return 0;
}

static void path_bin(const char* path, char* out, int max) {
  int i = 0;
  int dot = -1;
  while (path[i] && i < max - 5) {
    out[i] = path[i];
    if (path[i] == '.') dot = i;
    if (path[i] == '\\' || path[i] == '/' || path[i] == ':') dot = -1;
    i++;
  }
  if (dot < 0) dot = i;
  out[dot] = '.';
  out[dot + 1] = 'b';
  out[dot + 2] = 'i';
  out[dot + 3] = 'n';
  out[dot + 4] = '\0';
}

/* 0 ok, -1 nao achou, -2 sintaxe, -3 vazio, -4 org. Sem puts: o WIN chama isto. */
int asm_build(const char* path) {
  char* src;
  char saved[80];
  int n;
  if (!path) return -1;
  src = (char*)kmalloc(SRC_MAX);
  if (!src) return -1;
  n = file_load(path, src, SRC_MAX);
  if (n < 0) {
    kfree(src);
    return -1;
  }
  if (n >= SRC_MAX - 1) {
    kfree(src);
    return -5;
  }
  failed = 0;
  errmsg[0] = '\0';
  nlab = 0;
  prog_ok = 0;
  if (assemble_pass(src, n, 1) < 0 || assemble_pass(src, n, 2) < 0) {
    kfree(src);
    return -2;
  }
  prog_size = pc - org;
  prog_org = org;
  kfree(src);
  if (prog_size == 0) return -3;
  if (org == PROG_BASE) {
    uint32_t i;
    for (i = prog_size; i < PROG_LIM; i++)
      prog[i] = 0xC3;
    prog_ok = 1;
  } else {
    return -4;
  }
  path_bin(path, saved, (int)sizeof(saved));
  file_save(saved, (const char*)prog, (int)prog_size);
  return 0;
}

int cmd_asm(int argc, char** argv) {
  char num[16];
  int rc;
  if (argc < 2) {
    puts_k("Usage: asm arquivo.asm\n");
    puts_k("Gera .bin e deixa o codigo em 0x58000. Depois: run\n");
    return 1;
  }
  rc = asm_build(argv[1]);
  if (rc == -1) {
    puts_k("arquivo nao encontrado\n");
    return 1;
  }
  if (rc == -2) {
    puts_k("asm linha ");
    uint32_to_string((uint32_t)line_no, num, (int)sizeof(num));
    puts_k(num);
    puts_k(": ");
    puts_k(errmsg[0] ? errmsg : "erro");
    puts_k("\n");
    return 1;
  }
  if (rc == -3) {
    puts_k("sem instrucoes\n");
    return 1;
  }
  if (rc == -4) {
    puts_k("org diferente de 58000: run nao executa\n");
    return 1;
  }
  if (rc == -5) {
    puts_k("fonte grande demais\n");
    return 1;
  }
  puts_k("montado ");
  uint32_to_string(prog_size, num, (int)sizeof(num));
  puts_k(num);
  puts_k(" bytes em 0x00058000\n");
  return 0;
}

int cmd_run(int argc, char** argv) {
  typedef int (*entry_fn)(void (*puts)(const char*));
  int result;
  char num[16];
  (void)argc;
  (void)argv;
  if (!prog_ok || prog_org != PROG_BASE) {
    puts_k("nada montado em 0x58000. Use: asm arquivo.asm\n");
    return 1;
  }
  result = ((entry_fn)PROG_BASE)(puts_k);
  puts_k("\nretornou ");
  int32_to_string(result, num, (int)sizeof(num));
  puts_k(num);
  puts_k("\n");
  return 0;
}
