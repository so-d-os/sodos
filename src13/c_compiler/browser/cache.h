/* cache.h - cache em disco do navegador. A pasta e D:\temp\net\.
 * O nome do arquivo e um hash 8.3 (o FAT nao guarda a URL no nome).
 * cache_load devolve corpo alocado com malloc; http_free libera. */
#ifndef CACHE_H
#define CACHE_H

int cache_load(const char* url, int* status, char** body, int* body_len,
               char* ctype, int ctype_max, char* final_url, int url_max,
               int* tls, int* tls_ok);
int cache_store(const char* url, const char* final_url, int status,
                const char* ctype, const char* body, int body_len,
                int tls, int tls_ok);

#endif
