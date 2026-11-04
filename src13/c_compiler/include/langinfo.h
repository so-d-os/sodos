#ifndef _LANGINFO_H
#define _LANGINFO_H
#ifdef __cplusplus
extern "C" {
#endif
typedef int nl_item;
#define CODESET 14
#define D_T_FMT 1
#define D_FMT 2
#define T_FMT 3
#define RADIXCHAR 4
#define THOUSEP 5
char* nl_langinfo(nl_item item);
#ifdef __cplusplus
}
#endif
#endif
