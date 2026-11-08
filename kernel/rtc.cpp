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
