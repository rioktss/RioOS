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

#include "thread.h"
#include "interrupt.h"
#include "uart.h"
#include "timer.h"
#include "smp.h"

/* QEMU virt with GICv2. */
#define GICD_BASE 0x08000000ULL
#define GICC_BASE 0x08010000ULL

#define GICD_CTLR        0x000
#define GICD_IGROUPR0    0x080
#define GICD_ISENABLER0  0x100
#define GICD_ICENABLER0  0x180
#define GICD_ICPENDR0    0x280
#define GICD_IPRIORITYR0 0x400

#define GICC_CTLR 0x000
#define GICC_PMR  0x004
#define GICC_IAR  0x00C
#define GICC_EOIR 0x010

#define TIMER_IRQ 30U
#define IRQ_SPURIOUS_BASE 1020U
#define GIC_GROUP1_ENABLED 2U
/* Bit0 = EnableGrp0 / Enable, bit1 = EnableGrp1. Writing both is correct in
   every GICv2 view: single-state (QEMU virt default), secure and non-secure
   (the reserved bit is ignored). Writing only bit1 would leave the CPU
   interface disabled in a non-secure view. */
#define GIC_ENABLE_ALL 0x3U
#define GICC_CTLR_VALUE 0x7U /* EnableGrp0 | EnableGrp1 | AckCtl: IAR must return Group 1 IDs in Secure view (else 1022 forever = IRQ storm) */

static volatile uint32_t* const GICD_CTLR_REG =
    (volatile uint32_t*)(GICD_BASE + GICD_CTLR);

static volatile uint32_t* const GICD_IGROUPR0_REG =
    (volatile uint32_t*)(GICD_BASE + GICD_IGROUPR0);

static volatile uint32_t* const GICD_ISENABLER0_REG =
    (volatile uint32_t*)(GICD_BASE + GICD_ISENABLER0);

static volatile uint32_t* const GICD_ICENABLER0_REG =
    (volatile uint32_t*)(GICD_BASE + GICD_ICENABLER0);

static volatile uint32_t* const GICD_ICPENDR0_REG =
    (volatile uint32_t*)(GICD_BASE + GICD_ICPENDR0);

static volatile uint8_t* const GICD_IPRIORITYR0_REG =
    (volatile uint8_t*)(GICD_BASE + GICD_IPRIORITYR0);

static volatile uint32_t* const GICC_CTLR_REG =
    (volatile uint32_t*)(GICC_BASE + GICC_CTLR);

static volatile uint32_t* const GICC_PMR_REG =
    (volatile uint32_t*)(GICC_BASE + GICC_PMR);

static volatile uint32_t* const GICC_IAR_REG =
    (volatile uint32_t*)(GICC_BASE + GICC_IAR);

static volatile uint32_t* const GICC_EOIR_REG =
    (volatile uint32_t*)(GICC_BASE + GICC_EOIR);

static volatile uint64_t irq_total = 0;
static volatile uint32_t last_irq = 0xFFFFFFFFU;
static volatile uint64_t irq_timer = 0;
static volatile uint64_t irq_spurious = 0;
static volatile uint64_t irq_unhandled = 0;
static volatile uint64_t irq_nested = 0;
static volatile uint32_t irq_depth = 0;
static uint32_t spurious_run = 0;
static volatile uint32_t irq_max_depth = 0;

static void print_hex64(uint64_t value)
{
    static const char digits[] = "0123456789ABCDEF";

    uart_puts("0x");

    for (int i = 15; i >= 0; --i)
    {
        uint32_t shift = (uint32_t)i * 4U;
        uart_putc(digits[(value >> shift) & 0xFULL]);
    }
}

void interrupt_init()
{
    /* Keep CPU IRQ/FIQ/etc. masked during controller setup. */
    asm volatile(
        "msr daifset, #0xf\n"
        "dsb sy\n"
        "isb\n"
        ::: "memory");

    /* Disable the distributor while it is being configured. */
    *GICD_CTLR_REG = 0;

    /* Physical timer PPI 30 is a non-secure Group 1 interrupt on QEMU virt. */
    *GICD_IGROUPR0_REG |= (1U << TIMER_IRQ) | 1U;      /* + SGI 0: wake-up kick between CPUs */
    GICD_IPRIORITYR0_REG[TIMER_IRQ] = 0x80U;
    GICD_IPRIORITYR0_REG[0] = 0x80U;

    /* Start with the timer PPI disabled and any stale pending bit cleared. */
    *GICD_ICENABLER0_REG = (1U << TIMER_IRQ);
    *GICD_ICPENDR0_REG = (1U << TIMER_IRQ);

    /* Accept all Group 1 priorities and enable only Group 1 at the CPU. */
    *GICC_PMR_REG = 0xFFU;
    *GICC_CTLR_REG = GICC_CTLR_VALUE;

    /* Enable the distributor; the PPI stays disabled until start. */
    *GICD_CTLR_REG = GIC_ENABLE_ALL;

    asm volatile(
        "dsb sy\n"
        "isb\n"
        ::: "memory");

    uart_puts("Interrupt controller initialized.\r\n");
}

/* A secondary CPU: its own GIC CPU interface and the banked timer PPI. */
extern "C" void interrupt_init_secondary()
{
    *GICC_PMR_REG = 0xFFU;
    *GICC_CTLR_REG = GICC_CTLR_VALUE;
    *GICD_IGROUPR0_REG |= (1U << TIMER_IRQ) | 1U;
    GICD_IPRIORITYR0_REG[TIMER_IRQ] = 0x80U;
    GICD_IPRIORITYR0_REG[0] = 0x80U;
    *GICD_ICPENDR0_REG = (1U << TIMER_IRQ) | 1U;
    *GICD_ISENABLER0_REG = (1U << TIMER_IRQ) | 1U;
    asm volatile("dsb sy\n isb" ::: "memory");
}

/* Wake every other CPU out of wfi (SGI 0, target filter "all but self"). */
extern "C" void interrupt_kick_others()
{
    asm volatile("dsb sy" ::: "memory");
    *(volatile uint32_t*)(GICD_BASE + 0xF00) = (1U << 24) | 0U;
}

void interrupt_enable()
{
    /* Enable the timer PPI before unmasking CPU IRQs. */
    *GICD_ISENABLER0_REG = (1U << TIMER_IRQ) | 1U;

    asm volatile(
        "dsb sy\n"
        "isb\n"
        ::: "memory");

    asm volatile(
        "msr daifclr, #0x2\n"
        "dsb sy\n"
        "isb\n"
        ::: "memory");
}

void interrupt_disable()
{
    asm volatile(
        "msr daifset, #0x2\n"
        "dsb sy\n"
        "isb\n"
        ::: "memory");
}

uint64_t interrupt_count()
{
    return irq_total;
}

uint32_t interrupt_last_irq()
{
    return last_irq;
}

extern "C" void interrupt_dispatch(ExceptionFrame* frame)
{
    (void)frame;

    uint32_t iar = *GICC_IAR_REG;
    uint32_t irq = iar & 0x3FFU;

    /* 1020..1023 are GICv2 spurious/reserved interrupt IDs: count them,
       but never write EOIR for them. */
    if (irq >= IRQ_SPURIOUS_BASE)
    {
        ++irq_spurious;
        /* Safety net: a line that keeps asserting but is never acknowledged
           would starve the CPU. Mask the tick instead of freezing. */
        if (++spurious_run > 2000U)
        {
            *GICD_ICENABLER0_REG = (1U << TIMER_IRQ);
            spurious_run = 0;
        }
        return;
    }

    /* IRQs stay masked while we are in the handler, so depth must be 1.
       Track it so a future change that unmasks early is caught. */
    spurious_run = 0;
    uint32_t depth = irq_depth + 1U;
    irq_depth = depth;

    if (depth > 1U)
        ++irq_nested;

    if (depth > irq_max_depth)
        irq_max_depth = depth;

    irq_total++;
    last_irq = irq;

    switch (irq)
    {
        case TIMER_IRQ:
            ++irq_timer;
            timer_interrupt_handler();
            break;

        case 0:                          /* SGI 0: wake-up kick, nothing to do (the wake itself is the point) */
            break;

        default:
            ++irq_unhandled;

            /* Avoid flooding the UART if an unknown line keeps asserting:
               report only the first few occurrences. */
            if (irq_unhandled <= 8ULL)
            {
                uart_puts("Unhandled IRQ: ");
                print_hex64((uint64_t)irq);
                uart_puts("\r\n");
            }
            break;
    }

    irq_depth = depth - 1U;

    /* Complete the exact interrupt acknowledged by IAR. */
    *GICC_EOIR_REG = iar;

    asm volatile(
        "dsb sy\n"
        "isb\n"
        ::: "memory");
}

void interrupt_get_stats(InterruptStats* out)
{
    if (out == 0)
        return;

    out->total = irq_total;
    out->timer = irq_timer;
    out->spurious = irq_spurious;
    out->unhandled = irq_unhandled;
    out->nested = irq_nested;
    out->last_irq = last_irq;
    out->max_depth = irq_max_depth;
}

void interrupt_idle()
{
    /* Sleep until the next tick when the 100 Hz timer is guaranteed to wake
       us; otherwise (tick stopped via `irq off`, or IRQs masked) just hint. */
    /* Never sleep: input is polled, so a missing wake-up can't freeze typing. */
    asm volatile("yield" ::: "memory");
    /* Let background user tasks run while the shell waits for input. */
    if (thread_user_tasks() > 0) thread_yield();
    smp_idle_wait();        /* several CPUs: sleep until the next tick instead of burning a CPU (no-op otherwise) */
}

int interrupt_cpu_irq_enabled()
{
    uint64_t daif;

    asm volatile("mrs %0, daif" : "=r"(daif) : : "memory");

    return (daif & 0x80ULL) == 0;
}

void interrupt_timer_line_enable()
{
    *GICD_ICPENDR0_REG = (1U << TIMER_IRQ);
    *GICD_ISENABLER0_REG = (1U << TIMER_IRQ);

    asm volatile("dsb sy\nisb\n" ::: "memory");
}

void interrupt_timer_line_disable()
{
    *GICD_ICENABLER0_REG = (1U << TIMER_IRQ);
    *GICD_ICPENDR0_REG = (1U << TIMER_IRQ);

    asm volatile("dsb sy\nisb\n" ::: "memory");
}

extern "C" void interrupt_exception(
    uint64_t esr,
    uint64_t elr,
    uint64_t far,
    uint64_t spsr)
{
    uart_puts("\r\n*** KERNEL EXCEPTION ***\r\n");
    uart_puts("ESR  = ");
    print_hex64(esr);
    uart_puts("\r\nELR  = ");
    print_hex64(elr);
    uart_puts("\r\nFAR  = ");
    print_hex64(far);
    uart_puts("\r\nSPSR = ");
    print_hex64(spsr);
    uart_puts("\r\n");

    while (1)
        asm volatile("wfe");
}
