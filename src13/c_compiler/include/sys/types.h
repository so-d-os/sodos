#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
/* ssize_t e size_t ja vem de stddef.h */

typedef int off_t;
typedef unsigned mode_t;
typedef int pid_t;
typedef unsigned uid_t;
typedef unsigned gid_t;
typedef unsigned ino_t;
typedef unsigned dev_t;
#ifndef __time_t_defined
#define __time_t_defined
typedef long time_t;
#endif
typedef long clock_t;
typedef long suseconds_t;
typedef int key_t;
typedef long long off64_t;


#ifdef __cplusplus
}
#endif
typedef long loff_t;
/* tipos BSD (QEMU: net/eth.h, slirp) */
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef char* caddr_t;
#endif
