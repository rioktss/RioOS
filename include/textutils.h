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
