#include "rtc.h"

#define RTC_BASE 0x09010000ULL
#define RTC_DR    0x00ULL
#define RTC_LR    0x08ULL
#define RTC_CR    0x0CULL

#ifndef HOST_TEST
static volatile uint32_t* const RTC_DR_REG =
    (volatile uint32_t*)(RTC_BASE + RTC_DR);
static volatile uint32_t* const RTC_LR_REG =
    (volatile uint32_t*)(RTC_BASE + RTC_LR);
static volatile uint32_t* const RTC_CR_REG =
    (volatile uint32_t*)(RTC_BASE + RTC_CR);
#endif

uint64_t rtc_get_epoch()
{
#ifdef HOST_TEST
    return 0;
#else
    return (uint64_t)(*RTC_DR_REG);
#endif
}

int rtc_set_epoch(uint64_t epoch)
{
#ifdef HOST_TEST
    (void)epoch;
    return -1;
#else
    if (epoch > 0xFFFFFFFFULL)
        return -1;
    *RTC_LR_REG = (uint32_t)epoch;
    *RTC_CR_REG = 1U;
    return 0;
#endif
}
