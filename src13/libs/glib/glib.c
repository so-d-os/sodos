/* glib.c - subset GLib sobre LIBCX/k_* (gcc -mx32). Saida: GLIB.DLL */
#include <glib.h>
#include <so.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void* g_malloc(gsize n) {
  void* p;
  if (n == 0) n = 1;
  p = malloc(n);
  if (!p) k_exit(99);
  return p;
}

void* g_malloc0(gsize n) {
  void* p = g_malloc(n);
  memset(p, 0, n);
  return p;
}

void* g_realloc(void* p, gsize n) {
  void* q;
  if (n == 0) {
    free(p);
    return NULL;
  }
  q = realloc(p, n);
  if (!q) k_exit(99);
  return q;
}

void g_free(void* p) { free(p); }

gchar* g_strdup(const gchar* s) {
  gsize n;
  gchar* d;
  if (!s) return NULL;
  n = strlen(s) + 1;
  d = (gchar*)g_malloc(n);
  memcpy(d, s, n);
  return d;
}

gchar* g_strndup(const gchar* s, gsize n) {
  gsize i;
  gchar* d;
  if (!s) return NULL;
  for (i = 0; i < n && s[i]; i++) {
  }
  d = (gchar*)g_malloc(i + 1);
  memcpy(d, s, i);
  d[i] = 0;
  return d;
}

gint g_vsnprintf(gchar* buf, gulong n, const gchar* fmt, va_list ap) {
  return vsnprintf(buf, (size_t)n, fmt, ap);
}

gint g_snprintf(gchar* buf, gulong n, const gchar* fmt, ...) {
  va_list ap;
  gint r;
  va_start(ap, fmt);
  r = g_vsnprintf(buf, n, fmt, ap);
  va_end(ap);
  return r;
}

gchar* g_strdup_vprintf(const gchar* fmt, va_list ap) {
  char tmp[512];
  g_vsnprintf(tmp, sizeof tmp, fmt, ap);
  return g_strdup(tmp);
}

gchar* g_strdup_printf(const gchar* fmt, ...) {
  va_list ap;
  gchar* r;
  va_start(ap, fmt);
  r = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  return r;
}

gchar* g_strconcat(const gchar* first, ...) {
  va_list ap;
  const gchar* s;
  gsize n = 0;
  gchar* out;
  gchar* p;
  if (!first) return g_strdup("");
  n = strlen(first);
  va_start(ap, first);
  while ((s = va_arg(ap, const gchar*)) != NULL) n += strlen(s);
  va_end(ap);
  out = (gchar*)g_malloc(n + 1);
  p = out;
  strcpy(p, first);
  p += strlen(first);
  va_start(ap, first);
  while ((s = va_arg(ap, const gchar*)) != NULL) {
    strcpy(p, s);
    p += strlen(s);
  }
  va_end(ap);
  return out;
}

gchar** g_strsplit(const gchar* s, const gchar* delim, gint max) {
  guint cap = 8, n = 0;
  gchar** v;
  const gchar* p;
  gsize dlen;
  if (!s) return NULL;
  if (!delim || !delim[0]) {
    v = g_new(gchar*, 2);
    v[0] = g_strdup(s);
    v[1] = NULL;
    return v;
  }
  dlen = strlen(delim);
  v = g_new(gchar*, cap);
  p = s;
  while (*p && (max < 0 || (gint)n + 1 < max)) {
    const gchar* q = strstr(p, delim);
    gsize len;
    if (!q) break;
    len = (gsize)(q - p);
    if (n + 2 >= cap) {
      cap *= 2;
      v = g_renew(gchar*, v, cap);
    }
    v[n++] = g_strndup(p, len);
    p = q + dlen;
  }
  if (n + 2 >= cap) {
    cap += 2;
    v = g_renew(gchar*, v, cap);
  }
  v[n++] = g_strdup(p);
  v[n] = NULL;
  return v;
}

void g_strfreev(gchar** v) {
  guint i;
  if (!v) return;
  for (i = 0; v[i]; i++) g_free(v[i]);
  g_free(v);
}

guint g_strv_length(gchar** v) {
  guint n = 0;
  if (!v) return 0;
  while (v[n]) n++;
  return n;
}

GList* g_list_prepend(GList* list, gpointer data) {
  GList* n = g_new(GList, 1);
  n->data = data;
  n->next = list;
  n->prev = NULL;
  if (list) list->prev = n;
  return n;
}

GList* g_list_append(GList* list, gpointer data) {
  GList* n = g_new(GList, 1);
  GList* t = list;
  n->data = data;
  n->next = NULL;
  n->prev = NULL;
  if (!list) return n;
  while (t->next) t = t->next;
  t->next = n;
  n->prev = t;
  return list;
}

GList* g_list_remove(GList* list, gconstpointer data) {
  GList* t = list;
  while (t) {
    if (t->data == data) {
      if (t->prev) t->prev->next = t->next;
      else list = t->next;
      if (t->next) t->next->prev = t->prev;
      g_free(t);
      return list;
    }
    t = t->next;
  }
  return list;
}

void g_list_free(GList* list) {
  while (list) {
    GList* n = list->next;
    g_free(list);
    list = n;
  }
}

void g_list_free_full(GList* list, void (*free_func)(gpointer)) {
  while (list) {
    GList* n = list->next;
    if (free_func) free_func(list->data);
    g_free(list);
    list = n;
  }
}

guint g_list_length(GList* list) {
  guint n = 0;
  while (list) {
    n++;
    list = list->next;
  }
  return n;
}

GList* g_list_nth(GList* list, guint n) {
  while (list && n--) list = list->next;
  return list;
}

gpointer g_list_nth_data(GList* list, guint n) {
  GList* t = g_list_nth(list, n);
  return t ? t->data : NULL;
}

GSList* g_slist_prepend(GSList* list, gpointer data) {
  GSList* n = g_new(GSList, 1);
  n->data = data;
  n->next = list;
  return n;
}

GSList* g_slist_append(GSList* list, gpointer data) {
  GSList* n = g_new(GSList, 1);
  GSList* t = list;
  n->data = data;
  n->next = NULL;
  if (!list) return n;
  while (t->next) t = t->next;
  t->next = n;
  return list;
}

void g_slist_free(GSList* list) {
  while (list) {
    GSList* n = list->next;
    g_free(list);
    list = n;
  }
}

guint g_slist_length(GSList* list) {
  guint n = 0;
  while (list) {
    n++;
    list = list->next;
  }
  return n;
}

static void g_string_ensure(GString* s, gsize need) {
  gsize cap;
  if (need <= s->allocated_len) return;
  cap = s->allocated_len ? s->allocated_len : 16;
  while (cap < need) cap *= 2;
  s->str = (gchar*)g_realloc(s->str, cap);
  s->allocated_len = cap;
}

GString* g_string_new(const gchar* init) {
  GString* s = g_new0(GString, 1);
  if (init) {
    s->len = strlen(init);
    s->allocated_len = s->len + 1;
    s->str = g_strdup(init);
  } else {
    s->str = (gchar*)g_malloc(1);
    s->str[0] = 0;
    s->allocated_len = 1;
  }
  return s;
}

GString* g_string_append(GString* s, const gchar* val) {
  gsize n;
  if (!s || !val) return s;
  n = strlen(val);
  g_string_ensure(s, s->len + n + 1);
  memcpy(s->str + s->len, val, n + 1);
  s->len += n;
  return s;
}

GString* g_string_append_c(GString* s, gchar c) {
  if (!s) return s;
  g_string_ensure(s, s->len + 2);
  s->str[s->len++] = c;
  s->str[s->len] = 0;
  return s;
}

GString* g_string_printf(GString* s, const gchar* fmt, ...) {
  char tmp[512];
  va_list ap;
  if (!s) return s;
  va_start(ap, fmt);
  g_vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  g_free(s->str);
  s->str = g_strdup(tmp);
  s->len = strlen(s->str);
  s->allocated_len = s->len + 1;
  return s;
}

gchar* g_string_free(GString* s, gboolean free_segment) {
  gchar* r = NULL;
  if (!s) return NULL;
  if (free_segment) g_free(s->str);
  else r = s->str;
  g_free(s);
  return r;
}

gchar* g_build_filename(const gchar* first, ...) {
  va_list ap;
  const gchar* s;
  GString* gs;
  if (!first) return g_strdup("");
  gs = g_string_new(first);
  va_start(ap, first);
  while ((s = va_arg(ap, const gchar*)) != NULL) {
    if (gs->len && gs->str[gs->len - 1] != '/' && gs->str[gs->len - 1] != '\\')
      g_string_append_c(gs, '/');
    g_string_append(gs, s);
  }
  va_end(ap);
  return g_string_free(gs, FALSE);
}

struct _GPtrArrayPriv {
  gpointer* pdata;
  guint len;
  guint alloc;
  void (*free_func)(gpointer);
};

GPtrArray* g_ptr_array_new(void) {
  return g_ptr_array_new_with_free_func(NULL);
}

GPtrArray* g_ptr_array_new_with_free_func(void (*ff)(gpointer)) {
  struct _GPtrArrayPriv* a = g_new0(struct _GPtrArrayPriv, 1);
  a->alloc = 8;
  a->pdata = g_new(gpointer, a->alloc);
  a->free_func = ff;
  return (GPtrArray*)a;
}

void g_ptr_array_add(GPtrArray* arr, gpointer data) {
  struct _GPtrArrayPriv* a = (struct _GPtrArrayPriv*)arr;
  if (!a) return;
  if (a->len >= a->alloc) {
    a->alloc *= 2;
    a->pdata = g_renew(gpointer, a->pdata, a->alloc);
  }
  a->pdata[a->len++] = data;
}

void g_ptr_array_free(GPtrArray* arr, gboolean free_seg) {
  struct _GPtrArrayPriv* a = (struct _GPtrArrayPriv*)arr;
  guint i;
  if (!a) return;
  if (free_seg) {
    for (i = 0; i < a->len; i++) {
      if (a->free_func) a->free_func(a->pdata[i]);
    }
    g_free(a->pdata);
  }
  g_free(a);
}

gint64 g_get_monotonic_time(void) {
  return (gint64)k_ms() * 1000;
}
