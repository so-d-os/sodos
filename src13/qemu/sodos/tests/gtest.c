/* gtest.c - glib 2.76 no SODOS: o que o QEMU usa da glib. */
#include <glib.h>
#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond, name)                         \
  do {                                            \
    if (cond) printf("PASS %s\n", name);          \
    else { printf("FAIL %s\n", name); fails++; }  \
  } while (0)

static gpointer worker(gpointer data)
{
  int *counter = data;
  int i;
  for (i = 0; i < 1000; i++) {
    static GMutex m;
    g_mutex_lock(&m);
    (*counter)++;
    g_mutex_unlock(&m);
    if (i % 100 == 0) g_thread_yield();
  }
  return GINT_TO_POINTER(42);
}

static int ticks;
static gboolean on_tick(gpointer loop)
{
  if (++ticks == 3) {
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean on_idle(gpointer data)
{
  (*(int *)data)++;
  return G_SOURCE_REMOVE;
}

int main(void)
{
  /* GHashTable + GString */
  {
    GHashTable *h = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GString *s = g_string_new("SODOS");
    int i;
    for (i = 0; i < 500; i++) g_hash_table_insert(h, g_strdup_printf("k%d", i), GINT_TO_POINTER(i));
    CHECK(g_hash_table_size(h) == 500 && GPOINTER_TO_INT(g_hash_table_lookup(h, "k321")) == 321,
          "GHashTable 500 chaves");
    g_string_append_printf(s, " %s %d %.3f %llu", "qemu", -7, 3.14159, (unsigned long long)G_MAXUINT64);
    CHECK(strcmp(s->str, "SODOS qemu -7 3.142 18446744073709551615") == 0, "GString + printf 64-bit");
    printf("     \"%s\"\n", s->str);
    g_string_free(s, TRUE);
    g_hash_table_destroy(h);
  }
  /* printf posicional (gnulib) e tamanhos */
  {
    char *p = g_strdup_printf("%2$s-%1$s", "b", "a");
    CHECK(strcmp(p, "a-b") == 0 && G_MAXSIZE == (gsize)-1 && sizeof(gsize) == 8,
          "printf posicional + G_MAXSIZE 64 bits");
    g_free(p);
  }
  /* UTF-8 e iconv */
  {
    GError *err = NULL;
    gsize w = 0;
    const char *u = "a\xc3\xa7\xc3\xa3o"; /* "ação" */
    char *l1 = g_convert(u, -1, "ISO-8859-1", "UTF-8", NULL, &w, &err);
    CHECK(g_utf8_validate(u, -1, NULL) && g_utf8_strlen(u, -1) == 4, "g_utf8_validate/strlen");
    CHECK(l1 && w == 4 && (unsigned char)l1[1] == 0xe7, "g_convert UTF-8 -> Latin-1 (iconv)");
    g_free(l1);
    g_clear_error(&err);
  }
  /* GRegex (pcre2) */
  {
    GRegex *re = g_regex_new("(\\d+)-(\\w+)", 0, 0, NULL);
    GMatchInfo *mi = NULL;
    char *g2 = NULL;
    if (re && g_regex_match(re, "id 2026-sodos", 0, &mi)) g2 = g_match_info_fetch(mi, 2);
    CHECK(g2 && strcmp(g2, "sodos") == 0, "GRegex (pcre2)");
    g_free(g2);
    if (mi) g_match_info_free(mi);
    if (re) g_regex_unref(re);
  }
  /* GThread + GMutex */
  {
    int counter = 0;
    GThread *a = g_thread_new("a", worker, &counter);
    GThread *b = g_thread_new("b", worker, &counter);
    gpointer ra = g_thread_join(a), rb = g_thread_join(b);
    CHECK(counter == 2000 && GPOINTER_TO_INT(ra) == 42 && GPOINTER_TO_INT(rb) == 42,
          "GThread x2 + GMutex");
  }
  /* GMainLoop: idle + timeout (poll com espera) */
  {
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    int idle = 0;
    gint64 t0 = g_get_monotonic_time(), dt;
    g_idle_add(on_idle, &idle);
    g_timeout_add(50, on_tick, loop);
    g_main_loop_run(loop);
    dt = (g_get_monotonic_time() - t0) / 1000;
    CHECK(idle == 1 && ticks == 3 && dt >= 100 && dt < 2000, "GMainLoop idle + 3 timeouts de 50 ms");
    printf("     %lld ms\n", (long long)dt);
    g_main_loop_unref(loop);
  }
  printf("GTEST: %s (glib %u.%u.%u)\n", fails ? "FALHOU" : "ok", glib_major_version,
         glib_minor_version, glib_micro_version);
  return fails;
}
