/* gen.c - AST -> codigo de long mode gravado direto como .OBJ (COBJ).
 *
 * Maquina de pilha: o resultado fica em eax (edx:eax para long long).
 * A pilha logica tem slots de 4 bytes (sub esp,4 / mov [esp]), porque o
 * PUSH da CPU em long mode tem 8 bytes e quebraria [esp]/[esp+4].
 * O CALL empurra 8 bytes, entao o 1o argumento fica em [ebp+16].
 * Funcoes do proprio programa passam argumentos nessa pilha.
 * Funcoes k_* do kernel recebem os 6 primeiros em edi, esi, edx, ecx, r8d, r9d.
 * _start e DllMain sao chamados pelo kernel em registradores e copiam
 * os argumentos para a area local. Struct de retorno: ponteiro em [ebp+16],
 * "ret 4" (o ret de 64 bits ja remove os 8 bytes do endereco de volta). */
#include "cc.h"
#include "sox.h"

typedef struct {
  char* name;
  int sect;
  int value;
  int flags;
} OSym;

typedef struct {
  int sect;
  int offset;
  int sym;
  int type;
} ORel;

static Buf text;
static Buf data;
static int bss_size;
static OSym* osyms;
static int nosyms, caposyms;
static ORel* orels;
static int norels, caporels;

static int* lab_pos;
static int nlab, caplab;
typedef struct {
  int at;
  int label;
} Fix;
static Fix* fixes;
static int nfix, capfix;

static Obj* cur_fn;
static int ret_label;

static void gen_expr(Node* n);
static void gen_stmt(Node* n);
static void gen_addr(Node* n);

/* ---------- bytes ---------- */
static void e1(int b) { buf_byte(&text, b); }
static void e2(int a, int b) {
  e1(a);
  e1(b);
}
static void e3(int a, int b, int c) {
  e1(a);
  e1(b);
  e1(c);
}
static void e4(unsigned v) { buf_u32(&text, v); }
static int pc(void) { return text.len; }

static void patch4(Buf* b, int at, unsigned v) {
  b->p[at] = (char)v;
  b->p[at + 1] = (char)(v >> 8);
  b->p[at + 2] = (char)(v >> 16);
  b->p[at + 3] = (char)(v >> 24);
}

/* ---------- simbolos e relocacoes do .OBJ ---------- */
static int add_osym(char* name, int sect, int value, int flags) {
  if (nosyms >= caposyms) {
    caposyms = caposyms ? caposyms * 2 : 256;
    osyms = (OSym*)realloc(osyms, sizeof(OSym) * (size_t)caposyms);
    if (!osyms) error("sem memoria");
  }
  osyms[nosyms].name = name;
  osyms[nosyms].sect = sect;
  osyms[nosyms].value = value;
  osyms[nosyms].flags = flags;
  return nosyms++;
}

static int sym_of(Obj* v) {
  if (v->sym < 0) v->sym = add_osym(v->name, COB_SECT_UNDEF, 0, COB_SYM_GLOBAL);
  return v->sym;
}

static int helper_syms[8];
static const char* helper_names[] = { "__divdi3", "__udivdi3", "__moddi3", "__umoddi3" };
#define H_DIV 0
#define H_UDIV 1
#define H_MOD 2
#define H_UMOD 3

static int helper_sym(int h) {
  if (!helper_syms[h])
    helper_syms[h] = add_osym((char*)helper_names[h], COB_SECT_UNDEF, 0, COB_SYM_GLOBAL) + 1;
  return helper_syms[h] - 1;
}

static void add_orel(int sect, int offset, int sym, int type) {
  if (norels >= caporels) {
    caporels = caporels ? caporels * 2 : 256;
    orels = (ORel*)realloc(orels, sizeof(ORel) * (size_t)caporels);
    if (!orels) error("sem memoria");
  }
  orels[norels].sect = sect;
  orels[norels].offset = offset;
  orels[norels].sym = sym;
  orels[norels].type = type;
  norels++;
}

/* ---------- rotulos ---------- */
static int new_label(void) {
  if (nlab >= caplab) {
    caplab = caplab ? caplab * 2 : 256;
    lab_pos = (int*)realloc(lab_pos, sizeof(int) * (size_t)caplab);
    if (!lab_pos) error("sem memoria");
  }
  lab_pos[nlab] = -1;
  return nlab++;
}

static void place(int l) { lab_pos[l] = pc(); }

static void add_fix(int l) {
  if (nfix >= capfix) {
    capfix = capfix ? capfix * 2 : 256;
    fixes = (Fix*)realloc(fixes, sizeof(Fix) * (size_t)capfix);
    if (!fixes) error("sem memoria");
  }
  fixes[nfix].at = pc();
  fixes[nfix].label = l;
  nfix++;
  e4(0);
}

static void jmp_to(int l) {
  e1(0xE9);
  add_fix(l);
}

#define CC_E 0x84
#define CC_NE 0x85

static void jcc_to(int cc, int l) {
  e2(0x0F, cc);
  add_fix(l);
}

static void resolve_labels(void) {
  int i;
  for (i = 0; i < nfix; i++) {
    int at = fixes[i].at;
    int to = lab_pos[fixes[i].label];
    if (to < 0) error("rotulo interno sem posicao em %s", cur_fn->name);
    patch4(&text, at, (unsigned)(to - (at + 4)));
  }
  nfix = 0;
  nlab = 0;
}

/* ---------- instrucoes ---------- */
/* Slot de 4 bytes. O PUSH/POP da CPU em long mode mexe 8 e desalinha
 * o par edx:eax que o long long espera em [esp] e [esp+4]. */
static void push_eax(void) {
  e3(0x83, 0xEC, 0x04);
  e3(0x89, 0x04, 0x24);
}
static void push_edx(void) {
  e3(0x83, 0xEC, 0x04);
  e3(0x89, 0x14, 0x24);
}
static void pop_ecx(void) {
  e3(0x8B, 0x0C, 0x24);
  e3(0x83, 0xC4, 0x04);
}
static void push_esi32(void) {
  e3(0x83, 0xEC, 0x04);
  e3(0x89, 0x34, 0x24);
}
static void push_edi32(void) {
  e3(0x83, 0xEC, 0x04);
  e3(0x89, 0x3C, 0x24);
}
static void pop_edi32(void) {
  e3(0x8B, 0x3C, 0x24);
  e3(0x83, 0xC4, 0x04);
}
static void pop_esi32(void) {
  e3(0x8B, 0x34, 0x24);
  e3(0x83, 0xC4, 0x04);
}

static void mov_eax_imm(unsigned v) {
  if (v == 0) e2(0x31, 0xC0); /* xor eax, eax */
  else {
    e1(0xB8);
    e4(v);
  }
}

static void mov_edx_imm(unsigned v) {
  if (v == 0) e2(0x31, 0xD2);
  else {
    e1(0xBA);
    e4(v);
  }
}

static void add_esp(int n) {
  if (n == 0) return;
  if (n < 128) e3(0x83, 0xC4, n);
  else {
    e2(0x81, 0xC4);
    e4((unsigned)n);
  }
}

static void sub_esp(int n) {
  if (n == 0) return;
  if (n < 128) e3(0x83, 0xEC, n);
  else {
    e2(0x81, 0xEC);
    e4((unsigned)n);
  }
}

/* lea eax, [ebp+off] */
static void lea_local(int off) {
  if (off >= -128 && off < 128) e3(0x8D, 0x45, off & 0xFF);
  else {
    e2(0x8D, 0x85);
    e4((unsigned)off);
  }
}

/* mov eax, simbolo (+addend) */
static void mov_eax_sym(Obj* v) {
  e1(0xB8);
  add_orel(COB_SECT_TEXT, pc(), sym_of(v), COB_REL_ABS32);
  e4(0);
}

static void call_sym(int sym) {
  e1(0xE8);
  add_orel(COB_SECT_TEXT, pc(), sym, COB_REL_REL32);
  e4((unsigned)-4);
}

/* copia 'size' bytes de [eax] para [ecx]; no fim eax = destino */
static void copy_eax_to_ecx(int size) {
  e1(0x56);             /* push esi */
  e1(0x57);             /* push edi */
  e2(0x89, 0xC6);       /* mov esi, eax */
  e2(0x89, 0xCF);       /* mov edi, ecx */
  e2(0x89, 0xC8);       /* mov eax, ecx */
  e1(0xB9);             /* mov ecx, size */
  e4((unsigned)size);
  e2(0xF3, 0xA4);       /* rep movsb */
  e1(0x5F);             /* pop edi */
  e1(0x5E);             /* pop esi */
}

/* flags = (valor != 0) */
static void cmp_zero(Type* ty) {
  if (is_64(ty)) e2(0x09, 0xD0); /* or eax, edx */
  else e2(0x85, 0xC0);           /* test eax, eax */
}

static void setcc_eax(int cc) {
  e3(0x0F, cc, 0xC0);       /* setcc al */
  e3(0x0F, 0xB6, 0xC0);     /* movzx eax, al */
}

/* valor no endereco eax -> eax / edx:eax */
static void load(Type* ty) {
  switch (ty->kind) {
    case TY_ARRAY: case TY_STRUCT: case TY_UNION: case TY_FUNC: case TY_VOID:
      return;
  }
  if (ty->size == 1) {
    if (ty->is_unsigned || ty->kind == TY_BOOL) e3(0x0F, 0xB6, 0x00);
    else e3(0x0F, 0xBE, 0x00);
  } else if (ty->size == 2) {
    if (ty->is_unsigned) e3(0x0F, 0xB7, 0x00);
    else e3(0x0F, 0xBF, 0x00);
  } else if (ty->size == 8) {
    e3(0x8B, 0x50, 0x04); /* mov edx, [eax+4] */
    e2(0x8B, 0x00);       /* mov eax, [eax] */
  } else {
    e2(0x8B, 0x00);
  }
}

/* endereco no topo da pilha, valor em eax/edx:eax */
static void store(Type* ty) {
  pop_ecx();
  if (is_struct(ty)) {
    copy_eax_to_ecx(ty->size);
    return;
  }
  if (ty->size == 1) e2(0x88, 0x01);
  else if (ty->size == 2) e3(0x66, 0x89, 0x01);
  else if (ty->size == 8) {
    e2(0x89, 0x01);
    e3(0x89, 0x51, 0x04);
  } else e2(0x89, 0x01);
}

/* ---------- conversoes ---------- */
static void cast(Type* from, Type* to) {
  if (to->kind == TY_VOID) return;
  if (to->kind == TY_BOOL) {
    cmp_zero(from);
    setcc_eax(0x95);
    return;
  }
  if (!is_scalar(from) || !is_scalar(to)) return; /* array/funcao/struct: endereco */
  if (is_64(to)) {
    if (is_64(from)) return;
    if (from->is_unsigned || from->kind == TY_PTR || from->kind == TY_BOOL) mov_edx_imm(0);
    else e1(0x99); /* cdq */
    return;
  }
  if (to->size == 1) {
    if (from->size == 1 && from->is_unsigned == to->is_unsigned) return;
    if (to->is_unsigned) e3(0x0F, 0xB6, 0xC0);
    else e3(0x0F, 0xBE, 0xC0);
  } else if (to->size == 2) {
    if (from->size <= 2 && from->is_unsigned == to->is_unsigned) return;
    if (from->size == 1 && from->is_unsigned) return;
    if (to->is_unsigned) e3(0x0F, 0xB7, 0xC0);
    else e3(0x0F, 0xBF, 0xC0);
  }
}

/* ---------- enderecos ---------- */
static void gen_addr(Node* n) {
  switch (n->kind) {
    case ND_VAR:
      if (n->var->is_local) lea_local(n->var->offset);
      else mov_eax_sym(n->var);
      return;
    case ND_DEREF:
      gen_expr(n->lhs);
      return;
    case ND_COMMA:
      gen_expr(n->lhs);
      gen_addr(n->rhs);
      return;
    case ND_MEMBER:
      gen_addr(n->lhs);
      if (n->member->offset) {
        e1(0x05); /* add eax, imm32 */
        e4((unsigned)n->member->offset);
      }
      return;
    case ND_FUNCALL:
    case ND_ASSIGN:
    case ND_COND:
      if (is_struct(n->ty)) {
        gen_expr(n);
        return;
      }
      break;
    case ND_CAST:
      if (is_struct(n->ty)) {
        gen_addr(n->lhs);
        return;
      }
      break;
  }
  error_tok(n->tok, "nao e um lvalue");
}

/* ---------- operacoes de 64 bits: rhs em [esp], lhs em edx:eax ---------- */
static void gen_binary64(Node* n) {
  int uns = n->lhs->ty->is_unsigned;
  switch (n->kind) {
    case ND_ADD:
      e3(0x03, 0x04, 0x24);          /* add eax, [esp] */
      e1(0x13); e3(0x54, 0x24, 0x04); /* adc edx, [esp+4] */
      add_esp(8);
      return;
    case ND_SUB:
      e3(0x2B, 0x04, 0x24);
      e1(0x1B); e3(0x54, 0x24, 0x04);
      add_esp(8);
      return;
    case ND_BAND:
      e3(0x23, 0x04, 0x24);
      e1(0x23); e3(0x54, 0x24, 0x04);
      add_esp(8);
      return;
    case ND_BOR:
      e3(0x0B, 0x04, 0x24);
      e1(0x0B); e3(0x54, 0x24, 0x04);
      add_esp(8);
      return;
    case ND_BXOR:
      e3(0x33, 0x04, 0x24);
      e1(0x33); e3(0x54, 0x24, 0x04);
      add_esp(8);
      return;
    case ND_MUL:
      e1(0x8B); e3(0x4C, 0x24, 0x04); /* mov ecx, [esp+4]   (b_hi) */
      e3(0x0F, 0xAF, 0xC8);           /* imul ecx, eax       a_lo*b_hi */
      e1(0x0F); e3(0xAF, 0x14, 0x24); /* imul edx, [esp]     a_hi*b_lo */
      e2(0x01, 0xD1);                 /* add ecx, edx */
      e3(0xF7, 0x24, 0x24);           /* mul dword [esp]     a_lo*b_lo */
      e2(0x01, 0xCA);                 /* add edx, ecx */
      add_esp(8);
      return;
    case ND_DIV:
    case ND_MOD:
      push_edx();
      push_eax();
      if (n->kind == ND_DIV) call_sym(helper_sym(uns ? H_UDIV : H_DIV));
      else call_sym(helper_sym(uns ? H_UMOD : H_MOD));
      add_esp(16);
      return;
    case ND_EQ:
    case ND_NE:
      e3(0x33, 0x04, 0x24);           /* xor eax, [esp] */
      e1(0x33); e3(0x54, 0x24, 0x04); /* xor edx, [esp+4] */
      e2(0x09, 0xD0);                 /* or eax, edx */
      e3(0x0F, n->kind == ND_EQ ? 0x94 : 0x95, 0xC0);
      add_esp(8);
      e3(0x0F, 0xB6, 0xC0);
      return;
    case ND_LT:
      /* a - b com borrow: flags certos para a < b */
      e3(0x2B, 0x04, 0x24);           /* sub eax, [esp] */
      e1(0x1B); e3(0x54, 0x24, 0x04); /* sbb edx, [esp+4] */
      e3(0x0F, uns ? 0x92 : 0x9C, 0xC0); /* setb / setl */
      add_esp(8);
      e3(0x0F, 0xB6, 0xC0);
      return;
    case ND_LE:
      /* a <= b  ==  !(b < a) */
      e3(0x8B, 0x0C, 0x24);           /* mov ecx, [esp] */
      e2(0x29, 0xC1);                 /* sub ecx, eax */
      e1(0x8B); e3(0x4C, 0x24, 0x04); /* mov ecx, [esp+4] */
      e2(0x19, 0xD1);                 /* sbb ecx, edx */
      e3(0x0F, uns ? 0x93 : 0x9D, 0xC0); /* setae / setge */
      add_esp(8);
      e3(0x0F, 0xB6, 0xC0);
      return;
  }
  error_tok(n->tok, "operacao de 64 bits invalida");
}

/* lhs (64) em edx:eax, contagem em ecx */
static void gen_shift64(int kind, int uns) {
  if (kind == ND_SHL) {
    e3(0x0F, 0xA5, 0xC2); /* shld edx, eax, cl */
    e2(0xD3, 0xE0);       /* shl eax, cl */
    e3(0xF6, 0xC1, 0x20); /* test cl, 32 */
    e2(0x74, 0x04);       /* jz +4 */
    e2(0x89, 0xC2);       /* mov edx, eax */
    e2(0x31, 0xC0);       /* xor eax, eax */
  } else if (uns) {
    e3(0x0F, 0xAD, 0xD0); /* shrd eax, edx, cl */
    e2(0xD3, 0xEA);       /* shr edx, cl */
    e3(0xF6, 0xC1, 0x20);
    e2(0x74, 0x04);
    e2(0x89, 0xD0);       /* mov eax, edx */
    e2(0x31, 0xD2);       /* xor edx, edx */
  } else {
    e3(0x0F, 0xAD, 0xD0); /* shrd eax, edx, cl */
    e2(0xD3, 0xFA);       /* sar edx, cl */
    e3(0xF6, 0xC1, 0x20);
    e2(0x74, 0x05);
    e2(0x89, 0xD0);       /* mov eax, edx */
    e3(0xC1, 0xFA, 0x1F); /* sar edx, 31 */
  }
}

/* ---------- chamadas ---------- */
static void push_arg(Node* arg) {
  Type* ty = arg->ty;
  gen_expr(arg);
  if (is_struct(ty)) {
    int sz = align_to(ty->size, 4);
    sub_esp(sz);
    push_esi32();
    push_edi32();
    e2(0x89, 0xC6);                  /* mov esi, eax */
    e1(0x8D); e3(0x7C, 0x24, 0x08);  /* lea edi, [esp+8] */
    e1(0xB9);
    e4((unsigned)ty->size);          /* mov ecx, size */
    e2(0xF3, 0xA4);                  /* rep movsb */
    pop_edi32();
    pop_esi32();
    return;
  }
  if (is_64(ty)) push_edx();
  push_eax();
}

static int arg_size(Type* ty) {
  if (is_struct(ty)) return align_to(ty->size, 4);
  return is_64(ty) ? 8 : 4;
}

/* which: 0 edi, 1 esi, 2 edx, 3 ecx, 4 r8, 5 r9. Le [rsp+off]. */
static void mov_reg_rsp(int which, int off, int wide) {
  static const int mod0[] = {0x3C, 0x34, 0x14, 0x0C, 0x04, 0x0C};
  static const int mod1[] = {0x7C, 0x74, 0x54, 0x4C, 0x44, 0x4C};
  static const int mod2[] = {0xBC, 0xB4, 0x94, 0x8C, 0x84, 0x8C};
  int rex = wide ? 0x48 : 0;
  if (which >= 4) rex |= 0x44;
  if (rex) e1(rex);
  e1(0x8B);
  if (off == 0) {
    e1(mod0[which]);
    e1(0x24);
  } else if (off > 0 && off < 128) {
    e1(mod1[which]);
    e1(0x24);
    e1(off);
  } else {
    e1(mod2[which]);
    e1(0x24);
    e4((unsigned)off);
  }
}

/* Grava o registrador de entrada em [rbp+off] (area local de _start/DllMain). */
static void store_entry_arg(int which, int off, int wide) {
  int rex = wide ? 0x48 : 0;
  int reg = which == 0 ? 7 : which == 1 ? 6 : which == 2 ? 2 : which == 3 ? 1 :
            which == 4 ? 0 : 1;
  if (which >= 4) rex |= 0x44;
  if (rex) e1(rex);
  e1(0x89);
  if (off >= -128 && off < 128) {
    e1(0x45 | (reg << 3));
    e1(off & 0xFF);
  } else {
    e1(0x85 | (reg << 3));
    e4((unsigned)off);
  }
}

static int is_kcall(Node* fn) {
  const char* s;
  if (fn->kind != ND_VAR || !fn->var || !fn->var->name) return 0;
  s = fn->var->name;
  return s[0] == 'k' && s[1] == '_';
}

static int is_entry_name(Obj* fn) {
  return fn->name && (strcmp(fn->name, "_start") == 0 || strcmp(fn->name, "DllMain") == 0);
}

static void spill_entry(Obj* fn) {
  Obj* v;
  int i = 0;
  if (!is_entry_name(fn)) return;
  for (v = fn->params; v && i < 6; v = v->next, i++)
    store_entry_arg(i, v->offset, is_64(v->ty));
}

/* Tira os 6 primeiros argumentos da pilha e poe nos registradores do kernel.
 * Devolve quantos bytes ainda ficam na pilha (o 7o em diante). */
static int sysv_lift(Node* n, Node** args, int nargs, int total) {
  int off = 0;
  int ri = 0;
  int taken = 0;
  int stacked = total + (n->ret_buf ? 4 : 0);
  int i;
  if (n->ret_buf && ri < 6) {
    mov_reg_rsp(ri, off, 0);
    ri++;
    off += 4;
    taken += 4;
  }
  for (i = 0; i < nargs && ri < 6; i++) {
    int sz = arg_size(args[i]->ty);
    if (sz > 8) break;
    mov_reg_rsp(ri, off, sz == 8);
    ri++;
    off += sz;
    taken += sz;
  }
  add_esp(taken);
  return stacked - taken;
}

static void gen_funcall(Node* n) {
  Node* args[256];
  int nargs = 0;
  int i;
  int total = 0;
  int left;
  int kcall;
  Node* a;
  Node* fn = n->lhs;
  Type* rt = n->func_ty->base;
  for (a = n->args; a; a = a->next) {
    if (nargs >= 256) error_tok(n->tok, "argumentos demais");
    args[nargs++] = a;
  }
  for (i = nargs - 1; i >= 0; i--) {
    push_arg(args[i]);
    total += arg_size(args[i]->ty);
  }
  if (n->ret_buf) {
    lea_local(n->ret_buf->offset);
    push_eax();
  }
  kcall = is_kcall(fn);
  left = kcall ? sysv_lift(n, args, nargs, total) : total;
  if (fn->kind == ND_VAR && fn->var->ty->kind == TY_FUNC) {
    call_sym(sym_of(fn->var));
  } else {
    gen_expr(fn);
    e2(0xFF, 0xD0); /* call eax */
  }
  add_esp(left);
  /* o gcc nao estende char/short/_Bool no retorno */
  if (rt->kind == TY_BOOL) e3(0x0F, 0xB6, 0xC0);
  else if (rt->size == 1 && is_integer(rt)) e3(0x0F, rt->is_unsigned ? 0xB6 : 0xBE, 0xC0);
  else if (rt->size == 2 && is_integer(rt)) e3(0x0F, rt->is_unsigned ? 0xB7 : 0xBF, 0xC0);
}

/* ---------- expressoes ---------- */
static void gen_expr(Node* n) {
  int l1, l2;
  switch (n->kind) {
    case ND_NULL_EXPR:
      return;
    case ND_NUM:
      mov_eax_imm((unsigned)n->val);
      if (is_64(n->ty)) mov_edx_imm((unsigned)(n->val >> 32));
      return;
    case ND_NEG:
      gen_expr(n->lhs);
      if (is_64(n->ty)) {
        e2(0xF7, 0xD8);       /* neg eax */
        e3(0x83, 0xD2, 0x00); /* adc edx, 0 */
        e2(0xF7, 0xDA);       /* neg edx */
      } else {
        e2(0xF7, 0xD8);
      }
      return;
    case ND_VAR:
    case ND_MEMBER:
      gen_addr(n);
      load(n->ty);
      return;
    case ND_DEREF:
      gen_expr(n->lhs);
      load(n->ty);
      return;
    case ND_ADDR:
      gen_addr(n->lhs);
      return;
    case ND_ASSIGN:
      gen_addr(n->lhs);
      push_eax();
      gen_expr(n->rhs);
      store(n->ty);
      return;
    case ND_MEMZERO:
      e1(0x57);                        /* push edi */
      lea_local(n->var->offset);
      e2(0x89, 0xC7);                  /* mov edi, eax */
      e1(0xB9);
      e4((unsigned)n->var->ty->size);  /* mov ecx, size */
      e2(0x31, 0xC0);                  /* xor eax, eax */
      e2(0xF3, 0xAA);                  /* rep stosb */
      e1(0x5F);                        /* pop edi */
      return;
    case ND_COND:
      l1 = new_label();
      l2 = new_label();
      gen_expr(n->cond);
      cmp_zero(n->cond->ty);
      jcc_to(CC_E, l1);
      gen_expr(n->then);
      jmp_to(l2);
      place(l1);
      gen_expr(n->els);
      place(l2);
      return;
    case ND_NOT:
      gen_expr(n->lhs);
      cmp_zero(n->lhs->ty);
      setcc_eax(0x94);
      return;
    case ND_BNOT:
      gen_expr(n->lhs);
      e2(0xF7, 0xD0); /* not eax */
      if (is_64(n->ty)) e2(0xF7, 0xD2);
      return;
    case ND_LOGAND:
      l1 = new_label();
      l2 = new_label();
      gen_expr(n->lhs);
      cmp_zero(n->lhs->ty);
      jcc_to(CC_E, l1);
      gen_expr(n->rhs);
      cmp_zero(n->rhs->ty);
      jcc_to(CC_E, l1);
      mov_eax_imm(1);
      jmp_to(l2);
      place(l1);
      mov_eax_imm(0);
      place(l2);
      return;
    case ND_LOGOR:
      l1 = new_label();
      l2 = new_label();
      gen_expr(n->lhs);
      cmp_zero(n->lhs->ty);
      jcc_to(CC_NE, l1);
      gen_expr(n->rhs);
      cmp_zero(n->rhs->ty);
      jcc_to(CC_NE, l1);
      mov_eax_imm(0);
      jmp_to(l2);
      place(l1);
      mov_eax_imm(1);
      place(l2);
      return;
    case ND_FUNCALL:
      gen_funcall(n);
      return;
    case ND_CAST:
      gen_expr(n->lhs);
      cast(n->lhs->ty, n->ty);
      return;
    case ND_COMMA:
      gen_expr(n->lhs);
      gen_expr(n->rhs);
      return;
  }

  /* binarios */
  if (n->kind == ND_SHL || n->kind == ND_SHR) {
    gen_expr(n->rhs);
    push_eax();
    gen_expr(n->lhs);
    pop_ecx();
    if (is_64(n->lhs->ty)) {
      gen_shift64(n->kind, n->lhs->ty->is_unsigned);
    } else if (n->kind == ND_SHL) {
      e2(0xD3, 0xE0);
    } else if (n->lhs->ty->is_unsigned) {
      e2(0xD3, 0xE8);
    } else {
      e2(0xD3, 0xF8);
    }
    return;
  }

  if (is_64(n->lhs->ty)) {
    gen_expr(n->rhs);
    push_edx();
    push_eax();
    gen_expr(n->lhs);
    gen_binary64(n);
    return;
  }

  gen_expr(n->rhs);
  push_eax();
  gen_expr(n->lhs);
  pop_ecx();
  switch (n->kind) {
    case ND_ADD: e2(0x01, 0xC8); return;
    case ND_SUB: e2(0x29, 0xC8); return;
    case ND_MUL: e3(0x0F, 0xAF, 0xC1); return;
    case ND_DIV:
    case ND_MOD:
      if (n->ty->is_unsigned) {
        e2(0x31, 0xD2); /* xor edx, edx */
        e2(0xF7, 0xF1); /* div ecx */
      } else {
        e1(0x99);       /* cdq */
        e2(0xF7, 0xF9); /* idiv ecx */
      }
      if (n->kind == ND_MOD) e2(0x89, 0xD0); /* mov eax, edx */
      return;
    case ND_BAND: e2(0x21, 0xC8); return;
    case ND_BOR: e2(0x09, 0xC8); return;
    case ND_BXOR: e2(0x31, 0xC8); return;
    case ND_EQ: case ND_NE: case ND_LT: case ND_LE: {
      int uns = n->lhs->ty->is_unsigned;
      int cc;
      e2(0x39, 0xC8); /* cmp eax, ecx */
      if (n->kind == ND_EQ) cc = 0x94;
      else if (n->kind == ND_NE) cc = 0x95;
      else if (n->kind == ND_LT) cc = uns ? 0x92 : 0x9C;
      else cc = uns ? 0x96 : 0x9E;
      setcc_eax(cc);
      return;
    }
  }
  error_tok(n->tok, "expressao invalida");
}

/* ---------- comandos ---------- */
static int find_label(char* name, Token* tok) {
  Node* l;
  for (l = cur_fn->labels; l; l = l->label_next)
    if (l->label == name) return l->label_id;
  error_tok(tok, "rotulo '%s' nao existe", name);
  return 0;
}

static void gen_stmt(Node* n) {
  Node* x;
  int l1, l2;
  switch (n->kind) {
    case ND_IF:
      l1 = new_label();
      l2 = new_label();
      gen_expr(n->cond);
      cmp_zero(n->cond->ty);
      jcc_to(CC_E, l1);
      gen_stmt(n->then);
      jmp_to(l2);
      place(l1);
      if (n->els) gen_stmt(n->els);
      place(l2);
      return;
    case ND_FOR:
      l1 = new_label();
      n->brk = new_label();
      n->cont = new_label();
      if (n->init) gen_stmt(n->init);
      place(l1);
      if (n->cond) {
        gen_expr(n->cond);
        cmp_zero(n->cond->ty);
        jcc_to(CC_E, n->brk);
      }
      gen_stmt(n->then);
      place(n->cont);
      if (n->inc) gen_expr(n->inc);
      jmp_to(l1);
      place(n->brk);
      return;
    case ND_DO:
      l1 = new_label();
      n->brk = new_label();
      n->cont = new_label();
      place(l1);
      gen_stmt(n->then);
      place(n->cont);
      gen_expr(n->cond);
      cmp_zero(n->cond->ty);
      jcc_to(CC_NE, l1);
      place(n->brk);
      return;
    case ND_SWITCH:
      n->brk = new_label();
      gen_expr(n->cond);
      for (x = n->case_next; x; x = x->case_next) {
        x->label_id = new_label();
        e1(0x3D); /* cmp eax, imm32 */
        e4((unsigned)x->val);
        if (is_64(n->cond->ty)) {
          int skip = new_label();
          jcc_to(CC_NE, skip);
          e2(0x81, 0xFA); /* cmp edx, imm32 */
          e4((unsigned)(x->val >> 32));
          jcc_to(CC_E, x->label_id);
          place(skip);
        } else {
          jcc_to(CC_E, x->label_id);
        }
      }
      if (n->default_case) {
        n->default_case->label_id = new_label();
        jmp_to(n->default_case->label_id);
      } else {
        jmp_to(n->brk);
      }
      gen_stmt(n->then);
      place(n->brk);
      return;
    case ND_CASE:
      place(n->label_id);
      gen_stmt(n->lhs);
      return;
    case ND_BLOCK:
      for (x = n->body; x; x = x->next) gen_stmt(x);
      return;
    case ND_GOTO:
      jmp_to(find_label(n->label, n->tok));
      return;
    case ND_LABEL:
      place(n->label_id);
      gen_stmt(n->lhs);
      return;
    case ND_BREAK:
      jmp_to(n->target->brk);
      return;
    case ND_CONTINUE:
      jmp_to(n->target->cont);
      return;
    case ND_RETURN:
      if (n->lhs) {
        gen_expr(n->lhs);
        if (cur_fn->ret_struct) {
          e3(0x8B, 0x4D, 0x10); /* mov ecx, [ebp+16] */
          copy_eax_to_ecx(cur_fn->ty->base->size);
        }
      }
      jmp_to(ret_label);
      return;
    case ND_EXPR_STMT:
      gen_expr(n->lhs);
      return;
  }
  error_tok(n->tok, "comando invalido");
}

static void gen_function(Obj* fn) {
  int sub_at;
  Node* l;
  cur_fn = fn;
  nlab = 0;
  nfix = 0;
  while (text.len % 16) e1(0xCC);
  fn->sect = COB_SECT_TEXT;
  fn->value = pc();
  e1(0x55);       /* push ebp */
  e2(0x89, 0xE5); /* mov ebp, esp */
  e2(0x81, 0xEC); /* sub esp, N */
  sub_at = pc();
  e4(0);
  patch4(&text, sub_at, (unsigned)fn->stack_size);
  spill_entry(fn);
  ret_label = new_label();
  for (l = fn->labels; l; l = l->label_next) l->label_id = new_label();
  gen_stmt(fn->body);
  if (strcmp(fn->name, "main") == 0) mov_eax_imm(0);
  place(ret_label);
  if (fn->ret_struct) e3(0x8B, 0x45, 0x10); /* mov eax, [ebp+16] */
  e2(0x89, 0xEC); /* mov esp, ebp */
  e1(0x5D);       /* pop ebp */
  if (fn->ret_struct) e3(0xC2, 0x04, 0x00); /* ret 4 */
  else e1(0xC3);
  resolve_labels();
}

/* ---------- dados ---------- */
static void emit_data(Obj* v) {
  int al = v->ty->align > 0 ? v->ty->align : 1;
  if (al > 4) al = 4;
  if (v->ty->size < 0) error_tok(v->tok, "'%s' tem tipo incompleto", v->name);
  if (v->init_data) {
    Reloc* r;
    while (data.len % al) buf_byte(&data, 0);
    v->sect = COB_SECT_DATA;
    v->value = data.len;
    buf_add(&data, v->init_data, v->ty->size);
    for (r = v->rel; r; r = r->next) {
      add_orel(COB_SECT_DATA, v->value + r->offset, sym_of(r->target), COB_REL_ABS32);
      patch4(&data, v->value + r->offset, (unsigned)r->addend);
    }
  } else {
    bss_size = align_to(bss_size, al);
    v->sect = COB_SECT_BSS;
    v->value = bss_size;
    bss_size += v->ty->size > 0 ? v->ty->size : 0;
  }
}

static void write_obj(const char* path) {
  Buf out, st;
  int i;
  FILE* f;
  memset(&out, 0, sizeof(out));
  memset(&st, 0, sizeof(st));
  buf_byte(&st, 0);
  buf_u32(&out, COB_MAGIC);
  buf_u32(&out, (unsigned)text.len);
  buf_u32(&out, (unsigned)data.len);
  buf_u32(&out, (unsigned)bss_size);
  buf_u32(&out, (unsigned)nosyms);
  buf_u32(&out, (unsigned)norels);
  for (i = 0; i < nosyms; i++) st.len += (int)strlen(osyms[i].name) + 1;
  buf_u32(&out, (unsigned)st.len);
  st.len = 1;
  if (text.len) buf_add(&out, text.p, text.len);
  if (data.len) buf_add(&out, data.p, data.len);
  for (i = 0; i < nosyms; i++) {
    int n = (int)strlen(osyms[i].name) + 1;
    buf_u32(&out, (unsigned)st.len);
    buf_add(&st, osyms[i].name, n);
    buf_u32(&out, (unsigned)osyms[i].sect);
    buf_u32(&out, (unsigned)osyms[i].value);
    buf_u32(&out, (unsigned)osyms[i].flags);
  }
  for (i = 0; i < norels; i++) {
    buf_u32(&out, (unsigned)orels[i].sect);
    buf_u32(&out, (unsigned)orels[i].offset);
    buf_u32(&out, (unsigned)orels[i].sym);
    buf_u32(&out, (unsigned)orels[i].type);
  }
  buf_add(&out, st.p, st.len);
  f = fopen(path, "wb");
  if (!f) error("nao consegui criar %s", path);
  if (fwrite(out.p, 1, (size_t)out.len, f) != (size_t)out.len || fclose(f) != 0)
    error("erro gravando %s", path);
}

void codegen(Obj* globals, const char* out_path) {
  Obj* v;
  text.len = 0;
  data.len = 0;
  bss_size = 0;
  nosyms = 0;
  norels = 0;
  memset(helper_syms, 0, sizeof(helper_syms));
  for (v = globals; v; v = v->next)
    if (v->is_function && v->is_definition) gen_function(v);
  for (v = globals; v; v = v->next)
    if (!v->is_function && v->is_definition && !v->is_extern) emit_data(v);
  /* simbolos: definidos e os usados de fora */
  for (v = globals; v; v = v->next) {
    int defined = v->is_definition && !v->is_extern;
    int s;
    if (!defined && v->sym < 0) continue;
    if (defined && v->is_static && v->sym < 0 && !v->is_export) {
      /* static sem referencia por relocacao nao precisa de simbolo */
      continue;
    }
    s = sym_of(v);
    if (defined) {
      osyms[s].sect = v->sect;
      osyms[s].value = v->value;
      osyms[s].flags = (v->is_static ? 0 : COB_SYM_GLOBAL) | (v->is_export ? COB_SYM_EXPORT : 0);
    }
  }
  write_obj(out_path);
}
