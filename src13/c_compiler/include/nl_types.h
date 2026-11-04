/* nl_types.h minimo (ICU/libc++ usam apenas os tipos). */
#ifndef _NL_TYPES_H
#define _NL_TYPES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* nl_catd;
typedef int nl_item;

#define NL_SETD 1
#define NL_CAT_LOCALE 1

nl_catd catopen(const char* name, int oflag);
char* catgets(nl_catd catd, int set_id, int msg_id, const char* s);
int catclose(nl_catd catd);

#ifdef __cplusplus
}
#endif

#endif
