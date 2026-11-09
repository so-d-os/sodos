#ifndef _TERMIOS_H
#define _TERMIOS_H
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;
#define NCCS 32
struct termios {
  tcflag_t c_iflag, c_oflag, c_cflag, c_lflag;
  cc_t c_line;
  cc_t c_cc[NCCS];
  speed_t c_ispeed, c_ospeed;
};
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#define ICANON 0x0002
#define ECHO 0x0008
#define ISIG 0x0001
#define ICRNL 0x0100
#define OPOST 0x0001
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VMIN 6
#define VTIME 5
#define ISTRIP 0x0020
#define INPCK 0x0010
#define IXON 0x0400
#define IXOFF 0x1000
#define ECHOE 0x0010
#define ECHOK 0x0020
/* Resto do termios com os valores do Linux (QEMU: chardev serial/stdio). */
#define IGNBRK 0x0001
#define BRKINT 0x0002
#define IGNPAR 0x0004
#define PARMRK 0x0008
#define INLCR 0x0040
#define IGNCR 0x0080
#define IUCLC 0x0200
#define IXANY 0x0800
#define IMAXBEL 0x2000
#define OLCUC 0x0002
#define ONLCR 0x0004
#define OCRNL 0x0008
#define CSIZE 0x0030
#define CS5 0x0000
#define CS6 0x0010
#define CS7 0x0020
#define CS8 0x0030
#define CSTOPB 0x0040
#define CREAD 0x0080
#define PARENB 0x0100
#define PARODD 0x0200
#define HUPCL 0x0400
#define CLOCAL 0x0800
#define CBAUD 0x100f
#define CRTSCTS 0x80000000
#define ECHONL 0x0040
#define NOFLSH 0x0080
#define TOSTOP 0x0100
#define ECHOCTL 0x0200
#define ECHOKE 0x0800
#define IEXTEN 0x8000
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define B0 0000000
#define B50 0000001
#define B75 0000002
#define B110 0000003
#define B134 0000004
#define B150 0000005
#define B200 0000006
#define B300 0000007
#define B600 0000010
#define B1200 0000011
#define B1800 0000012
#define B2400 0000013
#define B4800 0000014
#define B9600 0000015
#define B19200 0000016
#define B38400 0000017
#define B57600 0010001
#define B115200 0010002
#define B230400 0010003
#define B460800 0010004
#define B500000 0010005
#define B576000 0010006
#define B921600 0010007
#define B1000000 0010010
#define TCIFLUSH 0
#define TCOFLUSH 1
#define TCIOFLUSH 2
#define TCOOFF 0
#define TCOON 1
#define TCIOFF 2
#define TCION 3
int tcgetattr(int fd, struct termios* t);
int tcsetattr(int fd, int act, const struct termios* t);
/* firefox/sodos/port/sodos_posix_extra.c (LP64) */
speed_t cfgetispeed(const struct termios* t);
speed_t cfgetospeed(const struct termios* t);
int cfsetispeed(struct termios* t, speed_t s);
int cfsetospeed(struct termios* t, speed_t s);
int cfsetspeed(struct termios* t, speed_t s);
void cfmakeraw(struct termios* t);
int tcflush(int fd, int q);
int tcdrain(int fd);
int tcsendbreak(int fd, int d);
int tcflow(int fd, int a);
#ifdef __cplusplus
}
#endif
#endif
