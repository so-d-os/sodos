/* dlfcn.h - dlopen sobre o loader de DLL do SODOS. */
#ifndef _DLFCN_H
#define _DLFCN_H

#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_DEFAULT ((void*)0)

void* dlopen(const char* file, int mode);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);
char* dlerror(void);

#endif
