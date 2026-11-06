/* sys/param.h (SODOS) */
#ifndef _SYS_PARAM_H
#define _SYS_PARAM_H
#include <limits.h>
#include <endian.h>
#include <sys/types.h>
#ifndef PATH_MAX
#define PATH_MAX 260
#endif
#define MAXPATHLEN PATH_MAX
#define MAXHOSTNAMELEN 64
#define NBBY 8
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#endif
