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