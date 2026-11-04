#ifndef _SYS_AUXV_H
#define _SYS_AUXV_H
#define AT_HWCAP 16
#define AT_HWCAP2 26
#ifdef __cplusplus
extern "C" {
#endif
unsigned long getauxval(unsigned long type);
#ifdef __cplusplus
}
#endif
#endif
