/* glib.h - subset GLib para o port MC (gcc -mx32 / GLIB.DLL). */
#ifndef _GLIB_H
#define _GLIB_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef char gchar;
typedef int gint;
typedef unsigned guint;
typedef int gboolean;
typedef void* gpointer;
typedef const void* gconstpointer;
typedef size_t gsize;
typedef unsigned long gulong;
typedef long long gint64;

#define TRUE 1
#define FALSE 0
#define G_GNUC_PRINTF(a, b)

void* g_malloc(gsize n);
void* g_malloc0(gsize n);
void* g_realloc(void* p, gsize n);
void g_free(void* p);
#define g_new(t, n) ((t*)g_malloc((gsize)(n) * sizeof(t)))
#define g_new0(t, n) ((t*)g_malloc0((gsize)(n) * sizeof(t)))
#define g_renew(t, p, n) ((t*)g_realloc((p), (gsize)(n) * sizeof(t)))

gchar* g_strdup(const gchar* s);
gchar* g_strndup(const gchar* s, gsize n);
gchar* g_strdup_printf(const gchar* fmt, ...) G_GNUC_PRINTF(1, 2);
gchar* g_strdup_vprintf(const gchar* fmt, va_list ap);
gchar* g_strconcat(const gchar* first, ...);
gint g_snprintf(gchar* buf, gulong n, const gchar* fmt, ...) G_GNUC_PRINTF(3, 4);
gint g_vsnprintf(gchar* buf, gulong n, const gchar* fmt, va_list ap);
gchar** g_strsplit(const gchar* s, const gchar* delim, gint max);
void g_strfreev(gchar** v);
guint g_strv_length(gchar** v);
gchar* g_build_filename(const gchar* first, ...);

typedef struct _GList GList;
struct _GList {
  gpointer data;
  GList* next;
  GList* prev;
};

GList* g_list_append(GList* list, gpointer data);
GList* g_list_prepend(GList* list, gpointer data);
GList* g_list_remove(GList* list, gconstpointer data);
void g_list_free(GList* list);
void g_list_free_full(GList* list, void (*free_func)(gpointer));
guint g_list_length(GList* list);
GList* g_list_nth(GList* list, guint n);
gpointer g_list_nth_data(GList* list, guint n);

typedef struct _GSList GSList;
struct _GSList {
  gpointer data;
  GSList* next;
};

GSList* g_slist_append(GSList* list, gpointer data);
GSList* g_slist_prepend(GSList* list, gpointer data);
void g_slist_free(GSList* list);
guint g_slist_length(GSList* list);

typedef struct _GString GString;
struct _GString {
  gchar* str;
  gsize len;
  gsize allocated_len;
};

GString* g_string_new(const gchar* init);
GString* g_string_append(GString* s, const gchar* val);
GString* g_string_append_c(GString* s, gchar c);
GString* g_string_printf(GString* s, const gchar* fmt, ...) G_GNUC_PRINTF(2, 3);
gchar* g_string_free(GString* s, gboolean free_segment);

typedef struct _GPtrArray GPtrArray;
struct _GPtrArray {
  gpointer* pdata;
  guint len;
};

GPtrArray* g_ptr_array_new(void);
GPtrArray* g_ptr_array_new_with_free_func(void (*ff)(gpointer));
void g_ptr_array_add(GPtrArray* a, gpointer data);
#define g_ptr_array_index(a, i) ((a)->pdata[(i)])
void g_ptr_array_free(GPtrArray* a, gboolean free_seg);

gint64 g_get_monotonic_time(void);

#ifdef __cplusplus
}
#endif

#endif
