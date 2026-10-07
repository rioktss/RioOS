#ifndef STRING_UTILS_H
#define STRING_UTILS_H

int str_equal(
    const char* a,
    const char* b
);

int str_len(
    const char* s
);

void str_copy(
    char* dst,
    const char* src,
    int max
);

#endif