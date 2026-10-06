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
    *GICC_CTLR_REG = GIC_GROUP1_ENABLED;

    /* Enable Group 1 at the distributor; the PPI stays disabled until start. */
    *GICD_CTLR_REG = GIC_GROUP1_ENABLED;

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

    /* 1020..1023 are GICv2 spurious/reserved interrupt IDs. */
    if (irq >= IRQ_SPURIOUS_BASE)
        return;

    irq_total++;
    last_irq = irq;

    switch (irq)
    {
        case TIMER_IRQ:
            timer_interrupt_handler();
            break;

        default:
            uart_puts("Unhandled IRQ: ");
            print_hex64((uint64_t)irq);
            uart_puts("\r\n");
            break;
    }

    /* Complete the exact interrupt acknowledged by IAR. */
    *GICC_EOIR_REG = iar;

    asm volatile(
        "dsb sy\n"
        "isb\n"
        ::: "memory");
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
