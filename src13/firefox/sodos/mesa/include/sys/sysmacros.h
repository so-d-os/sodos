#ifndef _SODOS_SYS_SYSMACROS_H
#define _SODOS_SYS_SYSMACROS_H
/* dev_t no estilo glibc (o SODOS nao tem nos de dispositivo). */
#define major(d) ((unsigned)(((unsigned long long)(d) >> 8) & 0xfffu))
#define minor(d) ((unsigned)(((unsigned long long)(d) & 0xffu) | (((unsigned long long)(d) >> 12) & 0xfff00u)))
#define makedev(ma, mi) ((((unsigned long long)(ma) & 0xfffu) << 8) | ((unsigned long long)(mi) & 0xffu) | (((unsigned long long)(mi) & 0xfff00u) << 12))
#endif
