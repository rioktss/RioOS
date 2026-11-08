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

#include "string.h"

int str_equal(const char* a, const char* b)
{
    if (a == 0 || b == 0)
        return 0;

    while (*a && *b)
    {
        if (*a != *b)
            return 0;

        a++;
        b++;
    }

    return *a == '\0' && *b == '\0';
}

int str_len(const char* s)
{
    if (s == 0)
        return 0;

    int length = 0;

    while (*s)
    {
        length++;
        s++;
    }

    return length;
}

void str_copy(
    char* dst,
    const char* src,
    int max
)
{
    if (dst == 0 || src == 0 || max <= 0)
        return;

    int i = 0;

    while (src[i] && i < max - 1)
    {
        dst[i] = src[i];
        i++;
    }

    dst[i] = '\0';
}

#ifndef HOST_TEST
/* Freestanding builds (-ffreestanding -fno-builtin) still need these: the
   compiler may emit calls to them for struct copies and large initialisers.
   Plain byte loops; -fno-builtin stops the compiler turning them back into
   calls to themselves. */
/* Under clang, optnone also guarantees these loops are never rewritten into a
   call to themselves (infinite recursion) by the loop-idiom pass. */
#if defined(__clang__)
#define MEMFN_ATTR __attribute__((optnone))
#else
#define MEMFN_ATTR
#endif

extern "C"
{
    MEMFN_ATTR void* memcpy(void* dst, const void* src, unsigned long n)
    {
        unsigned char* d = (unsigned char*)dst;
        const unsigned char* s = (const unsigned char*)src;

        while (n--)
            *d++ = *s++;

        return dst;
    }

    MEMFN_ATTR void* memmove(void* dst, const void* src, unsigned long n)
    {
        unsigned char* d = (unsigned char*)dst;
        const unsigned char* s = (const unsigned char*)src;

        if (d == s || n == 0)
            return dst;

        if (d < s)
        {
            while (n--)
                *d++ = *s++;
        }
        else
        {
            d += n;
            s += n;

            while (n--)
                *--d = *--s;
        }

        return dst;
    }

    MEMFN_ATTR void* memset(void* dst, int value, unsigned long n)
    {
        unsigned char* d = (unsigned char*)dst;

        while (n--)
            *d++ = (unsigned char)value;

        return dst;
    }

    MEMFN_ATTR int memcmp(const void* a, const void* b, unsigned long n)
    {
        const unsigned char* x = (const unsigned char*)a;
        const unsigned char* y = (const unsigned char*)b;

        while (n--)
        {
            if (*x != *y)
                return (int)*x - (int)*y;

            ++x;
            ++y;
        }

        return 0;
    }
}
#endif
