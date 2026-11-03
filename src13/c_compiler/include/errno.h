#ifndef _ERRNO_H
#define _ERRNO_H

int* __so_errno(void);
#define errno (*__so_errno())

#define ENOENT 2
#define EIO 5
#define EBADF 9
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENOSYS 38
#define EDOM 33
#define ERANGE 34

#endif
