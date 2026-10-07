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
extern "C"
{
    void* memcpy(void* dst, const void* src, unsigned long n)
    {
        unsigned char* d = (unsigned char*)dst;
        const unsigned char* s = (const unsigned char*)src;

        while (n--)
            *d++ = *s++;

        return dst;
    }

    void* memmove(void* dst, const void* src, unsigned long n)
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

    void* memset(void* dst, int value, unsigned long n)
    {
        unsigned char* d = (unsigned char*)dst;

        while (n--)
            *d++ = (unsigned char)value;

        return dst;
    }

    int memcmp(const void* a, const void* b, unsigned long n)
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
