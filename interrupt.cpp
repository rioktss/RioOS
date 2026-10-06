#include "interrupt.h"
#include "uart.h"
#include "timer.h"

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
#define GIC_ENABLE_ALL 3U

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
    *GICD_IGROUPR0_REG |= (1U << TIMER_IRQ);
    GICD_IPRIORITYR0_REG[TIMER_IRQ] = 0x80U;

    /* Start with the timer PPI disabled and any stale pending bit cleared. */
    *GICD_ICENABLER0_REG = (1U << TIMER_IRQ);
    *GICD_ICPENDR0_REG = (1U << TIMER_IRQ);

    /* Accept all Group 1 priorities and enable only Group 1 at the CPU. */
    *GICC_PMR_REG = 0xFFU;
    *GICC_CTLR_REG = GIC_ENABLE_ALL;

    /* Enable the distributor; the PPI stays disabled until start. */
    *GICD_CTLR_REG = GIC_ENABLE_ALL;

    asm volatile(
        "dsb sy\n"
        "isb\n"
        ::: "memory");

    uart_puts("Interrupt controller initialized.\r\n");
}

void interrupt_enable()
{
    /* Enable the timer PPI before unmasking CPU IRQs. */
    *GICD_ISENABLER0_REG = (1U << TIMER_IRQ);

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
        return;
    }

    /* IRQs stay masked while we are in the handler, so depth must be 1.
       Track it so a future change that unmasks early is caught. */
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
