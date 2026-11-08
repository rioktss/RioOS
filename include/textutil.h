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

#ifndef TEXTUTIL_H
#define TEXTUTIL_H

#include "types.h"

/*
 * Pure, allocation-free text utilities used by the shell for file commands
 * and for multi-stage pipelines (cat f | sort | uniq -c | head -n 5).
 *
 * Every filter reads a byte buffer and writes a byte buffer, so the same code
 * is exercised by host tests and by the freestanding kernel.
 */

#define TEXTUTIL_OK            0
#define TEXTUTIL_ERR_USAGE    -1   /* bad option or operand            */
#define TEXTUTIL_ERR_UNKNOWN  -2   /* not a known filter name          */
#define TEXTUTIL_ERR_OVERFLOW -3   /* output buffer too small          */

/* Returns 1 when `name` is a filter handled by textutil_filter(). */
int textutil_is_filter(const char* name);

/*
 * Run one filter. argv[0] is the filter name. `in`/`out` must not overlap.
 * Supported: head tail sort uniq rev tac nl cut tr grep wc
 */
int textutil_filter(int argc, char** argv,
                    const uint8_t* in, uint64_t in_len,
                    uint8_t* out, uint64_t out_cap, uint64_t* out_len);

/* Human-readable usage line for a filter (or 0 when unknown). */
const char* textutil_usage(const char* name);

/* Integer expression evaluator: + - * / % ( ) and unary minus. */
int textutil_eval(const char* expr, int64_t* result);

/* Path helpers. Output is always NUL-terminated and truncated to `cap`. */
void textutil_basename(const char* path, char* out, int cap);
void textutil_dirname(const char* path, char* out, int cap);

#endif
