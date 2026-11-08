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

#include "syscall.h"
#include "uart.h"
#include "keyboard.h"
#include "timer.h"
#include "memory.h"
#include "scheduler.h"
#include "user.h"
#include "mmu.h"
#include "version.h"
#include "fs.h"
#include "vfs.h"
#include "fd.h"
#include "security.h"
#include "netstack.h"
#include "process.h"
#include "thread.h"
#include "task.h"
#include "elf.h"
#include "rtc.h"
#include "sha256.h"
#ifndef HOST_TEST
#include "mapping.h"
#endif

extern "C" void user_exit_return();

void syscall_init()
{
    fd_init();
    uart_puts("Syscall interface initialized (EL1 + EL0, file descriptors ready).\r\n");
}

#ifndef HOST_TEST /* needs AArch64 register variables */
uint64_t syscall_invoke(uint64_t number,uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4,uint64_t a5)
{
    register uint64_t x0 asm("x0") = a0;
    register uint64_t x1 asm("x1") = a1;
    register uint64_t x2 asm("x2") = a2;
    register uint64_t x3 asm("x3") = a3;
    register uint64_t x4 asm("x4") = a4;
    register uint64_t x5 asm("x5") = a5;
    register uint64_t x8 asm("x8") = number;
    asm volatile("svc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x8) :: "memory");
    return x0;
}
#endif

int syscall_user_pointer_ok(const void* pointer, uint64_t length)
{
    return mmu_user_pointer_ok((uint64_t)pointer, length, 0);
}

int syscall_user_pointer_ok_write(const void* pointer, uint64_t length)
{
    return mmu_user_pointer_ok((uint64_t)pointer, length, 1);
}


/* Copy a NUL-terminated path out of user memory, byte by byte, so every
   byte is validated against the EL0 page permissions. */
static int select_fd_owner(int from_user)
{
    int pid = from_user ? user_current_pid() : scheduler_current_pid();
    if (pid < 0) return -24;
    return fd_set_process(pid);
}

static int copy_path(const char* src, char* dst, int from_user)
{
    if (src == 0 || dst == 0)
        return -1;

    const uint64_t base = (uint64_t)src;
    for (uint64_t i = 0; i < SYSCALL_PATH_MAX; ++i)
    {
        if (base > 0xFFFFFFFFFFFFFFFFULL - i)
            return -1;
        const char* current = (const char*)(base + i);
        if (from_user && !syscall_user_pointer_ok(current, 1))
            return -1;
        char c = *current;
        dst[i] = c;
        if (c == '\0')
            return 0;
    }
    dst[SYSCALL_PATH_MAX - 1] = '\0';
    return -1; /* no terminator within SYSCALL_PATH_MAX */
}

static int copy_dns_name(const char* src, char* dst, int from_user)
{
    if (!src || !dst) return -1;
    uint64_t base = (uint64_t)src;
    for (uint64_t i = 0; i < 254U; ++i)
    {
        if (base > 0xFFFFFFFFFFFFFFFFULL - i) return -1;
        const char* current = (const char*)(base + i);
        if (from_user && !syscall_user_pointer_ok(current, 1)) return -1;
        char c = *current;
        dst[i] = c;
        if (c == '\0') return i == 0 ? -1 : 0;
    }
    dst[253] = '\0';
    return -1;
}

void syscall_dispatch(ExceptionFrame* frame, int from_user)
{
    if (frame == 0) return;

    if ((frame->esr & 0xFFFFULL) != 0)
    {
        /* ELR_EL1 already holds the address of the instruction after the
           SVC on entry to a synchronous SVC exception (SVC always
           completes and is not re-executed), so it must not be advanced
           here. */
        frame->x[0] = (uint64_t)-22;
        return;
    }

    /* EL0 programs (nano, calc, ...) act with the credentials of the shell
       that launched them: UID 1000 after 'user', UID 0 after 'su root'.
       Forcing UID 1000 here made root's nano fail with "permission denied". */

    uint64_t number = frame->x[8];
    uint64_t result = (uint64_t)-38;

    switch (number)
    {
        case SYS_PING:
            result = 0x4D594B56ULL;
            break;

        case SYS_WRITE:
        {
            const uint64_t text_address = frame->x[0];
            if (text_address == 0 || (from_user && !syscall_user_pointer_ok((const void*)text_address, 1)))
            {
                result = (uint64_t)-14;
                break;
            }
            int written = 0;
            while (written < 512)
            {
                if (text_address > 0xFFFFFFFFFFFFFFFFULL - (uint64_t)written)
                {
                    result = (uint64_t)-14;
                    break;
                }
                const char* current = (const char*)(text_address + (uint64_t)written);
                if (from_user && !syscall_user_pointer_ok(current, 1))
                {
                    result = (uint64_t)-14;
                    break;
                }
                char c = *current;
                if (c == '\0')
                {
                    result = (uint64_t)written;
                    break;
                }
                uart_putc(c);
                ++written;
            }
            if (written == 512) result = (uint64_t)-7;
            break;
        }

        case SYS_UPTIME:
            result = timer_seconds();
            break;

        case SYS_GETPID:
        {
            int pid = from_user ? user_current_pid() : scheduler_current_pid();
            result = pid < 0 ? (uint64_t)-3 : (uint64_t)pid;
            break;
        }

        case SYS_SCHED_TICK:
            result = scheduler_ticks();
            break;

        case SYS_MEM_FREE:
            result = heap_free();
            break;

        case SYS_EXIT:
            if (!from_user)
            {
                result = (uint64_t)-1;
                break;
            }
            user_set_exit_code((int)frame->x[0]);
#ifndef HOST_TEST
            task_exit_current((int)frame->x[0], 0);   /* never returns */
#endif
            frame->x[0] = 0;
            frame->spsr = 0x3C5ULL;
            frame->elr = (uint64_t)user_exit_return;
            return;

        case SYS_UNAME:
        {
            const char* out = "MyKernel " MYKERNEL_VERSION_STRING " ARM64";
            char* dst = (char*)frame->x[0];
            if (dst == 0 || (from_user && !syscall_user_pointer_ok_write(dst, 32)))
            {
                result = (uint64_t)-14;
                break;
            }
            int i = 0;
            while (out[i] && i < 31) { dst[i] = out[i]; ++i; }
            dst[i] = '\0';
            result = (uint64_t)i;
            break;
        }

        case SYS_WRITE_BUF:
        {
            const char* buf = (const char*)frame->x[0];
            uint64_t len = frame->x[1];

            if (len == 0)
            {
                result = 0;
                break;
            }
            if (len > SYSCALL_IO_MAX)
            {
                result = (uint64_t)-22;
                break;
            }
            if (buf == 0 || (from_user && !syscall_user_pointer_ok(buf, len)))
            {
                result = (uint64_t)-14;
                break;
            }
            for (uint64_t i = 0; i < len; ++i)
                uart_putc(buf[i]);
#ifndef HOST_TEST
            uart_flush_output(); /* show the program's output right away */
#endif
            result = len;
            break;
        }

        case SYS_SLEEP_MS:
            if (frame->x[0] > SYSCALL_SLEEP_MAX_MS)
            {
                result = (uint64_t)-22;
                break;
            }
#ifndef HOST_TEST
            if (from_user)
            {
                /* other tasks run meanwhile; a deliverable signal ends the sleep early (result = ms left) */
                int me = user_current_pid();
                uint64_t left = frame->x[0];
                while (left > 0)
                {
                    uint64_t slice = left > 5 ? 5 : left;
                    thread_wait_ms(slice); left -= slice;
                    if (me >= 0)
                    {
                        process_alarm_check(me, timer_millis());
                        if ((process_sig_pending(me) & ~((1U << SIG_CHLD) | (1U << SIG_CONT))) != 0) break;
                    }
                }
                result = left;
                break;
            }
            else
#endif
            timer_delay_ms(frame->x[0]);
            result = 0;
            break;

        case SYS_READ:
        {
#ifndef HOST_TEST
            /* Flush captured TUI output before an EL0 program polls for input. */
            uart_flush_output();
#endif
            char* buf = (char*)frame->x[0];
            uint64_t len = frame->x[1];

            if (len == 0)
            {
                result = 0;
                break;
            }
            if (len > SYSCALL_IO_MAX)
            {
                result = (uint64_t)-22;
                break;
            }
            if (buf == 0 || (from_user && !syscall_user_pointer_ok_write(buf, len)))
            {
                result = (uint64_t)-14;
                break;
            }
            uint64_t got = 0;
            while (got < len)
            {
                char input_char = 0;
#ifdef HOST_TEST
                int serial_char = uart_try_getc();
                if (serial_char < 0) break;
                input_char = (char)serial_char;
#else
                if (from_user && user_current_pid() >= 0 && process_is_background(user_current_pid())) break;
                if (!keyboard_try_getc(&input_char)) break;
                if (from_user && task_console_key(user_current_pid(), input_char)) continue;
#endif
                buf[got++] = input_char;
            }
            result = got;
            break;
        }

        case SYS_TIME_MS:
            result = timer_millis();
            break;

        case SYS_GETARG:
        {
            char* dst = (char*)frame->x[0];
            uint64_t cap = frame->x[1];
            if (cap == 0 || cap > SYSCALL_FILE_MAX)
            {
                result = (uint64_t)-22;
                break;
            }
            if (dst == 0 || (from_user && !syscall_user_pointer_ok_write(dst, cap)))
            {
                result = (uint64_t)-14;
                break;
            }
            const char* args = user_get_args();
            uint64_t n = 0;
            while (args[n] && n + 1 < cap) { dst[n] = args[n]; ++n; }
            dst[n] = '\0';
            result = n;
            break;
        }

        case SYS_FILE_READ:
        {
            char path[SYSCALL_PATH_MAX];
            uint8_t* buf = (uint8_t*)frame->x[1];
            uint64_t cap = frame->x[2];

            if (copy_path((const char*)frame->x[0], path, from_user) != 0)
            {
                result = (uint64_t)-14;
                break;
            }
            if (cap == 0 || cap > SYSCALL_FILE_MAX)
            {
                result = (uint64_t)-22;
                break;
            }
            if (buf == 0 || (from_user && !syscall_user_pointer_ok_write(buf, cap)))
            {
                result = (uint64_t)-14;
                break;
            }
            uint64_t size = 0;
            int rc = fs_read_file(path, vfs_cwd(), buf, cap, &size);
            if (rc != 0)
            {
                /* Preserve actionable filesystem errors for userspace. */
                result = rc == -3 ? (uint64_t)-27 : (uint64_t)(int64_t)rc;
                break;
            }
            result = size;
            break;
        }

        case SYS_FILE_WRITE:
        {
            char path[SYSCALL_PATH_MAX];
            const uint8_t* buf = (const uint8_t*)frame->x[1];
            uint64_t len = frame->x[2];
            static const uint8_t empty = 0;

            if (copy_path((const char*)frame->x[0], path, from_user) != 0)
            {
                result = (uint64_t)-14;
                break;
            }
            if (len > SYSCALL_FILE_MAX)
            {
                result = (uint64_t)-22;
                break;
            }
            if (len == 0)
                buf = &empty;
            else if (buf == 0 || (from_user && !syscall_user_pointer_ok(buf, len)))
            {
                result = (uint64_t)-14;
                break;
            }
            int write_rc = fs_write_data(path, vfs_cwd(), buf, len);
            if (write_rc != 0)
            {
                result = (uint64_t)(int64_t)write_rc;
                break;
            }
            result = len;
            break;
        }

        case SYS_FD_OPEN:
        {
            if (select_fd_owner(from_user) != 0) { result = (uint64_t)-24; break; }
            char path[SYSCALL_PATH_MAX];
            uint64_t raw_flags = frame->x[1];
            if (copy_path((const char*)frame->x[0], path, from_user) != 0)
            {
                result = (uint64_t)-14;
                break;
            }
            if (raw_flags > 0x17FFULL)
            {
                result = (uint64_t)-22;
                break;
            }
            result = (uint64_t)(int64_t)fd_open(path, (uint32_t)raw_flags, vfs_cwd());
            break;
        }

        case SYS_FD_CLOSE:
        {
            if (select_fd_owner(from_user) != 0) { result = (uint64_t)-24; break; }
            if (frame->x[0] >= FD_MAX_DESCRIPTORS)
                result = (uint64_t)-9;
            else
                result = (uint64_t)(int64_t)fd_close((int)frame->x[0]);
            break;
        }

        case SYS_FD_READ:
        {
            if (select_fd_owner(from_user) != 0) { result = (uint64_t)-24; break; }
            int descriptor = frame->x[0] < FD_MAX_DESCRIPTORS ? (int)frame->x[0] : -1;
            void* buffer = (void*)frame->x[1];
            uint64_t length = frame->x[2];
            if (length > FD_IO_MAX)
            {
                result = (uint64_t)-22;
                break;
            }
            if (length && (buffer == 0 || (from_user && !syscall_user_pointer_ok_write(buffer, length))))
            {
                result = (uint64_t)-14;
                break;
            }
            result = (uint64_t)fd_read(descriptor, buffer, length);
#ifndef HOST_TEST
            while (from_user && (int64_t)result == -11 && fd_is_pipe(descriptor))
            {
                int me = user_current_pid();
                if (me >= 0) process_alarm_check(me, timer_millis());
                if (me >= 0 && (process_sig_pending(me) & ~((1U << SIG_CHLD) | (1U << SIG_CONT))) != 0) { result = (uint64_t)-4; break; }
                thread_wait_ms(1);
                (void)select_fd_owner(from_user);
                result = (uint64_t)fd_read(descriptor, buffer, length);
            }
#endif
            break;
        }

        case SYS_FD_WRITE:
        {
            if (select_fd_owner(from_user) != 0) { result = (uint64_t)-24; break; }
            int descriptor = frame->x[0] < FD_MAX_DESCRIPTORS ? (int)frame->x[0] : -1;
            const void* buffer = (const void*)frame->x[1];
            uint64_t length = frame->x[2];
            if (length > FD_IO_MAX)
            {
                result = (uint64_t)-22;
                break;
            }
            if (length && (buffer == 0 || (from_user && !syscall_user_pointer_ok(buffer, length))))
            {
                result = (uint64_t)-14;
                break;
            }
            result = (uint64_t)fd_write(descriptor, buffer, length);
#ifndef HOST_TEST
            if (from_user && (int64_t)result == -32)
            {
                int me = user_current_pid();
                if (me >= 0) (void)process_sig_raise(me, SIG_PIPE);
            }
            if (from_user && fd_is_pipe(descriptor) && ((int64_t)result == -11 ||
                ((int64_t)result > 0 && (uint64_t)result < length)))
            {
                /* Blocking pipe write: keep going until everything is queued. */
                uint64_t done = (int64_t)result > 0 ? result : 0;
                while (done < length)
                {
                    int me = user_current_pid();
                    if (me >= 0) process_alarm_check(me, timer_millis());
                    if (me >= 0 && (process_sig_pending(me) & ~((1U << SIG_CHLD) | (1U << SIG_CONT))) != 0) break;
                    thread_wait_ms(1);
                    (void)select_fd_owner(from_user);
                    int64_t w = fd_write(descriptor, (const char*)buffer + done, length - done);
                    if (w == -11) continue;
                    if (w < 0) { if (done == 0) done = (uint64_t)w; break; }
                    done += (uint64_t)w;
                }
                result = done;
            }
#endif
            break;
        }

        case SYS_FD_SEEK:
        {
            if (select_fd_owner(from_user) != 0) { result = (uint64_t)-24; break; }
            int descriptor = frame->x[0] < FD_MAX_DESCRIPTORS ? (int)frame->x[0] : -1;
            if (frame->x[2] > 2ULL)
                result = (uint64_t)-22;
            else
                result = (uint64_t)fd_seek(descriptor, (int64_t)frame->x[1], (int)frame->x[2]);
            break;
        }

        case SYS_FCNTL:
        {
            if (select_fd_owner(from_user) != 0) { result = (uint64_t)-24; break; }
            if (frame->x[0] >= FD_MAX_DESCRIPTORS) { result = (uint64_t)-9; break; }
            int fdn = (int)frame->x[0];
            switch (frame->x[1])
            {
                case 0:   /* F_DUPFD: lowest free descriptor >= arg */
                {
                    int minfd = (int)frame->x[2];
                    if (minfd < 0 || minfd >= FD_MAX_DESCRIPTORS) { result = (uint64_t)-22; break; }
                    int nd = fd_dup(fdn);
                    if (nd >= 0 && nd < minfd)
                    {
                        int d2 = fd_dup2(nd, minfd); (void)fd_close(nd); nd = d2;
                    }
                    result = (uint64_t)(int64_t)nd;
                    break;
                }
                case 1: result = (uint64_t)(int64_t)fd_get_cloexec(fdn); break;                    /* F_GETFD */
                case 2: result = (uint64_t)(int64_t)fd_set_cloexec(fdn, (int)(frame->x[2] & 1)); break;   /* F_SETFD */
                default: result = (uint64_t)-22; break;
            }
            break;
        }

        case SYS_FD_DUP:
        {
            if (select_fd_owner(from_user) != 0) { result = (uint64_t)-24; break; }
            if (frame->x[0] >= FD_MAX_DESCRIPTORS)
                result = (uint64_t)-9;
            else
                result = (uint64_t)(int64_t)fd_dup((int)frame->x[0]);
            break;
        }

        case SYS_FD_DUP2:
        {
            if (select_fd_owner(from_user) != 0) { result = (uint64_t)-24; break; }
            if (frame->x[0] >= FD_MAX_DESCRIPTORS || frame->x[1] >= FD_MAX_DESCRIPTORS)
                result = (uint64_t)-9;
            else
                result = (uint64_t)(int64_t)fd_dup2((int)frame->x[0], (int)frame->x[1]);
            break;
        }

        case SYS_FD_PIPE:
        {
            if (select_fd_owner(from_user) != 0) { result = (uint64_t)-24; break; }
            int* out = (int*)frame->x[0];
            if (out == 0 || (from_user && !syscall_user_pointer_ok_write(out, 2U * sizeof(int))))
            {
                result = (uint64_t)-14;
                break;
            }
            int descriptors[2] = {-1, -1};
            int rc = fd_pipe(descriptors);
            if (rc != 0)
            {
                result = (uint64_t)(int64_t)rc;
                break;
            }
            out[0] = descriptors[0];
            out[1] = descriptors[1];
            result = 0;
            break;
        }

        case SYS_FS_STAT:
        {
            char path[SYSCALL_PATH_MAX];
            FsStat* out = (FsStat*)frame->x[1];
            if (copy_path((const char*)frame->x[0], path, from_user) != 0 || out == 0 ||
                (from_user && !syscall_user_pointer_ok_write(out, sizeof(FsStat))))
            {
                result = (uint64_t)-14;
                break;
            }
            FsStat st;
            int rc = fs_stat_path(path, vfs_cwd(), &st);
            if (rc != 0) { result = (uint64_t)(int64_t)rc; break; }
            *out = st;
            result = 0;
            break;
        }

        case SYS_FS_TRUNCATE:
        {
            char path[SYSCALL_PATH_MAX];
            if (copy_path((const char*)frame->x[0], path, from_user) != 0)
            {
                result = (uint64_t)-14;
                break;
            }
            if (frame->x[1] > 4194304ULL)
            {
                result = (uint64_t)-27;
                break;
            }
            result = (uint64_t)(int64_t)fs_truncate(path, vfs_cwd(), frame->x[1]);
            break;
        }

        case SYS_FS_MKDIR:
        case SYS_FS_UNLINK:
        {
            char path[SYSCALL_PATH_MAX];
            if (copy_path((const char*)frame->x[0], path, from_user) != 0) { result = (uint64_t)-14; break; }
            int rc;
            if (number == SYS_FS_MKDIR) { rc = fs_resolve(path, vfs_cwd()) >= 0 ? -17 : fs_mkdir(path, vfs_cwd()); if (rc == -1) rc = -2; }
            else { rc = fs_rm(path, vfs_cwd()); if (rc == -1) rc = -2; else if (rc == -2) rc = -39; else if (rc == -3) rc = -1; else if (rc == -4 || rc == -5) rc = -5; }
            result = (uint64_t)(int64_t)(rc >= 0 ? 0 : rc);
            break;
        }

        case SYS_FS_RENAME:
        {
            char from[SYSCALL_PATH_MAX]; char to[SYSCALL_PATH_MAX];
            if (copy_path((const char*)frame->x[0], from, from_user) != 0 || copy_path((const char*)frame->x[1], to, from_user) != 0) { result = (uint64_t)-14; break; }
            int rc = fs_rename(from, to, vfs_cwd());
            result = (uint64_t)(int64_t)(rc == 0 ? 0 : rc == -1 ? -2 : rc == -2 ? -17 : rc == -6 ? -5 : -22);
            break;
        }

        case SYS_FS_READDIR:
        {
            char path[SYSCALL_PATH_MAX];
            char* out = (char*)frame->x[1]; uint64_t cap = frame->x[2];
            if (copy_path((const char*)frame->x[0], path, from_user) != 0 || out == 0 || cap == 0 ||
                (from_user && !syscall_user_pointer_ok_write(out, cap))) { result = (uint64_t)-14; break; }
            int id = fs_resolve(path, vfs_cwd());
            if (id < 0) { result = (uint64_t)-2; break; }
            if (fs_get_type(id) != FS_DIR) { result = (uint64_t)-20; break; }
            uint64_t used = 0; int n = fs_get_child_count(id);
            for (int i = 0; i < n; ++i)
            {
                int c = fs_get_child(id, i); if (c < 0) continue;
                const char* nm = fs_get_name(c); if (!nm) continue;
                int t = fs_get_type(c);
                char line[96]; int l = 0;
                line[l++] = t == FS_DIR ? 'd' : (t == FS_SYMLINK ? 'l' : 'f'); line[l++] = ' ';
                char dg[24]; int dn = 0; uint64_t sz = t == FS_FILE ? fs_file_size(c) : 0;
                do { dg[dn++] = (char)('0' + sz % 10); sz /= 10; } while (sz);
                while (dn) line[l++] = dg[--dn];
                line[l++] = ' ';
                for (int k = 0; nm[k] && l < 94; ++k) line[l++] = nm[k];
                line[l++] = '\n';
                if (used + (uint64_t)l > cap) break;
                for (int k = 0; k < l; ++k) out[used + (uint64_t)k] = line[k];
                used += (uint64_t)l;
            }
            result = used;
            break;
        }

        case SYS_FS_SYMLINK:
        case SYS_FS_LINK:
        {
            char a[SYSCALL_PATH_MAX]; char b[SYSCALL_PATH_MAX];
            if (copy_path((const char*)frame->x[0], a, from_user) != 0 || copy_path((const char*)frame->x[1], b, from_user) != 0) { result = (uint64_t)-14; break; }
            int rc = number == SYS_FS_SYMLINK ? fs_symlink(a, b, vfs_cwd()) : fs_link_hard(a, b, vfs_cwd());
            if (rc >= 0) rc = 0; else if (rc == -1) rc = -2;
            result = (uint64_t)(int64_t)rc;
            break;
        }

        case SYS_FS_READLINK:
        {
            char path[SYSCALL_PATH_MAX]; char tmp[300];
            char* out = (char*)frame->x[1]; uint64_t cap = frame->x[2];
            if (copy_path((const char*)frame->x[0], path, from_user) != 0 || out == 0 || cap == 0 ||
                (from_user && !syscall_user_pointer_ok_write(out, cap))) { result = (uint64_t)-14; break; }
            int rc = fs_readlink(path, vfs_cwd(), tmp, (int)sizeof(tmp));
            if (rc < 0) { result = (uint64_t)(int64_t)(rc == -1 ? -2 : rc); break; }
            uint64_t n = (uint64_t)rc < cap ? (uint64_t)rc : cap;      /* POSIX: truncates silently, no terminator */
            for (uint64_t i = 0; i < n; ++i) out[i] = tmp[i];
            result = n;
            break;
        }

        case SYS_FS_LSTAT:
        {
            char path[SYSCALL_PATH_MAX];
            FsStat* out = (FsStat*)frame->x[1];
            if (copy_path((const char*)frame->x[0], path, from_user) != 0 || out == 0 ||
                (from_user && !syscall_user_pointer_ok_write(out, sizeof(FsStat)))) { result = (uint64_t)-14; break; }
            FsStat st;
            int rc = fs_lstat_path(path, vfs_cwd(), &st);
            if (rc != 0) { result = (uint64_t)(int64_t)(rc == -1 ? -2 : rc); break; }
            *out = st;
            result = 0;
            break;
        }

        case SYS_FS_REALPATH:
        {
            char path[SYSCALL_PATH_MAX]; char tmp[256];
            char* out = (char*)frame->x[1]; uint64_t cap = frame->x[2];
            if (copy_path((const char*)frame->x[0], path, from_user) != 0 || out == 0 || cap < 2 ||
                (from_user && !syscall_user_pointer_ok_write(out, cap))) { result = (uint64_t)-14; break; }
            int id = fs_resolve(path, vfs_cwd());
            if (id < 0) { result = (uint64_t)(int64_t)(id == -40 ? -40 : -2); break; }
            vfs_path_of(id, tmp, (int)sizeof(tmp));
            int l = 0; while (tmp[l]) ++l;
            if ((uint64_t)l + 1 > cap) { result = (uint64_t)-34; break; }   /* ERANGE */
            for (int i = 0; i <= l; ++i) out[i] = tmp[i];
            result = (uint64_t)l;
            break;
        }

        case SYS_TIME_EPOCH:
            result = rtc_get_epoch();
            break;

        case SYS_FS_CHMOD:
        {
            char path[SYSCALL_PATH_MAX];
            if (copy_path((const char*)frame->x[0], path, from_user) != 0)
            {
                result = (uint64_t)-14;
                break;
            }
            if (frame->x[1] > 0777ULL)
            {
                result = (uint64_t)-22;
                break;
            }
            result = (uint64_t)(int64_t)fs_set_mode_path(path, vfs_cwd(), (uint32_t)frame->x[1]);
            break;
        }

        case SYS_NET_UDP_SOCKET:
        {
            int owner_pid = from_user ? user_current_pid() : scheduler_current_pid();
            if (owner_pid < 0) owner_pid = 0;
            result = (uint64_t)(int64_t)net_udp_socket(owner_pid);
            break;
        }

        case SYS_NET_UDP_BIND:
        {
            int owner_pid = from_user ? user_current_pid() : scheduler_current_pid();
            if (owner_pid < 0) owner_pid = 0;
            if (frame->x[0] > 0x7FFFFFFFULL || frame->x[1] > 65535ULL)
                result = (uint64_t)-22;
            else
                result = (uint64_t)(int64_t)net_udp_bind(owner_pid, (int)frame->x[0], (uint16_t)frame->x[1]);
            break;
        }

        case SYS_NET_UDP_SENDTO:
        {
            int owner_pid = from_user ? user_current_pid() : scheduler_current_pid();
            if (owner_pid < 0) owner_pid = 0;
            uint64_t length = frame->x[4];
            const void* buffer = (const void*)frame->x[3];
            if (frame->x[1] > 0xFFFFFFFFULL || frame->x[2] > 65535ULL ||
                length > NET_UDP_MAX_PAYLOAD)
                result = (uint64_t)-22;
            else if (length && (!buffer || (from_user && !syscall_user_pointer_ok(buffer, length))))
                result = (uint64_t)-14;
            else
                result = (uint64_t)(int64_t)net_udp_sendto(owner_pid,
                    frame->x[0] <= 0x7FFFFFFFULL ? (int)frame->x[0] : -1,
                    (uint32_t)frame->x[1], (uint16_t)frame->x[2], buffer, (uint32_t)length);
            break;
        }

        case SYS_NET_UDP_RECVFROM:
        {
            int owner_pid = from_user ? user_current_pid() : scheduler_current_pid();
            if (owner_pid < 0) owner_pid = 0;
            uint32_t* source_ip = (uint32_t*)frame->x[1];
            uint16_t* source_port = (uint16_t*)frame->x[2];
            void* buffer = (void*)frame->x[3];
            uint64_t capacity = frame->x[4];
            uint64_t timeout_ms = frame->x[5];
            if (capacity == 0 || capacity > NET_UDP_MAX_PAYLOAD || timeout_ms > NET_UDP_MAX_TIMEOUT_MS)
                result = (uint64_t)-22;
            else if (!source_ip || !source_port || !buffer ||
                     (from_user && (!syscall_user_pointer_ok_write(source_ip, sizeof(*source_ip)) ||
                                    !syscall_user_pointer_ok_write(source_port, sizeof(*source_port)) ||
                                    !syscall_user_pointer_ok_write(buffer, capacity))))
                result = (uint64_t)-14;
            else
                result = (uint64_t)(int64_t)net_udp_recvfrom(owner_pid,
                    frame->x[0] <= 0x7FFFFFFFULL ? (int)frame->x[0] : -1,
                    source_ip, source_port, buffer, (uint32_t)capacity, (uint32_t)timeout_ms);
            break;
        }

        case SYS_NET_UDP_CLOSE:
        {
            int owner_pid = from_user ? user_current_pid() : scheduler_current_pid();
            if (owner_pid < 0) owner_pid = 0;
            if (frame->x[0] > 0x7FFFFFFFULL) result = (uint64_t)-9;
            else result = (uint64_t)(int64_t)net_udp_close(owner_pid, (int)frame->x[0]);
            break;
        }

        case SYS_NET_DNS_LOOKUP:
        {
            char hostname[254];
            uint32_t* out_ip = (uint32_t*)frame->x[1];
            uint64_t timeout_ms = frame->x[2];
            if (timeout_ms == 0 || timeout_ms > NET_UDP_MAX_TIMEOUT_MS)
            {
                result = (uint64_t)-22;
                break;
            }
            if (copy_dns_name((const char*)frame->x[0], hostname, from_user) != 0 ||
                !out_ip || (from_user && !syscall_user_pointer_ok_write(out_ip, sizeof(*out_ip))))
            {
                result = (uint64_t)-14;
                break;
            }
            uint32_t resolved_ip = 0;
            int rc = net_dns_resolve(hostname, &resolved_ip, (uint32_t)timeout_ms);
            if (rc == 0)
            {
                *out_ip = resolved_ip;
                result = 0;
            }
            else result = (uint64_t)(int64_t)rc;
            break;
        }

        case SYS_NET_TCP_SOCKET:
        {
            int owner_pid = from_user ? user_current_pid() : scheduler_current_pid();
            if (owner_pid < 0) owner_pid = 0;
            result = (uint64_t)(int64_t)net_tcp_socket(owner_pid);
            break;
        }

        case SYS_NET_TCP_CONNECT:
        {
            int owner_pid = from_user ? user_current_pid() : scheduler_current_pid();
            if (owner_pid < 0) owner_pid = 0;
            if (frame->x[0] > 0x7FFFFFFFULL || frame->x[1] > 0xFFFFFFFFULL ||
                frame->x[2] > 65535ULL || frame->x[3] == 0 ||
                frame->x[3] > NET_TCP_MAX_TIMEOUT_MS)
                result = (uint64_t)-22;
            else
                result = (uint64_t)(int64_t)net_tcp_connect(owner_pid, (int)frame->x[0],
                    (uint32_t)frame->x[1], (uint16_t)frame->x[2], (uint32_t)frame->x[3]);
            break;
        }

        case SYS_NET_TCP_SEND:
        {
            int owner_pid = from_user ? user_current_pid() : scheduler_current_pid();
            if (owner_pid < 0) owner_pid = 0;
            const void* buffer = (const void*)frame->x[1];
            uint64_t length = frame->x[2];
            uint64_t timeout_ms = frame->x[3];
            if (frame->x[0] > 0x7FFFFFFFULL || length > NET_TCP_MAX_IO ||
                timeout_ms == 0 || timeout_ms > NET_TCP_MAX_TIMEOUT_MS)
                result = (uint64_t)-22;
            else if (length && (!buffer || (from_user && !syscall_user_pointer_ok(buffer, length))))
                result = (uint64_t)-14;
            else
                result = (uint64_t)(int64_t)net_tcp_send(owner_pid, (int)frame->x[0],
                    buffer, (uint32_t)length, (uint32_t)timeout_ms);
            break;
        }

        case SYS_NET_TCP_RECV:
        {
            int owner_pid = from_user ? user_current_pid() : scheduler_current_pid();
            if (owner_pid < 0) owner_pid = 0;
            void* buffer = (void*)frame->x[1];
            uint64_t capacity = frame->x[2];
            uint64_t timeout_ms = frame->x[3];
            if (frame->x[0] > 0x7FFFFFFFULL || capacity == 0 ||
                capacity > NET_TCP_MAX_IO || timeout_ms > NET_TCP_MAX_TIMEOUT_MS)
                result = (uint64_t)-22;
            else if (!buffer || (from_user && !syscall_user_pointer_ok_write(buffer, capacity)))
                result = (uint64_t)-14;
            else
                result = (uint64_t)(int64_t)net_tcp_recv(owner_pid, (int)frame->x[0],
                    buffer, (uint32_t)capacity, (uint32_t)timeout_ms);
            break;
        }

        case SYS_NET_TCP_LISTEN:
        {
            int owner_pid = from_user ? user_current_pid() : scheduler_current_pid();
            if (owner_pid < 0) owner_pid = 0;
            if (frame->x[0] > 0x7FFFFFFFULL || frame->x[1] == 0 || frame->x[1] > 65535ULL) result = (uint64_t)-22;
            else result = (uint64_t)(int64_t)net_tcp_listen(owner_pid, (int)frame->x[0], (uint16_t)frame->x[1]);
            break;
        }

        case SYS_NET_TCP_ACCEPT:
        {
            int owner_pid = from_user ? user_current_pid() : scheduler_current_pid();
            if (owner_pid < 0) owner_pid = 0;
            uint64_t* out = (uint64_t*)frame->x[1];
            if (frame->x[0] > 0x7FFFFFFFULL || frame->x[2] > NET_TCP_MAX_TIMEOUT_MS) { result = (uint64_t)-22; break; }
            if (out && from_user && !syscall_user_pointer_ok_write(out, sizeof(uint64_t))) { result = (uint64_t)-14; break; }
            uint32_t ip = 0; uint16_t port = 0;
            int rc = net_tcp_accept(owner_pid, (int)frame->x[0], &ip, &port, (uint32_t)frame->x[2]);
            if (rc > 0 && out) *out = (uint64_t)ip | ((uint64_t)port << 32);
            result = (uint64_t)(int64_t)rc;
            break;
        }

        case SYS_NET_TCP_CLOSE:
        {
            int owner_pid = from_user ? user_current_pid() : scheduler_current_pid();
            if (owner_pid < 0) owner_pid = 0;
            if (frame->x[0] > 0x7FFFFFFFULL) result = (uint64_t)-9;
            else result = (uint64_t)(int64_t)net_tcp_close(owner_pid, (int)frame->x[0]);
            break;
        }

#ifndef HOST_TEST
        case SYS_SIGACTION:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            int me = user_current_pid();
            int sig = (int)(int64_t)frame->x[0];
            if (sig <= 0 || sig >= 32 || sig == 9 || sig == 19) { result = (uint64_t)-22; break; }
            if (frame->x[1] == ~0ULL) { result = process_sig_handler(me, sig, 0, 0); break; }   /* query */
            uint64_t h = frame->x[1];
            if (h > 1 && (h < MMU_USER_BASE || h >= MMU_USER_LIMIT)) { result = (uint64_t)-14; break; }
            uint64_t old = 0;
            int rc = process_sig_set_action(me, sig, h, (uint32_t)frame->x[3], (uint32_t)(frame->x[3] >> 32), frame->x[2], &old);
            result = rc == 0 ? old : (uint64_t)(int64_t)rc;
            break;
        }

        case SYS_SIGPROCMASK:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            int me = user_current_pid();
            uint32_t cur = process_sig_mask(me), set = (uint32_t)frame->x[1];
            switch (frame->x[0])
            {
                case 0: (void)process_sig_set_mask(me, cur | set); break;
                case 1: (void)process_sig_set_mask(me, cur & ~set); break;
                case 2: (void)process_sig_set_mask(me, set); break;
                case 3: break;
                default: result = (uint64_t)-22; break;
            }
            if (frame->x[0] > 3) break;
            result = cur;
            break;
        }

        case SYS_SIGRETURN:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            int rc = task_sigreturn(frame);
            if (rc == 0) return;               /* frame now holds the interrupted context */
            result = (uint64_t)(int64_t)rc;
            break;
        }

        case SYS_ALARM_MS:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            result = process_alarm_set(user_current_pid(), frame->x[0], timer_millis());
            break;
        }

        case SYS_PAUSE:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            int me = user_current_pid();
            for (;;)
            {
                process_alarm_check(me, timer_millis());
                if ((process_sig_pending(me) & ~((1U << SIG_CHLD) | (1U << SIG_CONT))) != 0) break;
                thread_wait_ms(2);
            }
            result = (uint64_t)-4;
            break;
        }

        case SYS_SIGPENDING:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            int me = user_current_pid();
            result = process_sig_pending_raw(me) & process_sig_mask(me);
            break;
        }

        case SYS_FORK:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            result = (uint64_t)(int64_t)task_fork(frame);
            break;
        }

        case SYS_EXEC:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            char path[SYSCALL_PATH_MAX];
            char args[PROCESS_ARGS_SIZE];
            if (copy_path((const char*)frame->x[0], path, from_user) != 0) { result = (uint64_t)-14; break; }
            args[0] = '\0';
            if (frame->x[1] != 0)
            {
                int ok = 0;
                for (int i = 0; i < PROCESS_ARGS_SIZE; ++i)
                {
                    if (!syscall_user_pointer_ok((const char*)frame->x[1] + i, 1)) break;
                    args[i] = ((const char*)frame->x[1])[i];
                    if (args[i] == '\0') { ok = 1; break; }
                }
                if (!ok) { result = (uint64_t)-14; break; }
            }
            int rc = task_exec(frame, path, args);
            if (rc == 0) return;               /* frame now describes the fresh image */
            result = (uint64_t)(int64_t)rc;
            break;
        }

        case SYS_WAITPID:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            int* status_ptr = (int*)frame->x[1];
            if (status_ptr && !syscall_user_pointer_ok_write(status_ptr, sizeof(int))) { result = (uint64_t)-14; break; }
            int st = 0;
            int rc = task_wait(user_current_pid(), (int)(int64_t)frame->x[0], &st, (int)frame->x[2]);
            if (rc > 0 && status_ptr) *status_ptr = st;
            result = (uint64_t)(int64_t)rc;
            break;
        }

        case SYS_GETPPID:
        {
            int me = from_user ? user_current_pid() : scheduler_current_pid();
            result = (uint64_t)(int64_t)process_parent(me);
            break;
        }

        case SYS_KILL:
        {
            int me = from_user ? user_current_pid() : -1;
            int target = (int)(int64_t)frame->x[0];
            /* Privilege separation: an unprivileged process may only signal its own
               processes (same uid) — root may signal anyone. */
            if (from_user && me >= 0 && process_uid(me) != 0 &&
                process_uid(target) != process_uid(me))
            {
                result = (uint64_t)-1;
                break;
            }
            result = (uint64_t)(int64_t)task_kill_info(target, (int)(int64_t)frame->x[1], me >= 0 ? me : 0, 0, 0);
            break;
        }

        case SYS_SIGQUEUE:
        {
            int me = from_user ? user_current_pid() : -1;
            int target = (int)(int64_t)frame->x[0];
            if (from_user && me >= 0 && process_uid(me) != 0 && process_uid(target) != process_uid(me)) { result = (uint64_t)-1; break; }
            result = (uint64_t)(int64_t)task_kill_info(target, (int)(int64_t)frame->x[1], me >= 0 ? me : 0, -1 /* SI_QUEUE */, frame->x[2]);
            break;
        }

        case SYS_SIGALTSTACK:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            int me = user_current_pid();
            struct StackT { uint64_t sp; int32_t flags; int32_t pad; uint64_t size; };
            uint64_t osp = 0, osz = 0; (void)process_altstack_get(me, &osp, &osz);
            if (frame->x[1] != 0)
            {
                if (!syscall_user_pointer_ok_write((void*)frame->x[1], sizeof(StackT))) { result = (uint64_t)-14; break; }
                StackT* o = (StackT*)frame->x[1]; o->sp = osp; o->size = osz; o->flags = osp ? 0 : 2 /* SS_DISABLE */; o->pad = 0;
            }
            if (frame->x[0] != 0)
            {
                if (!syscall_user_pointer_ok((const void*)frame->x[0], sizeof(StackT))) { result = (uint64_t)-14; break; }
                const StackT* n = (const StackT*)frame->x[0];
                if (n->flags & 2) (void)process_altstack_set(me, 0, 0);
                else if (n->size < 2048 || n->sp < MMU_USER_BASE || n->sp + n->size > MMU_USER_LIMIT) { result = (uint64_t)-22; break; }
                else (void)process_altstack_set(me, n->sp, n->size);
            }
            result = 0;
            break;
        }

        case SYS_SIGWAIT:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            int me = user_current_pid();
            uint32_t set = (uint32_t)frame->x[0] & ~1U;
            uint64_t tmo = frame->x[1];
            uint64_t deadline = tmo == ~0ULL ? ~0ULL : timer_millis() + tmo;
            struct Info { int32_t signo, err, code, pid; uint64_t value; uint64_t addr; };
            int got = 0;
            for (;;)
            {
                process_alarm_check(me, timer_millis());
                got = process_sig_take_set(me, set);
                if (got) break;
                if ((process_sig_pending(me) & ~set & ~((1U << SIG_CHLD) | (1U << SIG_CONT))) != 0) { result = (uint64_t)-4; break; }
                if (deadline != ~0ULL && timer_millis() >= deadline) { result = (uint64_t)-11; break; }
                thread_wait_ms(2);
            }
            if (!got) break;
            if (frame->x[2] != 0 && syscall_user_pointer_ok_write((void*)frame->x[2], sizeof(Info)))
            {
                Info* o = (Info*)frame->x[2]; int sender = 0, code = 0; uint64_t value = 0;
                (void)process_sig_info_get(me, got, &sender, &code, &value);
                o->signo = got; o->err = 0; o->code = code; o->pid = sender; o->value = value; o->addr = 0;
            }
            result = (uint64_t)got;
            break;
        }

        case SYS_HTTP_GET:
        {
            /* x0 url, x1 buf, x2 cap, x3 flags (bit0 insecure), x4 cacert path or 0 */
            if (!from_user) { result = (uint64_t)-38; break; }
            char url[256]; char ca[SYSCALL_PATH_MAX]; int ok = 1;
            for (uint64_t i = 0; ok; ++i)
            {
                if (i >= sizeof(url) || !frame->x[0] || !syscall_user_pointer_ok((const char*)(frame->x[0] + i), 1)) { ok = 0; break; }
                url[i] = ((const char*)frame->x[0])[i]; if (!url[i]) break;
            }
            if (ok && frame->x[4] && copy_path((const char*)frame->x[4], ca, from_user) != 0) ok = 0;
            if (!ok) { result = (uint64_t)-14; break; }
            if (!frame->x[1] || frame->x[2] == 0 || frame->x[2] > 1048576ULL || !syscall_user_pointer_ok_write((void*)frame->x[1], frame->x[2])) { result = (uint64_t)-14; break; }
            result = (uint64_t)(int64_t)net_http_get_url(url, (int)(frame->x[3] & 1U), frame->x[4] ? ca : 0, (uint8_t*)frame->x[1], (uint32_t)frame->x[2]);
            break;
        }

        case SYS_SHA256:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            if (frame->x[1] > 0x40000000ULL || (frame->x[1] && !syscall_user_pointer_ok((const void*)frame->x[0], frame->x[1])) ||
                !frame->x[2] || !syscall_user_pointer_ok_write((void*)frame->x[2], 32)) { result = (uint64_t)-14; break; }
            uint8_t dg[32]; sha256((const void*)frame->x[0], frame->x[1], dg);
            for (int i = 0; i < 32; ++i) ((uint8_t*)frame->x[2])[i] = dg[i];
            result = 0;
            break;
        }

        case SYS_SETITIMER:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            if (frame->x[0] == ~0ULL)
            {   /* query: remaining ms in the result, interval ms stored at *x1 when given */
                uint64_t iv = 0;
                result = process_itimer_get(user_current_pid(), timer_millis(), &iv);
                if (frame->x[1] && syscall_user_pointer_ok_write((void*)frame->x[1], 8)) *(uint64_t*)frame->x[1] = iv;
                break;
            }
            result = process_itimer_set(user_current_pid(), frame->x[0], frame->x[1], timer_millis());
            break;
        }

        case SYS_YIELD:
            thread_yield();
            result = 0;
            break;

        case SYS_BRK:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            uint64_t out = 0;
            int rc = mmu_user_brk(user_current_pid(), frame->x[0], &out);
            result = rc == 0 ? out : (uint64_t)(int64_t)-12;
            break;
        }

        case SYS_MMAP:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            int me = user_current_pid();
            uint64_t length = frame->x[0];
            uint32_t prot = (uint32_t)frame->x[1];
            int64_t fdn = (int64_t)frame->x[2];
            uint64_t off = frame->x[3];
            if (length == 0 || length > (4ULL << 20)) { result = (uint64_t)-22; break; }
            uint64_t base = 0;
            uint32_t map_prot = (fdn >= 0) ? (prot | MMU_USER_WRITE) & ~MMU_USER_EXEC : prot;
            int rc = mmu_user_mmap(me, length, map_prot, &base);
            if (rc != 0) { result = (uint64_t)-12; break; }
            if (fdn >= 0)
            {
                /* Private file mapping: copy the requested window into fresh pages. */
                if (select_fd_owner(from_user) != 0) { (void)mmu_user_munmap(me, base, length); result = (uint64_t)-24; break; }
                int64_t cur = fd_seek((int)fdn, 0, 1);
                if (cur < 0 || fd_seek((int)fdn, (int64_t)off, 0) < 0)
                { (void)mmu_user_munmap(me, base, length); result = (uint64_t)-9; break; }
                uint64_t done = 0;
                while (done < length)
                {
                    uint64_t chunk = length - done > 1024 ? 1024 : length - done;
                    if (!syscall_user_pointer_ok_write((void*)(base + done), chunk)) break;
                    int64_t n = fd_read((int)fdn, (void*)(base + done), chunk);
                    if (n <= 0) break;
                    done += (uint64_t)n;
                }
                (void)fd_seek((int)fdn, cur, 0);
                if ((prot & MMU_USER_WRITE) == 0) { /* read-only request: harmless, pages stay private */ }
            }
            result = base;
            break;
        }

        case SYS_MMAPX:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            int me = user_current_pid();
            uint64_t length = frame->x[0];
            uint32_t prot = (uint32_t)frame->x[1];
            int64_t fdn = (int64_t)frame->x[2];
            uint64_t off = frame->x[3] & 0xFFFFFFFFULL;
            uint32_t mflags = (uint32_t)(frame->x[3] >> 32);
            if (mflags & MAP_FIXED_FLAG) { result = (uint64_t)-22; break; }          /* MAP_FIXED is not supported */
            if (length == 0 || length > (4ULL << 20) || prot == 0 || (prot & ~7U) != 0) { result = (uint64_t)-22; break; }
            if ((mflags & MAP_SHARED_FLAG) != 0 && (mflags & MAP_PRIVATE_FLAG) != 0) { result = (uint64_t)-22; break; }
            if ((mflags & MAP_SHARED_FLAG) == 0)
            {
                /* private: same as the legacy mapping call */
                uint32_t map_prot = (fdn >= 0 && !(mflags & MAP_ANON_FLAG)) ? (prot | MMU_USER_WRITE) & ~MMU_USER_EXEC : prot;
                uint64_t base = 0;
                if (mmu_user_mmap(me, length, map_prot, &base) != 0) { result = (uint64_t)-12; break; }
                if (fdn >= 0 && !(mflags & MAP_ANON_FLAG))
                {
                    if (select_fd_owner(from_user) != 0) { (void)mmu_user_munmap(me, base, length); result = (uint64_t)-24; break; }
                    int64_t cur = fd_seek((int)fdn, 0, 1);
                    if (cur < 0 || fd_seek((int)fdn, (int64_t)off, 0) < 0) { (void)mmu_user_munmap(me, base, length); result = (uint64_t)-9; break; }
                    uint64_t done = 0;
                    while (done < length)
                    {
                        uint64_t chunk = length - done > 1024 ? 1024 : length - done;
                        if (!syscall_user_pointer_ok_write((void*)(base + done), chunk)) break;
                        int64_t n = fd_read((int)fdn, (void*)(base + done), chunk);
                        if (n <= 0) break;
                        done += (uint64_t)n;
                    }
                    (void)fd_seek((int)fdn, cur, 0);
                }
                result = base;
                break;
            }
            if (!(mflags & MAP_ANON_FLAG) && fdn < 0) { result = (uint64_t)-9; break; }
            if (select_fd_owner(from_user) != 0) { result = (uint64_t)-24; break; }
            int64_t r = mapping_mmap_shared(me, length, prot, (mflags & MAP_ANON_FLAG) ? -1 : (int)fdn, off);
            result = (uint64_t)r;
            break;
        }

        case SYS_MSYNC:
            result = from_user ? (uint64_t)(int64_t)mapping_msync(user_current_pid(), frame->x[0], frame->x[1]) : (uint64_t)-38;
            break;

        case SYS_MUNMAP:
            if (!from_user) { result = (uint64_t)-38; break; }
            (void)mapping_munmap(user_current_pid(), frame->x[0], frame->x[1]);
            result = (uint64_t)(int64_t)mmu_user_munmap(user_current_pid(), frame->x[0], frame->x[1]);
            mapping_gc();
            break;

        case SYS_MPROTECT:
            result = from_user ? (uint64_t)(int64_t)mmu_user_mprotect(user_current_pid(), frame->x[0], frame->x[1], (uint32_t)frame->x[2]) : (uint64_t)-38;
            break;

        case SYS_GETUID:
        {
            int cp = user_current_pid();
            result = from_user ? ((uint64_t)process_uid(cp) | ((uint64_t)process_gid(cp) << 32)) : 0;
            break;
        }

        case SYS_SETUID:
        {
            int cp = user_current_pid();
            if (!from_user) { result = (uint64_t)-38; break; }
            if (process_uid(cp) != 0) { result = (uint64_t)-1; break; }   /* EPERM: only root */
            (void)process_set_creds(cp, (uint32_t)frame->x[0], (uint32_t)frame->x[1]);
            fs_set_credentials((uint32_t)frame->x[0], (uint32_t)frame->x[1]);
            result = 0;
            break;
        }

        case SYS_SPAWN:
        case SYS_SPAWN_FD:
        {
            if (!from_user) { result = (uint64_t)-38; break; }
            char path[SYSCALL_PATH_MAX];
            char args[PROCESS_ARGS_SIZE];
            if (copy_path((const char*)frame->x[0], path, from_user) != 0) { result = (uint64_t)-14; break; }
            args[0] = '\0';
            if (frame->x[1] != 0)
            {
                int ok = 0;
                for (int i = 0; i < PROCESS_ARGS_SIZE; ++i)
                {
                    if (!syscall_user_pointer_ok((const char*)frame->x[1] + i, 1)) break;
                    args[i] = ((const char*)frame->x[1])[i];
                    if (args[i] == '\0') { ok = 1; break; }
                }
                if (!ok) { result = (uint64_t)-14; break; }
            }
            int me = user_current_pid();
            int in_fd = -1, out_fd = -1, bg = 0;
            if (number == SYS_SPAWN_FD) { in_fd = (int)(int64_t)frame->x[2]; out_fd = (int)(int64_t)frame->x[3]; }
            else bg = (int)(frame->x[2] & 1);
            int rc = task_spawn(path, args, me, bg, in_fd, out_fd);
            (void)mmu_user_space_activate(me);
            (void)select_fd_owner(from_user);
            result = (uint64_t)(int64_t)rc;
            break;
        }

        case SYS_PROCINFO:
        {
            ProcInfo* out = (ProcInfo*)frame->x[1];
            int target = (int)(int64_t)frame->x[0];
            if (target < 0) target = user_current_pid();
            if (!out || (from_user && !syscall_user_pointer_ok_write(out, sizeof(ProcInfo)))) { result = (uint64_t)-14; break; }
            if (process_get_state(target) < 0) { result = (uint64_t)-3; break; }
            out->pid = target; out->ppid = process_parent(target); out->state = process_get_state(target);
            out->thread = process_thread(target); out->uid = process_uid(target); out->gid = process_gid(target);
            out->start_ms = process_start_ms(target);
            out->resident_pages = out->shared_pages = out->cow_faults = out->demand_faults = 0;
            (void)mmu_user_space_stats(target, &out->resident_pages, &out->shared_pages, &out->cow_faults, &out->demand_faults);
            const char* nm = process_name(target);
            int i = 0; while (nm[i] && i < 31) { out->name[i] = nm[i]; ++i; } out->name[i] = 0;
            result = 0;
            break;
        }
#endif

        default:
            result = (uint64_t)-38;
            break;
    }

    /* Do not advance ELR_EL1 here: for the SVC exception class it is
       already set to the instruction following the SVC, unlike a fault
       (data/instruction abort) where ELR points at the faulting
       instruction and must be skipped manually. Adding 4 here made the
       kernel eret one instruction past the caller's true return address,
       corrupting control flow on every syscall. */
    frame->x[0] = result;
}
