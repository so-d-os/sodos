/*
 * sec.c - Seguranca do advapi32 com estruturas de verdade: SID, ACL/ACE,
 * descritor de seguranca (absoluto e self-relative), SDDL basico e token.
 *
 * O SODOS nao tem usuarios separados nem verifica acesso: tudo e permitido.
 * Mas programas como o Chromium MONTAM descritores (pipes, memoria
 * compartilhada, tokens restritos do sandbox), leem de volta o que montaram e
 * param com CHECK se a leitura nao bater. Por isso aqui os dados sao
 * guardados e devolvidos como no Windows; so a decisao de acesso e sempre
 * "permitido" (AccessCheck, CheckTokenMembership).
 *
 * Esta tabela vem antes das outras (tables.c) e substitui os stubs antigos
 * de sys.c, shell.c e compat.c.
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"

#define SE_OWNER_DEFAULTED 0x1
#define SE_GROUP_DEFAULTED 0x2
#define SE_DACL_PRESENT 0x4
#define SE_SACL_PRESENT 0x10
#define SE_SELF_RELATIVE 0x8000

typedef void*(WINAPI* la_fn)(DWORD, size_t);
static void* lalloc(size_t n) {
  la_fn f = (la_fn)builtin_lookup("LocalAlloc");
  return f ? f(0x40, n) : 0; /* LMEM_ZEROINIT: o programa solta com LocalFree */
}

/* ---------- SID ---------- */
/* S-1-5-21-1329876819-1464486739-555819301-1000 (o usuario "sodos") */
static const uint8_t user_sid[] = {1, 5, 0, 0, 0, 0, 0, 5, 21, 0, 0, 0, 0x53, 0x4F, 0x44, 0x4F,
                                   0x53, 0x57, 0x49, 0x4E, 0x45, 0x21, 0x21, 0x21, 0xE8, 3, 0, 0};

static DWORD sid_len(const uint8_t* s) { return s ? 8u + 4u * s[1] : 0; }
static int sid_ok(const uint8_t* s) { return s && s[0] == 1 && s[1] <= 15; }
static void sid_make(uint8_t* out, uint64_t auth, int n, const DWORD* sub) {
  int i;
  out[0] = 1;
  out[1] = (uint8_t)n;
  for (i = 0; i < 6; i++) out[2 + i] = (uint8_t)(auth >> (8 * (5 - i)));
  memcpy(out + 8, sub, 4u * (unsigned)n);
}

static WINAPI DWORD GetLengthSid(const uint8_t* s) { return sid_len(s); }
static WINAPI BOOL IsValidSid(const uint8_t* s) { return sid_ok(s); }
static WINAPI BOOL CopySid(DWORD n, uint8_t* dst, const uint8_t* src) {
  DWORD len = sid_len(src);
  if (!sid_ok(src) || n < len) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  memcpy(dst, src, len);
  return 1;
}
static WINAPI BOOL EqualSid(const uint8_t* a, const uint8_t* b) {
  return sid_ok(a) && sid_ok(b) && sid_len(a) == sid_len(b) && memcmp(a, b, sid_len(a)) == 0;
}
static WINAPI BOOL EqualPrefixSid(const uint8_t* a, const uint8_t* b) {
  if (!sid_ok(a) || !sid_ok(b) || a[1] != b[1] || !a[1]) return 0;
  return memcmp(a, b, 8u + 4u * (a[1] - 1u)) == 0;
}
static WINAPI BOOL AllocateAndInitializeSid(const uint8_t* auth, uint8_t n, DWORD a0, DWORD a1, DWORD a2, DWORD a3,
                                            DWORD a4, DWORD a5, DWORD a6, DWORD a7, uint8_t** out) {
  DWORD sub[8];
  uint8_t* s;
  if (n > 8 || !out) return 0;
  s = (uint8_t*)wzalloc(8 + 32);
  if (!s) return 0;
  sub[0] = a0; sub[1] = a1; sub[2] = a2; sub[3] = a3;
  sub[4] = a4; sub[5] = a5; sub[6] = a6; sub[7] = a7;
  s[0] = 1;
  s[1] = n;
  memcpy(s + 2, auth, 6);
  memcpy(s + 8, sub, 4u * n);
  *out = s;
  return 1;
}
static WINAPI void* FreeSid(void* s) {
  if (s) k_free(s);
  return 0;
}
static WINAPI BOOL InitializeSid(uint8_t* s, const uint8_t* auth, uint8_t n) {
  if (!s || n > 15) return 0;
  s[0] = 1;
  s[1] = n;
  memcpy(s + 2, auth, 6);
  memset(s + 8, 0, 4u * n);
  return 1;
}
static WINAPI DWORD* GetSidSubAuthority(uint8_t* s, DWORD i) { return (DWORD*)(s + 8 + 4 * i); }
static WINAPI uint8_t* GetSidSubAuthorityCount(uint8_t* s) { return s + 1; }
static WINAPI uint8_t* GetSidIdentifierAuthority(uint8_t* s) { return s + 2; }

/* WELL_KNOWN_SID_TYPE -> autoridade e subautoridades */
static int well_known(int type, uint64_t* auth, DWORD* sub) {
  switch (type) {
    case 0: *auth = 0; sub[0] = 0; return 1;                        /* Null S-1-0-0 */
    case 1: *auth = 1; sub[0] = 0; return 1;                        /* World S-1-1-0 */
    case 2: *auth = 2; sub[0] = 0; return 1;                        /* Local S-1-2-0 */
    case 3: *auth = 3; sub[0] = 0; return 1;                        /* CreatorOwner */
    case 4: *auth = 3; sub[0] = 1; return 1;                        /* CreatorGroup */
    case 9: *auth = 5; sub[0] = 4; return 1;                        /* Interactive */
    case 13: *auth = 5; sub[0] = 7; return 1;                       /* Anonymous */
    case 17: *auth = 5; sub[0] = 11; return 1;                      /* AuthenticatedUser */
    case 18: *auth = 5; sub[0] = 12; return 1;                      /* RestrictedCode */
    case 22: *auth = 5; sub[0] = 18; return 1;                      /* LocalSystem */
    case 23: *auth = 5; sub[0] = 19; return 1;                      /* LocalService */
    case 24: *auth = 5; sub[0] = 20; return 1;                      /* NetworkService */
    case 26: *auth = 5; sub[0] = 32; sub[1] = 544; return 2;        /* BuiltinAdministrators */
    case 27: *auth = 5; sub[0] = 32; sub[1] = 545; return 2;        /* BuiltinUsers */
    case 65: *auth = 16; sub[0] = 0; return 1;                      /* UntrustedLabel */
    case 66: *auth = 16; sub[0] = 0x1000; return 1;                 /* LowLabel */
    case 67: *auth = 16; sub[0] = 0x2000; return 1;                 /* MediumLabel */
    case 68: *auth = 16; sub[0] = 0x3000; return 1;                 /* HighLabel */
    case 69: *auth = 16; sub[0] = 0x4000; return 1;                 /* SystemLabel */
    case 71: *auth = 3; sub[0] = 4; return 1;                       /* CreatorOwnerRights */
    case 84: *auth = 15; sub[0] = 2; sub[1] = 1; return 2;          /* BuiltinAnyPackage S-1-15-2-1 */
    case 92: *auth = 5; sub[0] = 113; return 1;                     /* LocalAccount */
  }
  /* desconhecido: um SID unico e estavel, para o programa seguir */
  *auth = 5;
  sub[0] = 0x5000 + (DWORD)type;
  return 1;
}
static WINAPI BOOL CreateWellKnownSid(int type, const uint8_t* dom, uint8_t* out, DWORD* size) {
  uint64_t auth;
  DWORD sub[4];
  int n = well_known(type, &auth, sub);
  (void)dom;
  if (!size) return 0;
  if (!out || *size < 8u + 4u * (DWORD)n) {
    *size = 8u + 4u * (DWORD)n;
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  sid_make(out, auth, n, sub);
  *size = 8u + 4u * (DWORD)n;
  return 1;
}
static WINAPI BOOL IsWellKnownSid(const uint8_t* s, int type) {
  uint8_t tmp[40];
  DWORD n = sizeof(tmp);
  return CreateWellKnownSid(type, 0, tmp, &n) && EqualSid(s, tmp);
}
static WINAPI BOOL GetWindowsAccountDomainSid(const uint8_t* s, uint8_t* dom, DWORD* size) {
  (void)s;
  if (!size) return 0;
  if (!dom || *size < 24) {
    *size = 24;
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  memcpy(dom, user_sid, 24);
  dom[1] = 4; /* S-1-5-21-a-b-c */
  *size = 24;
  return 1;
}

/* "S-1-5-32-544" ou apelido SDDL ("BA", "SY", "WD"...) */
static int sid_parse(const wchar16* s, uint8_t* out) {
  static const struct {
    char a, b;
    int type;
  } al[] = {{'W', 'D', 1}, {'S', 'Y', 22}, {'B', 'A', 26}, {'B', 'U', 27}, {'A', 'U', 17}, {'R', 'C', 18},
            {'I', 'U', 9},  {'L', 'S', 23}, {'N', 'S', 24}, {'A', 'N', 13}, {'C', 'O', 3},  {'C', 'G', 4},
            {'L', 'W', 66}, {'M', 'E', 67}, {'H', 'I', 68}, {'S', 'I', 69}, {'O', 'W', 71}, {'A', 'C', 84}};
  uint64_t auth = 0;
  DWORD sub[15];
  int n = 0, i;
  if (!s) return 0;
  if (s[0] && s[1] && !(s[0] == 'S' && s[1] == '-')) {
    for (i = 0; i < (int)(sizeof(al) / sizeof(al[0])); i++)
      if (s[0] == (wchar16)al[i].a && s[1] == (wchar16)al[i].b) {
        n = well_known(al[i].type, &auth, sub);
        sid_make(out, auth, n, sub);
        return 2;
      }
    if (s[0] == 'P' && s[1] == 'S') { /* principal self -> o usuario */
      memcpy(out, user_sid, sizeof(user_sid));
      return 2;
    }
    return 0;
  }
  if (!(s[0] == 'S' && s[1] == '-' && s[2] == '1' && s[3] == '-')) return 0;
  s += 4;
  while (*s >= '0' && *s <= '9') auth = auth * 10 + (uint64_t)(*s++ - '0');
  while (*s == '-' && n < 15) {
    DWORD v = 0;
    s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (DWORD)(*s++ - '0');
    sub[n++] = v;
  }
  sid_make(out, auth, n, sub);
  return 1;
}
static int sid_str_len(const wchar16* s) { /* quantos caracteres o SID ocupou */
  int k = 0;
  if (s[0] == 'S' && s[1] == '-') {
    while (s[k] == 'S' || s[k] == '-' || (s[k] >= '0' && s[k] <= '9')) k++;
    return k;
  }
  return 2;
}
static WINAPI BOOL ConvertStringSidToSidW(const wchar16* s, void** out) {
  uint8_t tmp[68];
  uint8_t* p;
  if (!out || !sid_parse(s, tmp)) {
    wset_lasterr(1337); /* ERROR_INVALID_SID */
    return 0;
  }
  p = (uint8_t*)lalloc(sid_len(tmp));
  if (!p) return 0;
  memcpy(p, tmp, sid_len(tmp));
  *out = p;
  return 1;
}
static int put_dec(char* o, uint64_t v) {
  char t[24];
  int k = 0, i;
  do {
    t[k++] = (char)('0' + v % 10);
    v /= 10;
  } while (v);
  for (i = 0; i < k; i++) o[i] = t[k - 1 - i];
  return k;
}
static WINAPI BOOL ConvertSidToStringSidW(const uint8_t* s, wchar16** out) {
  char buf[200];
  int o = 0, i;
  uint64_t auth = 0;
  wchar16* w;
  if (!sid_ok(s) || !out) {
    wset_lasterr(1337);
    return 0;
  }
  for (i = 0; i < 6; i++) auth = (auth << 8) | s[2 + i];
  buf[o++] = 'S';
  buf[o++] = '-';
  buf[o++] = '1';
  buf[o++] = '-';
  o += put_dec(buf + o, auth);
  for (i = 0; i < s[1]; i++) {
    DWORD v;
    memcpy(&v, s + 8 + 4 * i, 4);
    buf[o++] = '-';
    o += put_dec(buf + o, v);
  }
  buf[o] = 0;
  w = (wchar16*)lalloc((size_t)(o + 1) * 2);
  if (!w) return 0;
  for (i = 0; i <= o; i++) w[i] = (wchar16)buf[i];
  *out = w;
  return 1;
}
static WINAPI BOOL LookupAccountSidW(const wchar16* sys, const uint8_t* s, wchar16* name, DWORD* nn, wchar16* dom,
                                     DWORD* dn, int* use) {
  static const char un[] = "sodos", dm[] = "SODOS";
  int i;
  (void)sys;
  (void)s;
  if (!nn || !dn || *nn < 6 || *dn < 6 || !name || !dom) {
    if (nn) *nn = 6;
    if (dn) *dn = 6;
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  for (i = 0; i < 6; i++) {
    name[i] = (wchar16)un[i];
    dom[i] = (wchar16)dm[i];
  }
  *nn = 5;
  *dn = 5;
  if (use) *use = 1; /* SidTypeUser */
  return 1;
}

/* ---------- ACL ---------- */
struct acl {
  uint8_t rev, sbz1;
  uint16_t size, count, sbz2;
};
static WINAPI BOOL InitializeAcl(struct acl* a, DWORD len, DWORD rev) {
  if (!a || len < 8 || len > 0xFFFF) return 0;
  memset(a, 0, len);
  a->rev = (uint8_t)rev;
  a->size = (uint16_t)len;
  return 1;
}
static uint16_t acl_used(const struct acl* a) {
  const uint8_t* p = (const uint8_t*)(a + 1);
  int i;
  for (i = 0; i < a->count; i++) p += *(const uint16_t*)(p + 2);
  return (uint16_t)(p - (const uint8_t*)a);
}
static WINAPI BOOL IsValidAcl(const struct acl* a) { return a && a->size >= 8 && acl_used(a) <= a->size; }
static WINAPI BOOL GetAce(const struct acl* a, DWORD i, void** ace) {
  const uint8_t* p;
  DWORD k;
  if (!a || i >= a->count) return 0;
  p = (const uint8_t*)(a + 1);
  for (k = 0; k < i; k++) p += *(const uint16_t*)(p + 2);
  *ace = (void*)p;
  return 1;
}
static WINAPI BOOL AddAce(struct acl* a, DWORD rev, DWORD start, const void* list, DWORD len) {
  uint16_t used;
  const uint8_t* p = (const uint8_t*)list;
  DWORD n = 0, off = 0;
  (void)rev;
  if (!a) return 0;
  used = acl_used(a);
  if ((DWORD)used + len > a->size) {
    wset_lasterr(1344); /* ERROR_ALLOTTED_SPACE_EXCEEDED */
    return 0;
  }
  while (off < len) {
    off += *(const uint16_t*)(p + off + 2);
    n++;
  }
  /* insere na posicao start (0xFFFFFFFF = no fim) */
  {
    uint8_t* at = (uint8_t*)(a + 1);
    DWORD k;
    for (k = 0; k < start && k < a->count; k++) at += *(const uint16_t*)(at + 2);
    memmove(at + len, at, (size_t)(((uint8_t*)a + used) - at));
    memcpy(at, list, len);
  }
  a->count = (uint16_t)(a->count + n);
  return 1;
}
static BOOL add_ace(struct acl* a, uint8_t type, uint8_t flags, DWORD mask, const uint8_t* sid) {
  uint8_t ace[8 + 68];
  DWORD sl = sid_len(sid);
  uint16_t sz = (uint16_t)(8 + sl);
  if (!sid_ok(sid)) return 0;
  ace[0] = type;
  ace[1] = flags;
  memcpy(ace + 2, &sz, 2);
  memcpy(ace + 4, &mask, 4);
  memcpy(ace + 8, sid, sl);
  return AddAce(a, 2, 0xFFFFFFFFu, ace, sz);
}
static WINAPI BOOL AddAccessAllowedAce(struct acl* a, DWORD rev, DWORD mask, const uint8_t* sid) {
  (void)rev;
  return add_ace(a, 0, 0, mask, sid);
}
static WINAPI BOOL AddAccessAllowedAceEx(struct acl* a, DWORD rev, DWORD fl, DWORD mask, const uint8_t* sid) {
  (void)rev;
  return add_ace(a, 0, (uint8_t)fl, mask, sid);
}
static WINAPI BOOL AddAccessDeniedAce(struct acl* a, DWORD rev, DWORD mask, const uint8_t* sid) {
  (void)rev;
  return add_ace(a, 1, 0, mask, sid);
}
static WINAPI BOOL AddAccessDeniedAceEx(struct acl* a, DWORD rev, DWORD fl, DWORD mask, const uint8_t* sid) {
  (void)rev;
  return add_ace(a, 1, (uint8_t)fl, mask, sid);
}
static WINAPI BOOL AddMandatoryAce(struct acl* a, DWORD rev, DWORD fl, DWORD policy, const uint8_t* sid) {
  (void)rev;
  return add_ace(a, 0x11, (uint8_t)fl, policy, sid); /* SYSTEM_MANDATORY_LABEL_ACE */
}
static WINAPI BOOL GetAclInformation(const struct acl* a, void* info, DWORD len, int cls) {
  if (!a || !info) return 0;
  if (cls == 1 && len >= 4) { /* AclRevisionInformation */
    *(DWORD*)info = a->rev;
    return 1;
  }
  if (cls == 2 && len >= 12) { /* AclSizeInformation */
    DWORD* o = (DWORD*)info;
    o[0] = a->count;
    o[1] = acl_used(a);
    o[2] = a->size - o[1];
    return 1;
  }
  wset_lasterr(ERROR_INVALID_PARAMETER);
  return 0;
}

/* EXPLICIT_ACCESS_W + TRUSTEE_W (x64) */
struct trustee {
  void* multiple;
  int mult_op, form, type;
  wchar16* name;
};
struct explicit_access {
  DWORD perms;
  int mode;
  DWORD inherit;
  struct trustee t;
};
static const uint8_t* trustee_sid(const struct trustee* t, uint8_t* tmp) {
  if (t->form == 0) return (const uint8_t*)t->name; /* TRUSTEE_IS_SID */
  if (t->form == 1 && t->name) {                     /* TRUSTEE_IS_NAME */
    if (sid_parse(t->name, tmp)) return tmp;
    return user_sid; /* "CURRENT_USER" e nomes de conta */
  }
  return user_sid;
}
static WINAPI DWORD SetEntriesInAclW(DWORD n, const struct explicit_access* e, const struct acl* old,
                                     struct acl** out) {
  DWORD size = 8, i;
  struct acl* a;
  uint8_t tmp[68];
  if (!out) return ERROR_INVALID_PARAMETER;
  if (old) size = acl_used(old);
  for (i = 0; i < n; i++) size += 8 + 68;
  a = (struct acl*)lalloc(size);
  if (!a) return 8;
  if (old) {
    memcpy(a, old, acl_used(old));
    a->size = (uint16_t)size;
  } else {
    InitializeAcl(a, size, 2);
  }
  for (i = 0; i < n; i++) {
    const uint8_t* sid = trustee_sid(&e[i].t, tmp);
    if (e[i].mode == 1 || e[i].mode == 2) add_ace(a, 0, (uint8_t)e[i].inherit, e[i].perms, sid); /* GRANT/SET */
    else if (e[i].mode == 3) add_ace(a, 1, (uint8_t)e[i].inherit, e[i].perms, sid);             /* DENY */
  }
  *out = a;
  return 0;
}
static WINAPI DWORD GetExplicitEntriesFromAclW(const struct acl* a, DWORD* n, void** out) {
  (void)a;
  if (n) *n = 0;
  if (out) *out = 0;
  return 0;
}

/* ---------- descritor de seguranca ---------- */
struct sd_abs {
  uint8_t rev, sbz1;
  uint16_t control;
  uint32_t pad;
  uint8_t* owner;
  uint8_t* group;
  struct acl* sacl;
  struct acl* dacl;
};
struct sd_rel {
  uint8_t rev, sbz1;
  uint16_t control;
  uint32_t owner, group, sacl, dacl;
};
/* parte i (0 dono, 1 grupo, 2 sacl, 3 dacl) de qualquer forma de descritor */
static void* sd_part(const void* sd, int i) {
  const struct sd_abs* a = (const struct sd_abs*)sd;
  if (!sd) return 0;
  if (a->control & SE_SELF_RELATIVE) {
    const struct sd_rel* r = (const struct sd_rel*)sd;
    uint32_t off = i == 0 ? r->owner : i == 1 ? r->group : i == 2 ? r->sacl : r->dacl;
    return off ? (uint8_t*)sd + off : 0;
  }
  return i == 0 ? (void*)a->owner : i == 1 ? (void*)a->group : i == 2 ? (void*)a->sacl : (void*)a->dacl;
}
static WINAPI BOOL InitializeSecurityDescriptor(struct sd_abs* sd, DWORD rev) {
  if (!sd) return 0;
  memset(sd, 0, sizeof(*sd));
  sd->rev = (uint8_t)rev;
  return 1;
}
static WINAPI BOOL IsValidSecurityDescriptor(const struct sd_abs* sd) { return sd && sd->rev == 1; }
static WINAPI BOOL GetSecurityDescriptorControl(const struct sd_abs* sd, uint16_t* ctl, DWORD* rev) {
  if (!sd) return 0;
  if (ctl) *ctl = sd->control;
  if (rev) *rev = sd->rev;
  return 1;
}
static WINAPI BOOL SetSecurityDescriptorControl(struct sd_abs* sd, uint16_t mask, uint16_t bits) {
  if (!sd) return 0;
  sd->control = (uint16_t)((sd->control & ~mask) | (bits & mask));
  return 1;
}
static int sd_abs_only(struct sd_abs* sd) {
  if (!sd || (sd->control & SE_SELF_RELATIVE)) {
    wset_lasterr(1338); /* ERROR_INVALID_SECURITY_DESCR */
    return 0;
  }
  return 1;
}
static WINAPI BOOL SetSecurityDescriptorOwner(struct sd_abs* sd, uint8_t* sid, BOOL def) {
  if (!sd_abs_only(sd)) return 0;
  sd->owner = sid;
  sd->control = (uint16_t)((sd->control & ~SE_OWNER_DEFAULTED) | (def ? SE_OWNER_DEFAULTED : 0));
  return 1;
}
static WINAPI BOOL SetSecurityDescriptorGroup(struct sd_abs* sd, uint8_t* sid, BOOL def) {
  if (!sd_abs_only(sd)) return 0;
  sd->group = sid;
  sd->control = (uint16_t)((sd->control & ~SE_GROUP_DEFAULTED) | (def ? SE_GROUP_DEFAULTED : 0));
  return 1;
}
static WINAPI BOOL SetSecurityDescriptorDacl(struct sd_abs* sd, BOOL present, struct acl* a, BOOL def) {
  if (!sd_abs_only(sd)) return 0;
  sd->dacl = present ? a : 0;
  sd->control = (uint16_t)((sd->control & ~(SE_DACL_PRESENT | 8)) | (present ? SE_DACL_PRESENT : 0) |
                           (def ? 8 : 0));
  return 1;
}
static WINAPI BOOL SetSecurityDescriptorSacl(struct sd_abs* sd, BOOL present, struct acl* a, BOOL def) {
  if (!sd_abs_only(sd)) return 0;
  sd->sacl = present ? a : 0;
  sd->control = (uint16_t)((sd->control & ~(SE_SACL_PRESENT | 0x20)) | (present ? SE_SACL_PRESENT : 0) |
                           (def ? 0x20 : 0));
  return 1;
}
static WINAPI BOOL GetSecurityDescriptorOwner(const struct sd_abs* sd, void** sid, BOOL* def) {
  if (!sd) return 0;
  *sid = sd_part(sd, 0);
  if (def) *def = (sd->control & SE_OWNER_DEFAULTED) != 0;
  return 1;
}
static WINAPI BOOL GetSecurityDescriptorGroup(const struct sd_abs* sd, void** sid, BOOL* def) {
  if (!sd) return 0;
  *sid = sd_part(sd, 1);
  if (def) *def = (sd->control & SE_GROUP_DEFAULTED) != 0;
  return 1;
}
static WINAPI BOOL GetSecurityDescriptorDacl(const struct sd_abs* sd, BOOL* present, void** a, BOOL* def) {
  if (!sd) return 0;
  *present = (sd->control & SE_DACL_PRESENT) != 0;
  *a = *present ? sd_part(sd, 3) : 0;
  if (def) *def = (sd->control & 8) != 0;
  return 1;
}
static WINAPI BOOL GetSecurityDescriptorSacl(const struct sd_abs* sd, BOOL* present, void** a, BOOL* def) {
  if (!sd) return 0;
  *present = (sd->control & SE_SACL_PRESENT) != 0;
  *a = *present ? sd_part(sd, 2) : 0;
  if (def) *def = (sd->control & 0x20) != 0;
  return 1;
}
static DWORD part_len(const void* sd, int i) {
  const void* p = sd_part(sd, i);
  if (!p) return 0;
  return i < 2 ? sid_len((const uint8_t*)p) : ((const struct acl*)p)->size;
}
static WINAPI DWORD GetSecurityDescriptorLength(const struct sd_abs* sd) {
  DWORD n = sizeof(struct sd_rel), i, end;
  if (!sd) return 0;
  if (sd->control & SE_SELF_RELATIVE) {
    const struct sd_rel* r = (const struct sd_rel*)sd;
    uint32_t offs[4] = {r->owner, r->group, r->sacl, r->dacl};
    end = sizeof(struct sd_rel);
    for (i = 0; i < 4; i++)
      if (offs[i] && offs[i] + part_len(sd, (int)i) > end) end = offs[i] + part_len(sd, (int)i);
    return end;
  }
  for (i = 0; i < 4; i++) n += part_len(sd, (int)i);
  return n;
}
/* absoluto (ou relativo) -> self-relative em out */
static DWORD to_rel(const struct sd_abs* sd, uint8_t* out, DWORD max) {
  DWORD need = GetSecurityDescriptorLength(sd), off = sizeof(struct sd_rel), i;
  struct sd_rel* r = (struct sd_rel*)out;
  uint32_t* slots[4];
  if (!out || max < need) return need;
  memset(out, 0, need);
  r->rev = sd->rev ? sd->rev : 1;
  r->control = (uint16_t)(sd->control | SE_SELF_RELATIVE);
  slots[0] = &r->owner;
  slots[1] = &r->group;
  slots[2] = &r->sacl;
  slots[3] = &r->dacl;
  for (i = 0; i < 4; i++) {
    const void* p = sd_part(sd, (int)i);
    DWORD l = part_len(sd, (int)i);
    if (!p || !l) continue;
    memcpy(out + off, p, l);
    *slots[i] = off;
    off += l;
  }
  return need;
}
static WINAPI BOOL MakeSelfRelativeSD(const struct sd_abs* sd, uint8_t* out, DWORD* len) {
  DWORD need;
  if (!sd || !len) return 0;
  need = to_rel(sd, out, *len);
  if (!out || *len < need) {
    *len = need;
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  *len = need;
  return 1;
}
static WINAPI BOOL MakeAbsoluteSD(const struct sd_abs* rel, struct sd_abs* abs, DWORD* abs_len, struct acl* dacl,
                                  DWORD* dacl_len, struct acl* sacl, DWORD* sacl_len, uint8_t* owner,
                                  DWORD* owner_len, uint8_t* group, DWORD* group_len) {
  DWORD need[4], have[4], i;
  int small = 0;
  void* bufs[4];
  DWORD* lens[4];
  if (!rel) return 0;
  bufs[0] = owner; bufs[1] = group; bufs[2] = sacl; bufs[3] = dacl;
  lens[0] = owner_len; lens[1] = group_len; lens[2] = sacl_len; lens[3] = dacl_len;
  for (i = 0; i < 4; i++) {
    need[i] = part_len(rel, (int)i);
    have[i] = lens[i] ? *lens[i] : 0;
    if (have[i] < need[i] || (need[i] && !bufs[i])) small = 1;
  }
  if (!abs_len || *abs_len < sizeof(struct sd_abs) || !abs) small = 1;
  if (small) {
    if (abs_len) *abs_len = sizeof(struct sd_abs);
    for (i = 0; i < 4; i++)
      if (lens[i]) *lens[i] = need[i];
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  memset(abs, 0, sizeof(*abs));
  abs->rev = rel->rev;
  abs->control = (uint16_t)(rel->control & ~SE_SELF_RELATIVE);
  for (i = 0; i < 4; i++) {
    void* p = sd_part(rel, (int)i);
    if (p && need[i]) memcpy(bufs[i], p, need[i]);
    *lens[i] = need[i];
  }
  abs->owner = need[0] ? owner : 0;
  abs->group = need[1] ? group : 0;
  abs->sacl = need[2] ? sacl : 0;
  abs->dacl = need[3] ? dacl : 0;
  return 1;
}

/* SD self-relative so com dono = usuario e DACL ausente (tudo permitido) */
static uint8_t* sd_default(DWORD* len) {
  struct sd_abs a;
  uint8_t* out;
  DWORD n;
  InitializeSecurityDescriptor(&a, 1);
  a.owner = (uint8_t*)user_sid;
  a.group = (uint8_t*)user_sid;
  n = to_rel(&a, 0, 0);
  out = (uint8_t*)lalloc(n);
  if (!out) return 0;
  to_rel(&a, out, n);
  if (len) *len = n;
  return out;
}

/* ---------- SDDL ---------- */
static int rights_of(const wchar16* s, int n, DWORD* mask) {
  static const struct {
    char a, b;
    DWORD v;
  } r[] = {{'G', 'A', 0x10000000}, {'G', 'R', 0x80000000}, {'G', 'W', 0x40000000}, {'G', 'X', 0x20000000},
           {'F', 'A', 0x1F01FF},   {'F', 'R', 0x120089},   {'F', 'W', 0x120116},   {'F', 'X', 0x1200A0},
           {'K', 'A', 0xF003F},    {'R', 'C', 0x20000},    {'S', 'D', 0x10000},    {'W', 'D', 0x40000},
           {'W', 'O', 0x80000},    {'N', 'W', 1},          {'N', 'R', 2},          {'N', 'X', 4},
           {'C', 'C', 1},          {'D', 'C', 2},          {'L', 'C', 4},          {'S', 'W', 8},
           {'R', 'P', 0x10},       {'W', 'P', 0x20},       {'D', 'T', 0x40},       {'L', 'O', 0x80},
           {'C', 'R', 0x100}};
  int i, k;
  *mask = 0;
  if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    for (i = 2; i < n; i++) {
      wchar16 c = s[i];
      *mask = *mask * 16 + (DWORD)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
    }
    return 1;
  }
  for (i = 0; i + 1 < n; i += 2)
    for (k = 0; k < (int)(sizeof(r) / sizeof(r[0])); k++)
      if (s[i] == (wchar16)r[k].a && s[i + 1] == (wchar16)r[k].b) *mask |= r[k].v;
  return 1;
}
/* "D:P(A;;GA;;;SY)(A;OICI;FA;;;BA)S:(ML;;NW;;;LW)O:BAG:SY" -> self-relative */
static uint8_t* sddl_parse(const wchar16* s, DWORD* len) {
  struct sd_abs a;
  struct acl* acls[2] = {0, 0}; /* 0 dacl, 1 sacl */
  uint8_t owner[68], group[68];
  int have_o = 0, have_g = 0, i;
  uint8_t* out;
  DWORD n;
  InitializeSecurityDescriptor(&a, 1);
  for (i = 0; i < 2; i++) {
    acls[i] = (struct acl*)wzalloc(2048);
    if (!acls[i]) return 0;
    InitializeAcl(acls[i], 2048, 2);
  }
  while (s && *s) {
    wchar16 c = *s;
    if ((c == 'O' || c == 'G') && s[1] == ':') {
      uint8_t* dst = c == 'O' ? owner : group;
      s += 2;
      if (sid_parse(s, dst)) {
        if (c == 'O') have_o = 1;
        else have_g = 1;
      }
      s += sid_str_len(s);
      continue;
    }
    if ((c == 'D' || c == 'S') && s[1] == ':') {
      struct acl* acl = acls[c == 'D' ? 0 : 1];
      if (c == 'D') a.control |= SE_DACL_PRESENT;
      else a.control |= SE_SACL_PRESENT;
      s += 2;
      while (*s && *s != '(' && !((*s == 'O' || *s == 'G' || *s == 'D' || *s == 'S') && s[1] == ':')) s++;
      while (*s == '(') { /* (tipo;flags;direitos;;;sid) */
        const wchar16* f[6];
        int nf = 0, fl[6];
        uint8_t type = 0, flags = 0, sid[68];
        DWORD mask = 0;
        s++;
        f[nf] = s;
        while (*s && *s != ')') {
          if (*s == ';' && nf < 5) {
            fl[nf] = (int)(s - f[nf]);
            f[++nf] = s + 1;
          }
          s++;
        }
        fl[nf] = (int)(s - f[nf]);
        if (*s == ')') s++;
        if (nf < 5) continue;
        if (fl[0] == 1 && f[0][0] == 'A') type = 0;
        else if (fl[0] == 1 && f[0][0] == 'D') type = 1;
        else if (fl[0] == 2 && f[0][0] == 'M' && f[0][1] == 'L') type = 0x11;
        else if (fl[0] == 2 && f[0][0] == 'A' && f[0][1] == 'U') type = 2;
        {
          int k;
          for (k = 0; k + 1 < fl[1]; k += 2) {
            if (f[1][k] == 'O' && f[1][k + 1] == 'I') flags |= 1;
            if (f[1][k] == 'C' && f[1][k + 1] == 'I') flags |= 2;
            if (f[1][k] == 'N' && f[1][k + 1] == 'P') flags |= 4;
            if (f[1][k] == 'I' && f[1][k + 1] == 'O') flags |= 8;
            if (f[1][k] == 'I' && f[1][k + 1] == 'D') flags |= 0x10;
          }
        }
        rights_of(f[2], fl[2], &mask);
        if (sid_parse(f[5], sid)) add_ace(acl, type, flags, mask, sid);
      }
      continue;
    }
    s++;
  }
  if (have_o) a.owner = owner;
  if (have_g) a.group = group;
  if (a.control & SE_DACL_PRESENT) a.dacl = acls[0];
  if (a.control & SE_SACL_PRESENT) a.sacl = acls[1];
  /* as ACLs ficam do tamanho usado */
  for (i = 0; i < 2; i++) acls[i]->size = acl_used(acls[i]);
  n = to_rel(&a, 0, 0);
  out = (uint8_t*)lalloc(n);
  if (out) to_rel(&a, out, n);
  if (len) *len = n;
  k_free(acls[0]);
  k_free(acls[1]);
  return out;
}
static WINAPI BOOL ConvertStringSecurityDescriptorToSecurityDescriptorW(const wchar16* s, DWORD rev, void** sd,
                                                                        DWORD* len) {
  (void)rev;
  if (!sd) return 0;
  *sd = sddl_parse(s, len);
  return *sd != 0;
}
static WINAPI BOOL ConvertStringSecurityDescriptorToSecurityDescriptorA(const char* s, DWORD rev, void** sd,
                                                                        DWORD* len) {
  wchar16* w = wdup8to16(s);
  BOOL r = ConvertStringSecurityDescriptorToSecurityDescriptorW(w, rev, sd, len);
  k_free(w);
  return r;
}
static WINAPI BOOL ConvertSecurityDescriptorToStringSecurityDescriptorW(const void* sd, DWORD rev, DWORD what,
                                                                        wchar16** out, DWORD* len) {
  static const wchar16 txt[] = {'O', ':', 'B', 'A', 'G', ':', 'B', 'A', 'D', ':', 0};
  wchar16* w;
  (void)sd; (void)rev; (void)what;
  if (!out) return 0;
  w = (wchar16*)lalloc(sizeof(txt));
  if (!w) return 0;
  memcpy(w, txt, sizeof(txt));
  *out = w;
  if (len) *len = 10;
  return 1;
}

/* ---------- objetos: nada e imposto ---------- */
static WINAPI DWORD GetSecurityInfo(HANDLE h, int type, DWORD what, void** owner, void** group, void** dacl,
                                    void** sacl, void** sd) {
  uint8_t* d = sd_default(0);
  (void)h; (void)type; (void)what;
  if (!d) return 8;
  if (owner) *owner = sd_part(d, 0);
  if (group) *group = sd_part(d, 1);
  if (dacl) *dacl = 0;
  if (sacl) *sacl = 0;
  if (sd) *sd = d; /* as partes apontam para dentro dele (como no Windows) */
  return 0;
}
static WINAPI DWORD GetNamedSecurityInfoW(const wchar16* name, int type, DWORD what, void** owner, void** group,
                                          void** dacl, void** sacl, void** sd) {
  (void)name;
  return GetSecurityInfo(0, type, what, owner, group, dacl, sacl, sd);
}
static WINAPI DWORD SetSecurityInfo(HANDLE h, int type, DWORD what, void* o, void* g, void* d, void* s) {
  (void)h; (void)type; (void)what; (void)o; (void)g; (void)d; (void)s;
  return 0;
}
static WINAPI DWORD SetNamedSecurityInfoW(wchar16* name, int type, DWORD what, void* o, void* g, void* d, void* s) {
  (void)name; (void)type; (void)what; (void)o; (void)g; (void)d; (void)s;
  return 0;
}
static WINAPI BOOL SetKernelObjectSecurity(HANDLE h, DWORD what, const void* sd) {
  (void)h; (void)what; (void)sd;
  return 1;
}
static WINAPI BOOL GetKernelObjectSecurity(HANDLE h, DWORD what, void* sd, DWORD len, DWORD* need) {
  DWORD n = 0;
  uint8_t* d = sd_default(&n);
  (void)h; (void)what;
  if (!d) return 0;
  if (need) *need = n;
  if (!sd || len < n) {
    k_free(d);
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  memcpy(sd, d, n);
  k_free(d);
  return 1;
}
static WINAPI BOOL GetFileSecurityW(const wchar16* f, DWORD what, void* sd, DWORD len, DWORD* need) {
  (void)f;
  return GetKernelObjectSecurity(0, what, sd, len, need);
}
static WINAPI BOOL SetFileSecurityW(const wchar16* f, DWORD what, void* sd) {
  (void)f; (void)what; (void)sd;
  return 1;
}
static WINAPI BOOL CreatePrivateObjectSecurity(void* parent, void* creator, void** out, BOOL dir, HANDLE tok,
                                               void* map) {
  (void)parent; (void)creator; (void)dir; (void)tok; (void)map;
  if (!out) return 0;
  *out = sd_default(0);
  return *out != 0;
}
struct generic_map {
  DWORD r, w, x, all;
};
static WINAPI void MapGenericMask(DWORD* mask, const struct generic_map* m) {
  if (!mask || !m) return;
  if (*mask & 0x80000000u) *mask |= m->r;
  if (*mask & 0x40000000u) *mask |= m->w;
  if (*mask & 0x20000000u) *mask |= m->x;
  if (*mask & 0x10000000u) *mask |= m->all;
  *mask &= 0x0FFFFFFFu;
}
static WINAPI BOOL AccessCheck(void* sd, HANDLE tok, DWORD want, const struct generic_map* m, void* priv,
                               DWORD* privlen, DWORD* granted, BOOL* status) {
  (void)sd; (void)tok; (void)priv; (void)privlen;
  if (granted) {
    *granted = want;
    MapGenericMask(granted, m);
  }
  if (status) *status = 1;
  return 1;
}

/* ---------- token ---------- */
static WINAPI BOOL OpenProcessToken(HANDLE p, DWORD acc, HANDLE* tok) {
  struct wobj* o = wobj_new(OBJ_MISC);
  (void)p;
  (void)acc;
  *tok = o ? wh_new(o) : 0;
  return *tok != 0;
}
static WINAPI BOOL DuplicateTokenEx(HANDLE t, DWORD acc, void* sa, int lvl, int type, HANDLE* out) {
  (void)t; (void)sa; (void)lvl; (void)type;
  return OpenProcessToken(0, acc, out);
}
static WINAPI BOOL DuplicateToken(HANDLE t, int lvl, HANDLE* out) { return DuplicateTokenEx(t, 0, 0, lvl, 2, out); }
static WINAPI BOOL CreateRestrictedToken(HANDLE t, DWORD fl, DWORD nd, void* d, DWORD np, void* p, DWORD nr,
                                         void* r, HANDLE* out) {
  (void)t; (void)fl; (void)nd; (void)d; (void)np; (void)p; (void)nr; (void)r;
  return OpenProcessToken(0, 0, out);
}
static WINAPI BOOL SetTokenInformation(HANDLE t, int cls, void* buf, DWORD len) {
  (void)t; (void)cls; (void)buf; (void)len;
  return 1;
}
static WINAPI BOOL ImpersonateLoggedOnUser(HANDLE t) {
  (void)t;
  return 1;
}
static WINAPI BOOL RevertToSelf(void) { return 1; }
static WINAPI BOOL CheckTokenMembership(HANDLE tok, const void* sid, BOOL* member) {
  (void)tok;
  (void)sid;
  *member = 1; /* inclusive Administradores */
  return 1;
}

/* GetTokenInformation: monta a resposta em tmp e copia (ponteiros relativos ao buf) */
static BOOL tok_out(void* buf, DWORD len, DWORD* need, const uint8_t* tmp, DWORD n, const DWORD* ptr_offs, int nptr) {
  int i;
  if (need) *need = n;
  if (!buf || len < n) {
    wset_lasterr(ERROR_INSUFFICIENT_BUFFER);
    return 0;
  }
  memcpy(buf, tmp, n);
  for (i = 0; i < nptr; i++) { /* os ponteiros guardados como deslocamento viram endereco */
    uint64_t off;
    memcpy(&off, (uint8_t*)buf + ptr_offs[i], 8);
    off += (uint64_t)(uintptr_t)buf;
    memcpy((uint8_t*)buf + ptr_offs[i], &off, 8);
  }
  return 1;
}
static WINAPI BOOL GetTokenInformation(HANDLE tok, int cls, void* buf, DWORD len, DWORD* need) {
  uint8_t tmp[512];
  DWORD offs[8], n = 0;
  int np = 0;
  uint64_t v;
  (void)tok;
  memset(tmp, 0, sizeof(tmp));
  switch (cls) {
    case 1:  /* TokenUser: SID_AND_ATTRIBUTES {PSID, DWORD} + SID */
    case 4:  /* TokenOwner {PSID} */
    case 5:  /* TokenPrimaryGroup {PSID} */
    case 28: /* TokenLogonSid: TOKEN_GROUPS */
    {
      DWORD hdr = cls == 28 ? 24 : cls == 1 ? 16 : 8;
      v = hdr;
      if (cls == 28) {
        *(DWORD*)tmp = 1;
        memcpy(tmp + 8, &v, 8);
        *(DWORD*)(tmp + 16) = 0xC0000007u; /* LOGON_ID | MANDATORY | ENABLED */
        offs[np++] = 8;
      } else {
        memcpy(tmp, &v, 8);
        offs[np++] = 0;
      }
      memcpy(tmp + hdr, user_sid, sizeof(user_sid));
      n = hdr + sizeof(user_sid);
      break;
    }
    case 2: { /* TokenGroups: Everyone, Administradores, Users */
      static const int types[] = {1, 26, 27};
      DWORD off = 8 + 3 * 16, i;
      *(DWORD*)tmp = 3;
      for (i = 0; i < 3; i++) {
        DWORD sz = 32;
        CreateWellKnownSid(types[i], 0, tmp + off, &sz);
        v = off;
        memcpy(tmp + 8 + i * 16, &v, 8);
        *(DWORD*)(tmp + 8 + i * 16 + 8) = 7; /* MANDATORY | ENABLED_BY_DEFAULT | ENABLED */
        offs[np++] = 8 + i * 16;
        off += sz;
      }
      n = off;
      break;
    }
    case 3: { /* TokenPrivileges: os de um usuario comum (o Chromium exige pelo menos um) */
      /* SeShutdown 19, SeChangeNotify 23 (ligado), SeUndock 25, SeIncreaseWorkingSet 33, SeTimeZone 34 */
      static const DWORD luid[5] = {19, 23, 25, 33, 34};
      int k;
      *(DWORD*)tmp = 5;
      for (k = 0; k < 5; k++) {
        DWORD* e = (DWORD*)(tmp + 4 + 12 * k);
        e[0] = luid[k]; /* LUID.LowPart */
        e[1] = 0;       /* LUID.HighPart */
        e[2] = luid[k] == 23 ? 3u : 0u; /* SE_PRIVILEGE_ENABLED_BY_DEFAULT | ENABLED */
      }
      n = 4 + 12 * 5;
      break;
    }
    case 11: /* TokenRestrictedSids: nenhum */
      n = 8;
      break;
    case 6: { /* TokenDefaultDacl {PACL}: tudo para o usuario */
      struct acl* a = (struct acl*)(tmp + 8);
      InitializeAcl(a, 8 + 8 + sizeof(user_sid), 2);
      add_ace(a, 0, 0, 0x10000000u, user_sid);
      v = 8;
      memcpy(tmp, &v, 8);
      offs[np++] = 0;
      n = 8 + a->size;
      break;
    }
    case 8: *(DWORD*)tmp = 1; n = 4; break;  /* TokenType: Primary */
    case 9: *(DWORD*)tmp = 2; n = 4; break;  /* TokenImpersonationLevel */
    case 10: n = 56; break;                  /* TokenStatistics */
    case 12: *(DWORD*)tmp = 1; n = 4; break; /* TokenSessionId */
    case 18: *(DWORD*)tmp = 2; n = 4; break; /* TokenElevationType: Full */
    case 20: *(DWORD*)tmp = 1; n = 4; break; /* TokenElevation */
    case 21: *(DWORD*)tmp = 0; n = 4; break; /* TokenHasRestrictions */
    case 25: {                               /* TokenIntegrityLevel: High */
      DWORD sz = 32;
      v = 16;
      memcpy(tmp, &v, 8);
      *(DWORD*)(tmp + 8) = 0x20; /* SE_GROUP_INTEGRITY */
      CreateWellKnownSid(68, 0, tmp + 16, &sz);
      offs[np++] = 0;
      n = 16 + sz;
      break;
    }
    case 29: *(DWORD*)tmp = 0; n = 4; break; /* TokenIsAppContainer */
    case 31: n = 8; break;                   /* TokenAppContainerSid: NULL */
    case 34: *(DWORD*)tmp = 0; n = 4; break; /* TokenIsRestricted */
    default:
      if (need) *need = 0;
      wset_lasterr(ERROR_INVALID_PARAMETER);
      return 0;
  }
  return tok_out(buf, len, need, tmp, n, offs, np);
}

/* ---------- AppContainer: o SODOS nao isola; o perfil "existe" ---------- */
static WINAPI long DeriveAppContainerSidFromAppContainerName(const wchar16* name, void** sid) {
  uint8_t* s;
  DWORD sub[8], h = 2166136261u;
  int i;
  for (; name && *name; name++) h = (h ^ *name) * 16777619u;
  sub[0] = 2;
  for (i = 1; i < 8; i++) sub[i] = h = h * 2654435761u + (DWORD)i;
  s = (uint8_t*)lalloc(8 + 32);
  if (!s) return (long)0x8007000E;
  sid_make(s, 15, 8, sub);
  *sid = s;
  return 0;
}
static WINAPI long CreateAppContainerProfile(const wchar16* name, const wchar16* disp, const wchar16* desc,
                                             void* caps, DWORD ncaps, void** sid) {
  (void)disp; (void)desc; (void)caps; (void)ncaps;
  return DeriveAppContainerSidFromAppContainerName(name, sid);
}


/* ---------- TRUSTEE / EXPLICIT_ACCESS ---------- */
static WINAPI void BuildTrusteeWithSidW(struct trustee* t, void* sid) {
  if (!t) return;
  memset(t, 0, sizeof(*t));
  t->form = 0; /* TRUSTEE_IS_SID */
  t->type = 0; /* TRUSTEE_IS_UNKNOWN */
  t->name = (wchar16*)sid;
}
static WINAPI void BuildTrusteeWithNameW(struct trustee* t, wchar16* name) {
  if (!t) return;
  memset(t, 0, sizeof(*t));
  t->form = 1; /* TRUSTEE_IS_NAME */
  t->name = name;
}
static WINAPI void BuildExplicitAccessWithNameW(struct explicit_access* e, wchar16* name, DWORD perms, int mode,
                                                DWORD inherit) {
  if (!e) return;
  memset(e, 0, sizeof(*e));
  e->perms = perms;
  e->mode = mode;
  e->inherit = inherit;
  BuildTrusteeWithNameW(&e->t, name);
}
static WINAPI int GetTrusteeFormW(const struct trustee* t) { return t ? t->form : 0; }
static WINAPI wchar16* GetTrusteeNameW(const struct trustee* t) { return t ? t->name : 0; }
static WINAPI int GetTrusteeTypeW(const struct trustee* t) { return t ? t->type : 0; }
static WINAPI DWORD GetEffectiveRightsFromAclW(const struct acl* a, const struct trustee* t, DWORD* rights) {
  (void)a;
  (void)t;
  if (rights) *rights = 0x1F01FF; /* tudo */
  return 0;
}
static WINAPI DWORD BuildSecurityDescriptorW(void* owner, void* group, DWORD na, void* acc, DWORD nau, void* aud,
                                             void* old, DWORD* len, void** sd) {
  (void)owner; (void)group; (void)na; (void)acc; (void)nau; (void)aud; (void)old;
  if (!sd) return ERROR_INVALID_PARAMETER;
  *sd = sd_default(len);
  return *sd ? 0 : 8;
}

/* ---------- token (mais) ---------- */
static WINAPI BOOL SetThreadToken(HANDLE* th, HANDLE tok) {
  (void)th;
  (void)tok;
  return 1;
}
static WINAPI BOOL ImpersonateAnonymousToken(HANDLE th) {
  (void)th;
  return 1;
}
static WINAPI BOOL CreateAppContainerToken(HANDLE tok, void* caps, HANDLE* out) {
  (void)tok;
  (void)caps;
  return OpenProcessToken(0, 0, out);
}
static WINAPI DWORD AppContainerRegisterSid(void* sid, const wchar16* moniker, const wchar16* disp) {
  (void)sid; (void)moniker; (void)disp;
  return 0;
}
static WINAPI DWORD AppContainerUnregisterSid(void* sid) {
  (void)sid;
  return 0;
}
static WINAPI long NtOpenProcessTokenEx(HANDLE p, DWORD acc, DWORD attr, HANDLE* tok) {
  (void)attr;
  return OpenProcessToken(p, acc, tok) ? 0 : (long)0xC0000022;
}
static WINAPI long NtOpenProcessToken(HANDLE p, DWORD acc, HANDLE* tok) { return NtOpenProcessTokenEx(p, acc, 0, tok); }
static WINAPI long NtOpenThreadTokenEx(HANDLE t, DWORD acc, BOOL self, DWORD attr, HANDLE* tok) {
  (void)t; (void)acc; (void)self; (void)attr;
  if (tok) *tok = 0;
  return (long)0xC000007C; /* STATUS_NO_TOKEN: a thread nao personifica ninguem */
}
static WINAPI long NtOpenThreadToken(HANDLE t, DWORD acc, BOOL self, HANDLE* tok) {
  return NtOpenThreadTokenEx(t, acc, self, 0, tok);
}

#define S(n) {#n, (void*)n}
const struct wine_sym sec_syms[] = {
    S(BuildTrusteeWithSidW), S(BuildTrusteeWithNameW), S(BuildExplicitAccessWithNameW), S(GetTrusteeFormW),
    S(GetTrusteeNameW), S(GetTrusteeTypeW), S(GetEffectiveRightsFromAclW), S(BuildSecurityDescriptorW),
    S(SetThreadToken), S(ImpersonateAnonymousToken), S(CreateAppContainerToken), S(AppContainerRegisterSid),
    S(AppContainerUnregisterSid), S(NtOpenProcessTokenEx), S(NtOpenProcessToken), S(NtOpenThreadTokenEx),
    S(NtOpenThreadToken),
    S(GetLengthSid), S(IsValidSid), S(CopySid), S(EqualSid), S(EqualPrefixSid), S(AllocateAndInitializeSid),
    S(FreeSid), S(InitializeSid), S(GetSidSubAuthority), S(GetSidSubAuthorityCount), S(GetSidIdentifierAuthority),
    S(CreateWellKnownSid), S(IsWellKnownSid), S(GetWindowsAccountDomainSid), S(ConvertStringSidToSidW),
    S(ConvertSidToStringSidW), S(LookupAccountSidW), S(InitializeAcl), S(IsValidAcl), S(GetAce), S(AddAce),
    S(AddAccessAllowedAce), S(AddAccessAllowedAceEx), S(AddAccessDeniedAce), S(AddAccessDeniedAceEx),
    S(AddMandatoryAce), S(GetAclInformation), S(SetEntriesInAclW), S(GetExplicitEntriesFromAclW),
    S(InitializeSecurityDescriptor), S(IsValidSecurityDescriptor), S(GetSecurityDescriptorControl),
    S(SetSecurityDescriptorControl), S(SetSecurityDescriptorOwner), S(SetSecurityDescriptorGroup),
    S(SetSecurityDescriptorDacl), S(SetSecurityDescriptorSacl), S(GetSecurityDescriptorOwner),
    S(GetSecurityDescriptorGroup), S(GetSecurityDescriptorDacl), S(GetSecurityDescriptorSacl),
    S(GetSecurityDescriptorLength), S(MakeSelfRelativeSD), S(MakeAbsoluteSD),
    S(ConvertStringSecurityDescriptorToSecurityDescriptorW), S(ConvertStringSecurityDescriptorToSecurityDescriptorA),
    S(ConvertSecurityDescriptorToStringSecurityDescriptorW), S(GetSecurityInfo), S(GetNamedSecurityInfoW),
    S(SetSecurityInfo), S(SetNamedSecurityInfoW), S(SetKernelObjectSecurity), S(GetKernelObjectSecurity),
    S(GetFileSecurityW), S(SetFileSecurityW), S(CreatePrivateObjectSecurity), S(MapGenericMask), S(AccessCheck),
    S(OpenProcessToken), S(DuplicateTokenEx), S(DuplicateToken), S(CreateRestrictedToken), S(SetTokenInformation),
    S(ImpersonateLoggedOnUser), S(RevertToSelf), S(CheckTokenMembership), S(GetTokenInformation),
    S(DeriveAppContainerSidFromAppContainerName), S(CreateAppContainerProfile), {0, 0}};
