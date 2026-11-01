/* tglib.c - prova GLIB.DLL */
#include <glib.h>
#include <so.h>
#include <stdio.h>

int main(void) {
  GList* L = NULL;
  GString* s;
  gchar* p;
  L = g_list_append(L, g_strdup("a"));
  L = g_list_append(L, g_strdup("b"));
  s = g_string_new("hi");
  g_string_append(s, "-");
  g_string_append(s, (gchar*)g_list_nth_data(L, 0));
  p = g_strdup_printf("n=%u %s", g_list_length(L), s->str);
  printf("%s\n", p);
  g_free(p);
  g_string_free(s, TRUE);
  g_list_free_full(L, g_free);
  k_puts("tglib ok\n");
  return 0;
}
