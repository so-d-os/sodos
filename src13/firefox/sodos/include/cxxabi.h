/* cxxabi.h (SODOS) - o runtime C++ do SODOS nao tem demangler. */
#ifndef _CXXABI_H
#define _CXXABI_H
#include <stddef.h>
#ifdef __cplusplus
namespace __cxxabiv1 {
extern "C" char* __cxa_demangle(const char* mangled, char* out, size_t* len, int* status);
}
namespace abi = __cxxabiv1;
#endif
#endif
