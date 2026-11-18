/*
 * pkg.c - Windows.Management.Deployment.PackageManager: instala um .msix.
 *
 * No Windows, AddPackage registra o pacote no sistema. No SODOS "instalar" e
 * extrair o pacote (um ZIP) para
 *
 *     D:\WINE\APPS\<Nome>_<versao>_<arq>__<publicador>\
 *
 * e gravar INSTALL.TXT com o que foi instalado. O executavel do pacote so
 * roda se o WINE.DLL tiver as APIs que ele pede (um app Electron ainda nao).
 *
 * Interfaces atendidas (as que o instalador do Claude Desktop usa):
 *   IPackageManager       FindPackages, FindPackagesByUserSecurityId,
 *                         AddPackageAsync, e as variantes de Add/Stage/Register
 *   IUriRuntimeClassFactory.CreateUri
 *   IAsyncOperationWithProgress / IAsyncInfo  (operacao ja concluida)
 *   IDeploymentResult     ErrorText, ExtendedErrorCode
 *   IIterable / IIterator vazios
 */
#include <so.h>
#include <string.h>
#include "wine_int.h"
#include "winrt.h"
#include "zip.h"

const uint8_t* wfile_map(const char* path, uint64_t* size, void** tofree);
const char* wine_dir(const char* rel);
void wensure_dir(const char* path);
void wpublisher_id(const wchar16* publisher, char out[14]);

/* ---------- extracao ---------- */
struct pkg_result {
  long hr;
  char text[200];
  char dir[200];
  unsigned files, bytes_mb;
};

/* atributo="valor" depois de <Identity */
static int xml_attr(const char* xml, const char* tag, const char* attr, char* out, int max) {
  const char* t = strstr(xml, tag);
  const char* end;
  const char* a;
  size_t al = strlen(attr);
  int n = 0;
  if (!t) return 0;
  end = strchr(t, '>');
  if (!end) return 0;
  for (a = t; a < end; a++) {
    if ((a[-1] == ' ' || a[-1] == '\n' || a[-1] == '\t' || a[-1] == '\r') && strncmp(a, attr, al) == 0 && a[al] == '=') {
      char q = a[al + 1];
      const char* v = a + al + 2;
      while (v < end + 1 && *v != q && n < max - 1) out[n++] = *v++;
      out[n] = 0;
      return 1;
    }
  }
  return 0;
}

/* &quot; &amp; ... do XML */
static void xml_unescape(char* s) {
  char* d = s;
  while (*s) {
    if (*s == '&') {
      if (strncmp(s, "&quot;", 6) == 0) { *d++ = '"'; s += 6; continue; }
      if (strncmp(s, "&amp;", 5) == 0) { *d++ = '&'; s += 5; continue; }
      if (strncmp(s, "&apos;", 6) == 0) { *d++ = '\''; s += 6; continue; }
      if (strncmp(s, "&lt;", 4) == 0) { *d++ = '<'; s += 4; continue; }
      if (strncmp(s, "&gt;", 4) == 0) { *d++ = '>'; s += 4; continue; }
    }
    *d++ = *s++;
  }
  *d = 0;
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void install_msix(const char* path, struct pkg_result* r) {
  uint64_t size = 0;
  void* tofree = 0;
  const uint8_t* data = wfile_map(path, &size, &tofree);
  struct zip z;
  struct zip_entry en;
  char name[80], ver[40], arch[16], pub[300], pid[16], full[200];
  uint64_t total = 0;
  unsigned t0 = k_ms(), fail = 0;
  memset(r, 0, sizeof(*r));
  r->hr = E_FAIL;
  if (!data || !zip_open(&z, data, size)) {
    strcpy(r->text, "pacote ilegivel (nao e um ZIP/MSIX)");
    goto out;
  }
  /* manifesto: identidade do pacote */
  name[0] = ver[0] = arch[0] = pub[0] = 0;
  while (zip_next(&z, &en)) {
    if (wstreq(en.name, "AppxManifest.xml")) {
      char* xml = (char*)zip_extract(&z, &en);
      if (xml) {
        xml[en.usize] = 0;
        xml_attr(xml, "<Identity", "Name", name, (int)sizeof(name));
        xml_attr(xml, "<Identity", "Version", ver, (int)sizeof(ver));
        xml_attr(xml, "<Identity", "ProcessorArchitecture", arch, (int)sizeof(arch));
        xml_attr(xml, "<Identity", "Publisher", pub, (int)sizeof(pub));
        k_free(xml);
      }
      break;
    }
  }
  if (!name[0] || !ver[0]) {
    strcpy(r->text, "AppxManifest.xml sem <Identity>");
    goto out;
  }
  xml_unescape(pub);
  {
    wchar16* wp16 = wdup8to16(pub);
    if (wp16) wpublisher_id(wp16, pid);
    else strcpy(pid, "0000000000000");
    k_free(wp16);
  }
  strcpy(full, name);
  strcat(full, "_");
  strcat(full, ver);
  strcat(full, "_");
  strcat(full, arch[0] ? arch : "neutral");
  strcat(full, "__");
  strcat(full, pid);
  {
    char rel[240];
    strcpy(rel, "APPS\\");
    strcat(rel, full);
    strncpy(r->dir, wine_dir(rel), sizeof(r->dir) - 1);
  }
  wlog("instalando pacote ", full);
  wlog("  em ", r->dir);
  /* todos os arquivos */
  zip_open(&z, data, size);
  while (zip_next(&z, &en)) {
    char out[400];
    uint8_t* buf;
    size_t o;
    const char* s;
    char* slash;
    if (en.is_dir) continue;
    strcpy(out, r->dir);
    o = strlen(out);
    out[o++] = '\\';
    for (s = en.name; *s && o < sizeof(out) - 1; s++) {
      /* nomes do MSIX vem com %20 etc. */
      if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) {
        out[o++] = (char)(hexv(s[1]) * 16 + hexv(s[2]));
        s += 2;
      } else {
        out[o++] = *s == '/' ? '\\' : *s;
      }
    }
    out[o] = 0;
    if (o >= 255) {
      if (fail++ < 5) wlog("  caminho longo demais: ", en.name);
      continue;
    }
    slash = strrchr(out, '\\');
    if (slash) {
      *slash = 0;
      wensure_dir(out);
      *slash = '\\';
    }
    {
      unsigned ta = k_ms(), tb;
      buf = zip_extract(&z, &en);
      if (!buf) {
        if (fail++ < 5) wlog("  falhou ao descomprimir: ", en.name);
        continue;
      }
      tb = k_ms();
      if (k_fsave(out, (const char*)buf, (int)en.usize) < 0) {
        if (fail++ < 5) wlog("  falhou ao gravar: ", out);
      } else {
        r->files++;
        total += en.usize;
      }
      k_free(buf);
      if (en.usize >= (8u << 20) || r->files <= 3) {
        wlog("  arquivo ", en.name);
        wlogx("    MB ", en.usize >> 20);
        wlogx("    ms descomprimindo ", tb - ta);
        wlogx("    ms gravando ", k_ms() - tb);
      }
    }
    if (r->files && (r->files & 63) == 0) {
      wlogx("  arquivos gravados ", r->files);
      wlogx("  MB ", total >> 20);
      wlogx("  segundos ", (k_ms() - t0) / 1000);
    }
  }
  r->bytes_mb = (unsigned)(total >> 20);
  {
    char info[600], note[300];
    strcpy(note, r->dir);
    strcat(note, "\\INSTALL.TXT");
    strcpy(info, "Pacote: ");
    strcat(info, full);
    strcat(info, "\r\nPublicador: ");
    strncat(info, pub, 300);
    strcat(info, "\r\nOrigem: ");
    strncat(info, path, 200);
    strcat(info, "\r\nInstalado pelo WINE.DLL do SODOS (PackageManager.AddPackage).\r\n");
    k_fsave(note, info, (int)strlen(info));
  }
  wlogx("pacote instalado, arquivos ", r->files);
  wlogx("  MB ", r->bytes_mb);
  wlogx("  falhas ", fail);
  wlogx("  segundos ", (k_ms() - t0) / 1000);
  if (r->files && fail * 20 < r->files + 20) {
    r->hr = S_OK;
  } else {
    strcpy(r->text, "falha gravando os arquivos do pacote");
  }
out:
  if (tofree) k_free(tofree);
  if (r->hr != S_OK) wlog("instalacao do pacote falhou: ", r->text);
}

/* ---------- objetos WinRT ---------- */
/* IIterator<T> vazio: 6 Current, 7 HasCurrent, 8 MoveNext, 9 GetMany */
static long iter_handler(struct comobj* self, int slot, uintptr_t* a) {
  (void)self;
  switch (slot) {
    case 6: *(void**)a[0] = 0; return (long)0x8000000B; /* E_BOUNDS */
    case 7: *(uint8_t*)a[0] = 0; return S_OK;
    case 8: *(uint8_t*)a[0] = 0; return S_OK;
    case 9: *(DWORD*)a[2] = 0; return S_OK;
  }
  return E_NOTIMPL;
}
/* IIterable<T>: 6 First.  IVectorView<T>: 6 GetAt, 7 Size, 8 IndexOf, 9 GetMany */
static long iterable_handler(struct comobj* self, int slot, uintptr_t* a) {
  (void)self;
  if (slot == 6) {
    *(void**)a[0] = com_new("IIterator(vazio)", iter_handler, 0);
    return S_OK;
  }
  if (slot == 7) {
    *(DWORD*)a[0] = 0;
    return S_OK;
  }
  return E_NOTIMPL;
}

/* IDeploymentResult: 6 ErrorText, 7 ActivityId, 8 ExtendedErrorCode */
static long result_handler(struct comobj* self, int slot, uintptr_t* a) {
  struct pkg_result* r = (struct pkg_result*)self->data;
  switch (slot) {
    case 6: return whstring_from8(r->text, (void**)a[0]);
    case 7: memset((void*)a[0], 0, 16); return S_OK;
    case 8: *(long*)a[0] = r->hr; return S_OK;
    case 9: *(uint8_t*)a[0] = 1; return S_OK; /* IDeploymentResult2.IsRegistered */
  }
  return E_NOTIMPL;
}

/* Operacao assincrona ja concluida.
 * IAsyncOperationWithProgress: 6 put_Progress, 7 get_Progress, 8 put_Completed,
 *                              9 get_Completed, 10 GetResults
 * IAsyncInfo {00000036-...}:   6 Id, 7 Status, 8 ErrorCode, 9 Cancel, 10 Close */
static long asyncinfo_handler(struct comobj* self, int slot, uintptr_t* a) {
  struct pkg_result* r = (struct pkg_result*)self->data;
  switch (slot) {
    case 6: *(DWORD*)a[0] = 1; return S_OK;
    case 7: *(DWORD*)a[0] = r->hr == S_OK ? 1 : 3; return S_OK; /* Completed / Error */
    case 8: *(long*)a[0] = r->hr; return S_OK;
    case 9: case 10: return S_OK;
  }
  return E_NOTIMPL;
}
static long asyncop_handler(struct comobj* self, int slot, uintptr_t* a) {
  struct pkg_result* r = (struct pkg_result*)self->data;
  if (slot == 0) { /* QI */
    const DWORD* iid = (const DWORD*)a[0];
    if (iid[0] == 0x00000036u) {
      *(void**)a[1] = com_new("IAsyncInfo", asyncinfo_handler, r);
      return S_OK;
    }
    return E_NOTIMPL;
  }
  switch (slot) {
    case 6: return S_OK; /* put_Progress: nao ha progresso a relatar */
    case 7: *(void**)a[0] = 0; return S_OK;
    case 8: { /* put_Completed(handler): ja terminou, chama agora */
      struct comobj** h = (struct comobj**)a[0];
      typedef long (WINAPI * invoke_fn)(void*, void*, DWORD);
      if (h && *h) ((invoke_fn)((void**)*h)[3])(h, self, r->hr == S_OK ? 1 : 3);
      return S_OK;
    }
    case 9: *(void**)a[0] = 0; return S_OK;
    case 10:
      *(void**)a[0] = com_new("DeploymentResult", result_handler, r);
      return S_OK;
  }
  return E_NOTIMPL;
}

/* Uri: guarda o texto; IUriRuntimeClass 6 AbsoluteUri, 7 DisplayUri, 16 RawUri... */
static long uri_handler(struct comobj* self, int slot, uintptr_t* a) {
  if (slot == 6 || slot == 7 || slot == 16) return whstring_from8((const char*)self->data, (void**)a[0]);
  return E_NOTIMPL;
}
static long urifactory_handler(struct comobj* self, int slot, uintptr_t* a) {
  (void)self;
  if (slot == 6) { /* CreateUri(HSTRING, out) */
    char* s = whstring8((void*)a[0]);
    struct comobj* u = com_new("Windows.Foundation.Uri", uri_handler, s);
    if (wine_debug) wlog("CreateUri ", s ? s : "");
    *(void**)a[1] = u;
    return u ? S_OK : E_OUTOFMEMORY;
  }
  return E_NOTIMPL;
}

/* "file:///D:/WINE/TEMP/x.msix" ou "D:\..." -> caminho do SODOS */
static void uri_to_path(const char* uri, char* out, int max) {
  const char* s = uri;
  int o = 0;
  if (strncmp(s, "file:///", 8) == 0) s += 8;
  else if (strncmp(s, "file://", 7) == 0) s += 7;
  for (; *s && o < max - 1; s++) {
    if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) {
      out[o++] = (char)(hexv(s[1]) * 16 + hexv(s[2]));
      s += 2;
    } else {
      out[o++] = *s == '/' ? '\\' : *s;
    }
  }
  out[o] = 0;
}

static long add_package(struct comobj* uri, void** out) {
  struct pkg_result* r = (struct pkg_result*)wzalloc(sizeof(*r));
  char path[300];
  if (!r) return E_OUTOFMEMORY;
  if (!uri || !uri->data) {
    r->hr = E_FAIL;
    strcpy(r->text, "sem URI do pacote");
  } else {
    uri_to_path((const char*)uri->data, path, (int)sizeof(path));
    install_msix(path, r);
  }
  *out = com_new("AsyncOperation(Deployment)", asyncop_handler, r);
  return S_OK;
}

/* PackageManager. O QI devolve uma "face" por interface, para os slots
 * dizerem de qual interface e o metodo (u[0] = 1o DWORD do IID). */
static long pm_handler(struct comobj* self, int slot, uintptr_t* a) {
  DWORD kind = (DWORD)self->u[0];
  if (slot == 0) {
    const DWORD* iid = (const DWORD*)a[0];
    struct comobj* f;
    /* IUnknown, IInspectable, IAgileObject: o proprio objeto */
    if (iid[0] == 0 || iid[0] == 0xAF86E2E0u || iid[0] == 0x94EA2B94u) return E_NOTIMPL;
    f = com_new("PackageManager", pm_handler, self->data);
    if (!f) return E_OUTOFMEMORY;
    f->u[0] = iid[0];
    *(void**)a[1] = f;
    return S_OK;
  }
  /* IPackageManager {9A7D4B65-...} (ou face desconhecida usada como tal) */
  if (kind == 0x9A7D4B65u || kind == 0) {
    switch (slot) {
      case 6:  /* AddPackageAsync(uri, deps, options, out) */
      case 7:  /* UpdatePackageAsync(uri, deps, options, out) */
      case 9:  /* StagePackageAsync(uri, deps, out) */
        return add_package((struct comobj*)a[0], (void**)(slot == 9 ? a[2] : a[3]));
      case 11: /* FindPackages(out) */
        *(void**)a[0] = com_new("IIterable<Package>(vazio)", iterable_handler, 0);
        return S_OK;
      case 12: /* FindPackagesByUserSecurityId(sid, out) */
        *(void**)a[1] = com_new("IIterable<Package>(vazio)", iterable_handler, 0);
        return S_OK;
      case 13: /* FindPackagesByNamePublisher(name, publisher, out) */
        *(void**)a[2] = com_new("IIterable<Package>(vazio)", iterable_handler, 0);
        return S_OK;
      case 14: /* FindPackagesByUserSecurityIdNamePublisher(sid, name, publisher, out) */
        *(void**)a[3] = com_new("IIterable<Package>(vazio)", iterable_handler, 0);
        return S_OK;
      case 19: /* FindPackagesByPackageFamilyName(family, out) */
        *(void**)a[1] = com_new("IIterable<Package>(vazio)", iterable_handler, 0);
        return S_OK;
      case 20: /* FindPackagesByUserSecurityIdPackageFamilyName(sid, family, out) */
        *(void**)a[2] = com_new("IIterable<Package>(vazio)", iterable_handler, 0);
        return S_OK;
    }
  }
  if (wine_debug) wlogx("PackageManager: interface (1o DWORD do IID) ", kind);
  return E_NOTIMPL;
}

com_handler pkg_handler_for(const char* cls) {
  if (wstreq(cls, "Windows.Management.Deployment.PackageManager")) return pm_handler;
  if (wstreq(cls, "Windows.Foundation.Uri")) return urifactory_handler;
  return 0;
}
