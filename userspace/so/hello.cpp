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

/* Shared object loaded at run time by dltest via dlopen(). */
typedef unsigned long size_t;
extern "C" int printf(const char*, ...);
extern "C" int getpid(void);

static int counter = 10;                       /* writable data, needs RELATIVE reloc for the pointer below */
static const char* const names[] = { "alpha", "beta", "gamma" };

extern "C" int libhello_add(int a, int b) { return a + b + counter; }
extern "C" const char* libhello_name(int i) { return names[i % 3]; }
extern "C" int libhello_bump(void) { return ++counter; }
extern "C" void libhello_greet(const char* who) { printf("libhello: hi %s from pid %d\n", who, getpid()); }
static int mul(int a, int b) { return a * b; }
static int (*op_table[])(int, int) = { libhello_add, mul };
extern "C" int libhello_call(int idx, int a, int b) { return op_table[idx](a, b); }
