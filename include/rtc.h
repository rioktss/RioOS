#ifndef RTC_H
#define RTC_H

#include "types.h"

/* QEMU virt PL031 RTC.  No interrupt is used. */
uint64_t rtc_get_epoch();
int rtc_set_epoch(uint64_t epoch);

#endif
