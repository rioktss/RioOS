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

    uint32_t saved_fs_uid = fs_current_uid();
    uint32_t saved_fs_gid = fs_current_gid();
    if (from_user) fs_set_credentials(1000U, 1000U);

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
            result = len;
            break;
        }

        case SYS_SLEEP_MS:
            if (frame->x[0] > SYSCALL_SLEEP_MAX_MS)
            {
                result = (uint64_t)-22;
                break;
            }
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
                if (!keyboard_try_getc(&input_char)) break;
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
            if (raw_flags > 0x7FFULL)
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
            if (frame->x[1] > 1048576ULL)
            {
                result = (uint64_t)-27;
                break;
            }
            result = (uint64_t)(int64_t)fs_truncate(path, vfs_cwd(), frame->x[1]);
            break;
        }

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
    if (from_user) fs_set_credentials(saved_fs_uid, saved_fs_gid);
}
