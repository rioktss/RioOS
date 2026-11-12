#ifndef TTY_H
#define TTY_H
#include "types.h"

#define TTY_NCCS 19
/* c_iflag */ #define TTY_INLCR 0x40
#define TTY_ICRNL 0x100
#define TTY_IXON 0x400
/* c_oflag */ #define TTY_OPOST 1
#define TTY_ONLCR 4
/* c_lflag */ #define TTY_ISIG 1
#define TTY_ICANON 2
#define TTY_ECHO 8
#define TTY_ECHOE 0x10
#define TTY_ECHOK 0x20
/* c_cc indexes */ #define TTY_VINTR 0
#define TTY_VQUIT 1
#define TTY_VERASE 2
#define TTY_VKILL 3
#define TTY_VEOF 4
#define TTY_VTIME 5
#define TTY_VMIN 6

struct TtyTermios
{
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_line;
    uint8_t c_cc[TTY_NCCS];
    uint8_t pad[4];
    uint32_t c_ispeed, c_ospeed;
};

int64_t tty_read(uint8_t* buf, uint64_t len);   /* >0 bytes, 0 EOF / VMIN=0 timeout, -11 would block */
int tty_poll();                                  /* 1 when a read would not block */
int tty_get(TtyTermios* out);
int tty_set(int pid, const TtyTermios* in, int action);
void tty_winsize(uint16_t out[4]);               /* rows, cols, xpixel, ypixel */
void tty_reset();
void tty_process_exit(int pid);                  /* restores defaults when the process that changed them exits */
#endif
