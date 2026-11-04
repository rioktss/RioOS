#ifndef TEXTUTILS_H
#define TEXTUTILS_H

#include "types.h"

/* Small regular-expression engine: . * + ? ^ $ [set] [^set] \d \w \s \. and top-level alternation a|b.
   Returns 1 and the leftmost match [*ms,*me) inside text[0..len), or 0. */
int re_search(const char* re, const char* text, int len, int* ms, int* me);

/* Text filters: sed awk rev tac nl xargs. Same contract as the other filters:
   0 ok, -1 not one of these, -2 bad usage. `out` has room for 131072 bytes. */
int textutils_is_filter(const char* name);
int textutils_apply(char** w, int n, const char* in, uint32_t inlen, char* out, uint32_t* outlen, uint32_t outcap);

#endif
