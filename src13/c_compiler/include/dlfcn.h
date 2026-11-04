#ifndef _DLFCN_H
#define _DLFCN_H
#ifdef __cplusplus
extern "C" {
#endif
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_DEFAULT ((void*)0)
void* dlopen(const char* file, int mode);
void* dlsym(void* handle, const char* name);
char* dlerror(void);
int dlclose(void* handle);
#ifdef __cplusplus
}
#endif
#endif
