#include "fault.h"
#include "uart.h"

static uint64_t user_faults = 0;
static uint64_t kernel_faults = 0;

static const char* status_name(uint32_t fsc, uint32_t* level)
{
    *level = 4;

    switch (fsc & 0x3CU)
    {
        case 0x04: *level = fsc & 3U; return "translation fault";
        case 0x08: *level = fsc & 3U; return "access flag fault";
        case 0x0C: *level = fsc & 3U; return "permission fault";
        case 0x10: return "synchronous external abort";
        case 0x20: return "alignment fault";
        default: break;
    }

    if (fsc == 0x21U)
        return "alignment fault";

    return "other abort";
}

void fault_decode(uint64_t esr, FaultInfo* out)
{
    if (out == 0)
        return;

    uint32_t ec = (uint32_t)((esr >> 26) & 0x3FU);
    uint32_t fsc = (uint32_t)(esr & 0x3FU);

    out->kind = FAULT_OTHER;
    out->from_user = 0;
    out->is_write = 0;
    out->cause = "unknown";
    out->level = 4;

    switch (ec)
    {
        case 0x20: case 0x21:
            out->kind = FAULT_INSTRUCTION_ABORT;
            out->from_user = ec == 0x20U;
            out->cause = status_name(fsc, &out->level);
            break;

        case 0x24: case 0x25:
            out->kind = FAULT_DATA_ABORT;
            out->from_user = ec == 0x24U;
            out->is_write = (int)((esr >> 6) & 1ULL);
            out->cause = status_name(fsc, &out->level);
            break;

        case 0x22: case 0x26:
            out->kind = FAULT_ALIGNMENT;
            out->cause = ec == 0x22U ? "PC alignment fault" : "SP alignment fault";
            break;

        case 0x00:
            out->kind = FAULT_ILLEGAL;
            out->cause = "unknown reason";
            break;

        case 0x0E:
            out->kind = FAULT_ILLEGAL;
            out->cause = "illegal execution state";
            break;

        default:
            break;
    }
}

static void put_hex(uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    uart_puts("0x");

    for (int i = 15; i >= 0; --i)
        uart_putc(digits[(value >> ((uint32_t)i * 4U)) & 0xFULL]);
}

void fault_report(uint64_t esr, uint64_t elr, uint64_t far, uint64_t spsr)
{
    FaultInfo info;
    fault_decode(esr, &info);

    if (info.from_user)
        ++user_faults;
    else
        ++kernel_faults;

    uart_puts(info.from_user ? "[FAULT] user " : "[FAULT] kernel ");

    switch (info.kind)
    {
        case FAULT_INSTRUCTION_ABORT: uart_puts("instruction abort: "); break;
        case FAULT_DATA_ABORT:
            uart_puts(info.is_write ? "data abort (write): " : "data abort (read): ");
            break;
        case FAULT_ALIGNMENT: uart_puts("alignment: "); break;
        case FAULT_ILLEGAL: uart_puts("illegal instruction/state: "); break;
        default: uart_puts("exception: "); break;
    }

    uart_puts(info.cause);

    if (info.level < 4U)
    {
        uart_puts(" level ");
        uart_putc((char)('0' + info.level));
    }

    uart_puts("\r\n  ELR=");  put_hex(elr);
    uart_puts(" FAR=");       put_hex(far);
    uart_puts(" ESR=");       put_hex(esr);
    uart_puts(" SPSR=");      put_hex(spsr);
    uart_puts("\r\n");
}

uint64_t fault_user_count() { return user_faults; }
uint64_t fault_kernel_count() { return kernel_faults; }
