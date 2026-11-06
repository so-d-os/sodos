/* dlfcn.h - dlopen sobre o loader de DLL do SODOS. */
#ifndef _DLFCN_H
#define _DLFCN_H

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_DEFAULT ((void*)0)

void* dlopen(const char* file, int mode);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);
char* dlerror(void);

/* lote2 */
#define RTLD_NOLOAD 4
#define RTLD_NODELETE 0x1000
typedef struct {
  const char* dli_fname;
  void* dli_fbase;
  const char* dli_sname;
  void* dli_saddr;
} Dl_info;
int dladdr(const void* addr, Dl_info* info);

/* lote3 */
#define RTLD_NEXT ((void*)-1)

#ifdef __cplusplus
}
#endif

#endif
