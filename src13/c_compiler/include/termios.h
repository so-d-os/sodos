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
int tcgetattr(int fd, struct termios* t);
int tcsetattr(int fd, int act, const struct termios* t);
#ifdef __cplusplus
}
#endif
#endif
