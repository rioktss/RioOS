/*
 * MIT License
 *
 * Copyright (c) 2026 Rio okta kusuma
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files, to deal in the Software
 * without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the
 * Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "tty.h"
#include "uart.h"
#include "keyboard.h"
#include "timer.h"

/* Console line discipline: termios flags, canonical line editing with echo, raw VMIN/VTIME reads.
   One console, one state. Output post-processing (OPOST/ONLCR) and signal generation (ISIG: ^C/^Z) are always performed
   by the console and job-control layer and are not switchable. */

#define LINE_MAX_CHARS 255

static TtyTermios t;
static int inited = 0;
static int owner_pid = -1;
static char line[LINE_MAX_CHARS + 2];
static int line_len = 0;       /* characters collected */
static int line_pos = 0;       /* characters already consumed by read() */
static int line_ready = 0;
static int eof_pending = 0;
static int pending_ch = -1;
static uint64_t vtime_deadline = 0;

static void defaults()
{
    t.c_iflag = TTY_ICRNL | TTY_IXON;
    t.c_oflag = TTY_OPOST | TTY_ONLCR;
    t.c_cflag = 0xBF;                         /* CS8 | CREAD | B38400 */
    t.c_lflag = TTY_ISIG | TTY_ICANON | TTY_ECHO | TTY_ECHOE | TTY_ECHOK;
    t.c_line = 0;
    for (int i = 0; i < TTY_NCCS; ++i) t.c_cc[i] = 0;
    t.c_cc[TTY_VINTR] = 3; t.c_cc[TTY_VQUIT] = 28; t.c_cc[TTY_VERASE] = 0x7F; t.c_cc[TTY_VKILL] = 21;
    t.c_cc[TTY_VEOF] = 4; t.c_cc[TTY_VMIN] = 1; t.c_cc[TTY_VTIME] = 0;
    t.c_ispeed = t.c_ospeed = 38400;
    line_len = line_pos = line_ready = eof_pending = 0;
    pending_ch = -1;
    vtime_deadline = 0;
}

static void ensure() { if (!inited) { defaults(); inited = 1; } }

static void echo_str(const char* s) { while (*s) uart_putc(*s++); }

static int getc_raw(char* c)
{
    if (pending_ch >= 0) { *c = (char)pending_ch; pending_ch = -1; return 1; }
    return keyboard_try_getc(c);
}

void tty_reset() { defaults(); inited = 1; owner_pid = -1; }

void tty_process_exit(int pid)
{
    if (pid >= 0 && pid == owner_pid) tty_reset();
}

int tty_get(TtyTermios* out) { ensure(); *out = t; return 0; }

int tty_set(int pid, const TtyTermios* in, int action)
{
    ensure();
    int was_canon = (t.c_lflag & TTY_ICANON) != 0;
    t = *in;
    if (action == 2 /* TCSAFLUSH */ || (was_canon && !(t.c_lflag & TTY_ICANON)))
    {
        if (action == 2) { char c; while (getc_raw(&c)) { } }
        line_len = line_pos = line_ready = eof_pending = 0;
    }
    vtime_deadline = 0;
    owner_pid = pid;
    return 0;
}

void tty_winsize(uint16_t out[4]) { out[0] = 24; out[1] = 80; out[2] = 0; out[3] = 0; }

static void erase_one()
{
    if (line_len == 0) return;
    --line_len;
    if (t.c_lflag & TTY_ECHO) echo_str((t.c_lflag & TTY_ECHOE) ? "\b \b" : "^?");
}

/* feed one raw character into the canonical editor */
static void canon_feed(char c)
{
    if ((t.c_iflag & TTY_ICRNL) && c == '\r') c = '\n';
    else if ((t.c_iflag & TTY_INLCR) && c == '\n') c = '\r';
    if (c == (char)t.c_cc[TTY_VEOF] && t.c_cc[TTY_VEOF])
    {
        if (line_len == 0) eof_pending = 1; else line_ready = 1;
        return;
    }
    if (c == (char)t.c_cc[TTY_VERASE] || c == 8) { erase_one(); return; }
    if (c == (char)t.c_cc[TTY_VKILL])
    {
        while (line_len) erase_one();
        return;
    }
    if (c == 23)                                   /* ^W: erase the previous word */
    {
        while (line_len && line[line_len - 1] == ' ') erase_one();
        while (line_len && line[line_len - 1] != ' ') erase_one();
        return;
    }
    if (c == '\n')
    {
        line[line_len++] = '\n';
        if (t.c_lflag & TTY_ECHO) echo_str("\r\n");
        line_ready = 1;
        return;
    }
    if (line_len >= LINE_MAX_CHARS) return;
    line[line_len++] = c;
    if (t.c_lflag & TTY_ECHO)
    {
        if ((unsigned char)c < 32 && c != '\t') { uart_putc('^'); uart_putc((char)(c + 64)); }
        else uart_putc(c);
    }
}

int64_t tty_read(uint8_t* buf, uint64_t len)
{
    ensure();
    if (len == 0) return 0;
    if (!(t.c_lflag & TTY_ICANON))
    {
        uint64_t got = 0;
        char c;
        while (got < len && getc_raw(&c))
        {
            if ((t.c_iflag & TTY_ICRNL) && c == '\r') c = '\n';
            if (t.c_lflag & TTY_ECHO) uart_putc(c);
            buf[got++] = (uint8_t)c;
        }
        if (got > 0) { vtime_deadline = 0; return (int64_t)got; }
        if (t.c_cc[TTY_VMIN] == 0)
        {
            if (t.c_cc[TTY_VTIME] == 0) return 0;                         /* pure poll */
            uint64_t now = timer_millis();
            if (vtime_deadline == 0) vtime_deadline = now + (uint64_t)t.c_cc[TTY_VTIME] * 100ULL;
            if (now >= vtime_deadline) { vtime_deadline = 0; return 0; }
        }
        return -11;
    }
    /* canonical */
    char c;
    while (!line_ready && !eof_pending && getc_raw(&c)) canon_feed(c);
    if (line_ready)
    {
        uint64_t avail = (uint64_t)(line_len - line_pos);
        uint64_t n = len < avail ? len : avail;
        for (uint64_t i = 0; i < n; ++i) buf[i] = (uint8_t)line[line_pos + (int)i];
        line_pos += (int)n;
        if (line_pos >= line_len) { line_len = line_pos = 0; line_ready = 0; }
        return (int64_t)n;
    }
    if (eof_pending) { eof_pending = 0; return 0; }
    return -11;
}

int tty_poll()
{
    ensure();
    if (!(t.c_lflag & TTY_ICANON)) return (pending_ch >= 0 || keyboard_has_data()) ? 1 : 0;
    if (line_ready || eof_pending) return 1;
    char c;
    while (!line_ready && !eof_pending && getc_raw(&c)) canon_feed(c);
    return (line_ready || eof_pending) ? 1 : 0;
}
