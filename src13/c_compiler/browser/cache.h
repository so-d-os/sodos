/* cache.h - cache em disco do navegador: Z:\temp\browser\<host>\ (disco em
 * RAM; sem ele, D:\temp\browser). Nomes longos, HHHHHHHH_caminho.web.
 * cache_load devolve corpo alocado com malloc; http_free libera. */
#ifndef CACHE_H
#define CACHE_H

int cache_load(const char* url, int* status, char** body, int* body_len,
               char* ctype, int ctype_max, char* final_url, int url_max,
               int* tls, int* tls_ok);
int cache_store(const char* url, const char* final_url, int status,
                const char* ctype, const char* body, int body_len,
                int tls, int tls_ok);

/* pasta raiz do cache, para a barra de estado */
const char* cache_where(void);

#endif
