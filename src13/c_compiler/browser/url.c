/* url.c - analisa e resolve URLs */
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include "browser.h"

static void copy_n(char* dst, int max, const char* src, int n) {
  if (n > max - 1) n = max - 1;
  if (n < 0) n = 0;
  memcpy(dst, src, (size_t)n);
  dst[n] = 0;
}

static int starts_ci(const char* s, const char* p) {
  while (*p) {
    if (tolower((unsigned char)*s) != *p) return 0;
    s++;
    p++;
  }
  return 1;
}

int url_parse(const char* s, url_t* u) {
  const char* p;
  const char* h;
  int n;
  memset(u, 0, sizeof(*u));
  while (*s == ' ') s++;
  n = (int)strlen(s);
  while (n > 0 && s[n - 1] == ' ') n--;
  if (n == 0) return -1;
  if (starts_ci(s, "about:")) {
    strcpy(u->scheme, "about");
    copy_n(u->path, URL_MAX, s + 6, n - 6);
    return 0;
  }
  if (n >= 2 && isalpha((unsigned char)s[0]) && s[1] == ':' && (n == 2 || s[2] != '/')) {
    strcpy(u->scheme, "file");
    copy_n(u->path, URL_MAX, s, n);
    return 0;
  }
  if (starts_ci(s, "https://")) {
    strcpy(u->scheme, "https");
    u->port = 443;
    p = s + 8;
  } else if (starts_ci(s, "http://")) {
    strcpy(u->scheme, "http");
    u->port = 80;
    p = s + 7;
  } else {
    const char* c = strstr(s, "://");
    if (c && c - s < 10) return -1;   /* ftp:// etc. */
    strcpy(u->scheme, "https");
    u->port = 443;
    p = s;
  }
  h = p;
  while (p < s + n && *p != '/' && *p != ':' && *p != '?' && *p != '#') p++;
  copy_n(u->host, (int)sizeof(u->host), h, (int)(p - h));
  for (h = u->host; *h; h++) {
    if (!isalnum((unsigned char)*h) && *h != '.' && *h != '-' && *h != '_') return -1;
  }
  {
    char* q;
    for (q = u->host; *q; q++) *q = (char)tolower((unsigned char)*q);
  }
  if (!u->host[0]) return -1;
  if (p < s + n && *p == ':') {
    p++;
    u->port = 0;
    while (p < s + n && isdigit((unsigned char)*p)) u->port = u->port * 10 + (*p++ - '0');
    if (u->port <= 0 || u->port > 65535) return -1;
  }
  if (p < s + n && *p != '#') {
    const char* e = p;
    while (e < s + n && *e != '#') e++;
    if (*p == '?') {
      u->path[0] = '/';
      copy_n(u->path + 1, URL_MAX - 1, p, (int)(e - p));
    } else {
      copy_n(u->path, URL_MAX, p, (int)(e - p));
    }
  } else {
    strcpy(u->path, "/");
  }
  for (h = u->path; *h; h++)
    if (*h == ' ') return -1;
  return 0;
}

void url_format(const url_t* u, char* out, int max) {
  if (strcmp(u->scheme, "about") == 0) {
    snprintf(out, (size_t)max, "about:%s", u->path);
  } else if (strcmp(u->scheme, "file") == 0) {
    snprintf(out, (size_t)max, "%s", u->path);
  } else if ((strcmp(u->scheme, "https") == 0 && u->port == 443) ||
             (strcmp(u->scheme, "http") == 0 && u->port == 80)) {
    snprintf(out, (size_t)max, "%s://%s%s", u->scheme, u->host, u->path);
  } else {
    snprintf(out, (size_t)max, "%s://%s:%d%s", u->scheme, u->host, u->port, u->path);
  }
}

/* remove "." e ".." de um caminho absoluto (sem a query) */
static void normalize(char* path) {
  char out[URL_MAX];
  char* q = strchr(path, '?');
  char query[URL_MAX];
  int n = 0;
  const char* p = path;
  query[0] = 0;
  if (q) {
    strcpy(query, q);
    *q = 0;
  }
  while (*p) {
    if (p[0] == '/' && p[1] == '.' && (p[2] == '/' || p[2] == 0)) {
      p += 2;
      if (!*p) out[n++] = '/';
      continue;
    }
    if (p[0] == '/' && p[1] == '.' && p[2] == '.' && (p[3] == '/' || p[3] == 0)) {
      while (n > 0 && out[n - 1] != '/') n--;
      if (n > 0) n--;
      p += 3;
      if (!*p) out[n++] = '/';
      continue;
    }
    if (n < URL_MAX - 1) out[n++] = *p;
    p++;
  }
  if (n == 0) out[n++] = '/';
  out[n] = 0;
  snprintf(path, URL_MAX, "%s%s", out, query);
}

int url_resolve(const char* base, const char* href, char* out, int max) {
  url_t b;
  char tmp[URL_MAX];
  int n;
  while (*href == ' ' || *href == '\n' || *href == '\t' || *href == '\r') href++;
  n = (int)strlen(href);
  while (n > 0 && (href[n - 1] == ' ' || href[n - 1] == '\n' || href[n - 1] == '\r')) n--;
  copy_n(tmp, (int)sizeof(tmp), href, n);
  if (!tmp[0]) return -1;
  if (starts_ci(tmp, "javascript:") || starts_ci(tmp, "mailto:") || starts_ci(tmp, "tel:") ||
      starts_ci(tmp, "data:"))
    return -1;
  if (starts_ci(tmp, "http://") || starts_ci(tmp, "https://") || starts_ci(tmp, "about:")) {
    url_t u;
    if (url_parse(tmp, &u) != 0) return -1;
    url_format(&u, out, max);
    return 0;
  }
  if (url_parse(base, &b) != 0) return -1;
  if (tmp[0] == '#') {
    url_format(&b, out, max);
    return 0;
  }
  {
    char* hash = strchr(tmp, '#');
    if (hash) *hash = 0;
  }
  if (strcmp(b.scheme, "file") == 0 || strcmp(b.scheme, "about") == 0) {
    url_t u;
    if (url_parse(tmp, &u) != 0) return -1;
    url_format(&u, out, max);
    return 0;
  }
  if (tmp[0] == '/' && tmp[1] == '/') {
    char full[URL_MAX + 8];
    url_t u;
    snprintf(full, sizeof(full), "%s:%s", b.scheme, tmp);
    if (url_parse(full, &u) != 0) return -1;
    url_format(&u, out, max);
    return 0;
  }
  if (tmp[0] == '/') {
    copy_n(b.path, URL_MAX, tmp, (int)strlen(tmp));
  } else if (tmp[0] == '?') {
    char* q = strchr(b.path, '?');
    if (q) *q = 0;
    if ((int)(strlen(b.path) + strlen(tmp)) < URL_MAX) strcat(b.path, tmp);
  } else {
    char* q = strchr(b.path, '?');
    char* slash;
    if (q) *q = 0;
    slash = strrchr(b.path, '/');
    if (slash) slash[1] = 0;
    else strcpy(b.path, "/");
    if ((int)(strlen(b.path) + strlen(tmp)) < URL_MAX) strcat(b.path, tmp);
  }
  normalize(b.path);
  url_format(&b, out, max);
  return 0;
}
