/* sys/ioctl.h - ioctl minimo (FIONREAD/FIONBIO do soquete). */
#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FIONREAD 0x541B
#define FIONBIO 0x5421

/* _IO/_IOR/_IOW: os do kernel quando os headers UAPI estao no caminho (QEMU),
 * senao a mesma codificacao do asm-generic. */
#if defined(__has_include) && __has_include(<asm-generic/ioctl.h>)
#include <asm-generic/ioctl.h>
#else
#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT + _IOC_SIZEBITS)
#define _IOC_NONE 0U
#define _IOC_WRITE 1U
#define _IOC_READ 2U
#define _IOC(dir, type, nr, size)   (((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IO(type, nr) _IOC(_IOC_NONE, (type), (nr), 0)
#define _IOR(type, nr, t) _IOC(_IOC_READ, (type), (nr), sizeof(t))
#define _IOW(type, nr, t) _IOC(_IOC_WRITE, (type), (nr), sizeof(t))
#define _IOWR(type, nr, t) _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), sizeof(t))
#endif

/* linhas de modem (valores do Linux); o SODOS nao tem tty serial para
 * processos, entao estes ioctls falham com ENOTTY. */
#define TIOCMGET 0x5415
#define TIOCMBIS 0x5416
#define TIOCMBIC 0x5417
#define TIOCMSET 0x5418
#define TIOCSBRK 0x5427
#define TIOCCBRK 0x5428
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCM_LE 0x001
#define TIOCM_DTR 0x002
#define TIOCM_RTS 0x004
#define TIOCM_ST 0x008
#define TIOCM_SR 0x010
#define TIOCM_CTS 0x020
#define TIOCM_CAR 0x040
#define TIOCM_RNG 0x080
#define TIOCM_DSR 0x100
#define TIOCM_CD TIOCM_CAR
#define TIOCM_RI TIOCM_RNG

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif

#endif
